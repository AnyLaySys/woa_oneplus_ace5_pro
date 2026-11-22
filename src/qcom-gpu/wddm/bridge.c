#include "adapter.h"
#include "../uapi.h"
#include "../regs.h"
#include <wdmsec.h>
#include "../deferred-free.h"

typedef struct {
    LIST_ENTRY Entry;
    ULONGLONG Address,Size;
    ULONG Handle;
    BOOLEAN Zero,Released;
    ULONGLONG Pages[];
} A830_SPARSE;
typedef struct {
    LIST_ENTRY Entry,Sparse;
    ULONG NextSparse;
    PULONG SparseRefs;
    GPU_BO Buffers[GPU_MAX_BUFFERS];
    PVOID Maps[GPU_MAX_BUFFERS];
    PEPROCESS Owner;
    ULONGLONG Lease;
    LONG PendingRefs;
    KSPIN_LOCK RefLock;
    BOOLEAN Draining;
    KEVENT Drained;
    A830_ALLOCATION *Imports[GPU_MAX_BUFFERS];
    GPU_DEFERRED_FREE Deferred;
} A830_CLIENT;
typedef struct {
    PDEVICE_OBJECT Device;
    A830_ADAPTER *Adapter;
    GPU *Hardware;
    LIST_ENTRY Clients;
    LIST_ENTRY Shared;
    ULONGLONG NextLease;
    KMUTEX Lock;
    RTL_BITMAP Addresses;
    PULONG AddressBits;
    ULONG AddressHint;
    EX_RUNDOWN_REF ClockAccess;
    BOOLEAN ClockStopped;
    BOOLEAN DeviceDeleted;
    KEVENT GovernorStop;
    HANDLE GovernorThread;
    PDRIVER_DISPATCH Previous[IRP_MJ_MAXIMUM_FUNCTION+1];
} A830_BRIDGE;
static BOOLEAN KernelRange(ULONGLONG address,SIZE_T bytes) {
    if(!bytes)return FALSE;
    if(address>=A830_SEGMENT_ADDRESS && address<A830_COMMAND_ADDRESS+A830_COMMAND_BYTES)return bytes<=A830_COMMAND_ADDRESS+A830_COMMAND_BYTES-address;
    return address>=A830_SHARED_ADDRESS && address<GPU_VA_END && bytes<=GPU_VA_END-address;
}
static A830_BRIDGE Bridge;
static VOID CollectClientDeferred(A830_BRIDGE *g,A830_CLIENT *c);
static VOID CollectSparse(A830_BRIDGE *g,A830_CLIENT *c);
static VOID CleanupSparse(A830_BRIDGE *g,A830_CLIENT *c);
static NTSTATUS ResetSparse(A830_BRIDGE *g,A830_CLIENT *c);

static VOID FreeAddress(A830_BRIDGE *g,ULONGLONG address,ULONGLONG size) {
    if(g->AddressBits && address>=GPU_CLIENT_VA_BASE && address<GPU_CLIENT_VA_END)
        RtlClearBits(&g->Addresses,(ULONG)((address-GPU_CLIENT_VA_BASE)>>PAGE_SHIFT),(ULONG)(size>>PAGE_SHIFT));
}
static BOOLEAN FenceComplete(GPU *hardware,ULONGLONG fence) {
    if(!hardware || !hardware->Online)return TRUE;
    KeMemoryBarrier();return hardware->Memory.Cpu && *(volatile ULONGLONG *)hardware->Memory.Cpu>=fence;
}
static VOID MarkDeferred(A830_BRIDGE *g,A830_CLIENT *c,ULONG slot,BOOLEAN imported) {
    ULONGLONG fence=g->Hardware?g->Hardware->Submitted:0;
    GpuDeferFree(&c->Deferred,slot,imported,fence);
    if(g->Adapter)A830WakeDisplay(g->Adapter);
}
/* Bridge.Lock is held throughout lookup and copying. SharedRelease and the
   deferred collector therefore cannot free a frame that flips away midway. */
