#include "../wddm/adapter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static PVOID Pools[8192];
static ULONG PoolCount,MdlCount,MapCount,UnmapCount,FailMapCall,FailUnmap;
static LONG FailPoolAfter=-1,LockDepth;
static ULONG FailCpuMap;
static ULONGLONG SystemBytes=16ULL<<30;
static ULONG MaxMapBytes;
typedef struct { ULONGLONG Address;ULONG Size;PFN_NUMBER First; } MAP_RECORD;
static MAP_RECORD Records[128];
static PVOID NTAPI Pool(POOL_FLAGS flags,SIZE_T bytes,ULONG tag) {
    UNREFERENCED_PARAMETER(flags);UNREFERENCED_PARAMETER(tag);
    if(FailPoolAfter==0)return NULL;if(FailPoolAfter>0)FailPoolAfter--;
    PVOID p=calloc(1,bytes);assert(p);
    for(ULONG i=0;i<RTL_NUMBER_OF(Pools);i++)if(!Pools[i]){Pools[i]=p;PoolCount++;return p;}
    abort();
}
static VOID NTAPI FreePool(PVOID p,ULONG tag) {
    UNREFERENCED_PARAMETER(tag);
    for(ULONG i=0;i<RTL_NUMBER_OF(Pools);i++)if(Pools[i]==p){Pools[i]=NULL;PoolCount--;break;}
    free(p);
}
static VOID NTAPI FreePlain(PVOID p){FreePool(p,0);}
static PPHYSICAL_MEMORY_RANGE NTAPI PhysicalRanges(PVOID partition,ULONG flags) {
    UNREFERENCED_PARAMETER(partition);UNREFERENCED_PARAMETER(flags);
    PPHYSICAL_MEMORY_RANGE r=calloc(3,sizeof(*r));assert(r);
    r[0].BaseAddress.QuadPart=0x100000;r[0].NumberOfBytes.QuadPart=(LONGLONG)(SystemBytes/2);
    r[1].BaseAddress.QuadPart=(LONGLONG)(SystemBytes+0x100000);r[1].NumberOfBytes.QuadPart=(LONGLONG)(SystemBytes-SystemBytes/2);
    return r;
}
static PMDL NTAPI Mdl(PVOID address,ULONG bytes,BOOLEAN secondary,BOOLEAN quota,PIRP irp) {
    UNREFERENCED_PARAMETER(secondary);UNREFERENCED_PARAMETER(quota);UNREFERENCED_PARAMETER(irp);
    SIZE_T pages=ADDRESS_AND_SIZE_TO_SPAN_PAGES(address,bytes);
    PMDL mdl=calloc(1,sizeof(*mdl)+pages*sizeof(PFN_NUMBER));assert(mdl);
    mdl->Size=sizeof(*mdl);mdl->ByteCount=bytes;mdl->ByteOffset=(ULONG)((ULONG_PTR)address&(PAGE_SIZE-1));
    mdl->StartVa=(PVOID)((ULONG_PTR)address&~(ULONG_PTR)(PAGE_SIZE-1));MdlCount++;return mdl;
}
static VOID NTAPI FreeMdl(PMDL mdl){assert(MdlCount);MdlCount--;free(mdl);}
static PVOID NTAPI MapCpu(PMDL mdl,KPROCESSOR_MODE mode,MEMORY_CACHING_TYPE cache,PVOID address,ULONG bugcheck,ULONG priority) {
    UNREFERENCED_PARAMETER(mdl);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(cache);
    UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(bugcheck);UNREFERENCED_PARAMETER(priority);
    return FailCpuMap?NULL:malloc(1);
}
static VOID NTAPI UnmapCpu(PVOID p,PMDL mdl){UNREFERENCED_PARAMETER(mdl);free(p);}
static VOID NTAPI Mutex(PKMUTEX mutex,ULONG level){UNREFERENCED_PARAMETER(mutex);UNREFERENCED_PARAMETER(level);}
static NTSTATUS NTAPI Wait(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alert,PLARGE_INTEGER time) {
    UNREFERENCED_PARAMETER(object);UNREFERENCED_PARAMETER(reason);UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(alert);UNREFERENCED_PARAMETER(time);assert(LockDepth==0);LockDepth++;return STATUS_SUCCESS;
}
static LONG NTAPI Release(PKMUTEX mutex,BOOLEAN wait){UNREFERENCED_PARAMETER(mutex);UNREFERENCED_PARAMETER(wait);assert(LockDepth==1);LockDepth--;return 0;}
PVOID (NTAPI *__imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=Pool;
VOID (NTAPI *__imp_ExFreePoolWithTag)(PVOID,ULONG)=FreePool;
VOID (NTAPI *__imp_ExFreePool)(PVOID)=FreePlain;
PPHYSICAL_MEMORY_RANGE (NTAPI *__imp_MmGetPhysicalMemoryRangesEx2)(PVOID,ULONG)=PhysicalRanges;
PMDL (NTAPI *__imp_IoAllocateMdl)(PVOID,ULONG,BOOLEAN,BOOLEAN,PIRP)=Mdl;
VOID (NTAPI *__imp_IoFreeMdl)(PMDL)=FreeMdl;
PVOID (NTAPI *__imp_MmMapLockedPagesSpecifyCache)(PMDL,KPROCESSOR_MODE,MEMORY_CACHING_TYPE,PVOID,ULONG,ULONG)=MapCpu;
VOID (NTAPI *__imp_MmUnmapLockedPages)(PVOID,PMDL)=UnmapCpu;
VOID (NTAPI *__imp_KeInitializeMutex)(PKMUTEX,ULONG)=Mutex;
NTSTATUS (NTAPI *__imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=Wait;
LONG (NTAPI *__imp_KeReleaseMutex)(PKMUTEX,BOOLEAN)=Release;
static VOID NTAPI UnusedMdl(PMDL mdl){UNREFERENCED_PARAMETER(mdl);abort();}
static PVOID NTAPI UnusedContiguous(SIZE_T bytes,PHYSICAL_ADDRESS low,PHYSICAL_ADDRESS high,PHYSICAL_ADDRESS boundary,MEMORY_CACHING_TYPE cache) {
    UNREFERENCED_PARAMETER(bytes);UNREFERENCED_PARAMETER(low);UNREFERENCED_PARAMETER(high);UNREFERENCED_PARAMETER(boundary);UNREFERENCED_PARAMETER(cache);abort();
}
static VOID NTAPI UnusedFree(PVOID p){UNREFERENCED_PARAMETER(p);abort();}
static PHYSICAL_ADDRESS NTAPI UnusedPhysical(PVOID p){UNREFERENCED_PARAMETER(p);abort();}
static PEPROCESS NTAPI UnusedProcess(VOID){abort();}
VOID (NTAPI *__imp_MmBuildMdlForNonPagedPool)(PMDL)=UnusedMdl;
PVOID (NTAPI *__imp_MmAllocateContiguousMemorySpecifyCache)(SIZE_T,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,MEMORY_CACHING_TYPE)=UnusedContiguous;
VOID (NTAPI *__imp_MmFreeContiguousMemory)(PVOID)=UnusedFree;
PHYSICAL_ADDRESS (NTAPI *__imp_MmGetPhysicalAddress)(PVOID)=UnusedPhysical;
PEPROCESS (NTAPI *__imp_IoGetCurrentProcess)(VOID)=UnusedProcess;
NTSTATUS GpuAllocate(GPU_BO *b,ULONG bytes){UNREFERENCED_PARAMETER(b);UNREFERENCED_PARAMETER(bytes);abort();}
VOID GpuFree(GPU_BO *b){UNREFERENCED_PARAMETER(b);abort();}
NTSTATUS A830SharedCreate(A830_ALLOCATION *a){UNREFERENCED_PARAMETER(a);abort();}
VOID A830SharedDestroy(A830_ALLOCATION *a){UNREFERENCED_PARAMETER(a);abort();}
NTSTATUS A830ScanoutStart(PVOID a){UNREFERENCED_PARAMETER(a);abort();}
VOID A830ScanoutStop(PVOID a){UNREFERENCED_PARAMETER(a);abort();}
NTSTATUS A830Map(A830_ADAPTER *a,GPU_BO *b,ULONGLONG address) {
    UNREFERENCED_PARAMETER(a);assert(LockDepth==1 && A830KernelRange(address,b->Size));
    assert(b->Mdl && b->Size && b->Size<=GPU_BUFFER_LIMIT && MapCount<RTL_NUMBER_OF(Records));
    Records[MapCount].Address=address;Records[MapCount].Size=b->Size;Records[MapCount].First=MmGetMdlPfnArray(b->Mdl)[0];
    MapCount++;if(b->Size>MaxMapBytes)MaxMapBytes=b->Size;
    return MapCount==FailMapCall?STATUS_DEVICE_HARDWARE_ERROR:STATUS_SUCCESS;
}
NTSTATUS A830BridgeUnmap(GPU_BO *b) {
    assert(LockDepth==1 && A830KernelRange(b->Address,b->Size));UnmapCount++;
    return FailUnmap?STATUS_DEVICE_HARDWARE_ERROR:STATUS_SUCCESS;
}
VOID GpuUnmap(GPU *g,ULONG vm,GPU_BO *b){UNREFERENCED_PARAMETER(g);UNREFERENCED_PARAMETER(vm);assert(NT_SUCCESS(A830BridgeUnmap(b)));}
static NTSTATUS Paging(A830_ADAPTER *a,PMDL source,SIZE_T page,SIZE_T count,ULONG offset) {
    A830_DMA dma;ULONG command;DXGKARG_BUILDPAGINGBUFFER p={0};
    p.pDmaBuffer=&command;p.DmaSize=sizeof(command);p.pDmaBufferPrivateData=&dma;p.DmaBufferPrivateDataSize=sizeof(dma);
    if(source){p.Operation=DXGK_OPERATION_MAP_APERTURE_SEGMENT;p.MapApertureSegment.SegmentId=2;
        p.MapApertureSegment.pMdl=source;p.MapApertureSegment.OffsetInPages=page;
        p.MapApertureSegment.NumberOfPages=count;p.MapApertureSegment.MdlOffset=offset;}
    else {p.Operation=DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;p.UnmapApertureSegment.SegmentId=2;
        p.UnmapApertureSegment.OffsetInPages=page;p.UnmapApertureSegment.NumberOfPages=count;}
    return A830Paging(a,&p);
}
int main(void) {
    const ULONGLONG mb=1ULL<<20,gb=1ULL<<30;
    assert(A830_SHARED_BASE>=0xc0000000ULL+2*0x04000000ULL);
    assert(A830_SHARED_END<=GPU_CLIENT_VA_BASE && GPU_CLIENT_VA_END<=A830_SHARED_ADDRESS);
    assert(A830_SHARED_END>A830_SHARED_BASE && A830_SHARED_END-A830_SHARED_BASE>=GPU_MAX_BUFFER_SIZE);
    assert(A830SharedBudget(128*mb,64*mb)==0);
    assert(A830SharedBudget(gb,64*mb)==448*mb);
    assert(A830SharedBudget(4*gb,64*mb)==1984*mb);
    assert(A830SharedBudget(8*gb,64*mb)==4032*mb);
    assert(A830SharedBudget(12*gb,64*mb)==GPU_VA_END-A830_SHARED_ADDRESS);
    assert(A830SharedBudget(~0ULL,64*mb)==GPU_VA_END-A830_SHARED_ADDRESS);
    A830_ADAPTER *a=calloc(1,sizeof(*a));assert(a);a->BridgeAttached=TRUE;
    FailPoolAfter=0;assert(A830SharedStart(a)==STATUS_INSUFFICIENT_RESOURCES && !PoolCount && !a->Memory.Shared.Bytes);
    FailPoolAfter=-1;assert(A830SharedStart(a)==STATUS_SUCCESS && PoolCount==1);
    assert(a->Memory.Shared.Bytes==4*gb && A830SharedStart(a)==STATUS_DEVICE_BUSY);
    PMDL source=Mdl((PVOID)7,6*PAGE_SIZE,FALSE,FALSE,NULL);
    for(ULONG i=0;i<7;i++)MmGetMdlPfnArray(source)[i]=i*3;
    SIZE_T last=a->Memory.Shared.PageCount-1;
    assert(Paging(a,source,last,2,0)==STATUS_INVALID_PARAMETER && !MapCount);
    assert(Paging(a,source,last,1,7)==STATUS_INVALID_PARAMETER && !MapCount);
    assert(Paging(a,source,255,3,1)==STATUS_SUCCESS && PoolCount==3 && Records[0].First==3);
    assert(Paging(a,source,256,1,0)==STATUS_CONFLICTING_ADDRESSES && MapCount==1);
    PVOID cpu=NULL;PMDL view=NULL;
    assert(A830MapSharedCpu(a,A830_SHARED_ADDRESS+255*PAGE_SIZE+19,2*PAGE_SIZE,&cpu,&view)==STATUS_SUCCESS);
    assert(view->ByteOffset==19 && MmGetMdlPfnArray(view)[0]==3 && MmGetMdlPfnArray(view)[2]==9);
    UnmapCpu(cpu,view);FreeMdl(view);
    assert(A830SharedRestore(a)==STATUS_SUCCESS && MapCount==2 && Records[1].First==3);
    assert(Paging(a,NULL,255,3,0)==STATUS_SUCCESS && PoolCount==1);
    assert(A830MapSharedCpu(a,255*PAGE_SIZE,PAGE_SIZE,&cpu,&view)==STATUS_DEVICE_NOT_READY && !view);
    assert(Paging(a,source,last,1,0)==STATUS_SUCCESS && Records[MapCount-1].Address>MAXULONG);
    assert(A830MapSharedCpu(a,A830_SHARED_ADDRESS+last*PAGE_SIZE,PAGE_SIZE,&cpu,&view)==STATUS_SUCCESS);
    assert(MmGetMdlPfnArray(view)[0]==0);UnmapCpu(cpu,view);FreeMdl(view);
    FailCpuMap=1;assert(A830MapSharedCpu(a,last*PAGE_SIZE,PAGE_SIZE,&cpu,&view)==STATUS_INSUFFICIENT_RESOURCES && !view);FailCpuMap=0;
    FailUnmap=1;assert(Paging(a,NULL,last,1,0)==STATUS_DEVICE_HARDWARE_ERROR && a->Failed && PoolCount==2);
    assert(A830MapSharedCpu(a,last*PAGE_SIZE,PAGE_SIZE,&cpu,&view)==STATUS_SUCCESS);
    UnmapCpu(cpu,view);FreeMdl(view);FailUnmap=0;a->Failed=0;
    assert(Paging(a,NULL,last,1,0)==STATUS_SUCCESS && PoolCount==1);
    FailPoolAfter=1;assert(Paging(a,source,255,3,0)==STATUS_INSUFFICIENT_RESOURCES && PoolCount==1);FailPoolAfter=-1;
    FailMapCall=MapCount+1;assert(Paging(a,source,255,3,0)==STATUS_DEVICE_HARDWARE_ERROR && PoolCount==1);FailMapCall=0;
    PMDL large=Mdl(NULL,GPU_BUFFER_LIMIT+2*PAGE_SIZE,FALSE,FALSE,NULL);
    SIZE_T largePages=MmGetMdlByteCount(large)/PAGE_SIZE;
    for(SIZE_T i=0;i<largePages;i++)MmGetMdlPfnArray(large)[i]=(PFN_NUMBER)(i+10);
    ULONG oldUnmaps=UnmapCount;FailMapCall=MapCount+2;
    assert(Paging(a,large,0,largePages,0)==STATUS_DEVICE_HARDWARE_ERROR && UnmapCount==oldUnmaps+2 && PoolCount==1);
    FailMapCall=0;assert(Paging(a,large,0,largePages,0)==STATUS_SUCCESS && MaxMapBytes==GPU_BUFFER_LIMIT);
    assert(Paging(a,NULL,0,largePages,0)==STATUS_SUCCESS && PoolCount==1);
    FreeMdl(large);
    PMDL full=Mdl((PVOID)1,MAXULONG-(PAGE_SIZE-1),FALSE,FALSE,NULL);
    for(SIZE_T i=0;i<a->Memory.Shared.PageCount;i++)MmGetMdlPfnArray(full)[i]=(PFN_NUMBER)(i+20);
    ULONG fullStart=MapCount;oldUnmaps=UnmapCount;
    assert(Paging(a,full,0,a->Memory.Shared.PageCount,0)==STATUS_SUCCESS && MapCount==fullStart+8);
    assert(Records[MapCount-1].Address+Records[MapCount-1].Size==GPU_VA_END);
    assert(Paging(a,NULL,0,a->Memory.Shared.PageCount,0)==STATUS_SUCCESS && UnmapCount==oldUnmaps+8 && PoolCount==1);
    FreeMdl(full);FreeMdl(source);A830SharedStop(a);A830SharedStop(a);
    assert(!PoolCount && !MdlCount && !LockDepth && !a->Memory.Shared.Bytes && !a->Memory.Shared.Blocks);
    SystemBytes=gb;assert(A830SharedStart(a)==STATUS_SUCCESS && a->Memory.Shared.Bytes==448*mb);
    A830SharedStop(a);assert(!PoolCount);free(a);
    puts("Shared memory: system budget, disjoint 64-bit VA, sparse PFNs, MDL offsets, PFN0, reset restore, OOM rollback, chunked maps, cleanup PASS");
    return 0;
}
