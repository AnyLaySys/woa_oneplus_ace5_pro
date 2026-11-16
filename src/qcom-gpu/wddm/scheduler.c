#include "adapter.h"

/* Only Rendered records are on the context token table. Submit removes a
   record atomically and transfers its owned refs to A830_JOB (Queued/Issued).
   DMA private data carries a context+sequence token, never the prepared pointer.
   Cancel/Destroy can therefore discard unsubmitted work without stale pointers
   or releasing a packet that the GPU may still read. */
typedef struct { LIST_ENTRY Entry; A830_DMA Dma; } A830_RENDERED;
static BOOLEAN TakeRendered(A830_ADAPTER *a,const A830_DMA *token,A830_DMA *out) {
    BOOLEAN found=FALSE;KIRQL old;KeAcquireSpinLock(&a->ContextLock,&old);
    for(PLIST_ENTRY e=a->Contexts.Flink;e!=&a->Contexts;e=e->Flink) {
        A830_CONTEXT *c=CONTAINING_RECORD(e,A830_CONTEXT,Entry);
        if(c!=token->Context)continue;
        KeAcquireSpinLockAtDpcLevel(&c->TokenLock);
        for(PLIST_ENTRY v=c->Rendered.Flink;v!=&c->Rendered;v=v->Flink) {
            A830_RENDERED *record=CONTAINING_RECORD(v,A830_RENDERED,Entry);
            if(record->Dma.Sequence!=token->Sequence)continue;
            RemoveEntryList(v);*out=record->Dma;ExFreePoolWithTag(record,GPU_TAG);found=TRUE;break;
        }
        KeReleaseSpinLockFromDpcLevel(&c->TokenLock);break;
    }
    KeReleaseSpinLock(&a->ContextLock,old);return found;
}
VOID A830CancelRendered(A830_ADAPTER *a,const A830_DMA *token) {
    if(token->Operation!=5 || !token->Context)return;
    A830_JOB job={0};if(TakeRendered(a,token,&job.Dma))A830FinishJob(&job,FALSE);
}
VOID A830CancelRenderedClient(A830_ADAPTER *a,PEPROCESS process,ULONGLONG lease) {
    if(!a || !process)return;
    LIST_ENTRY cancelled;InitializeListHead(&cancelled);
    KIRQL old;KeAcquireSpinLock(&a->ContextLock,&old);
    for(PLIST_ENTRY e=a->Contexts.Flink;e!=&a->Contexts;e=e->Flink) {
        A830_CONTEXT *c=CONTAINING_RECORD(e,A830_CONTEXT,Entry);if(c->Process!=process)continue;
        KeAcquireSpinLockAtDpcLevel(&c->TokenLock);
        if(c->ClientLease==lease) {
            /* This also revokes a Render still preparing BO refs, before it
               has published its token. Its publication rechecks Closing. */
            c->Closing=TRUE;
            A830_COMPLETION *page=(PVOID)c->CompletionBuffer.Cpu;
            if(!InterlockedCompareExchange((volatile LONG *)&page->Status,STATUS_DEVICE_REMOVED,0))
                InterlockedIncrement((volatile LONG *)&page->Generation);
            KeSetEvent(c->Completion,IO_NO_INCREMENT,FALSE);
        }
        PLIST_ENTRY v=c->Rendered.Flink;
        while(v!=&c->Rendered) {
            A830_RENDERED *record=CONTAINING_RECORD(v,A830_RENDERED,Entry);v=v->Flink;
            if(record->Dma.Lease==lease){RemoveEntryList(&record->Entry);InsertTailList(&cancelled,&record->Entry);}
        }
        KeReleaseSpinLockFromDpcLevel(&c->TokenLock);
    }
    KeReleaseSpinLock(&a->ContextLock,old);
    while(!IsListEmpty(&cancelled)) {
        A830_RENDERED *record=CONTAINING_RECORD(RemoveHeadList(&cancelled),A830_RENDERED,Entry);
        A830_JOB job={0};job.Dma=record->Dma;ExFreePoolWithTag(record,GPU_TAG);A830FinishJob(&job,FALSE);
    }
}
NTSTATUS A830ContextStart(A830_CONTEXT *c,A830_CONTEXT_DATA *data) {
    ExInitializeRundownProtection(&c->Jobs);
    NTSTATUS status=GpuAllocateCached(&c->CompletionBuffer,PAGE_SIZE);if(!NT_SUCCESS(status))return status;
    __try {c->CompletionMap=MmMapLockedPagesSpecifyCache(c->CompletionBuffer.Mdl,UserMode,MmCached,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute|MdlMappingNoWrite);}
    __except(EXCEPTION_EXECUTE_HANDLER){status=GetExceptionCode();}
    if(!c->CompletionMap){GpuFree(&c->CompletionBuffer);return NT_SUCCESS(status)?STATUS_INSUFFICIENT_RESOURCES:status;}
    A830_COMPLETION *page=(PVOID)c->CompletionBuffer.Cpu;page->Generation=(ULONG)c->Adapter->Generation;
    data->CompletionPage=(ULONGLONG)c->CompletionMap;
    KIRQL old;KeAcquireSpinLock(&c->Adapter->ContextLock,&old);InsertTailList(&c->Adapter->Contexts,&c->Entry);KeReleaseSpinLock(&c->Adapter->ContextLock,old);
    return STATUS_SUCCESS;
}
VOID A830ContextStop(A830_CONTEXT *c) {
    LIST_ENTRY cancelled;InitializeListHead(&cancelled);
    KIRQL old;KeAcquireSpinLock(&c->Adapter->ContextLock,&old);
    KeAcquireSpinLockAtDpcLevel(&c->TokenLock);c->Closing=TRUE;
    if(c->CompletionBuffer.Mdl)RemoveEntryList(&c->Entry);
    while(!IsListEmpty(&c->Rendered))InsertTailList(&cancelled,RemoveHeadList(&c->Rendered));
    KeReleaseSpinLockFromDpcLevel(&c->TokenLock);KeReleaseSpinLock(&c->Adapter->ContextLock,old);
    while(!IsListEmpty(&cancelled)) {
        A830_RENDERED *record=CONTAINING_RECORD(RemoveHeadList(&cancelled),A830_RENDERED,Entry);
        A830_JOB job={0};job.Dma=record->Dma;ExFreePoolWithTag(record,GPU_TAG);A830FinishJob(&job,FALSE);
    }
    /* Only jobs already transferred to Submit remain; their refs are released
       by actual fence retirement or quiesced reset, never by a counter reset. */
    ExWaitForRundownProtectionRelease(&c->Jobs);
    if(c->CompletionMap) {
        KAPC_STATE state;BOOLEAN attach=PsGetCurrentProcess()!=c->Process;
        if(attach)KeStackAttachProcess(c->Process,&state);
        MmUnmapLockedPages(c->CompletionMap,c->CompletionBuffer.Mdl);
        if(attach)KeUnstackDetachProcess(&state);
    }
    GpuFree(&c->CompletionBuffer);
}
VOID A830FinishJob(A830_JOB *job,BOOLEAN signal) {
    A830ReleasePresent(&job->Dma);
    A830_CONTEXT *c=job->Dma.Context;
    if(c) {
        A830_COMPLETION *page=(PVOID)c->CompletionBuffer.Cpu;
        if(signal && !c->Adapter->Stopping && !c->Adapter->Failed) {
            if(!job->HardwareFence)InterlockedExchange64((volatile LONG64 *)&page->Completed,job->Dma.Sequence);
        }
        else {InterlockedExchange((volatile LONG *)&page->Status,STATUS_DEVICE_REMOVED);InterlockedIncrement((volatile LONG *)&page->Generation);}
        KeMemoryBarrier();
    }
    A830ReleasePrepared(job->Dma.Prepared);job->Dma.Prepared=NULL;
    PKEVENT event=job->Dma.Completion;job->Dma.Completion=NULL;
    if(event){KeSetEvent(event,IO_NO_INCREMENT,FALSE);ObDereferenceObject(event);}
    if(c){ExReleaseRundownProtection(&c->Jobs);job->Dma.Context=NULL;}
}
VOID A830RecycleJob(A830_ADAPTER *a,A830_JOB *job) {
    if(job->Dma.Operation!=5)InterlockedDecrement(&a->CpuBarriers);
    if(job->Dynamic)ExFreePoolWithTag(job,GPU_TAG);
    else ExInterlockedInsertTailList(&a->Free,&job->Entry,&a->QueueLock);
}
NTSTATUS APIENTRY A830Render(HANDLE context,DXGKARG_RENDER *p) {
    A830_CONTEXT *ctx=context;
    const ULONG header=(ULONG)FIELD_OFFSET(A830_BATCH_COMMAND,Commands);
    if(p->CommandLength>=header &&
        p->CommandLength<=sizeof(A830_BATCH_COMMAND) && !p->MultipassOffset && !p->PatchLocationListInSize) {
        if(p->DmaSize<p->CommandLength || p->DmaBufferPrivateDataSize<sizeof(A830_DMA))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        __try {RtlCopyMemory(p->pDmaBuffer,p->pCommand,p->CommandLength);}
        __except(EXCEPTION_EXECUTE_HANDLER){return GetExceptionCode();}
        const A830_BATCH_COMMAND *batch=p->pDmaBuffer;
        if(batch->Version==A830_ABI_VERSION && batch->Operation==A830_OP_CLIENT) {
            if(!batch->Count || batch->Count>256 || batch->Reserved || !batch->Lease ||
                p->CommandLength!=header+batch->Count*sizeof(GPU_SUBMIT) || !ctx->Completion)return STATUS_INVALID_PARAMETER;
            A830_DMA *dma=p->pDmaBufferPrivateData;RtlZeroMemory(dma,sizeof(*dma));
            dma->Operation=5;dma->Process=ctx->Process;dma->Lease=batch->Lease;dma->Count=batch->Count;
            dma->Completion=ctx->Completion;
            if(!batch->Fence || !ctx->CompletionBuffer.Cpu)return STATUS_INVALID_PARAMETER;
            A830_COMPLETION *page=(PVOID)ctx->CompletionBuffer.Cpu;
            if(batch->Fence<=page->Submitted || page->Status || !ExAcquireRundownProtection(&ctx->Jobs))return STATUS_DEVICE_REMOVED;
            KIRQL gate;KeAcquireSpinLock(&ctx->TokenLock,&gate);
            BOOLEAN revoked=ctx->Closing || (ctx->ClientLease && ctx->ClientLease!=batch->Lease);
            if(!revoked)ctx->ClientLease=batch->Lease;
            KeReleaseSpinLock(&ctx->TokenLock,gate);
            if(revoked){ExReleaseRundownProtection(&ctx->Jobs);return STATUS_DEVICE_REMOVED;}
            NTSTATUS prepared=A830Prepare(ctx->Process,batch->Lease,batch->Commands,batch->Count,&dma->Prepared);
            if(!NT_SUCCESS(prepared)){ExReleaseRundownProtection(&ctx->Jobs);return prepared;}
            dma->Context=ctx;dma->Sequence=batch->Fence;
            A830_RENDERED *record=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*record),GPU_TAG);
            if(!record){A830ReleasePrepared(dma->Prepared);dma->Prepared=NULL;ExReleaseRundownProtection(&ctx->Jobs);return STATUS_INSUFFICIENT_RESOURCES;}
            RtlCopyMemory(dma->Payload.Commands,batch->Commands,batch->Count*sizeof(GPU_SUBMIT));
            record->Dma=*dma;ObReferenceObject(record->Dma.Completion);
            KIRQL old;KeAcquireSpinLock(&ctx->TokenLock,&old);
            if(ctx->Closing || batch->Fence<=page->Submitted || page->Status) {
                KeReleaseSpinLock(&ctx->TokenLock,old);A830_JOB cancelled={0};cancelled.Dma=record->Dma;
                ExFreePoolWithTag(record,GPU_TAG);dma->Context=NULL;dma->Prepared=NULL;dma->Completion=NULL;A830FinishJob(&cancelled,FALSE);return STATUS_DEVICE_REMOVED;
            }
            InsertTailList(&ctx->Rendered,&record->Entry);InterlockedExchange64((volatile LONG64 *)&page->Submitted,batch->Fence);
            KeReleaseSpinLock(&ctx->TokenLock,old);
            dma->Prepared=NULL;dma->Completion=NULL;
            p->pDmaBuffer=(PUCHAR)p->pDmaBuffer+p->CommandLength;
            p->pDmaBufferPrivateData=(PUCHAR)p->pDmaBufferPrivateData+sizeof(*dma);
            return STATUS_SUCCESS;
        }
    }
    if(p->CommandLength!=sizeof(A830_COMMAND) || p->MultipassOffset || p->PatchLocationListInSize)return STATUS_INVALID_PARAMETER;
    if(p->DmaSize<sizeof(A830_COMMAND) || p->DmaBufferPrivateDataSize<sizeof(A830_DMA))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    __try { RtlCopyMemory(p->pDmaBuffer,p->pCommand,sizeof(A830_COMMAND)); }
    __except(EXCEPTION_EXECUTE_HANDLER){return GetExceptionCode();}
    A830_COMMAND *c=p->pDmaBuffer;
    if(c->Version!=A830_ABI_VERSION || c->Operation!=A830_OP_FILL || c->Allocation>=p->AllocationListSize ||
        !c->Bytes || c->Bytes>4096 || ((c->Offset|c->Bytes)&3))return STATUS_INVALID_PARAMETER;
    if(!p->PatchLocationListOutSize || !p->pPatchLocationListOut)return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    D3DDDI_PATCHLOCATIONLIST *patch=p->pPatchLocationListOut;
    RtlZeroMemory(patch,sizeof(*patch));patch->AllocationIndex=c->Allocation;
    patch->PatchOffset=FIELD_OFFSET(A830_COMMAND,Offset);patch->AllocationOffset=c->Offset;
    p->pPatchLocationListOut++;
    DXGKARG_PATCH initial={0};initial.pDmaBuffer=p->pDmaBuffer;initial.DmaBufferSize=p->DmaSize;
    initial.pDmaBufferPrivateData=p->pDmaBufferPrivateData;initial.DmaBufferPrivateDataSize=p->DmaBufferPrivateDataSize;
    initial.pAllocationList=p->pAllocationList;initial.AllocationListSize=p->AllocationListSize;
    initial.DmaBufferSubmissionEndOffset=sizeof(A830_COMMAND);
    NTSTATUS status=A830Patch(ctx->Adapter,&initial);if(!NT_SUCCESS(status))return status;
    p->pDmaBuffer=(PUCHAR)p->pDmaBuffer+sizeof(A830_COMMAND);
    p->pDmaBufferPrivateData=(PUCHAR)p->pDmaBufferPrivateData+sizeof(A830_DMA);
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY A830Patch(HANDLE adapter,const DXGKARG_PATCH *p) {
    UNREFERENCED_PARAMETER(adapter);
    if(p->Flags.Paging)return STATUS_SUCCESS;
    if(p->DmaBufferPrivateDataSize>=sizeof(A830_DMA) && ((A830_DMA *)p->pDmaBufferPrivateData)->Operation==6)
        return STATUS_SUCCESS;
    if(p->DmaBufferPrivateDataSize>=sizeof(A830_DMA) &&
        ((A830_DMA *)p->pDmaBufferPrivateData)->Operation==5) {
        const A830_DMA *dma=p->pDmaBufferPrivateData;
        /* CLIENT IBs use fixed GPU virtual addresses and have no relocations.
           VidSch may submit a nonzero command-buffer subrange (for example
           after Present); only its bounds matter, not an artificial start=0. */
        if(p->DmaBufferSubmissionStartOffset>p->DmaBufferSubmissionEndOffset ||
            p->DmaBufferSubmissionEndOffset>p->DmaBufferSize ||
            p->DmaBufferPrivateDataSubmissionStartOffset>p->DmaBufferPrivateDataSubmissionEndOffset ||
            p->DmaBufferPrivateDataSubmissionEndOffset>p->DmaBufferPrivateDataSize ||
            !dma->Process || !dma->Lease || !dma->Count || dma->Count>256)
            return STATUS_INVALID_PARAMETER;
        return STATUS_SUCCESS;
    }
    if(p->DmaBufferSubmissionStartOffset || p->DmaBufferSubmissionEndOffset!=sizeof(A830_COMMAND) ||
        p->DmaBufferSize<sizeof(A830_COMMAND) || p->DmaBufferPrivateDataSize<sizeof(A830_DMA))return STATUS_INVALID_PARAMETER;
    const A830_COMMAND *c=p->pDmaBuffer;
    if(c->Version!=A830_ABI_VERSION || c->Operation!=A830_OP_FILL || c->Allocation>=p->AllocationListSize ||
        !c->Bytes || c->Bytes>4096 || ((c->Offset|c->Bytes)&3))return STATUS_INVALID_PARAMETER;
    const DXGK_ALLOCATIONLIST *entry=p->pAllocationList+c->Allocation;
    A830_ALLOCATION *allocation=entry->hDeviceSpecificAllocation;
    if(!allocation || !entry->WriteOperation || entry->SegmentId!=1 || c->Offset>allocation->Resource.Size || c->Bytes>allocation->Resource.Size-c->Offset)return STATUS_INVALID_PARAMETER;
    ULONGLONG offset=entry->PhysicalAddress.QuadPart;
    if(offset>=A830_SEGMENT_ADDRESS)offset-=A830_SEGMENT_ADDRESS;
    if(offset>A830_SEGMENT_BYTES || c->Offset>A830_SEGMENT_BYTES-offset || c->Bytes>A830_SEGMENT_BYTES-offset-c->Offset)return STATUS_INVALID_PARAMETER;
    A830_DMA *dma=p->pDmaBufferPrivateData;RtlZeroMemory(dma,sizeof(*dma));
    dma->Operation=1;dma->Offset=(ULONG)offset+c->Offset;dma->Bytes=c->Bytes;dma->Value=c->Value;
    return STATUS_SUCCESS;
}
BOOLEAN A830InterruptComplete(PVOID context) {
    A830_ADAPTER *a=context;
    if(!a->Hardware.Online || !a->Hardware.Memory.Cpu || a->Stopping || a->Failed)return TRUE;
    ULONGLONG completed=*(volatile ULONGLONG *)a->Hardware.Memory.Cpu;
    KeMemoryBarrier();
    while(a->IrqRead!=a->IrqWrite) {
        KeMemoryBarrier();ULONG slot=a->IrqRead%RTL_NUMBER_OF(a->IrqRecords);
        if(a->IrqRecords[slot].HardwareFence>completed)break;
        DXGKARGCB_NOTIFY_INTERRUPT_DATA notification={0};notification.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;
        notification.DmaCompleted.SubmissionFenceId=a->IrqRecords[slot].Fence;
        A830_CONTEXT *c=a->IrqRecords[slot].Context;
        if(c) {
            A830_COMPLETION *page=(PVOID)c->CompletionBuffer.Cpu;
            InterlockedExchange64((volatile LONG64 *)&page->Completed,a->IrqRecords[slot].Sequence);
        }
        A830PublishFence(&a->Completed,a->IrqRecords[slot].Fence);
        a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle,&notification);

        InterlockedIncrement((volatile LONG *)&a->IrqRead);
        InterlockedExchange(&a->SchedulerDpcPending,1);
        a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
    }
    return TRUE;
}
static BOOLEAN PollAtInterrupt(PVOID context) {
    A830_ADAPTER *a=context;
    A830InterruptComplete(a);
    return TRUE;
}
static VOID PollCompletion(A830_ADAPTER *a) {
    if(a->Stopping || a->Failed || a->IrqRead==a->IrqWrite)return;
    BOOLEAN result=FALSE;

    NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,PollAtInterrupt,a,0,&result);
    if(!NT_SUCCESS(status))InterlockedExchange(&a->Failed,status);
}
static PLARGE_INTEGER WorkerTimeout(A830_ADAPTER *a,BOOLEAN dpu,ULONGLONG now,ULONGLONG nextVsync,PLARGE_INTEGER timeout) {
    BOOLEAN pending=FALSE;
    if(!a->Stopping && !a->Failed) {
        KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
        pending=a->IrqRead!=a->IrqWrite || !IsListEmpty(&a->Pending) || !IsListEmpty(&a->Inflight) || a->PreemptPending || a->CpuBarriers;
        KeReleaseSpinLock(&a->QueueLock,old);
    }
    if(a->VsyncEnabled && !dpu) {
        timeout->QuadPart=nextVsync>now?-(LONGLONG)(nextVsync-now):0;
        if(pending && timeout->QuadPart<-100000)timeout->QuadPart=-100000;
        return timeout;
    }
    if(!pending)return NULL;
    timeout->QuadPart=-100000;return timeout;
}
/* Caller holds QueueLock. Ring records are single-producer, ISR consumes only
   fully published records and never takes a PASSIVE/DISPATCH spin lock. */