NTSTATUS A830SharedCreate(A830_ALLOCATION *allocation) {
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_DEVICE_NOT_READY;
    if(Bridge.Hardware && Bridge.Hardware->Online) {
        ULONG pitch=allocation->Resource.Pitch,height=allocation->Resource.Height;
        ULONG rows=(height+63)&~63u;
        if(!pitch || rows>GPU_BUFFER_LIMIT/pitch){status=STATUS_INVALID_PARAMETER;goto done;}
        ULONG size=pitch*rows;
        status=(allocation->Resource.Flags&A830_RESOURCE_PRIMARY)?A830ScanoutAllocate(Bridge.Adapter,&allocation->Buffer,size):STATUS_NOT_SUPPORTED;
        if(!NT_SUCCESS(status))status=GpuAllocate(&allocation->Buffer,size);
        if(!NT_SUCCESS(status))goto done;
        /* A pool slot may be larger than the requested primary. Reserve the
           complete mapped BO extent, never only the resource's visible bytes. */
        size=allocation->Buffer.Size;
        ULONGLONG address=A830_SHARED_BASE;BOOLEAN overlap;
        do {
            overlap=FALSE;
            for(PLIST_ENTRY e=Bridge.Shared.Flink;e!=&Bridge.Shared;e=e->Flink) {
                GPU_BO *bo=&CONTAINING_RECORD(e,A830_ALLOCATION,Entry)->Buffer;
                if(address<bo->Address+bo->Size && address+size>bo->Address){address=bo->Address+bo->Size;overlap=TRUE;break;}
            }
        } while(overlap);
        if(address+size>A830_SHARED_END){if(!A830ScanoutFree(Bridge.Adapter,&allocation->Buffer))GpuFree(&allocation->Buffer);status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
        status=GpuMap(Bridge.Hardware,0,&allocation->Buffer,address);
        if(!NT_SUCCESS(status)){if(!A830ScanoutFree(Bridge.Adapter,&allocation->Buffer))GpuFree(&allocation->Buffer);goto done;}
        InsertTailList(&Bridge.Shared,&allocation->Entry);
        allocation->References=1;
    }
done:KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
NTSTATUS A830BridgeReserveDisplay(PVOID context,GPU_BO buffers[2]) {
    A830_ADAPTER *a=context;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=Bridge.Adapter==a && Bridge.Hardware && Bridge.Hardware->Online?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
    if(NT_SUCCESS(status))status=A830ScanoutStart(a);
    if(NT_SUCCESS(status) && !A830ScanoutStagingReady(a))status=STATUS_INSUFFICIENT_RESOURCES;
    if(NT_SUCCESS(status) && a->Hardware.Submitted>(ULONGLONG)*(volatile ULONG *)a->Hardware.Memory.Cpu)status=STATUS_DEVICE_BUSY;
    for(ULONG i=0;NT_SUCCESS(status) && i<2;i++) {
        status=A830ScanoutStaging(a,i,buffers+i);
        if(NT_SUCCESS(status))status=GpuMap(&a->Hardware,0,buffers+i,0xc0000000ULL+i*0x04000000ULL);
        if(!NT_SUCCESS(status) && i && buffers[0].Address)GpuUnmap(&a->Hardware,0,buffers);
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
static VOID SharedRelease(A830_ALLOCATION *allocation) {
    if(allocation->References && --allocation->References)return;
    if(!allocation->ReleasePending){allocation->ReleaseFence=Bridge.Hardware?Bridge.Hardware->Submitted:0;allocation->ReleasePending=TRUE;}
    if(!FenceComplete(Bridge.Hardware,allocation->ReleaseFence))return;
    if(InterlockedCompareExchange(&allocation->ScanoutReferences,0,0))return;
    if(Bridge.Adapter && A830DpuReferences(Bridge.Adapter,allocation->Buffer.Physical))return;
    if(allocation->Buffer.Mdl) {
        RemoveEntryList(&allocation->Entry);
        if(Bridge.Hardware && Bridge.Hardware->Online) {
            GpuUnmap(Bridge.Hardware,0,&allocation->Buffer);
        }
        if(!A830ScanoutFree(Bridge.Adapter,&allocation->Buffer))GpuFree(&allocation->Buffer);
    }
    ExFreePoolWithTag(allocation,GPU_TAG);
}
/* Bridge.Lock is held. Pending slot IDs remain occupied until actual free;
   callers cannot make a new allocation reuse a deferred object's identity. */
static VOID CollectClientDeferred(A830_BRIDGE *g,A830_CLIENT *c) {
    GPU *hardware=g->Hardware;ULONGLONG completed=0;KeMemoryBarrier();
    if(hardware && hardware->Memory.Cpu)completed=*(volatile ULONGLONG *)hardware->Memory.Cpu;
    if(!GpuDeferredReady(&c->Deferred,(ULONG)InterlockedCompareExchange(&c->PendingRefs,0,0),hardware && hardware->Online,hardware && hardware->Memory.Cpu,completed))return;
    CollectSparse(g,c);
    BOOLEAN attached=FALSE;KAPC_STATE state;
    for(ULONG i=0;i<GPU_MAX_BUFFERS;i++) {
        if(c->Deferred.Buffers[i] && (!c->SparseRefs || !c->SparseRefs[i])) {
            GPU_BO *bo=c->Buffers+i;
            ULONGLONG address=bo->Address;
            if(c->Maps[i]) {
                if(!attached && PsGetCurrentProcess()!=c->Owner){KeStackAttachProcess(c->Owner,&state);attached=TRUE;}
                MmUnmapLockedPages(c->Maps[i],bo->Mdl);c->Maps[i]=NULL;
            }
            if(bo->Mdl && g->Hardware && g->Hardware->Online)GpuUnmap(g->Hardware,0,bo);
            FreeAddress(g,address,bo->Size);
            GpuFree(bo);c->Deferred.Buffers[i]=FALSE;
        }
    }
    if(attached)KeUnstackDetachProcess(&state);
    for(ULONG i=0;i<GPU_MAX_BUFFERS;i++)if(c->Deferred.Imports[i]) {
        A830_ALLOCATION *allocation=c->Imports[i];c->Imports[i]=NULL;c->Deferred.Imports[i]=FALSE;
        if(allocation)SharedRelease(allocation);
    }
    c->Deferred.Fence=0;
}
VOID A830BridgeCollectShared(VOID) {
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    for(PLIST_ENTRY e=Bridge.Clients.Flink;e!=&Bridge.Clients;e=e->Flink)
        CollectClientDeferred(&Bridge,CONTAINING_RECORD(e,A830_CLIENT,Entry));
    PLIST_ENTRY entry=Bridge.Shared.Flink;
    while(entry!=&Bridge.Shared) {
        A830_ALLOCATION *allocation=CONTAINING_RECORD(entry,A830_ALLOCATION,Entry);entry=entry->Flink;
        if(!allocation->References)SharedRelease(allocation);
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);
}
VOID A830SharedDestroy(A830_ALLOCATION *allocation) {
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    SharedRelease(allocation);
    KeReleaseMutex(&Bridge.Lock,FALSE);
}
static VOID LoadPowerPolicy(GPU *gpu) {
    UNICODE_STRING path;RtlInitUnicodeString(&path,L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\qcom_gpu\\Parameters");
    OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    HANDLE key;if(!NT_SUCCESS(ZwOpenKey(&key,KEY_QUERY_VALUE,&attributes)))return;
    PCWSTR names[]={L"FixedGpuKhz",L"MaxGpuKhz"};PULONG values[]={&gpu->FixedKhz,&gpu->MaxKhz};
    for(ULONG i=0;i<RTL_NUMBER_OF(names);i++) {
        UNICODE_STRING name;RtlInitUnicodeString(&name,names[i]);ULONG length=0;
        UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION)+sizeof(ULONG)];
        PKEY_VALUE_PARTIAL_INFORMATION info=(PVOID)buffer;
        if(NT_SUCCESS(ZwQueryValueKey(key,&name,KeyValuePartialInformation,buffer,sizeof(buffer),&length)) &&
            info->Type==REG_DWORD && info->DataLength==sizeof(ULONG)) {
            ULONG khz=*(PULONG)info->Data;
            if(!khz || (gpu->PerfCount>1 && khz>=160000 && khz<=gpu->PerfKhz[gpu->PerfCount-1]))*values[i]=khz;
        }
    }
    if(gpu->MaxKhz && gpu->FixedKhz>gpu->MaxKhz)gpu->FixedKhz=gpu->MaxKhz;
    ZwClose(key);
}
static VOID Governor(PVOID context) {
    A830_BRIDGE *g=context;LARGE_INTEGER interval;interval.QuadPart=-DVFS_SAMPLE_100NS;
    while(KeWaitForSingleObject(&g->GovernorStop,Executive,KernelMode,FALSE,&interval)==STATUS_TIMEOUT) {
        if(ExAcquireRundownProtection(&g->ClockAccess)) {
            if(g->Hardware)GpuGovern(g->Hardware);
            ExReleaseRundownProtection(&g->ClockAccess);
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static VOID PauseClock(A830_BRIDGE *g) {
    if(!g->ClockStopped){ExWaitForRundownProtectionRelease(&g->ClockAccess);g->ClockStopped=TRUE;}
}
static VOID ResumeClock(A830_BRIDGE *g) {
    if(g->ClockStopped){ExReInitializeRundownProtection(&g->ClockAccess);g->ClockStopped=FALSE;}
}
VOID A830BridgeStopHardware(A830_ADAPTER *a) {
    /* The worker is joined by the caller. Serialize with governor HFI votes,
       client allocation/map/free and timestamp readers before tearing down
       their hardware storage. Never acquire Bridge.Lock inside MappingLock. */
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    if(Bridge.Adapter==a && Bridge.Hardware==&a->Hardware)PauseClock(&Bridge);
    GpuStop(&a->Hardware);
    KeReleaseMutex(&Bridge.Lock,FALSE);
}
static VOID Cleanup(A830_BRIDGE *g,A830_CLIENT *c) {
    if(!c->Owner)return;
    if(!NT_SUCCESS(GpuWait(g->Hardware,g->Hardware->Submitted,10000000))) {
        BOOLEAN stopped=g->ClockStopped;
        PauseClock(g);GpuStop(g->Hardware);if(!stopped)ResumeClock(g);
    }
    CleanupSparse(g,c);
    KAPC_STATE state;BOOLEAN attach=PsGetCurrentProcess()!=c->Owner;
    if(attach)KeStackAttachProcess(c->Owner,&state);
    for(ULONG i=0;i<RTL_NUMBER_OF(c->Buffers);i++) {
        GPU_BO *bo=c->Buffers+i;
        ULONGLONG address=bo->Address;
        if(g->Hardware->Online)GpuUnmap(g->Hardware,0,bo);
        if(c->Maps[i])MmUnmapLockedPages(c->Maps[i],bo->Mdl);
        c->Maps[i]=NULL;
        FreeAddress(g,address,bo->Size);
        GpuFree(bo);c->Deferred.Buffers[i]=FALSE;
    }
    if(attach)KeUnstackDetachProcess(&state);
    for(ULONG i=0;i<RTL_NUMBER_OF(c->Imports);i++) {
        if(c->Imports[i]){SharedRelease(c->Imports[i]);c->Imports[i]=NULL;}
        c->Deferred.Imports[i]=FALSE;
    }
    c->Deferred.Fence=0;
    ObDereferenceObject(c->Owner);c->Owner=NULL;
}
static ULONGLONG Address(A830_BRIDGE *g,ULONGLONG size,ULONGLONG requested) {
    const ULONG pages=(ULONG)(GPU_CLIENT_VA_SIZE>>PAGE_SHIFT);
    if(!g->AddressBits) {
        g->AddressBits=ExAllocatePool2(POOL_FLAG_NON_PAGED,pages/8,GPU_TAG);
        if(!g->AddressBits)return 0;
        RtlInitializeBitMap(&g->Addresses,g->AddressBits,pages);
    }
    if(!size || (size&(PAGE_SIZE-1)) || size>GPU_CLIENT_VA_SIZE)return 0;
    ULONG count=(ULONG)(size>>PAGE_SHIFT),index;
    if(requested) {
        if(requested<GPU_CLIENT_VA_BASE || requested>=GPU_CLIENT_VA_END || (requested&(PAGE_SIZE-1)) || size>GPU_CLIENT_VA_END-requested)return 0;
        index=(ULONG)((requested-GPU_CLIENT_VA_BASE)>>PAGE_SHIFT);
        if(!RtlAreBitsClear(&g->Addresses,index,count))return 0;
        RtlSetBits(&g->Addresses,index,count);
    } else index=RtlFindClearBitsAndSet(&g->Addresses,count,g->AddressHint);
    if(index==MAXULONG)return 0;
    g->AddressHint=index+count;
    if(g->AddressHint>=pages)g->AddressHint=0;
    return GPU_CLIENT_VA_BASE+((ULONGLONG)index<<PAGE_SHIFT);
}
#include "sparse.inc"
static NTSTATUS Submit(A830_BRIDGE *g,A830_CLIENT *c,const GPU_SUBMIT *entries,ULONG count) {
    GPU_COMMAND commands[GPU_MAX_COMMANDS];
    if(!count || count>RTL_NUMBER_OF(commands))return STATUS_INVALID_PARAMETER;
    for(ULONG i=0;i<count;i++) {
        const GPU_SUBMIT *s=entries+i;
        if(!s->Handle || s->Handle>RTL_NUMBER_OF(c->Buffers) || s->Reserved || c->Deferred.Buffers[s->Handle-1])return STATUS_INVALID_PARAMETER;
        commands[i]=(GPU_COMMAND){c->Buffers+s->Handle-1,s->Offset,s->Words};
    }
    NTSTATUS status=GpuSubmitContextBatch(g->Hardware,commands,count,c->Lease);
    return status;
}
struct A830_PREPARED { A830_CLIENT *Client; ULONG Count; GPU_COMMAND Commands[GPU_MAX_COMMANDS]; };
NTSTATUS A830Prepare(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *entries,ULONG count,A830_PREPARED **out) {
    if(!count || count>GPU_MAX_COMMANDS || KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
    A830_PREPARED *p=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*p),GPU_TAG);if(!p)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=STATUS_INVALID_HANDLE;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    for(PLIST_ENTRY e=Bridge.Clients.Flink;e!=&Bridge.Clients;e=e->Flink) {
        A830_CLIENT *c=CONTAINING_RECORD(e,A830_CLIENT,Entry);
        if(c->Owner!=process || c->Lease!=lease || c->Draining)continue;
        status=STATUS_SUCCESS;
        for(ULONG i=0;i<count;i++) {
            const GPU_SUBMIT *v=entries+i;
            if(!v->Handle || v->Handle>GPU_MAX_BUFFERS || v->Reserved || !v->Words || v->Words>0xfffff || (v->Offset&3) || c->Deferred.Buffers[v->Handle-1]) {status=STATUS_INVALID_PARAMETER;break;}
            GPU_BO *b=c->Buffers+v->Handle-1;
            if(!b->Mdl || !b->Address || v->Offset>b->Size || v->Words>(b->Size-v->Offset)/4){status=STATUS_INVALID_PARAMETER;break;}
            p->Commands[i]=(GPU_COMMAND){b,v->Offset,v->Words};
        }
        if(NT_SUCCESS(status)){p->Client=c;p->Count=count;InterlockedIncrement(&c->PendingRefs);KeClearEvent(&c->Drained);}
        break;
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(p,GPU_TAG);return status;}
    *out=p;return STATUS_SUCCESS;
}
NTSTATUS A830Issue(A830_PREPARED *p,PULONGLONG fence) {
    NTSTATUS status=GpuSubmitContextBatch(Bridge.Hardware,p->Commands,p->Count,p->Client->Lease);
    if(NT_SUCCESS(status)){*fence=Bridge.Hardware->Submitted;}
    return status;
}
VOID A830ReleasePrepared(A830_PREPARED *p) {
    if(!p)return;
    KIRQL old;KeAcquireSpinLock(&p->Client->RefLock,&old);
    if(!InterlockedDecrement(&p->Client->PendingRefs)) {
        KeSetEvent(&p->Client->Drained,IO_NO_INCREMENT,FALSE);
        /* Cancelled Render may never generate a GPU interrupt. Wake the
           passive deferred-free collector when its last owner disappears. */
        if(Bridge.Adapter)A830WakeDisplay(Bridge.Adapter);
    }
    KeReleaseSpinLock(&p->Client->RefLock,old);
    ExFreePoolWithTag(p,GPU_TAG);
}
/* Called with Bridge.Lock: release it while queued jobs drain, otherwise the
   worker and user resource-destruction path would wait on each other. */
static VOID DrainClient(A830_CLIENT *c) {
    c->Draining=TRUE;
    KeReleaseMutex(&Bridge.Lock,FALSE);
    while(InterlockedCompareExchange(&c->PendingRefs,0,0)) {
        KeClearEvent(&c->Drained);
        if(InterlockedCompareExchange(&c->PendingRefs,0,0))KeWaitForSingleObject(&c->Drained,Executive,KernelMode,FALSE,NULL);
    }
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    /* A final release sets PendingRefs=0 before signalling Drained. Join its
       short critical section before cleanup is allowed to free the client. */
    KIRQL old;KeAcquireSpinLock(&c->RefLock,&old);KeReleaseSpinLock(&c->RefLock,old);
    c->Draining=FALSE;
}
static NTSTATUS StartHardware(A830_BRIDGE *g) {
    if(!g->Adapter || !g->Hardware)return STATUS_DEVICE_NOT_READY;
    if(g->Hardware->Online)return STATUS_SUCCESS;
    NTSTATUS status=g->Hardware->Registers?STATUS_SUCCESS:
        GpuBind(g->Hardware,g->Adapter->GpuRegisters,g->Adapter->GpuRegisterSize);
    if(NT_SUCCESS(status)) {
        status=GpuStart(g->Hardware);
        if(NT_SUCCESS(status))LoadPowerPolicy(g->Hardware);
    }
    return status;
}
NTSTATUS A830BridgeReset(VOID) {
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_DEVICE_NOT_READY;
    if(Bridge.Adapter && Bridge.Hardware) {
        PauseClock(&Bridge);
        GpuStop(Bridge.Hardware);status=StartHardware(&Bridge);
        for(PLIST_ENTRY entry=Bridge.Clients.Flink;NT_SUCCESS(status) && entry!=&Bridge.Clients;entry=entry->Flink) {
            A830_CLIENT *c=CONTAINING_RECORD(entry,A830_CLIENT,Entry);
            for(ULONG i=0;NT_SUCCESS(status) && i<RTL_NUMBER_OF(c->Buffers);i++) {
                GPU_BO *b=c->Buffers+i;
                if(b->Mdl)status=GpuMap(Bridge.Hardware,0,b,b->Address);
            }
            if(NT_SUCCESS(status))status=ResetSparse(&Bridge,c);
        }
        for(PLIST_ENTRY entry=Bridge.Shared.Flink;NT_SUCCESS(status) && entry!=&Bridge.Shared;entry=entry->Flink) {
            GPU_BO *bo=&CONTAINING_RECORD(entry,A830_ALLOCATION,Entry)->Buffer;
            status=GpuMap(Bridge.Hardware,0,bo,bo->Address);
        }
        ResumeClock(&Bridge);
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
NTSTATUS A830BridgeMap(GPU_BO *bo,ULONGLONG address) {
    if(!bo || !KernelRange(address,bo->Size))
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=StartHardware(&Bridge);
    if(NT_SUCCESS(status))status=GpuWait(Bridge.Hardware,Bridge.Hardware->Submitted,10000000);
    if(NT_SUCCESS(status))status=GpuMap(Bridge.Hardware,0,bo,address);
    KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
NTSTATUS A830BridgeUnmap(GPU_BO *bo) {
    if(!bo || !KernelRange(bo->Address,bo->Size))return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_DEVICE_NOT_READY;
    if(Bridge.Adapter && Bridge.Hardware && Bridge.Hardware->Online) {
        status=GpuWait(Bridge.Hardware,Bridge.Hardware->Submitted,10000000);
        if(NT_SUCCESS(status))GpuUnmap(Bridge.Hardware,0,bo);
        else {PauseClock(&Bridge);GpuStop(Bridge.Hardware);ResumeClock(&Bridge);}
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
NTSTATUS A830BridgeExecute(const GPU_COMMAND *commands,ULONG count) {
    if(!commands || !count || count>GPU_MAX_COMMANDS)return STATUS_INVALID_PARAMETER;
    for(ULONG i=0;i<count;i++)if(!commands[i].Buffer ||
        !KernelRange(commands[i].Buffer->Address,commands[i].Buffer->Size))return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=Bridge.Adapter && Bridge.Hardware?GpuSubmitBatch(Bridge.Hardware,commands,count):STATUS_DEVICE_NOT_READY;
    if(NT_SUCCESS(status))status=GpuWait(Bridge.Hardware,Bridge.Hardware->Submitted,10000000);
    KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
NTSTATUS A830BridgeExecuteClient(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *commands,ULONG count) {
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_INVALID_HANDLE;
    if(Bridge.Adapter && Bridge.Hardware && process && lease) {
        for(PLIST_ENTRY entry=Bridge.Clients.Flink;entry!=&Bridge.Clients;entry=entry->Flink) {
            A830_CLIENT *c=CONTAINING_RECORD(entry,A830_CLIENT,Entry);
            if(c->Owner==process && c->Lease==lease) {
                status=Submit(&Bridge,c,commands,count);
                if(NT_SUCCESS(status)) {
                    status=GpuWait(Bridge.Hardware,Bridge.Hardware->Submitted,10000000);
                }
                break;
            }
        }
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);return status;
}
NTSTATUS A830BridgeTimestamp(PULONGLONG counter) {
    if(!ExAcquireRundownProtection(&Bridge.ClockAccess))return STATUS_DEVICE_NOT_READY;
    GPU *hardware=Bridge.Hardware;
    NTSTATUS status=hardware?GpuTimestamp(hardware,counter):STATUS_DEVICE_NOT_READY;
    ExReleaseRundownProtection(&Bridge.ClockAccess);return status;
}
static NTSTATUS Io(A830_BRIDGE *g,A830_CLIENT *c,PIRP request,ULONG code,ULONG inLength,ULONG outLength) {
    PVOID in=NULL,out=NULL;ULONG_PTR bytes=0;NTSTATUS status=STATUS_INVALID_PARAMETER;
    in=out=request->AssociatedIrp.SystemBuffer;
    if((inLength || outLength) && !in)return STATUS_INVALID_PARAMETER;
    if(code==GPU_IOCTL_SPARSE_CAPS || code==GPU_IOCTL_VA_RESERVE || code==GPU_IOCTL_VA_RELEASE || code==GPU_IOCTL_VA_BIND)
        return SparseIo(g,c,request,code,inLength,outLength);


    if(code==GPU_IOCTL_SHARED_RETAIN && inLength==sizeof(GPU_SHARED_IMPORT) && outLength>=sizeof(ULONG)) {
        GPU_SHARED_IMPORT import=*(GPU_SHARED_IMPORT *)in;
        if(!import.Size || import.Address<A830_SHARED_BASE || import.Address>=A830_SHARED_END || import.Size>A830_SHARED_END-import.Address)return STATUS_INVALID_PARAMETER;
        ULONG slot=0;while(slot<RTL_NUMBER_OF(c->Imports) && (c->Imports[slot] || c->Deferred.Imports[slot]))slot++;
        if(slot==RTL_NUMBER_OF(c->Imports))return STATUS_INSUFFICIENT_RESOURCES;
        for(PLIST_ENTRY e=g->Shared.Flink;e!=&g->Shared;e=e->Flink) {
            A830_ALLOCATION *allocation=CONTAINING_RECORD(e,A830_ALLOCATION,Entry);
            if(allocation->Buffer.Address==import.Address && import.Size<=allocation->Buffer.Size) {
                allocation->References++;c->Imports[slot]=allocation;*(PULONG)out=0x80000000u|(slot+1);bytes=sizeof(ULONG);status=STATUS_SUCCESS;break;
            }
        }
    } else if(code==GPU_IOCTL_SHARED_RELEASE && inLength==sizeof(ULONG) && !outLength) {
        ULONG slot=(*(PULONG)in&0x7fffffffu)-1;
        if(slot>=RTL_NUMBER_OF(c->Imports) || !c->Imports[slot] || c->Deferred.Imports[slot])return STATUS_INVALID_HANDLE;
        MarkDeferred(g,c,slot,TRUE);CollectClientDeferred(g,c);status=STATUS_SUCCESS;
    } else if(code==GPU_IOCTL_QUERY && outLength>=sizeof(GPU_INFO)) {
        const GPU_INFO info={GPU_UAPI_VERSION,0x44050001,g->Hardware->Fault};
        RtlCopyMemory(out,&info,sizeof(info));bytes=sizeof(info);status=STATUS_SUCCESS;
    } else if(code==GPU_IOCTL_CLIENT_LEASE && !inLength && outLength>=sizeof(ULONGLONG)) {
        *(PULONGLONG)out=c->Lease;bytes=sizeof(ULONGLONG);status=STATUS_SUCCESS;
    } else if(!g->Hardware->Online)status=STATUS_DEVICE_NOT_READY;
    else if((code==GPU_IOCTL_ALLOC || code==GPU_IOCTL_ALLOC_CACHED) && inLength==sizeof(ULONG) && outLength>=sizeof(GPU_ALLOCATION)) {
        ULONG size=*(PULONG)in;
        status=STATUS_INSUFFICIENT_RESOURCES;
        for(ULONG i=0;i<RTL_NUMBER_OF(c->Buffers);i++)if(!c->Buffers[i].Mdl) {
            GPU_BO *bo=c->Buffers+i;
            status=code==GPU_IOCTL_ALLOC_CACHED?GpuAllocateCached(bo,size):GpuAllocate(bo,size);
            if(!NT_SUCCESS(status)){break;}
            ULONGLONG address=Address(g,bo->Size,0);
            if(!address){GpuFree(bo);status=STATUS_INSUFFICIENT_RESOURCES;break;}
            status=GpuMap(g->Hardware,0,bo,address);
            if(!NT_SUCCESS(status)){FreeAddress(g,address,bo->Size);GpuStop(g->Hardware);GpuFree(bo);break;}
            KAPC_STATE state;BOOLEAN attach=PsGetCurrentProcess()!=c->Owner;
            if(attach)KeStackAttachProcess(c->Owner,&state);
            __try { c->Maps[i]=MmMapLockedPagesSpecifyCache(bo->Mdl,UserMode,bo->Cached?MmCached:MmWriteCombined,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute); }
            __except(EXCEPTION_EXECUTE_HANDLER) { status=GetExceptionCode(); }
            if(attach)KeUnstackDetachProcess(&state);
            if(!c->Maps[i]){GpuUnmap(g->Hardware,0,bo);FreeAddress(g,address,bo->Size);GpuFree(bo);if(NT_SUCCESS(status))status=STATUS_INSUFFICIENT_RESOURCES;break;}
            GPU_ALLOCATION allocation={i+1,bo->Size,bo->Address,(ULONGLONG)c->Maps[i]};
            RtlCopyMemory(out,&allocation,sizeof(allocation));bytes=sizeof(allocation);break;
        }

    } else if(code==GPU_IOCTL_FENCE_READ && inLength==sizeof(ULONGLONG) && outLength>=sizeof(ULONGLONG)) {
        ULONGLONG address=*(PULONGLONG)in;
        if(address&7)return STATUS_INVALID_PARAMETER;
        status=STATUS_INVALID_ADDRESS;
        for(ULONG i=0;i<RTL_NUMBER_OF(c->Buffers);i++) {
            GPU_BO *bo=c->Buffers+i;
            if(!bo->Mdl || c->Deferred.Buffers[i] || address<bo->Address ||
               address-bo->Address>bo->Size || sizeof(ULONGLONG)>bo->Size-(address-bo->Address))continue;
            if(bo->Cached)return STATUS_NOT_SUPPORTED;
            KeMemoryBarrier();*(PULONGLONG)out=*(volatile ULONGLONG *)(bo->Cpu+(SIZE_T)(address-bo->Address));
            bytes=sizeof(ULONGLONG);status=STATUS_SUCCESS;break;
        }
    } else if(code==GPU_IOCTL_CACHE && inLength==sizeof(GPU_CACHE)) {
        GPU_CACHE range=*(GPU_CACHE *)in;
        if(!range.Handle || range.Handle>RTL_NUMBER_OF(c->Buffers) || range.Readback>1 || c->Deferred.Buffers[range.Handle-1])return STATUS_INVALID_PARAMETER;
        GPU_BO *bo=c->Buffers+range.Handle-1;
        if(!bo->Mdl || !bo->Cached || !range.Size || range.Offset>=bo->Size || range.Size>bo->Size-range.Offset)return STATUS_INVALID_PARAMETER;
        PVOID virtualAddress=(PVOID)((ULONG_PTR)MmGetMdlVirtualAddress(bo->Mdl)+range.Offset);
        PMDL mdl=IoAllocateMdl(virtualAddress,range.Size,FALSE,FALSE,NULL);
        if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
        IoBuildPartialMdl(bo->Mdl,mdl,virtualAddress,range.Size);
        KeFlushIoBuffers(mdl,range.Readback!=0,TRUE);IoFreeMdl(mdl);status=STATUS_SUCCESS;
    } else if(code==GPU_IOCTL_FREE && inLength==sizeof(ULONG)) {
        ULONG id=*(PULONG)in;
        if(id && id<=RTL_NUMBER_OF(c->Buffers) && c->Buffers[id-1].Mdl && !c->Deferred.Buffers[id-1]) {
            MarkDeferred(g,c,id-1,FALSE);CollectClientDeferred(g,c);status=STATUS_SUCCESS;
        }
    }
    request->IoStatus.Information=bytes;return status;
}
static NTSTATUS Dispatch(PDEVICE_OBJECT device,PIRP request) {
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(request);
    if(device!=Bridge.Device)return Bridge.Previous[stack->MajorFunction](device,request);
    NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;request->IoStatus.Information=0;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)status=STATUS_INVALID_DEVICE_STATE;
    else {
        KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
        switch(stack->MajorFunction) {
        case IRP_MJ_CREATE:
            if(!Bridge.Adapter)status=STATUS_DEVICE_NOT_READY;
            else if(request->RequestorMode==KernelMode){stack->FileObject->FsContext=(PVOID)(ULONG_PTR)1;status=STATUS_SUCCESS;}
            else {
                A830_CLIENT *c=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*c),GPU_TAG);
                if(!c)status=STATUS_INSUFFICIENT_RESOURCES;
                else {
                    c->Owner=IoGetRequestorProcess(request);
                    if(!c->Owner){ExFreePoolWithTag(c,GPU_TAG);status=STATUS_INVALID_PARAMETER;}
                    else {
                        InitializeListHead(&c->Sparse);
                        ObReferenceObject(c->Owner);c->Lease=++Bridge.NextLease;KeInitializeEvent(&c->Drained,NotificationEvent,TRUE);KeInitializeSpinLock(&c->RefLock);
                        InsertTailList(&Bridge.Clients,&c->Entry);stack->FileObject->FsContext=c;status=STATUS_SUCCESS;
                    }
                }
            }
            break;
        case IRP_MJ_CLEANUP: {
            A830_CLIENT *c=stack->FileObject->FsContext;
            if(c && c!=(PVOID)(ULONG_PTR)1) {
                /* Closing the owner file revokes unsubmitted packets. Tokens
                   already transferred to Submit remain pinned and drain normally. */
                c->Draining=TRUE;KeReleaseMutex(&Bridge.Lock,FALSE);
                A830CancelRenderedClient(Bridge.Adapter,c->Owner,c->Lease);
                KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
                DrainClient(c);if(Bridge.Hardware)Cleanup(&Bridge,c);
                RemoveEntryList(&c->Entry);ExFreePoolWithTag(c,GPU_TAG);stack->FileObject->FsContext=NULL;
            }
            status=STATUS_SUCCESS;break;
        }
        case IRP_MJ_CLOSE:status=STATUS_SUCCESS;break;
        case IRP_MJ_DEVICE_CONTROL: {
            A830_CLIENT *c=stack->FileObject->FsContext;
            if(!Bridge.Adapter || !c || c==(PVOID)(ULONG_PTR)1 || !c->Owner || IoGetRequestorProcess(request)!=c->Owner)
                status=STATUS_DEVICE_NOT_READY;
            else status=Io(&Bridge,c,request,stack->Parameters.DeviceIoControl.IoControlCode,
                stack->Parameters.DeviceIoControl.InputBufferLength,stack->Parameters.DeviceIoControl.OutputBufferLength);
            break;
        }
        }
        KeReleaseMutex(&Bridge.Lock,FALSE);
    }
    request->IoStatus.Status=status;IoCompleteRequest(request,IO_NO_INCREMENT);return status;
}
VOID A830BridgeAttach(A830_ADAPTER *adapter) {
    if(!Bridge.Device)return;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    Bridge.Adapter=adapter;Bridge.Hardware=&adapter->Hardware;
    if(adapter->Hardware.Online)LoadPowerPolicy(&adapter->Hardware);
    adapter->BridgeAttached=TRUE;
    ResumeClock(&Bridge);
    KeReleaseMutex(&Bridge.Lock,FALSE);
}
VOID A830BridgeDetach(A830_ADAPTER *adapter) {
    if(!Bridge.Device)return;
    KeWaitForSingleObject(&Bridge.Lock,Executive,KernelMode,FALSE,NULL);
    if(Bridge.Adapter==adapter) {
        adapter->BridgeAttached=FALSE;
        PauseClock(&Bridge);
        for(PLIST_ENTRY entry=Bridge.Clients.Flink;entry!=&Bridge.Clients;entry=entry->Flink)
            Cleanup(&Bridge,CONTAINING_RECORD(entry,A830_CLIENT,Entry));
        Bridge.Adapter=NULL;Bridge.Hardware=NULL;
    }
    KeReleaseMutex(&Bridge.Lock,FALSE);
}
static NTSTATUS SetDeviceLabel(PDEVICE_OBJECT device) {
    struct { SYSTEM_MANDATORY_LABEL_ACE Ace; UCHAR Tail[SECURITY_MAX_SID_SIZE]; } entry={0};
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID sid=(PSID)&entry.Ace.SidStart;
    NTSTATUS status=RtlInitializeSid(sid,&authority,1);if(!NT_SUCCESS(status))return status;
    *RtlSubAuthoritySid(sid,0)=SECURITY_MANDATORY_LOW_RID;
    ULONG bytes=FIELD_OFFSET(SYSTEM_MANDATORY_LABEL_ACE,SidStart)+RtlLengthSid(sid);
    entry.Ace.Header.AceType=SYSTEM_MANDATORY_LABEL_ACE_TYPE;entry.Ace.Header.AceSize=(USHORT)bytes;
    entry.Ace.Mask=SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
    struct { ACL Header; UCHAR Entries[sizeof(entry)]; } sacl={0};
    status=RtlCreateAcl(&sacl.Header,sizeof(ACL)+bytes,ACL_REVISION);
    if(NT_SUCCESS(status))status=RtlAddAce(&sacl.Header,ACL_REVISION,MAXULONG,&entry.Ace,bytes);
    SECURITY_DESCRIPTOR descriptor;
    if(NT_SUCCESS(status))status=RtlCreateSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION);
    if(NT_SUCCESS(status)){descriptor.Control|=SE_SACL_PRESENT;descriptor.Sacl=&sacl.Header;}
    HANDLE handle=NULL;
    if(NT_SUCCESS(status))status=ObOpenObjectByPointer(device,OBJ_KERNEL_HANDLE,NULL,WRITE_OWNER,NULL,KernelMode,&handle);
    if(NT_SUCCESS(status)){status=ZwSetSecurityObject(handle,LABEL_SECURITY_INFORMATION,&descriptor);ZwClose(handle);}
    return status;
}
NTSTATUS A830BridgeInitialize(PDRIVER_OBJECT object) {
    KeInitializeMutex(&Bridge.Lock,0);
    InitializeListHead(&Bridge.Clients);
    InitializeListHead(&Bridge.Shared);
    ExInitializeRundownProtection(&Bridge.ClockAccess);
    KeInitializeEvent(&Bridge.GovernorStop,NotificationEvent,FALSE);
    UNICODE_STRING name,link,sddl;
    RtlInitUnicodeString(&name,L"\\Device\\QcomGpu");RtlInitUnicodeString(&link,L"\\DosDevices\\QcomGpu");
    RtlInitUnicodeString(&sddl,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;S-1-5-90-0)(A;;GRGWGX;;;AU)(A;;GRGWGX;;;RC)(A;;GRGWGX;;;S-1-15-2-1)(A;;GRGWGX;;;S-1-15-2-2)");
    static const GUID security={0xe4ce73d1,0x7f02,0x4f0f,{0xa8,0x30,0x89,0xbb,0x4c,0x3d,0x21,0x54}};

    NTSTATUS status=IoCreateDeviceSecure(object,0,&name,FILE_DEVICE_UNKNOWN,FILE_DEVICE_SECURE_OPEN|FILE_DEVICE_ALLOW_APPCONTAINER_TRAVERSAL,FALSE,&sddl,&security,&Bridge.Device);
    if(!NT_SUCCESS(status))return status;
    SetDeviceLabel(Bridge.Device);
    status=IoCreateSymbolicLink(&link,&name);if(!NT_SUCCESS(status)){IoDeleteDevice(Bridge.Device);Bridge.Device=NULL;return status;}
    ULONG functions[]={IRP_MJ_CREATE,IRP_MJ_CLOSE,IRP_MJ_CLEANUP,IRP_MJ_DEVICE_CONTROL};
    for(ULONG i=0;i<RTL_NUMBER_OF(functions);i++){ULONG index=functions[i];Bridge.Previous[index]=object->MajorFunction[index];object->MajorFunction[index]=Dispatch;}
    OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    status=PsCreateSystemThread(&Bridge.GovernorThread,SYNCHRONIZE,&attributes,NULL,NULL,Governor,&Bridge);
    if(!NT_SUCCESS(status)){IoDeleteSymbolicLink(&link);IoDeleteDevice(Bridge.Device);Bridge.Device=NULL;return status;}
    Bridge.Device->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
}
VOID A830BridgeRemove(VOID) {
    if(Bridge.GovernorThread){KeSetEvent(&Bridge.GovernorStop,IO_NO_INCREMENT,FALSE);ZwWaitForSingleObject(Bridge.GovernorThread,FALSE,NULL);ZwClose(Bridge.GovernorThread);Bridge.GovernorThread=NULL;}
    if(!Bridge.Device || Bridge.DeviceDeleted)return;
    UNICODE_STRING link;RtlInitUnicodeString(&link,L"\\DosDevices\\QcomGpu");IoDeleteSymbolicLink(&link);
    Bridge.DeviceDeleted=TRUE;IoDeleteDevice(Bridge.Device);
}
VOID A830BridgeDestroy(VOID) {
    A830BridgeRemove();Bridge.Device=NULL;
    if(Bridge.AddressBits){ExFreePoolWithTag(Bridge.AddressBits,GPU_TAG);Bridge.AddressBits=NULL;}
}
NTSTATUS A830Map(A830_ADAPTER *a,GPU_BO *buffer,ULONGLONG address) {
    return a->BridgeAttached?A830BridgeMap(buffer,address):GpuMap(&a->Hardware,0,buffer,address);
}
