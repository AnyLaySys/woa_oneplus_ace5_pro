#include "adapter.h"
#include "gpu-scanout.h"
#include "gpu-scanout-builder.h"

/* Scanout stays below the client BO range and the shared-memory range. */
#define FRAME_ADDRESS 0xb0000000ULL
#define FRAME_LIMIT (64u*1024*1024)
#define COPY_ADDRESS 0xb8000000ULL
#define COPY_BYTES 0x20000u
#define STAGING_ADDRESS 0xc0000000ULL
#define STAGING_STEP 0x04000000ULL
typedef struct {
    A830_ALLOCATION *Allocation;
    ULONGLONG Source,Address;
    ULONG Size,Pitch,Format;
    BOOLEAN Valid,Hdr;
} COPY_REQUEST;
typedef struct {
    GPU_BO Frame,Commands,Staging[2];
    ULONG StagingIndex,Format;BOOLEAN StagingMapped,Hdr;
    ULONG ByteOffset;
    ULONGLONG Fence,Address;
    BOOLEAN Pending;
    volatile LONG Barrier;
    COPY_REQUEST Queued,Active;
} GPU_SCANOUT;
typedef struct { A830_ADAPTER *Adapter;COPY_REQUEST Request;BOOLEAN Take,Release; } REQUEST_ACCESS;
typedef struct { A830_ADAPTER *Adapter;GPU_BO *Frame;ULONGLONG Address;ULONG Format;BOOLEAN Hdr;NTSTATUS Status; } DPU_COPY_ACCESS;
static BOOLEAN QueueCopiedFrame(PVOID context) {
    DPU_COPY_ACCESS *access=context;
    access->Status=A830DpuQueue(access->Adapter,access->Frame->Physical,
        (access->Adapter->DisplayInfo.Width*4+255)&~255u,access->Address,access->Format,access->Hdr);
    return TRUE;
}
static VOID DropRequest(COPY_REQUEST *r) {
    if(r->Valid && r->Allocation)InterlockedDecrement(&r->Allocation->ScanoutReferences);
    RtlZeroMemory(r,sizeof(*r));
}
/* Dxgkrnl calls MMIO flips under the display interrupt lock. All other
   producer and consumer access uses SynchronizeExecution on that same lock. */