static NTSTATUS IssueJob(A830_ADAPTER *a,A830_JOB *job) {
    if(a->IrqWrite-a->IrqRead>=RTL_NUMBER_OF(a->IrqRecords))return STATUS_DEVICE_BUSY;
    NTSTATUS status=A830Issue(job->Dma.Prepared,&job->HardwareFence);
    if(!NT_SUCCESS(status))return status;
    ULONG slot=a->IrqWrite%RTL_NUMBER_OF(a->IrqRecords);
    a->IrqRecords[slot].HardwareFence=job->HardwareFence;a->IrqRecords[slot].Fence=job->Fence;
    a->IrqRecords[slot].Context=job->Dma.Context;a->IrqRecords[slot].Sequence=job->Dma.Sequence;
    KeMemoryBarrier();a->IrqWrite++;

    /* Catch a very fast completion whose interrupt preceded publication. */
    BOOLEAN result=FALSE;
    a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,A830InterruptComplete,a,0,&result);
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY A830Submit(HANDLE adapter,const DXGKARG_SUBMITCOMMAND *p) {
    A830_ADAPTER *a=adapter;
    /* Publish the accepted WDDM fence before any command can complete.
       QueueLock also prevents an older concurrent caller from moving it back. */
    KIRQL publish;KeAcquireSpinLock(&a->QueueLock,&publish);
    A830PublishFence(&a->LastSubmitted,p->SubmissionFenceId);
    KeReleaseSpinLock(&a->QueueLock,publish);
    /* SubmitCommand must never return an error: VidSch treats one as 0x119. */
    if(p->EngineOrdinal || p->NodeOrdinal || p->DmaBufferPrivateDataSubmissionStartOffset ||
       p->DmaBufferPrivateDataSubmissionEndOffset!=sizeof(A830_DMA) || p->DmaBufferPrivateDataSize<sizeof(A830_DMA)) {
        InterlockedExchange(&a->Failed,STATUS_INVALID_PARAMETER);KeSetEvent(&a->Wake,0,FALSE);return STATUS_SUCCESS;
    }
    PLIST_ENTRY entry=ExInterlockedRemoveHeadList(&a->Free,&a->QueueLock);
    A830_JOB *job=entry?CONTAINING_RECORD(entry,A830_JOB,Entry):ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(A830_JOB),GPU_TAG);
    if(!job){
        InterlockedExchange(&a->Failed,STATUS_INSUFFICIENT_RESOURCES);
        A830_DMA *dma=p->pDmaBufferPrivateData;
        A830CancelRendered(a,dma);
        InterlockedExchange(&a->FaultFence,(LONG)p->SubmissionFenceId);KeSetEvent(&a->Wake,0,FALSE);return STATUS_SUCCESS;
    }
    job->Dynamic=!entry;job->HardwareFence=0;job->Fence=p->SubmissionFenceId;
    RtlCopyMemory(&job->Dma,p->pDmaBufferPrivateData,sizeof(job->Dma));
    if(job->Dma.Context) {
        A830_DMA token=job->Dma;
        if(!TakeRendered(a,&token,&job->Dma)) {
            /* Context teardown/Cancel already owns this token. Do not access
               the context pointer or a prepared allocation after lookup fails. */
            job->Dma.Context=NULL;job->Dma.Prepared=NULL;job->Dma.Completion=NULL;job->Dma.Operation=0;
            InterlockedExchange(&a->Failed,STATUS_DEVICE_REMOVED);InterlockedExchange(&a->FaultFence,(LONG)p->SubmissionFenceId);
            if(job->Dynamic)ExFreePoolWithTag(job,GPU_TAG);else ExInterlockedInsertTailList(&a->Free,&job->Entry,&a->QueueLock);
            KeSetEvent(&a->Wake,0,FALSE);return STATUS_SUCCESS;
        }
    }
    /* Render owns these refs until Submit transfers them; Cancel releases
       unsubmitted DMA data. Ordinary paging/Present packets have no Context. */
    if(job->Dma.Completion && !job->Dma.Context)ObReferenceObject(job->Dma.Completion);
    NTSTATUS pinned=A830PinPresent(&job->Dma);
    if(!NT_SUCCESS(pinned)) {
        A830FinishJob(job,FALSE);
        if(job->Dynamic)ExFreePoolWithTag(job,GPU_TAG);else ExInterlockedInsertTailList(&a->Free,&job->Entry,&a->QueueLock);
        InterlockedExchange(&a->Failed,pinned);InterlockedExchange(&a->FaultFence,(LONG)p->SubmissionFenceId);KeSetEvent(&a->Wake,0,FALSE);return STATUS_SUCCESS;
    }
    if(job->Dma.Operation!=5)InterlockedIncrement(&a->CpuBarriers);
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    NTSTATUS issued=STATUS_DEVICE_BUSY;
    if(!a->Stopping && !a->Failed && job->Dma.Prepared && !a->CpuBarriers && !a->WorkerBusy && IsListEmpty(&a->Pending))
        issued=IssueJob(a,job);
    if(NT_SUCCESS(issued))InsertTailList(&a->Inflight,&job->Entry);
    else InsertTailList(&a->Pending,&job->Entry);
    KeReleaseSpinLock(&a->QueueLock,old);
    KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);return STATUS_SUCCESS;
}
typedef struct { A830_ADAPTER *Adapter; ULONG Fence; } A830_FENCE_SNAPSHOT;
static BOOLEAN FreezeAtInterrupt(PVOID context) {
    A830_FENCE_SNAPSHOT *snapshot=context;A830_ADAPTER *a=snapshot->Adapter;
    /* Called with QueueLock and the hardware interrupt lock held. Stop
       completion notifications before taking the stable reset boundary. */
    InterlockedExchange(&a->Stopping,1);
    snapshot->Fence=A830FenceLatest(a->LastSubmitted,a->Completed);
    a->LastSubmitted=snapshot->Fence;return TRUE;
}
ULONG A830FreezeFences(A830_ADAPTER *a) {
    A830_FENCE_SNAPSHOT snapshot={a,0};KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    BOOLEAN result=FALSE;
    NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,FreezeAtInterrupt,&snapshot,0,&result);
    if(!NT_SUCCESS(status)){InterlockedExchange(&a->Stopping,1);snapshot.Fence=A830FenceLatest(a->LastSubmitted,a->Completed);}
    KeReleaseSpinLock(&a->QueueLock,old);return snapshot.Fence;
}
static BOOLEAN Notify(PVOID context) {
    A830_ADAPTER *a=context;
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle,&a->Notify);
    InterlockedExchange(&a->SchedulerDpcPending,1);
    return a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}
