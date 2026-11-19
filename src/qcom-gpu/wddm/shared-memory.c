#include "adapter.h"

#define SHARED_BLOCK_PAGES 256u
#define UNMAPPED_PFN ((PFN_NUMBER)-1)
struct A830_SHARED_BLOCK { PFN_NUMBER Pfns[SHARED_BLOCK_PAGES]; ULONG Used; };

static BOOLEAN Range(const A830_SHARED_MEMORY *s,SIZE_T page,SIZE_T count) {
    return s->Blocks && count && page<s->PageCount && count<=s->PageCount-page;
}
static PFN_NUMBER Pfn(const A830_SHARED_MEMORY *s,SIZE_T page) {
    A830_SHARED_BLOCK *b=s->Blocks[page/SHARED_BLOCK_PAGES];
    return b?b->Pfns[page%SHARED_BLOCK_PAGES]:UNMAPPED_PFN;
}
NTSTATUS A830PhysicalAddress(A830_ADAPTER *a,ULONG segment,ULONGLONG offset,PULONGLONG physical) {
    if(offset&4095)return STATUS_INVALID_PARAMETER;
    if(!segment){*physical=offset;return STATUS_SUCCESS;}
    if(segment==1) {
        if(!a->Memory.Segment.Mdl || offset>=a->Memory.Segment.Size)return STATUS_INVALID_PARAMETER;
        *physical=a->Memory.Segment.Physical+offset;return STATUS_SUCCESS;
    }
    A830_SHARED_MEMORY *s=&a->Memory.Shared;
    if(segment!=2 || !s->Blocks || offset>=s->Bytes)return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&s->Lock,Executive,KernelMode,FALSE,NULL);
    PFN_NUMBER pfn=s->Blocks && offset<s->Bytes?Pfn(s,(SIZE_T)(offset>>12)):UNMAPPED_PFN;
    KeReleaseMutex(&s->Lock,FALSE);
    if(pfn==UNMAPPED_PFN)return STATUS_INVALID_ADDRESS;
    *physical=(ULONGLONG)pfn<<12;return STATUS_SUCCESS;
}
static VOID DropEmpty(A830_SHARED_MEMORY *s,SIZE_T page,SIZE_T count) {
    SIZE_T first=page/SHARED_BLOCK_PAGES,last=(page+count-1)/SHARED_BLOCK_PAGES;
    for(SIZE_T i=first;i<=last;i++)if(s->Blocks[i] && !s->Blocks[i]->Used) {
        ExFreePoolWithTag(s->Blocks[i],GPU_TAG);s->Blocks[i]=NULL;
    }
}
static NTSTATUS UnmapPages(A830_ADAPTER *a,SIZE_T page,SIZE_T count) {
    while(count) {
        SIZE_T chunk=min(count,(SIZE_T)(GPU_BUFFER_LIMIT/PAGE_SIZE));
        GPU_BO b={0};b.Size=(ULONG)(chunk*PAGE_SIZE);b.Address=A830_SHARED_ADDRESS+(ULONGLONG)page*PAGE_SIZE;
        NTSTATUS status=STATUS_SUCCESS;
        if(a->BridgeAttached)status=A830BridgeUnmap(&b);
        else GpuUnmap(&a->Hardware,0,&b);
        if(!NT_SUCCESS(status))return status;
        page+=chunk;count-=chunk;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS MapPages(A830_ADAPTER *a,SIZE_T page,SIZE_T count,const PFN_NUMBER *source,SIZE_T *attempted) {
    SIZE_T done=0;*attempted=0;
    while(done<count) {
        SIZE_T chunk=min(count-done,(SIZE_T)(GPU_BUFFER_LIMIT/PAGE_SIZE));
        ULONG bytes=(ULONG)(chunk*PAGE_SIZE);
        PMDL mdl=IoAllocateMdl(NULL,bytes,FALSE,FALSE,NULL);
        if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
        PPFN_NUMBER target=MmGetMdlPfnArray(mdl);
        if(source)RtlCopyMemory(target,source+done,chunk*sizeof(*target));
        else for(SIZE_T i=0;i<chunk;i++)target[i]=Pfn(&a->Memory.Shared,page+done+i);
        mdl->MdlFlags|=MDL_PAGES_LOCKED;
        GPU_BO b={0};b.Mdl=mdl;b.Size=bytes;
        *attempted=done+chunk;
        NTSTATUS status=A830Map(a,&b,A830_SHARED_ADDRESS+(ULONGLONG)(page+done)*PAGE_SIZE);
        IoFreeMdl(mdl);if(!NT_SUCCESS(status))return status;
        done+=chunk;
    }
    return STATUS_SUCCESS;
}
NTSTATUS A830SharedStart(A830_ADAPTER *a) {
    A830_SHARED_MEMORY *s=&a->Memory.Shared;
    if(s->Blocks)return STATUS_DEVICE_BUSY;

    PPHYSICAL_MEMORY_RANGE ranges=MmGetPhysicalMemoryRangesEx2(NULL,0);
    if(!ranges)return STATUS_INSUFFICIENT_RESOURCES;
    ULONGLONG total=0;NTSTATUS status=STATUS_SUCCESS;
    for(PPHYSICAL_MEMORY_RANGE r=ranges;r->NumberOfBytes.QuadPart;r++) {
        if(r->NumberOfBytes.QuadPart<0 || (ULONGLONG)r->NumberOfBytes.QuadPart>~0ULL-total) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        total+=(ULONGLONG)r->NumberOfBytes.QuadPart;
    }
    ExFreePool(ranges);if(!NT_SUCCESS(status))return status;
    ULONGLONG bytes=A830SharedBudget(total,A830_SEGMENT_BYTES);
    if(!bytes)return STATUS_INSUFFICIENT_RESOURCES;
    SIZE_T pages=(SIZE_T)(bytes/PAGE_SIZE),blocks=(pages+SHARED_BLOCK_PAGES-1)/SHARED_BLOCK_PAGES;
    A830_SHARED_BLOCK **directory=ExAllocatePool2(POOL_FLAG_NON_PAGED,blocks*sizeof(*directory),GPU_TAG);
    if(!directory)return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeMutex(&s->Lock,0);s->Bytes=bytes;s->PageCount=pages;s->BlockCount=blocks;s->Blocks=directory;
    return STATUS_SUCCESS;
}
VOID A830SharedStop(A830_ADAPTER *a) {
    A830_SHARED_MEMORY *s=&a->Memory.Shared;if(!s->Blocks)return;
    KeWaitForSingleObject(&s->Lock,Executive,KernelMode,FALSE,NULL);
    if(s->Blocks) {
        for(SIZE_T i=0;i<s->BlockCount;i++)if(s->Blocks[i])ExFreePoolWithTag(s->Blocks[i],GPU_TAG);
        ExFreePoolWithTag(s->Blocks,GPU_TAG);s->Blocks=NULL;s->Bytes=0;s->PageCount=0;s->BlockCount=0;
    }
    KeReleaseMutex(&s->Lock,FALSE);
}
NTSTATUS A830SharedMap(A830_ADAPTER *a,SIZE_T page,SIZE_T count,PMDL source,ULONG offset) {
    A830_SHARED_MEMORY *s=&a->Memory.Shared;
    if(!source || !Range(s,page,count))return STATUS_INVALID_PARAMETER;
    SIZE_T sourcePages=ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(source),MmGetMdlByteCount(source));
    if(offset>sourcePages || count>sourcePages-offset)return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&s->Lock,Executive,KernelMode,FALSE,NULL);
    if(!Range(s,page,count)){KeReleaseMutex(&s->Lock,FALSE);return STATUS_INVALID_PARAMETER;}
    NTSTATUS status=STATUS_SUCCESS;
    const PFN_NUMBER *pfns=MmGetMdlPfnArray(source)+offset;
    for(SIZE_T i=0;i<count;i++) {
        if(Pfn(s,page+i)!=UNMAPPED_PFN){status=STATUS_CONFLICTING_ADDRESSES;break;}
        if(pfns[i]==UNMAPPED_PFN){status=STATUS_INVALID_PARAMETER;break;}
    }
    SIZE_T first=page/SHARED_BLOCK_PAGES,last=(page+count-1)/SHARED_BLOCK_PAGES;
    for(SIZE_T i=first;NT_SUCCESS(status) && i<=last;i++)if(!s->Blocks[i]) {
        A830_SHARED_BLOCK *b=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*b),GPU_TAG);
        if(!b){status=STATUS_INSUFFICIENT_RESOURCES;break;}
        RtlFillMemory(b->Pfns,sizeof(b->Pfns),0xff);s->Blocks[i]=b;
    }
    if(NT_SUCCESS(status)) {
        SIZE_T attempted=0;status=MapPages(a,page,count,pfns,&attempted);
        if(!NT_SUCCESS(status) && attempted) {
            NTSTATUS rollback=UnmapPages(a,page,attempted);
            if(!NT_SUCCESS(rollback))InterlockedExchange(&a->Failed,rollback);
        }
    }
    if(NT_SUCCESS(status))for(SIZE_T i=0;i<count;i++) {
        A830_SHARED_BLOCK *b=s->Blocks[(page+i)/SHARED_BLOCK_PAGES];
        b->Pfns[(page+i)%SHARED_BLOCK_PAGES]=pfns[i];b->Used++;
    }
    else DropEmpty(s,page,count);
    KeReleaseMutex(&s->Lock,FALSE);return status;
}
NTSTATUS A830SharedUnmap(A830_ADAPTER *a,SIZE_T page,SIZE_T count) {
    A830_SHARED_MEMORY *s=&a->Memory.Shared;if(!Range(s,page,count))return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&s->Lock,Executive,KernelMode,FALSE,NULL);
    if(!Range(s,page,count)){KeReleaseMutex(&s->Lock,FALSE);return STATUS_INVALID_PARAMETER;}
    NTSTATUS status=UnmapPages(a,page,count);
    if(NT_SUCCESS(status)) {
        for(SIZE_T i=0;i<count;i++) {
            A830_SHARED_BLOCK *b=s->Blocks[(page+i)/SHARED_BLOCK_PAGES];
            if(b && b->Pfns[(page+i)%SHARED_BLOCK_PAGES]!=UNMAPPED_PFN) {
                b->Pfns[(page+i)%SHARED_BLOCK_PAGES]=UNMAPPED_PFN;b->Used--;
            }
        }
        DropEmpty(s,page,count);
    } else InterlockedExchange(&a->Failed,status);
    KeReleaseMutex(&s->Lock,FALSE);return status;
}
NTSTATUS A830SharedRestore(A830_ADAPTER *a) {
    A830_SHARED_MEMORY *s=&a->Memory.Shared;if(!s->Blocks)return STATUS_SUCCESS;
    KeWaitForSingleObject(&s->Lock,Executive,KernelMode,FALSE,NULL);
    SIZE_T page=0;NTSTATUS status=STATUS_SUCCESS;
    while(page<s->PageCount && NT_SUCCESS(status)) {
        if(!s->Blocks[page/SHARED_BLOCK_PAGES]) {
            page=min(s->PageCount,(page/SHARED_BLOCK_PAGES+1)*SHARED_BLOCK_PAGES);continue;
        }
        if(Pfn(s,page)==UNMAPPED_PFN){page++;continue;}
        SIZE_T start=page;while(page<s->PageCount && Pfn(s,page)!=UNMAPPED_PFN)page++;
        SIZE_T attempted=0;status=MapPages(a,start,page-start,NULL,&attempted);
    }
    KeReleaseMutex(&s->Lock,FALSE);return status;
}
NTSTATUS A830MapSharedCpu(A830_ADAPTER *a,ULONGLONG address,SIZE_T bytes,PVOID *map,PMDL *mapping) {
    *map=NULL;*mapping=NULL;A830_SHARED_MEMORY *s=&a->Memory.Shared;
    if(address>=A830_SHARED_ADDRESS)address-=A830_SHARED_ADDRESS;
    if(!s->Blocks || !bytes || bytes>MAXULONG || address>=s->Bytes || bytes>s->Bytes-address)
        return STATUS_INVALID_PARAMETER;
    SIZE_T first=(SIZE_T)(address/PAGE_SIZE);ULONG offset=(ULONG)(address&(PAGE_SIZE-1));
    SIZE_T count=(offset+bytes+PAGE_SIZE-1)/PAGE_SIZE;
    PMDL mdl=IoAllocateMdl((PVOID)(ULONG_PTR)offset,(ULONG)bytes,FALSE,FALSE,NULL);
    if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
    KeWaitForSingleObject(&s->Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_DEVICE_NOT_READY;PPFN_NUMBER pfns=MmGetMdlPfnArray(mdl);PVOID cpu=NULL;
    if(!s->Blocks || address>=s->Bytes || bytes>s->Bytes-address)goto done;
    for(SIZE_T i=0;i<count;i++) {
        pfns[i]=Pfn(s,first+i);if(pfns[i]==UNMAPPED_PFN)goto done;
    }
    mdl->MdlFlags|=MDL_PAGES_LOCKED;status=STATUS_INSUFFICIENT_RESOURCES;
    __try {cpu=MmMapLockedPagesSpecifyCache(mdl,KernelMode,MmCached,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute);}
    __except(EXCEPTION_EXECUTE_HANDLER){status=GetExceptionCode();}
    if(cpu){*map=cpu;*mapping=mdl;status=STATUS_SUCCESS;}
done:
    KeReleaseMutex(&s->Lock,FALSE);if(!cpu)IoFreeMdl(mdl);return status;
}
