#include "../wddm/adapter.h"
#include <stdio.h>
#include <stdlib.h>

static LONG outstanding,references,rundown,waits;
static A830_ADAPTER *retireOnWait;
static ULONGLONG hardwareCompleted;
static BOOLEAN completeBeforeSubmitReturns;
static ULONG completionTimerArms;
static void Check(int condition){if(!condition)abort();}
static PVOID NTAPI Pool(POOL_FLAGS flags,SIZE_T bytes,ULONG tag){UNREFERENCED_PARAMETER(flags);UNREFERENCED_PARAMETER(tag);return calloc(1,bytes);}
static VOID NTAPI Free(PVOID p,ULONG tag){UNREFERENCED_PARAMETER(tag);free(p);}
PVOID (NTAPI *__imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=Pool;
VOID (NTAPI *__imp_ExFreePoolWithTag)(PVOID,ULONG)=Free;
static KIRQL NTAPI Acquire(PKSPIN_LOCK lock){UNREFERENCED_PARAMETER(lock);return PASSIVE_LEVEL;}
static VOID NTAPI Release(PKSPIN_LOCK lock,KIRQL old){UNREFERENCED_PARAMETER(lock);UNREFERENCED_PARAMETER(old);}
static VOID NTAPI AtDpc(PKSPIN_LOCK lock){UNREFERENCED_PARAMETER(lock);}
KIRQL (NTAPI *__imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=Acquire;
VOID (NTAPI *__imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=Release;
VOID (NTAPI *__imp_KeAcquireSpinLockAtDpcLevel)(PKSPIN_LOCK)=AtDpc;
VOID (NTAPI *__imp_KeReleaseSpinLockFromDpcLevel)(PKSPIN_LOCK)=AtDpc;
static BOOLEAN NTAPI RundownAcquire(PEX_RUNDOWN_REF r){UNREFERENCED_PARAMETER(r);rundown++;return TRUE;}
static VOID NTAPI RundownRelease(PEX_RUNDOWN_REF r){UNREFERENCED_PARAMETER(r);Check(rundown>0);rundown--;}
static VOID NTAPI RundownWait(PEX_RUNDOWN_REF r){UNREFERENCED_PARAMETER(r);waits++;if(retireOnWait){A830_ADAPTER *a=retireOnWait;retireOnWait=NULL;Check(outstanding==1 && rundown==1);hardwareCompleted=1;A830InterruptComplete(a);A830Retire(a);}Check(rundown==0);}
BOOLEAN (NTAPI *__imp_ExAcquireRundownProtection)(PEX_RUNDOWN_REF)=RundownAcquire;
VOID (NTAPI *__imp_ExReleaseRundownProtection)(PEX_RUNDOWN_REF)=RundownRelease;
VOID (NTAPI *__imp_ExWaitForRundownProtectionRelease)(PEX_RUNDOWN_REF)=RundownWait;
static LONG_PTR FASTCALL Reference(PVOID p){UNREFERENCED_PARAMETER(p);return ++references;}
static LONG_PTR FASTCALL Dereference(PVOID p){UNREFERENCED_PARAMETER(p);Check(references>0);return --references;}
LONG_PTR (FASTCALL *__imp_ObfReferenceObject)(PVOID)=Reference;
LONG_PTR (FASTCALL *__imp_ObfDereferenceObject)(PVOID)=Dereference;
static LONG NTAPI SetEvent(PRKEVENT event,KPRIORITY priority,BOOLEAN wait){UNREFERENCED_PARAMETER(event);UNREFERENCED_PARAMETER(priority);UNREFERENCED_PARAMETER(wait);return 0;}
LONG (NTAPI *__imp_KeSetEvent)(PRKEVENT,KPRIORITY,BOOLEAN)=SetEvent;
static BOOLEAN NTAPI SetCompletionTimer(PEX_TIMER timer,LONGLONG due,LONGLONG period,PEXT_SET_PARAMETERS parameters){UNREFERENCED_PARAMETER(parameters);Check(timer==(PEX_TIMER)(ULONG_PTR)1 && due==-2000 && period==0);completionTimerArms++;return FALSE;}
BOOLEAN (NTAPI *__imp_ExSetTimer)(PEX_TIMER,LONGLONG,LONGLONG,PEXT_SET_PARAMETERS)=SetCompletionTimer;
static VOID NTAPI Unmap(PVOID p,PMDL mdl){UNREFERENCED_PARAMETER(p);UNREFERENCED_PARAMETER(mdl);abort();}
static VOID NTAPI FreeMdl(PMDL mdl){UNREFERENCED_PARAMETER(mdl);abort();}
VOID (NTAPI *__imp_MmUnmapLockedPages)(PVOID,PMDL)=Unmap;
VOID (NTAPI *__imp_IoFreeMdl)(PMDL)=FreeMdl;
static PEPROCESS NTAPI Process(VOID){return (PEPROCESS)(ULONG_PTR)1;}
PEPROCESS (NTAPI *__imp_IoGetCurrentProcess)(VOID)=Process;
static HANDLE NTAPI ProcessId(VOID){return (HANDLE)(ULONG_PTR)1;}
HANDLE (NTAPI *__imp_PsGetCurrentProcessId)(VOID)=ProcessId;
static VOID NTAPI Attach(PRKPROCESS process,PRKAPC_STATE state){UNREFERENCED_PARAMETER(process);UNREFERENCED_PARAMETER(state);abort();}
static VOID NTAPI Detach(PRKAPC_STATE state){UNREFERENCED_PARAMETER(state);abort();}
VOID (NTAPI *__imp_KeStackAttachProcess)(PRKPROCESS,PRKAPC_STATE)=Attach;
VOID (NTAPI *__imp_KeUnstackDetachProcess)(PRKAPC_STATE)=Detach;
NTSTATUS A830Prepare(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *commands,ULONG count,A830_PREPARED **out){UNREFERENCED_PARAMETER(process);UNREFERENCED_PARAMETER(lease);UNREFERENCED_PARAMETER(commands);UNREFERENCED_PARAMETER(count);outstanding++;*out=(A830_PREPARED *)(ULONG_PTR)1;return STATUS_SUCCESS;}
VOID A830ReleasePrepared(A830_PREPARED *p){if(p){Check(outstanding>0);outstanding--;}}
VOID GpuFree(GPU_BO *bo){UNREFERENCED_PARAMETER(bo);}
static ULONGLONG TestTime(VOID){return 0;}
#define KeQueryInterruptTime TestTime
#ifndef MAXULONGLONG
#define MAXULONGLONG ((ULONGLONG)-1)
#endif
static PLIST_ENTRY NTAPI PushHead(PLIST_ENTRY h,PLIST_ENTRY e,PKSPIN_LOCK lock){UNREFERENCED_PARAMETER(lock);InsertHeadList(h,e);return e;}
static PLIST_ENTRY NTAPI PushTail(PLIST_ENTRY h,PLIST_ENTRY e,PKSPIN_LOCK lock){UNREFERENCED_PARAMETER(lock);InsertTailList(h,e);return e;}
static PLIST_ENTRY NTAPI Pop(PLIST_ENTRY h,PKSPIN_LOCK lock){UNREFERENCED_PARAMETER(lock);return IsListEmpty(h)?NULL:RemoveHeadList(h);}
PLIST_ENTRY (NTAPI *__imp_ExInterlockedInsertHeadList)(PLIST_ENTRY,PLIST_ENTRY,PKSPIN_LOCK)=PushHead;
PLIST_ENTRY (NTAPI *__imp_ExInterlockedInsertTailList)(PLIST_ENTRY,PLIST_ENTRY,PKSPIN_LOCK)=PushTail;
PLIST_ENTRY (NTAPI *__imp_ExInterlockedRemoveHeadList)(PLIST_ENTRY,PKSPIN_LOCK)=Pop;
static VOID NTAPI RundownInit(PEX_RUNDOWN_REF r){UNREFERENCED_PARAMETER(r);}
VOID (NTAPI *__imp_ExInitializeRundownProtection)(PEX_RUNDOWN_REF)=RundownInit;
static PVOID NTAPI Map(PMDL mdl,KPROCESSOR_MODE mode,MEMORY_CACHING_TYPE cache,PVOID address,ULONG bugcheck,ULONG priority){UNREFERENCED_PARAMETER(mdl);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(cache);UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(bugcheck);UNREFERENCED_PARAMETER(priority);abort();}
PVOID (NTAPI *__imp_MmMapLockedPagesSpecifyCache)(PMDL,KPROCESSOR_MODE,MEMORY_CACHING_TYPE,PVOID,ULONG,ULONG)=Map;
static NTSTATUS NTAPI Wait(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout){UNREFERENCED_PARAMETER(object);UNREFERENCED_PARAMETER(reason);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(alertable);UNREFERENCED_PARAMETER(timeout);abort();}
NTSTATUS (NTAPI *__imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=Wait;
static VOID NTAPI Terminate(NTSTATUS status){UNREFERENCED_PARAMETER(status);abort();}
VOID (NTAPI *__imp_PsTerminateSystemThread)(NTSTATUS)=Terminate;
NTSTATUS GpuAllocateCached(GPU_BO *bo,ULONG size){UNREFERENCED_PARAMETER(bo);UNREFERENCED_PARAMETER(size);abort();}
NTSTATUS GpuSubmit(GPU *gpu,GPU_BO *bo,ULONG offset,ULONG words){UNREFERENCED_PARAMETER(gpu);UNREFERENCED_PARAMETER(bo);UNREFERENCED_PARAMETER(offset);UNREFERENCED_PARAMETER(words);abort();}
NTSTATUS GpuWait(GPU *gpu,ULONGLONG fence,ULONGLONG timeout){UNREFERENCED_PARAMETER(gpu);UNREFERENCED_PARAMETER(fence);UNREFERENCED_PARAMETER(timeout);abort();}
NTSTATUS A830Issue(A830_PREPARED *p,PULONGLONG fence){Check(p!=NULL);*fence=1;return STATUS_SUCCESS;}
VOID A830Record(PCWSTR label,ULONG value){UNREFERENCED_PARAMETER(label);UNREFERENCED_PARAMETER(value);abort();}
VOID A830BridgeCollectShared(VOID){abort();}
NTSTATUS A830CopyPresent(const A830_DMA *dma){UNREFERENCED_PARAMETER(dma);abort();}
NTSTATUS A830PinPresent(A830_DMA *dma){Check(dma->Operation!=6);return STATUS_SUCCESS;}
NTSTATUS A830MapPresent(A830_ADAPTER *adapter,A830_DMA *dma){UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(dma);abort();}
VOID A830ReleasePresent(A830_DMA *dma){Check(!dma->SourceMdl && !dma->DestinationMdl && !dma->SourcePinned && !dma->DestinationPinned);}
NTSTATUS A830CopyPresentToFrame(const A830_DMA *dma,PUCHAR destination,ULONG pitch,ULONG width,ULONG height){UNREFERENCED_PARAMETER(dma);UNREFERENCED_PARAMETER(destination);UNREFERENCED_PARAMETER(pitch);UNREFERENCED_PARAMETER(width);UNREFERENCED_PARAMETER(height);abort();}
NTSTATUS A830BridgeExecuteClient(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *commands,ULONG count){UNREFERENCED_PARAMETER(process);UNREFERENCED_PARAMETER(lease);UNREFERENCED_PARAMETER(commands);UNREFERENCED_PARAMETER(count);abort();}
NTSTATUS A830BridgeExecute(const GPU_COMMAND *commands,ULONG count){UNREFERENCED_PARAMETER(commands);UNREFERENCED_PARAMETER(count);abort();}
BOOLEAN A830DpuEnabled(PVOID adapter){UNREFERENCED_PARAMETER(adapter);abort();}
ULONG A830RefreshRate(const A830_ADAPTER *adapter){return adapter->RefreshRate==120?120u:60u;}

/* Include the actual implementation so the test can exercise the token
   transfer used by Submit without loading hardware or reimplementing it. */
#pragma warning(push)
/* Unexpected hardware stubs abort, so unexercised paths become unreachable. */
#pragma warning(disable:4702)
#include "../wddm/scheduler.c"
#pragma warning(pop)

static VOID APIENTRY TestNotify(HANDLE adapter,const DXGKARGCB_NOTIFY_INTERRUPT_DATA *data){UNREFERENCED_PARAMETER(adapter);Check(data->InterruptType==DXGK_INTERRUPT_DMA_COMPLETED && data->DmaCompleted.SubmissionFenceId==1);}
static BOOLEAN APIENTRY Queue(HANDLE adapter){UNREFERENCED_PARAMETER(adapter);return TRUE;}
static NTSTATUS APIENTRY Synchronize(HANDLE adapter,PKSYNCHRONIZE_ROUTINE routine,PVOID context,ULONG message,PBOOLEAN result){UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(message);if(completeBeforeSubmitReturns && routine==A830InterruptComplete){Check(((A830_ADAPTER *)context)->LastSubmitted==1);hardwareCompleted=1;}*result=routine(context);return STATUS_SUCCESS;}
static void Setup(A830_ADAPTER *a,A830_CONTEXT *c,A830_COMPLETION *page) {
    RtlZeroMemory(a,sizeof(*a));RtlZeroMemory(c,sizeof(*c));RtlZeroMemory(page,sizeof(*page));
    InitializeListHead(&a->Contexts);InitializeListHead(&c->Rendered);
    InitializeListHead(&a->Pending);InitializeListHead(&a->Inflight);InitializeListHead(&a->Free);
    hardwareCompleted=0;a->Hardware.Memory.Cpu=(PUCHAR)&hardwareCompleted;a->Hardware.Online=TRUE;
    a->Dxgk.DxgkCbNotifyInterrupt=TestNotify;a->Dxgk.DxgkCbQueueDpc=Queue;a->Dxgk.DxgkCbSynchronizeExecution=Synchronize;
    c->Adapter=a;c->Process=Process();c->Completion=(PKEVENT)(ULONG_PTR)1;c->CompletionBuffer.Cpu=(PUCHAR)page;
    InsertTailList(&a->Contexts,&c->Entry);Check(!outstanding && !rundown && !references);
}
static A830_DMA Render(A830_CONTEXT *c,ULONGLONG seq) {
    A830_BATCH_COMMAND batch={0};A830_DMA dma={0};UCHAR gpu[A830_DMA_BYTES];DXGKARG_RENDER args={0};
    batch.Version=A830_ABI_VERSION;batch.Operation=A830_OP_CLIENT;batch.Count=1;batch.Lease=7;batch.Fence=seq;
    batch.Commands[0]=(GPU_SUBMIT){1,0,4,0};
    args.pCommand=&batch;args.CommandLength=FIELD_OFFSET(A830_BATCH_COMMAND,Commands)+sizeof(GPU_SUBMIT);
    args.pDmaBuffer=gpu;args.DmaSize=sizeof(gpu);args.pDmaBufferPrivateData=&dma;args.DmaBufferPrivateDataSize=sizeof(dma);
    Check(NT_SUCCESS(A830Render(c,&args)));Check(outstanding==1 && rundown==1 && references==1);
    Check(dma.Context==c && dma.Sequence==seq && !dma.Prepared && !dma.Completion);return dma;
}
int main(void) {
    static A830_ADAPTER adapter;A830_CONTEXT context;A830_COMPLETION page;A830_DMA token;
    DXGKARG_SUBMITCOMMAND submit={0};submit.SubmissionFenceId=1;
    submit.DmaBufferPrivateDataSize=submit.DmaBufferPrivateDataSubmissionEndOffset=sizeof(token);
    Setup(&adapter,&context,&page);token=Render(&context,1);UNREFERENCED_PARAMETER(token);
    A830ContextStop(&context);Check(!outstanding && !rundown && !references && waits==1);
    Setup(&adapter,&context,&page);token=Render(&context,1);
    A830CancelRendered(&adapter,&token);A830CancelRendered(&adapter,&token);Check(!outstanding && !rundown && !references);
    A830ContextStop(&context);
    Setup(&adapter,&context,&page);token=Render(&context,1);submit.pDmaBufferPrivateData=&token;
    completeBeforeSubmitReturns=TRUE;Check(NT_SUCCESS(A830Submit(&adapter,&submit)));completeBeforeSubmitReturns=FALSE;
    Check(adapter.LastSubmitted==1 && adapter.Completed==1 && page.Completed==1);A830Retire(&adapter);A830ContextStop(&context);
    Setup(&adapter,&context,&page);token=Render(&context,1);submit.pDmaBufferPrivateData=&token;
    LARGE_INTEGER timeout;
    Check(WorkerTimeout(&adapter,TRUE,0,83333,&timeout)==NULL);
    Check(NT_SUCCESS(A830Submit(&adapter,&submit)));
    Check(WorkerTimeout(&adapter,TRUE,0,83333,&timeout)==&timeout && timeout.QuadPart==-10000);
    adapter.CompletionTimer=(PEX_TIMER)(ULONG_PTR)1;
    Check(WorkerTimeout(&adapter,TRUE,0,83333,&timeout)==NULL && completionTimerArms==1);
    PollCompletion(&adapter);Check(page.Completed==0 && adapter.Completed==0 && outstanding==1);
    hardwareCompleted=1;adapter.Hardware.Online=FALSE;PollCompletion(&adapter);Check(page.Completed==0);
    adapter.Hardware.Online=TRUE;adapter.Stopping=1;PollCompletion(&adapter);Check(page.Completed==0);
    adapter.Stopping=0;A830CompletionTick(adapter.CompletionTimer,&adapter);
    Check(page.Completed==1 && adapter.Completed==1 && adapter.PolledCompletions==1);
    A830Retire(&adapter);Check(!outstanding && WorkerTimeout(&adapter,TRUE,0,83333,&timeout)==NULL && completionTimerArms==1);
    A830ContextStop(&context);
    adapter.LastSubmitted=762;adapter.Completed=763;adapter.Hardware.Submitted=90000;
    Check(A830FreezeFences(&adapter)==763 && adapter.Stopping && adapter.LastSubmitted==763);
    Check(A830PublishFence(&adapter.LastSubmitted,762)==763);
    Check(A830PublishFence(&adapter.Completed,762)==763);
    Check(A830FenceLatest(0xffffffffu,1)==1 && A830FenceLatest(1,0xffffffffu)==1);
    Setup(&adapter,&context,&page);token=Render(&context,1);
    submit.pDmaBufferPrivateData=&token;
    submit.DmaBufferPrivateDataSize=submit.DmaBufferPrivateDataSubmissionEndOffset=sizeof(token);
    Check(NT_SUCCESS(A830Submit(&adapter,&submit)));Check(!TakeRendered(&adapter,&token,&token));
    Check(outstanding==1 && adapter.IrqWrite==1 && !IsListEmpty(&adapter.Inflight) && page.Completed==0);
    retireOnWait=&adapter;
    A830ContextStop(&context);Check(!retireOnWait && !outstanding && !rundown && !references);
    Setup(&adapter,&context,&page);token=Render(&context,1);
    A830CancelRenderedClient(&adapter,Process(),7);Check(!outstanding && !rundown && !references && context.Closing);
    A830ContextStop(&context);
    A830_DMA client={0};client.Operation=5;client.Process=Process();client.Lease=7;client.Count=1;
    DXGKARG_PATCH patch={0};patch.pDmaBufferPrivateData=&client;patch.DmaBufferPrivateDataSize=sizeof(client);
    patch.DmaBufferPrivateDataSubmissionEndOffset=sizeof(client);patch.DmaBufferSize=64;
    patch.DmaBufferSubmissionStartOffset=16;patch.DmaBufferSubmissionEndOffset=48;
    Check(NT_SUCCESS(A830Patch(&adapter,&patch)));
    patch.DmaBufferSubmissionStartOffset=49;Check(A830Patch(&adapter,&patch)==STATUS_INVALID_PARAMETER);
    Check(adapter.PatchDiagnostics.Failures==1 && adapter.PatchDiagnostics.Start==49);
    InitializeListHead(&adapter.Pending);A830_JOB paging={0};paging.Dma.Operation=3;paging.Dma.Bytes=4096;paging.Fence=0x565d;
    InsertTailList(&adapter.Pending,&paging.Entry);adapter.Stopping=0;adapter.Failed=STATUS_DEVICE_HARDWARE_ERROR;
    Check(A830TakePending(&adapter)==NULL && !IsListEmpty(&adapter.Pending) && paging.Fence==0x565d);
    adapter.Failed=0;Check(A830TakePending(&adapter)==&paging && IsListEmpty(&adapter.Pending));
    Setup(&adapter,&context,&page);token=Render(&context,1);submit.pDmaBufferPrivateData=&token;
    adapter.CpuBarriers=1;
    Check(NT_SUCCESS(A830Submit(&adapter,&submit)) && !IsListEmpty(&adapter.Pending));
    adapter.PreemptPending=1;adapter.Preemption=19;ULONG preemption=0;
    Check(!TakePreemption(&adapter,&preemption) && adapter.PreemptPending && !page.Status && outstanding==1);
    A830_JOB *queued=A830TakePending(&adapter);Check(queued && queued->Dma.Prepared && !page.Completed);
    Check(NT_SUCCESS(IssueJob(&adapter,queued)));InsertTailList(&adapter.Inflight,&queued->Entry);
    Check(!TakePreemption(&adapter,&preemption) && adapter.PreemptPending && !page.Status && outstanding==1);
    hardwareCompleted=1;A830InterruptComplete(&adapter);A830Retire(&adapter);
    Check(TakePreemption(&adapter,&preemption) && preemption==19 && !adapter.PreemptPending);
    Check(page.Completed==1 && !page.Status && !page.Generation && !outstanding && !rundown && !references);
    Check(!TakePreemption(&adapter,&preemption));A830ContextStop(&context);
    puts("KMD context/token lifecycle, interrupt-before-submit-return, lost completion interrupt, bounded idle wait, no false completion, no fence regression, reset boundary, serial wrap, preemption drains queued and in-flight jobs: PASS");return 0;
}