static VOID Signal(A830_ADAPTER *a) {
    BOOLEAN result=FALSE;
    NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,Notify,a,0,&result);
    if(!NT_SUCCESS(status))InterlockedExchange(&a->Failed,status);
}
static BOOLEAN TakePreemption(A830_ADAPTER *a,PULONG fence) {
    if(!a->PreemptPending)return FALSE;
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    BOOLEAN ready=a->PreemptPending && IsListEmpty(&a->Pending) && IsListEmpty(&a->Inflight);
    if(ready){*fence=a->Preemption;InterlockedExchange(&a->PreemptPending,0);}
    KeReleaseSpinLock(&a->QueueLock,old);return ready;
}
VOID A830Retire(A830_ADAPTER *a) {
    if(a->Failed && !a->Stopping)return;
    ULONGLONG completed=a->Hardware.Memory.Cpu?*(volatile ULONGLONG *)a->Hardware.Memory.Cpu:0;
    for(;;) {
        PLIST_ENTRY entry=ExInterlockedRemoveHeadList(&a->Inflight,&a->QueueLock);if(!entry)break;
        A830_JOB *job=CONTAINING_RECORD(entry,A830_JOB,Entry);
        if(!a->Stopping && !a->Failed && (job->HardwareFence>completed || (LONG)(job->Fence-a->Completed)>0)) {
            ExInterlockedInsertHeadList(&a->Inflight,entry,&a->QueueLock);break;
        }
        A830FinishJob(job,TRUE);A830RecycleJob(a,job);
    }
}
static BOOLEAN NotifyVsync(PVOID context) {
    A830_ADAPTER *a=context;DXGKARGCB_NOTIFY_INTERRUPT_DATA notification={0};
    notification.InterruptType=DXGK_INTERRUPT_CRTC_VSYNC;
    notification.Flags.ValidPhysicalAdapterMask=1;notification.CrtcVsync.PhysicalAdapterMask=1;
    notification.CrtcVsync.PhysicalAddress.QuadPart=(LONGLONG)a->ScanoutAddress;
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle,&notification);
    InterlockedExchange(&a->SchedulerDpcPending,1);
    return a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}