static BOOLEAN AccessRequest(PVOID context) {
    REQUEST_ACCESS *access=context;A830_ADAPTER *a=access->Adapter;GPU_SCANOUT *s=a->GpuScanout;
    if(access->Take) {
        access->Request=s->Queued;RtlZeroMemory(&s->Queued,sizeof(s->Queued));
    } else if(access->Release) {
        if(!s->Queued.Valid && !s->Pending && !s->Active.Valid && InterlockedExchange(&s->Barrier,0))
            InterlockedDecrement(&a->CpuBarriers);
    } else {
        DropRequest(&s->Queued);s->Queued=access->Request;
        if(!InterlockedExchange(&s->Barrier,1))InterlockedIncrement(&a->CpuBarriers);
    }
    return TRUE;
}
static NTSTATUS SynchronizeRequest(REQUEST_ACCESS *access) {
    if(KeGetCurrentIrql()>DISPATCH_LEVEL){AccessRequest(access);return STATUS_SUCCESS;}
    BOOLEAN result=FALSE;A830_ADAPTER *a=access->Adapter;
    return a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,AccessRequest,access,0,&result);
}
NTSTATUS QcomGpuScanoutQueue(PVOID context,PVOID allocation,ULONGLONG offset,ULONG pitch,ULONGLONG address) {
    A830_ADAPTER *a=context;A830_ALLOCATION *bo=allocation;
    if(!a->GpuScanout)return STATUS_DEVICE_NOT_READY;
    REQUEST_ACCESS access={0};access.Adapter=a;
    access.Request.Allocation=bo;access.Request.Pitch=pitch;access.Request.Address=address;access.Request.Valid=TRUE;
    access.Request.Format=bo?bo->Resource.Format:a->SourceFormat;
    access.Request.Hdr=a->HdrEnabled!=0;
    if(bo && bo->Buffer.Mdl) {
        access.Request.Source=bo->Buffer.Address;access.Request.Size=bo->Buffer.Size;
        InterlockedIncrement(&bo->ScanoutReferences);
    } else {
        access.Request.Allocation=NULL;
        access.Request.Source=a->Memory.Segment.Address+offset;
        access.Request.Size=(ULONG)(A830_SEGMENT_BYTES-offset);
    }
    NTSTATUS status=SynchronizeRequest(&access);
    if(!NT_SUCCESS(status))DropRequest(&access.Request);
    else A830WakeDisplay(a);
    return status;
}
BOOLEAN QcomGpuScanoutStagingReady(PVOID context) {
    GPU_SCANOUT *s=((A830_ADAPTER *)context)->GpuScanout;
    return s && s->StagingMapped && s->Staging[0].Address && s->Staging[1].Address;
}
NTSTATUS QcomGpuScanoutReserveStaging(PVOID context) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    if(s && s->StagingMapped)return STATUS_SUCCESS;
    if(s && s->Pending) {
        NTSTATUS status=QcomGpuScanoutRetire(a);if(status!=STATUS_SUCCESS)return status;
        DropRequest(&s->Active);
    }
    GPU_BO buffers[2]={0};NTSTATUS status=A830BridgeReserveDisplay(a,buffers);
    if(!NT_SUCCESS(status))return status;
    if(!s)return STATUS_DEVICE_NOT_READY;
    s->Staging[0]=buffers[0];s->Staging[1]=buffers[1];KeMemoryBarrier();s->StagingMapped=TRUE;
    return STATUS_SUCCESS;
}
BOOLEAN QcomGpuScanoutBusy(PVOID context) {
    GPU_SCANOUT *s=((A830_ADAPTER *)context)->GpuScanout;
    return s && InterlockedCompareExchange(&s->Barrier,0,0)!=0;
}
BOOLEAN QcomGpuScanoutEnabled(PVOID context) {return ((A830_ADAPTER *)context)->GpuScanout!=NULL;}
NTSTATUS QcomGpuScanoutRemap(PVOID context) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;
    if(!s)return STATUS_DEVICE_NOT_READY;
    if(s->Pending && a->Hardware.Submitted)return STATUS_DEVICE_BUSY;
    /* Submitted==0 is established by a hardware reset, after old DMA has
       stopped. Never carry an old-generation completion into a new ring. */
    if(!a->Hardware.Submitted){s->Pending=FALSE;s->Fence=0;}
    NTSTATUS status=GpuMap(&a->Hardware,0,&s->Frame,FRAME_ADDRESS);
    if(NT_SUCCESS(status))status=GpuMap(&a->Hardware,0,&s->Commands,COPY_ADDRESS);
    if(s->StagingMapped)for(ULONG i=0;NT_SUCCESS(status) && i<2;i++)status=GpuMap(&a->Hardware,0,s->Staging+i,STAGING_ADDRESS+STAGING_STEP*i);
    return status;
}
NTSTATUS QcomGpuScanoutAfterReset(PVOID context) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;if(!s)return STATUS_SUCCESS;
    REQUEST_ACCESS access={0};access.Adapter=a;access.Take=TRUE;
    NTSTATUS status=SynchronizeRequest(&access);if(!NT_SUCCESS(status))return status;
    DropRequest(&access.Request);DropRequest(&s->Active);s->Pending=FALSE;s->Fence=0;
    access.Take=FALSE;access.Release=TRUE;
    status=SynchronizeRequest(&access);if(!NT_SUCCESS(status))return status;
    s->Frame.Address=0;s->Commands.Address=0;
    for(ULONG i=0;i<2;i++)s->Staging[i].Address=0;
    return QcomGpuScanoutRemap(a);
}
NTSTATUS QcomGpuScanoutStart(PVOID context) {
    A830_ADAPTER *a=context;
    if(a->GpuScanout || !a->Hardware.Online)return STATUS_NOT_SUPPORTED;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    ULONGLONG physical=(ULONGLONG)a->DisplayInfo.PhysicAddress.QuadPart;
    ULONG offset=(ULONG)(physical&(PAGE_SIZE-1));
    if(!physical || !a->FramebufferSize ||
       a->FramebufferSize>FRAME_LIMIT-offset-PAGE_SIZE)return STATUS_INVALID_PARAMETER;
    ULONG bytes=(ULONG)((a->FramebufferSize+offset+PAGE_SIZE-1)&~(SIZE_T)(PAGE_SIZE-1));
    GPU_SCANOUT *s=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*s),GPU_TAG);
    if(!s)return STATUS_INSUFFICIENT_RESOURCES;
    s->ByteOffset=offset;s->Frame.Size=bytes;s->Frame.Physical=physical-offset;
    /* Construct only a PFN descriptor. These firmware pages are borrowed:
       never map them a second time and never pass this BO to GpuFree. */
    s->Frame.Mdl=IoAllocateMdl(NULL,bytes,FALSE,FALSE,NULL);
    if(!s->Frame.Mdl){ExFreePoolWithTag(s,GPU_TAG);return STATUS_INSUFFICIENT_RESOURCES;}
    s->Frame.Mdl->MdlFlags|=MDL_PAGES_LOCKED;
    PPFN_NUMBER pages=MmGetMdlPfnArray(s->Frame.Mdl);
    for(ULONG i=0;i<bytes/PAGE_SIZE;i++)pages[i]=(PFN_NUMBER)((physical-offset)/PAGE_SIZE+i);
    if(A830ScanoutStagingReady(a)) {
        if(!NT_SUCCESS(A830ScanoutStaging(a,0,s->Staging)) || !NT_SUCCESS(A830ScanoutStaging(a,1,s->Staging+1))) {
            IoFreeMdl(s->Frame.Mdl);ExFreePoolWithTag(s,GPU_TAG);return STATUS_DEVICE_NOT_READY;
        }
        s->StagingMapped=TRUE;
    }
    a->GpuScanout=s;
    NTSTATUS status=GpuAllocate(&s->Commands,COPY_BYTES);
    if(NT_SUCCESS(status))status=QcomGpuScanoutRemap(a);
    if(!NT_SUCCESS(status)){QcomGpuScanoutStop(a);return status;}
    return STATUS_SUCCESS;
}
NTSTATUS QcomGpuScanoutIssue(PVOID context,ULONGLONG source,ULONG sourceBytes,ULONG pitch,ULONGLONG address,PULONGLONG fence) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;
    if(!s || !fence)return STATUS_INVALID_PARAMETER;
    if(!a->Hardware.Online || !a->Hardware.Memory.Cpu)return STATUS_DEVICE_NOT_READY;
    if(a->Hardware.Fault)return (NTSTATUS)a->Hardware.Fault;
    if(s->Pending)return STATUS_DEVICE_BUSY;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    /* This is a display barrier, so the source has been rendered and cleaned.
       Reject a misuse rather than add a blocking wait to the hot path. */
    if(a->Hardware.Submitted>(ULONGLONG)*(volatile ULONG *)a->Hardware.Memory.Cpu)return STATUS_DEVICE_BUSY;
    ULONGLONG destination=s->Frame.Address+s->ByteOffset;SIZE_T destinationBytes=a->FramebufferSize;ULONG destinationPitch=a->DisplayInfo.Pitch;
    if(A830DpuEnabled(a)) {
        if(!s->StagingMapped)return STATUS_DEVICE_NOT_READY;
        ULONG index=0;while(index<2 && A830DpuReferences(a,s->Staging[index].Physical))index++;
        if(index==2)return STATUS_DEVICE_BUSY; /* TE completion wakes the worker. */
        s->StagingIndex=index;destination=s->Staging[index].Address;
        destinationBytes=s->Staging[index].Size;destinationPitch=(a->DisplayInfo.Width*4+255)&~255u;
    }
    ULONG count=A830BuildGpuCopy((unsigned *)s->Commands.Cpu,s->Commands.Size/4,
        source,sourceBytes,pitch,destination,destinationBytes,destinationPitch,
        a->DisplayInfo.Width,a->DisplayInfo.Height);
    if(!count)return STATUS_INVALID_PARAMETER;
    KeMemoryBarrier();
    NTSTATUS status=GpuSubmit(&a->Hardware,&s->Commands,0,count);
    if(!NT_SUCCESS(status))return status;
    s->Fence=a->Hardware.Submitted;s->Address=address;s->Pending=TRUE;*fence=s->Fence;
    return STATUS_SUCCESS;
}
NTSTATUS QcomGpuScanoutRetire(PVOID context) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;
    if(!s)return STATUS_DEVICE_NOT_READY;
    if(!s->Pending)return STATUS_SUCCESS;
    if(!a->Hardware.Online || !a->Hardware.Memory.Cpu)return STATUS_DEVICE_NOT_READY;
    if(a->Hardware.Fault)return (NTSTATUS)a->Hardware.Fault;
    KeMemoryBarrier();
    if((ULONGLONG)*(volatile ULONG *)a->Hardware.Memory.Cpu<s->Fence)return STATUS_PENDING;
    if(A830DpuEnabled(a)) {
        GPU_BO *frame=s->Staging+s->StagingIndex;
        DPU_COPY_ACCESS access={a,frame,s->Address,s->Format,s->Hdr,STATUS_UNSUCCESSFUL};BOOLEAN result=FALSE;
        NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,QueueCopiedFrame,&access,0,&result);
        if(!NT_SUCCESS(status))return status;
        if(!NT_SUCCESS(access.Status))return access.Status;
        /* WDDM scanout address is acknowledged only by DPU's TE callback. */
    } else {
        InterlockedExchange64((volatile LONG64 *)&a->ScanoutAddress,(LONG64)s->Address);

    }
    s->Pending=FALSE;
    return STATUS_SUCCESS;
}
BOOLEAN QcomGpuScanoutPump(PVOID context) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;if(!s)return FALSE;
    if(s->Pending) {
        NTSTATUS retired=QcomGpuScanoutRetire(a);
        if(retired==STATUS_PENDING)return TRUE;
        if(!NT_SUCCESS(retired)){InterlockedExchange(&a->Failed,retired);return TRUE;}
        DropRequest(&s->Active);
    }
    if(!s->Active.Valid) {
        REQUEST_ACCESS access={0};access.Adapter=a;access.Take=TRUE;
        NTSTATUS status=SynchronizeRequest(&access);
        if(!NT_SUCCESS(status)){InterlockedExchange(&a->Failed,status);return TRUE;}
        s->Active=access.Request;
    }
    if(s->Active.Valid) {
        s->Format=s->Active.Format;s->Hdr=s->Active.Hdr;
        NTSTATUS status=QcomGpuScanoutIssue(a,s->Active.Source,s->Active.Size,s->Active.Pitch,s->Active.Address,&s->Fence);
        if(status==STATUS_DEVICE_BUSY)return TRUE;
        if(!NT_SUCCESS(status)){InterlockedExchange(&a->Failed,status);return TRUE;}
        return TRUE;
    }
    REQUEST_ACCESS access={0};access.Adapter=a;access.Release=TRUE;
    NTSTATUS status=SynchronizeRequest(&access);
    if(!NT_SUCCESS(status)){InterlockedExchange(&a->Failed,status);return TRUE;}
    return InterlockedCompareExchange(&s->Barrier,0,0)!=0;
}
NTSTATUS QcomGpuScanoutStop(PVOID context) {
    A830_ADAPTER *a=context;GPU_SCANOUT *s=a->GpuScanout;if(!s)return STATUS_SUCCESS;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    if(a->Hardware.Online) {
        NTSTATUS status=GpuWait(&a->Hardware,a->Hardware.Submitted,10000000);
        if(!NT_SUCCESS(status))return status; /* keep borrowed mapping alive */
        if(s->Commands.Address)GpuUnmap(&a->Hardware,0,&s->Commands);
        if(s->Frame.Address)GpuUnmap(&a->Hardware,0,&s->Frame);
        for(ULONG i=0;i<2;i++)if(s->Staging[i].Address)GpuUnmap(&a->Hardware,0,s->Staging+i);
    }
    REQUEST_ACCESS access={0};access.Adapter=a;access.Take=TRUE;
    NTSTATUS synchronized=SynchronizeRequest(&access);
    if(!NT_SUCCESS(synchronized))return synchronized;
    DropRequest(&access.Request);DropRequest(&s->Active);s->Pending=FALSE;
    access.Take=FALSE;access.Release=TRUE;
    synchronized=SynchronizeRequest(&access);if(!NT_SUCCESS(synchronized))return synchronized;
    GpuFree(&s->Commands);
    if(s->Frame.Mdl)IoFreeMdl(s->Frame.Mdl);
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);a->GpuScanout=NULL;KeReleaseSpinLock(&a->QueueLock,old);
    ExFreePoolWithTag(s,GPU_TAG);return STATUS_SUCCESS;
}
