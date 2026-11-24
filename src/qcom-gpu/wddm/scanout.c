#include "adapter.h"
#include "scanout.h"

/* Reserve each scanout independently: a single large contiguous reservation
 * is unnecessary. Exhaustion falls back to an ordinary BO in SharedCreate. */
#define SCANOUT_COUNT 9
typedef struct { GPU_BO Buffer;BOOLEAN Used; } SCANOUT_SLOT;
typedef struct { FAST_MUTEX Lock;BOOLEAN Closing;SCANOUT_SLOT Slots[SCANOUT_COUNT];GPU_BO Staging[2]; } SCANOUT_POOL;
static NTSTATUS AllocateLow(GPU_BO *b,ULONG size) {
    PHYSICAL_ADDRESS low={0},high,skip={0};high.QuadPart=MAXULONG;
    if(!size || size>GPU_BUFFER_LIMIT || (size&(PAGE_SIZE-1)))return STATUS_INVALID_PARAMETER;
    b->Cpu=MmAllocateContiguousMemorySpecifyCache(size,low,high,skip,MmWriteCombined);
    if(!b->Cpu)return STATUS_INSUFFICIENT_RESOURCES;
    b->Mdl=IoAllocateMdl(b->Cpu,size,FALSE,FALSE,NULL);
    if(!b->Mdl){MmFreeContiguousMemory(b->Cpu);RtlZeroMemory(b,sizeof(*b));return STATUS_INSUFFICIENT_RESOURCES;}
    MmBuildMdlForNonPagedPool(b->Mdl);b->Physical=MmGetPhysicalAddress(b->Cpu).QuadPart;
    b->Size=size;b->Contiguous=TRUE;
    if(!b->Physical || b->Physical>MAXULONG || size-1>MAXULONG-b->Physical){GpuFree(b);return STATUS_INVALID_ADDRESS;}
    RtlZeroMemory(b->Cpu,b->Size);return STATUS_SUCCESS;
}
BOOLEAN A830ScanoutStagingReady(PVOID context) {
    A830_ADAPTER *a=context;SCANOUT_POOL *pool=a->ScanoutPool;
    return pool && !pool->Closing && pool->Staging[0].Mdl && pool->Staging[1].Mdl;
}
NTSTATUS A830ScanoutStaging(PVOID context,ULONG index,GPU_BO *buffer) {
    A830_ADAPTER *a=context;SCANOUT_POOL *pool=a->ScanoutPool;
    if(index>=2 || !buffer || !A830ScanoutStagingReady(a))return STATUS_DEVICE_NOT_READY;
    *buffer=pool->Staging[index];return STATUS_SUCCESS;
}
NTSTATUS A830ScanoutStart(PVOID context) {
    A830_ADAPTER *a=context;
    if(a->ScanoutPool)return A830ScanoutStagingReady(a)?STATUS_SUCCESS:STATUS_DEVICE_BUSY;
    SCANOUT_POOL *pool=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*pool),GPU_TAG);
    if(!pool)return STATUS_SUCCESS;
    ExInitializeFastMutex(&pool->Lock);
    ULONGLONG size=(ULONGLONG)((a->DisplayInfo.Width*4+255)&~255u)*((a->DisplayInfo.Height+63)&~63u);
    if(!size || size>GPU_BUFFER_LIMIT){ExFreePoolWithTag(pool,GPU_TAG);return STATUS_INVALID_PARAMETER;}
    /* Reserve the fallback first. Never enable DPU if a later allocation
       failure could leave it without a GPU-only, double-buffered escape path. */
    for(ULONG i=0;i<2;i++)if(!NT_SUCCESS(AllocateLow(pool->Staging+i,(ULONG)size))) {
        for(ULONG j=0;j<2;j++)GpuFree(pool->Staging+j);
        ExFreePoolWithTag(pool,GPU_TAG);return STATUS_SUCCESS;
    }
    for(ULONG i=0;i<SCANOUT_COUNT;i++)if(!NT_SUCCESS(AllocateLow(&pool->Slots[i].Buffer,(ULONG)size)))break;
    a->ScanoutPool=pool;return STATUS_SUCCESS;
}
NTSTATUS A830ScanoutAllocate(PVOID context,GPU_BO *buffer,ULONG bytes) {
    A830_ADAPTER *a=context;SCANOUT_POOL *pool=a->ScanoutPool;
    if(!pool)return AllocateLow(buffer,(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1));
    NTSTATUS status=STATUS_INSUFFICIENT_RESOURCES;ExAcquireFastMutex(&pool->Lock);
    if(!pool->Closing)for(ULONG i=0;i<SCANOUT_COUNT;i++) {
        SCANOUT_SLOT *slot=pool->Slots+i;
        if(!slot->Used && slot->Buffer.Mdl && bytes<=slot->Buffer.Size){slot->Used=TRUE;*buffer=slot->Buffer;status=STATUS_SUCCESS;break;}
    }
    BOOLEAN closing=pool->Closing;ExReleaseFastMutex(&pool->Lock);
    /* Pool exhaustion must not fail a DWM primary. A separately allocated
       low contiguous BO can still scan directly; SharedCreate retains its
       ordinary BO fallback if physical contiguous allocation also fails. */
    if(!NT_SUCCESS(status) && !closing)status=AllocateLow(buffer,(bytes+PAGE_SIZE-1)&~(PAGE_SIZE-1));
    return status;
}
BOOLEAN A830ScanoutFree(PVOID context,GPU_BO *buffer) {
    A830_ADAPTER *a=context;SCANOUT_POOL *pool=a?a->ScanoutPool:NULL;
    if(!pool)return FALSE;
    BOOLEAN found=FALSE,used=FALSE;ExAcquireFastMutex(&pool->Lock);
    for(ULONG i=0;i<SCANOUT_COUNT;i++) {
        SCANOUT_SLOT *slot=pool->Slots+i;
        if(slot->Buffer.Mdl && buffer->Mdl==slot->Buffer.Mdl){
            slot->Used=FALSE;found=TRUE;RtlZeroMemory(buffer,sizeof(*buffer));
            if(pool->Closing)GpuFree(&slot->Buffer);
        }
        used|=slot->Used;
    }
    BOOLEAN destroy=pool->Closing && !used;
    ExReleaseFastMutex(&pool->Lock);
    if(destroy){for(ULONG i=0;i<2;i++)GpuFree(pool->Staging+i);a->ScanoutPool=NULL;ExFreePoolWithTag(pool,GPU_TAG);}
    return found;
}
VOID A830ScanoutStop(PVOID context) {
    A830_ADAPTER *a=context;SCANOUT_POOL *pool=a->ScanoutPool;if(!pool)return;
    BOOLEAN used=FALSE;ExAcquireFastMutex(&pool->Lock);pool->Closing=TRUE;
    for(ULONG i=0;i<SCANOUT_COUNT;i++)if(!pool->Slots[i].Used)GpuFree(&pool->Slots[i].Buffer);else used=TRUE;
    ExReleaseFastMutex(&pool->Lock);
    if(!used){for(ULONG i=0;i<2;i++)GpuFree(pool->Staging+i);a->ScanoutPool=NULL;ExFreePoolWithTag(pool,GPU_TAG);}
}