static NTSTATUS Execute(A830_ADAPTER *a,const A830_DMA *dma) {
    if(dma->Operation==0)return STATUS_SUCCESS;
    if(dma->Operation==6) {
        NTSTATUS status=A830MapPresent(a,(A830_DMA *)dma);if(!NT_SUCCESS(status))return status;
        status=A830CopyPresent(dma);if(!NT_SUCCESS(status))return status;

        if(!A830DpuEnabled(a) && a->Framebuffer && a->ScanoutPitch && dma->Destination==a->Memory.Segment.Cpu+(SIZE_T)a->ScanoutOffset) {
            status=A830CopyPresentToFrame(dma,a->Framebuffer,a->DisplayInfo.Pitch,a->DisplayInfo.Width,a->DisplayInfo.Height);
            if(!NT_SUCCESS(status))return status;


        }
        KeMemoryBarrier();return STATUS_SUCCESS;
    }
    if(dma->Operation==5)
        return A830BridgeExecuteClient(dma->Process,dma->Lease,dma->Payload.Commands,dma->Count);
    if(dma->Operation==2 || dma->Operation==3) {
        if(dma->Operation==2)RtlFillMemoryUlong(dma->Destination,dma->Bytes,dma->Value);
        else RtlMoveMemory(dma->Destination,dma->Source,dma->Bytes);
        KeMemoryBarrier();return STATUS_SUCCESS;
    }
    if(dma->Operation!=1 || !dma->Bytes || dma->Bytes>4096 || ((dma->Offset|dma->Bytes)&3) ||
        dma->Offset>A830_SEGMENT_BYTES || dma->Bytes>A830_SEGMENT_BYTES-dma->Offset)return STATUS_INVALID_PARAMETER;
    PULONG commands=(PULONG)a->Memory.Commands.Cpu;
    ULONG count=dma->Bytes/4+2,parity=count;parity^=parity>>8;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
    commands[0]=0x703D0000|count|((~parity&1)<<15);
    ULONGLONG address=A830_SEGMENT_ADDRESS+dma->Offset;commands[1]=(ULONG)address;commands[2]=(ULONG)(address>>32);
    for(ULONG i=0;i<dma->Bytes/4;i++)commands[i+3]=dma->Value;
    if(a->BridgeAttached) {
        GPU_COMMAND command={&a->Memory.Commands,0,count+1};
        NTSTATUS status=A830BridgeExecute(&command,1);
        return status;
    }
    NTSTATUS status=GpuSubmit(&a->Hardware,&a->Memory.Commands,0,count+1);
    if(NT_SUCCESS(status))status=GpuWait(&a->Hardware,a->Hardware.Submitted,10000000);
    return status;
}
A830_JOB *A830TakePending(A830_ADAPTER *a) {
    /* A prior render/display failure is not a failure of the next system
       paging packet. Keep that packet queued for reset; never manufacture
       DMA_PAGE_FAULTED for a CPU transfer we have not even executed. */
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    PLIST_ENTRY entry=(!a->Stopping && !a->Failed && !IsListEmpty(&a->Pending))?RemoveHeadList(&a->Pending):NULL;
    KeReleaseSpinLock(&a->QueueLock,old);
    return entry?CONTAINING_RECORD(entry,A830_JOB,Entry):NULL;
}
VOID A830Worker(PVOID context) {
    A830_ADAPTER *a=context;
    const ULONGLONG vsyncPeriod=(10000000ULL+A830RefreshRate(a)/2)/A830RefreshRate(a);
    ULONGLONG nextVsync=KeQueryInterruptTime()+vsyncPeriod;
    for(;;) {
        LARGE_INTEGER timeout;ULONGLONG now=KeQueryInterruptTime();
        BOOLEAN dpu=A830DpuEnabled(a);
        KeWaitForSingleObject(&a->Wake,Executive,KernelMode,FALSE,WorkerTimeout(a,dpu,now,nextVsync,&timeout));
        PollCompletion(a);
        A830Retire(a);
        A830DpuWork(a);
        A830BridgeCollectShared();
        if(a->Stopping)break;
        LONG fault=InterlockedExchange(&a->FaultFence,0);
        if(fault) {
            RtlZeroMemory(&a->Notify,sizeof(a->Notify));a->Notify.InterruptType=DXGK_INTERRUPT_DMA_PAGE_FAULTED;
            a->Notify.DmaPageFaulted.FaultedFenceId=(ULONG)fault;
            a->Notify.DmaPageFaulted.PageFaultFlags=DXGK_PAGE_FAULT_ENGINE_RESET_REQUIRED;
            a->Notify.DmaPageFaulted.FaultErrorCode.IsDeviceSpecificCode=1;
            a->Notify.DmaPageFaulted.FaultErrorCode.DeviceSpecificCode=(ULONG)a->Failed&0x7fffffffu;Signal(a);
        }
        now=KeQueryInterruptTime();BOOLEAN tick=!dpu && a->VsyncEnabled && now>=nextVsync;
        if(tick)nextVsync=now+vsyncPeriod;
        if(QcomGpuScanoutEnabled(a))QcomGpuScanoutPump(a);
        if(!dpu && !QcomGpuScanoutEnabled(a) && (tick || !a->VsyncEnabled) && InterlockedExchange(&a->FlipPending,0) && a->Framebuffer && ExAcquireRundownProtection(&a->ScanoutAccess)) {
            ULONGLONG offset=a->ScanoutOffset;ULONG pitch=a->ScanoutPitch;
            PUCHAR source=a->ScanoutCpu?a->ScanoutCpu:a->Memory.Segment.Cpu+(SIZE_T)offset;
            if(pitch<a->DisplayInfo.Width*4 || offset>A830_SEGMENT_BYTES ||
                (ULONGLONG)pitch*a->DisplayInfo.Height>A830_SEGMENT_BYTES-offset){ExReleaseRundownProtection(&a->ScanoutAccess);continue;}
            for(ULONG y=0;y<a->DisplayInfo.Height;y++)
                RtlCopyMemory(a->Framebuffer+(SIZE_T)y*a->DisplayInfo.Pitch,
                    source+(SIZE_T)y*pitch,a->DisplayInfo.Width*4);
            KeMemoryBarrier();
            ExReleaseRundownProtection(&a->ScanoutAccess);


        }
        if(tick && a->ScanoutPitch) {
            BOOLEAN result=FALSE;
            NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,NotifyVsync,a,0,&result);
            if(!NT_SUCCESS(status))InterlockedExchange(&a->Failed,status);
        }
        InterlockedExchange(&a->WorkerBusy,1);
        for(;;) {
            if(a->Failed || a->Stopping)break;
            if(QcomGpuScanoutEnabled(a) && QcomGpuScanoutPump(a))break;
            if(a->PreemptPending && !IsListEmpty(&a->Inflight))break;
            ULONG preemption;
            if(TakePreemption(a,&preemption)) {
                RtlZeroMemory(&a->Notify,sizeof(a->Notify));a->Notify.InterruptType=DXGK_INTERRUPT_DMA_PREEMPTED;
                a->Notify.DmaPreempted.PreemptionFenceId=preemption;a->Notify.DmaPreempted.LastCompletedFenceId=a->Completed;
                Signal(a);
            }
            A830_JOB *job=A830TakePending(a);if(!job)break;
            PLIST_ENTRY entry=&job->Entry;
            if(a->Failed || a->Stopping){ExInterlockedInsertHeadList(&a->Pending,entry,&a->QueueLock);break;}
            /* CPU paging/Blt jobs form a global ordering barrier. Never run
               them before earlier GPU work, and never issue later work across one. */
            if(!IsListEmpty(&a->Inflight) && job->Dma.Operation!=5) {
                ExInterlockedInsertHeadList(&a->Pending,entry,&a->QueueLock);break;
            }
            if(job->Dma.Operation==5 && job->Dma.Prepared) {
                KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
                NTSTATUS issued=IssueJob(a,job);
                KeReleaseSpinLock(&a->QueueLock,old);
                if(issued==STATUS_DEVICE_BUSY){ExInterlockedInsertHeadList(&a->Pending,entry,&a->QueueLock);break;}
                if(NT_SUCCESS(issued)){ExInterlockedInsertTailList(&a->Inflight,entry,&a->QueueLock);continue;}
                InterlockedExchange(&a->Failed,issued);
            }
            NTSTATUS status=Execute(a,&job->Dma);
            if(!NT_SUCCESS(status)) {

                InterlockedExchange(&a->Failed,status);RtlZeroMemory(&a->Notify,sizeof(a->Notify));
                a->Notify.InterruptType=DXGK_INTERRUPT_DMA_PAGE_FAULTED;
                a->Notify.DmaPageFaulted.FaultedFenceId=job->Fence;
                a->Notify.DmaPageFaulted.FaultedPrimitiveAPISequenceNumber=MAXULONGLONG;
                a->Notify.DmaPageFaulted.FaultedPipelineStage=DXGK_RENDER_PIPELINE_STAGE_UNKNOWN;
                a->Notify.DmaPageFaulted.FaultedBindTableEntry=MAXULONG;
                a->Notify.DmaPageFaulted.PageFaultFlags=DXGK_PAGE_FAULT_ENGINE_RESET_REQUIRED;
                a->Notify.DmaPageFaulted.FaultErrorCode.IsDeviceSpecificCode=1;
                a->Notify.DmaPageFaulted.FaultErrorCode.DeviceSpecificCode=(ULONG)status&0x7fffffffu;
                Signal(a);
                A830FinishJob(job,TRUE);
                A830RecycleJob(a,CONTAINING_RECORD(entry,A830_JOB,Entry));break;
            }

            A830PublishFence(&a->Completed,job->Fence);RtlZeroMemory(&a->Notify,sizeof(a->Notify));
            a->Notify.InterruptType=DXGK_INTERRUPT_DMA_COMPLETED;a->Notify.DmaCompleted.SubmissionFenceId=job->Fence;
            Signal(a);
            A830FinishJob(job,TRUE);
            A830RecycleJob(a,CONTAINING_RECORD(entry,A830_JOB,Entry));
            if(a->Stopping || a->Failed)break;
            if(!dpu && a->VsyncEnabled && KeQueryInterruptTime()>=nextVsync)break;
        }
        InterlockedExchange(&a->WorkerBusy,0);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
