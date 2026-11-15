#include "../wddm/adapter.h"
#include <stdlib.h>
#include <stdio.h>

static PMDL NTAPI UnexpectedAllocate(PVOID address,ULONG length,BOOLEAN secondary,BOOLEAN quota,PIRP irp) {
    UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(length);UNREFERENCED_PARAMETER(secondary);
    UNREFERENCED_PARAMETER(quota);UNREFERENCED_PARAMETER(irp);abort();
}
static PVOID NTAPI UnexpectedMap(PMDL mdl,KPROCESSOR_MODE mode,MEMORY_CACHING_TYPE cache,PVOID address,ULONG bugcheck,ULONG priority) {
    UNREFERENCED_PARAMETER(mdl);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(cache);
    UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(bugcheck);UNREFERENCED_PARAMETER(priority);abort();
}
static VOID NTAPI UnexpectedUnmap(PVOID address,PMDL mdl){UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(mdl);abort();}
static VOID NTAPI UnexpectedFree(PMDL mdl){UNREFERENCED_PARAMETER(mdl);abort();}
PMDL (NTAPI *__imp_IoAllocateMdl)(PVOID,ULONG,BOOLEAN,BOOLEAN,PIRP)=UnexpectedAllocate;
PVOID (NTAPI *__imp_MmMapLockedPagesSpecifyCache)(PMDL,KPROCESSOR_MODE,MEMORY_CACHING_TYPE,PVOID,ULONG,ULONG)=UnexpectedMap;
VOID (NTAPI *__imp_MmUnmapLockedPages)(PVOID,PMDL)=UnexpectedUnmap;
VOID (NTAPI *__imp_IoFreeMdl)(PMDL)=UnexpectedFree;
static PVOID NTAPI UnexpectedPool(POOL_FLAGS flags,SIZE_T size,ULONG tag){UNREFERENCED_PARAMETER(flags);UNREFERENCED_PARAMETER(tag);return calloc(1,size);}
static VOID NTAPI UnexpectedPoolFree(PVOID address,ULONG tag){UNREFERENCED_PARAMETER(tag);free(address);}
static VOID NTAPI UnexpectedContiguousFree(PVOID address){UNREFERENCED_PARAMETER(address);abort();}
static PHYSICAL_ADDRESS NTAPI UnexpectedPhysical(PVOID address){UNREFERENCED_PARAMETER(address);abort();}
static PVOID NTAPI UnexpectedContiguous(SIZE_T size,PHYSICAL_ADDRESS low,PHYSICAL_ADDRESS high,PHYSICAL_ADDRESS boundary,MEMORY_CACHING_TYPE cache) {
    UNREFERENCED_PARAMETER(size);UNREFERENCED_PARAMETER(low);UNREFERENCED_PARAMETER(high);UNREFERENCED_PARAMETER(boundary);UNREFERENCED_PARAMETER(cache);abort();
}
PVOID (NTAPI *__imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=UnexpectedPool;
VOID (NTAPI *__imp_ExFreePoolWithTag)(PVOID,ULONG)=UnexpectedPoolFree;
VOID (NTAPI *__imp_MmBuildMdlForNonPagedPool)(PMDL)=UnexpectedFree;
PVOID (NTAPI *__imp_MmAllocateContiguousMemorySpecifyCache)(SIZE_T,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,MEMORY_CACHING_TYPE)=UnexpectedContiguous;
VOID (NTAPI *__imp_MmFreeContiguousMemory)(PVOID)=UnexpectedContiguousFree;
PHYSICAL_ADDRESS (NTAPI *__imp_MmGetPhysicalAddress)(PVOID)=UnexpectedPhysical;
NTSTATUS GpuAllocate(GPU_BO *bo,ULONG size){UNREFERENCED_PARAMETER(bo);UNREFERENCED_PARAMETER(size);abort();}
VOID GpuFree(GPU_BO *bo){UNREFERENCED_PARAMETER(bo);abort();}
NTSTATUS A830SharedCreate(A830_ALLOCATION *allocation){UNREFERENCED_PARAMETER(allocation);return STATUS_INSUFFICIENT_RESOURCES;}
VOID A830SharedDestroy(A830_ALLOCATION *allocation){UNREFERENCED_PARAMETER(allocation);abort();}
NTSTATUS A830ScanoutStart(PVOID adapter){UNREFERENCED_PARAMETER(adapter);abort();}
VOID A830ScanoutStop(PVOID adapter){UNREFERENCED_PARAMETER(adapter);abort();}
static PEPROCESS NTAPI UnexpectedProcess(VOID){abort();}
PEPROCESS (NTAPI *__imp_IoGetCurrentProcess)(VOID)=UnexpectedProcess;
NTSTATUS A830Map(A830_ADAPTER *a,GPU_BO *bo,ULONGLONG address){UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(bo);UNREFERENCED_PARAMETER(address);abort();}
NTSTATUS A830BridgeUnmap(GPU_BO *bo){UNREFERENCED_PARAMETER(bo);abort();}
VOID GpuUnmap(GPU *g,ULONG vm,GPU_BO *bo){UNREFERENCED_PARAMETER(g);UNREFERENCED_PARAMETER(vm);UNREFERENCED_PARAMETER(bo);abort();}
VOID A830Record(PCWSTR name,ULONG value){UNREFERENCED_PARAMETER(name);UNREFERENCED_PARAMETER(value);}
NTSTATUS A830SharedStart(A830_ADAPTER *a){UNREFERENCED_PARAMETER(a);abort();}
VOID A830SharedStop(A830_ADAPTER *a){UNREFERENCED_PARAMETER(a);abort();}
NTSTATUS A830SharedMap(A830_ADAPTER *a,SIZE_T page,SIZE_T count,PMDL mdl,ULONG offset) {
    UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(page);UNREFERENCED_PARAMETER(count);UNREFERENCED_PARAMETER(mdl);UNREFERENCED_PARAMETER(offset);abort();
}
NTSTATUS A830SharedUnmap(A830_ADAPTER *a,SIZE_T page,SIZE_T count) {
    UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(page);UNREFERENCED_PARAMETER(count);abort();
}
NTSTATUS A830MapSharedCpu(A830_ADAPTER *a,ULONGLONG address,SIZE_T bytes,PVOID *map,PMDL *mdl) {
    UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(bytes);UNREFERENCED_PARAMETER(map);UNREFERENCED_PARAMETER(mdl);abort();
}

int main(void) {
    A830_ADAPTER *a=calloc(1,sizeof(*a));if(!a)return 1;
    a->Memory.Shared.Bytes=A830SharedBudget(16ULL<<30,A830_SEGMENT_BYTES);
    a->Memory.Segment.Cpu=malloc(0x10000);PUCHAR system=malloc(0x4000);if(!a->Memory.Segment.Cpu || !system)return 1;
    memset(a->Memory.Segment.Cpu,0xA5,0x10000);memset(system,0xCC,0x4000);
    MDL mdl={0};mdl.MdlFlags=MDL_MAPPED_TO_SYSTEM_VA;mdl.MappedSystemVa=system;mdl.ByteCount=0x4000;
    A830_DMA dma;ULONG command;DXGKARG_BUILDPAGINGBUFFER p={0};
    p.Operation=DXGK_OPERATION_TRANSFER;p.DmaSize=sizeof(command);p.DmaBufferPrivateDataSize=sizeof(dma);
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    p.Transfer.TransferOffset=0x2000;p.Transfer.TransferSize=0x1000;p.Transfer.MdlOffset=1;
    p.Transfer.Source.pMdl=&mdl;p.Transfer.Destination.SegmentId=1;p.Transfer.Destination.SegmentAddress.QuadPart=A830_SEGMENT_ADDRESS;
    if(A830Paging(a,&p)!=STATUS_SUCCESS || dma.Operation!=3 || dma.Source!=system+0x1000 ||
        dma.Destination!=a->Memory.Segment.Cpu+0x2000 || dma.Bytes!=0x1000)return 1;
    memmove(dma.Destination,dma.Source,dma.Bytes);
    for(ULONG i=0;i<0x10000;i++)if(a->Memory.Segment.Cpu[i]!=(i>=0x2000 && i<0x3000?0xCC:0xA5))return 1;
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    p.Transfer.Source.SegmentId=1;p.Transfer.Source.SegmentAddress.QuadPart=A830_SEGMENT_ADDRESS+0x1000;
    p.Transfer.Destination.SegmentId=0;p.Transfer.Destination.pMdl=&mdl;
    if(A830Paging(a,&p)!=STATUS_SUCCESS || dma.Source!=a->Memory.Segment.Cpu+0x3000 || dma.Destination!=system+0x1000)return 1;
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;p.Transfer.Source.SegmentAddress.QuadPart=-1;
    if(A830Paging(a,&p)!=STATUS_INVALID_PARAMETER)return 1;
    for(ULONG flags=0;flags<2;flags++) {
        A830_RESOURCE resource={A830_ABI_VERSION,4096,16,16,64,D3DDDIFMT_A8R8G8B8,flags};
        DXGK_ALLOCATIONINFO info={0};info.pPrivateDriverData=&resource;info.PrivateDriverDataSize=sizeof(resource);
        DXGKARG_CREATEALLOCATION create={0};create.NumAllocations=1;create.pAllocationInfo=&info;
        if(A830CreateAllocation(a,&create)!=STATUS_SUCCESS || !info.FlagsWddm2.CpuVisible ||
           !info.FlagsWddm2.AccessedPhysically || info.FlagsWddm2.Cached!=flags || info.PreferredSegment.SegmentId0!=(flags?2u:1u))return 1;
        DXGKARG_DESTROYALLOCATION destroy={0};destroy.NumAllocations=1;destroy.pAllocationList=&info.hAllocation;
        if(A830DestroyAllocation(a,&destroy)!=STATUS_SUCCESS)return 1;
    }
    {
        A830_RESOURCE resource={A830_ABI_VERSION,A830_SEGMENT_BYTES+PAGE_SIZE,16,16,64,D3DDDIFMT_A8R8G8B8,1};
        DXGK_ALLOCATIONINFO info={0};info.pPrivateDriverData=&resource;info.PrivateDriverDataSize=sizeof(resource);
        DXGKARG_CREATEALLOCATION create={0};create.NumAllocations=1;create.pAllocationInfo=&info;
        if(A830CreateAllocation(a,&create)!=STATUS_SUCCESS || info.PreferredSegment.SegmentId0!=2)return 1;
        DXGKARG_DESTROYALLOCATION destroy={0};destroy.NumAllocations=1;destroy.pAllocationList=&info.hAllocation;
        if(A830DestroyAllocation(a,&destroy)!=STATUS_SUCCESS)return 1;
        resource.Flags=0;info.hAllocation=NULL;
        if(A830CreateAllocation(a,&create)!=STATUS_INVALID_PARAMETER || info.hAllocation)return 1;
    }
    {
        A830_RESOURCE resource={A830_ABI_VERSION,4096,16,16,64,D3DDDIFMT_A8R8G8B8,A830_RESOURCE_OWNED};
        DXGK_ALLOCATIONINFO info={0};info.pPrivateDriverData=&resource;info.PrivateDriverDataSize=sizeof(resource);
        DXGKARG_CREATEALLOCATION create={0};create.Flags.Resource=1;create.NumAllocations=1;create.pAllocationInfo=&info;
        if(A830CreateAllocation(a,&create)!=STATUS_INSUFFICIENT_RESOURCES || info.hAllocation || create.hResource)return 1;
    }
    free(system);free(a->Memory.Segment.Cpu);free(a);
    puts("{\"paging_transfer_offset\":\"PASS\",\"mdl_page_offset\":\"PASS\",\"untouched_pages\":\"PASS\",\"address_overflow\":\"PASS\"}");return 0;
}
