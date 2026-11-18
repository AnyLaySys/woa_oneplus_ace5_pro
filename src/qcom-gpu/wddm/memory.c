#include "adapter.h"

NTSTATUS A830MemoryStart(A830_ADAPTER *a) {
    GPU_BO *b=&a->Memory.Segment;
    PHYSICAL_ADDRESS low={0},high,skip={0};high.QuadPart=0xFFFFFFFFFFF;
    b->Size=A830_SEGMENT_BYTES;
    b->Cpu=MmAllocateContiguousMemorySpecifyCache(b->Size,low,high,skip,MmWriteCombined);
    if(!b->Cpu)return STATUS_INSUFFICIENT_RESOURCES;
    b->Mdl=IoAllocateMdl(b->Cpu,b->Size,FALSE,FALSE,NULL);
    if(!b->Mdl){MmFreeContiguousMemory(b->Cpu);RtlZeroMemory(b,sizeof(*b));return STATUS_INSUFFICIENT_RESOURCES;}
    MmBuildMdlForNonPagedPool(b->Mdl);b->Contiguous=TRUE;b->Physical=MmGetPhysicalAddress(b->Cpu).QuadPart;
    RtlZeroMemory(b->Cpu,b->Size);
    NTSTATUS status=A830SharedStart(a);
    if(NT_SUCCESS(status))status=A830Map(a,b,A830_SEGMENT_ADDRESS);
    if(NT_SUCCESS(status))status=GpuAllocate(&a->Memory.Commands,A830_COMMAND_BYTES);
    if(NT_SUCCESS(status))status=A830Map(a,&a->Memory.Commands,A830_COMMAND_ADDRESS);
    if(NT_SUCCESS(status))status=A830ScanoutStart(a);
    return status;
}
VOID A830MemoryStop(A830_ADAPTER *a) {
    A830ScanoutStop(a);

    A830SharedStop(a);GpuFree(&a->Memory.Commands);GpuFree(&a->Memory.Segment);
}
NTSTATUS APIENTRY A830CreateAllocation(HANDLE adapter,DXGKARG_CREATEALLOCATION *p) {

    PULONG resource=NULL;
    if(p->Flags.Resource && !p->hResource) {
        resource=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*resource),GPU_TAG);
        if(!resource)return STATUS_INSUFFICIENT_RESOURCES;
        *resource=A830_ABI_VERSION;p->hResource=resource;
    }
    ULONG i=0;NTSTATUS status=STATUS_INVALID_PARAMETER;
    for(;i<p->NumAllocations;i++) {
        DXGK_ALLOCATIONINFO *info=p->pAllocationInfo+i;
        if(info->PrivateDriverDataSize!=sizeof(A830_RESOURCE) || !info->pPrivateDriverData)break;
        A830_RESOURCE r=*(A830_RESOURCE *)info->pPrivateDriverData;
        BOOLEAN shared=(r.Flags&1) || ((r.Flags&A830_RESOURCE_OWNED) && !(r.Flags&A830_RESOURCE_PRIMARY));
        ULONGLONG limit=shared?min(((A830_ADAPTER *)adapter)->Memory.Shared.Bytes,(ULONGLONG)GPU_BUFFER_LIMIT):A830_SEGMENT_BYTES;
        if(r.Version!=A830_ABI_VERSION || !r.Size || r.Size>limit || (r.Flags&~7u) || r.Rotation>D3DDDI_ROTATION_270)break;
        if(r.Flags&A830_RESOURCE_OWNED) {
            ULONG bytes=A830PixelBytes(r.Format);
            if(!bytes || !r.Width || !r.Height || r.Width>MAXULONG/bytes ||
               r.Pitch<r.Width*bytes || r.Height>r.Size/r.Pitch)break;
        }
        A830_ALLOCATION *allocation=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*allocation),GPU_TAG);
        if(!allocation){status=STATUS_INSUFFICIENT_RESOURCES;break;}
        allocation->Resource=r;info->hAllocation=allocation;info->Size=(r.Size+4095)&~4095u;
        if(r.Flags&A830_RESOURCE_OWNED) {
            status=A830SharedCreate(allocation);
            if(!NT_SUCCESS(status)){ExFreePoolWithTag(allocation,GPU_TAG);info->hAllocation=NULL;break;}
            info->Size=(r.Flags&A830_RESOURCE_PRIMARY)?((r.Size+4095)&~4095u):4096;
        }
        info->PitchAlignedSize=0;info->HintedBank.Value=0;info->PhysicalAdapterIndex=0;
        info->pAllocationUsageHint=NULL;info->AllocationPriority=D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        info->Alignment=4096;info->PreferredSegment.Value=0;info->PreferredSegment.SegmentId0=1;
        info->SupportedReadSegmentSet=1;info->SupportedWriteSegmentSet=1;info->EvictionSegmentSet=0;
        if(r.Flags&1) {
            info->PreferredSegment.SegmentId0=2;info->SupportedReadSegmentSet=info->SupportedWriteSegmentSet=2;
        }
        info->FlagsWddm2.Value&=~0x0007C3FFu;
        info->FlagsWddm2.CpuVisible=1;info->FlagsWddm2.AccessedPhysically=1;
        if(r.Flags&1)info->FlagsWddm2.Cached=1;
        if(r.Flags&A830_RESOURCE_OWNED) {
            info->FlagsWddm2.CpuVisible=0;info->FlagsWddm2.Cached=0;
            info->PreferredSegment.SegmentId0=1;info->SupportedReadSegmentSet=info->SupportedWriteSegmentSet=1;
        }
    }
    if(i==p->NumAllocations)return STATUS_SUCCESS;
    while(i){i--;A830_ALLOCATION *allocation=p->pAllocationInfo[i].hAllocation;if(allocation->Buffer.Mdl)A830SharedDestroy(allocation);else ExFreePoolWithTag(allocation,GPU_TAG);p->pAllocationInfo[i].hAllocation=NULL;}
    if(resource){ExFreePoolWithTag(resource,GPU_TAG);p->hResource=NULL;}
    return status;
}
NTSTATUS APIENTRY A830DestroyAllocation(HANDLE adapter,const DXGKARG_DESTROYALLOCATION *p) {
    UNREFERENCED_PARAMETER(adapter);
    for(ULONG i=0;i<p->NumAllocations;i++){A830_ALLOCATION *allocation=p->pAllocationList[i];if(allocation->Buffer.Mdl)A830SharedDestroy(allocation);else ExFreePoolWithTag(allocation,GPU_TAG);}
    if(p->Flags.DestroyResource && p->hResource)ExFreePoolWithTag(p->hResource,GPU_TAG);
    return STATUS_SUCCESS;
}
NTSTATUS APIENTRY A830Escape(HANDLE adapter,const DXGKARG_ESCAPE *p) {
    A830_ADAPTER *a=adapter;
    if(p->hContext && p->PrivateDriverDataSize==sizeof(A830_CONTEXT_QUERY) && p->pPrivateDriverData) {
        A830_CONTEXT_QUERY *query=p->pPrivateDriverData;A830_CONTEXT *c=p->hContext;
        if(query->Version!=A830_ABI_VERSION || query->Reserved || c->Adapter!=a || c->Process!=PsGetCurrentProcess())return STATUS_INVALID_PARAMETER;
        query->CompletionPage=(ULONGLONG)c->CompletionMap;return query->CompletionPage?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
    }
    if(!p->hDevice || !p->pPrivateDriverData || p->PrivateDriverDataSize!=sizeof(A830_ALLOCATION_ADDRESS))return STATUS_INVALID_PARAMETER;
    A830_ALLOCATION_ADDRESS *address=p->pPrivateDriverData;
    if(address->Version!=A830_ABI_VERSION || !address->Allocation || address->Reserved)return STATUS_INVALID_PARAMETER;
    DXGKARGCB_GETHANDLEDATA get={0};get.Type=DXGK_HANDLE_ALLOCATION;get.hObject=address->Allocation;
    DXGKARG_RELEASE_HANDLE release=NULL;
    A830_ALLOCATION *allocation=a->Dxgk.DxgkCbAcquireHandleData(&get,&release);
    if(!allocation || !allocation->Buffer.Mdl){if(release){DXGKARGCB_RELEASEHANDLEDATA drop={release,DXGK_HANDLE_ALLOCATION};a->Dxgk.DxgkCbReleaseHandleData(drop);}return STATUS_INVALID_HANDLE;}
    address->Address=allocation->Buffer.Address;address->Size=allocation->Buffer.Size;
    address->Pitch=allocation->Resource.Pitch;address->Format=allocation->Resource.Format;
    if(release){DXGKARGCB_RELEASEHANDLEDATA drop={release,DXGK_HANDLE_ALLOCATION};a->Dxgk.DxgkCbReleaseHandleData(drop);}
    return STATUS_SUCCESS;
}
static PVOID Segment(A830_ADAPTER *a,ULONGLONG offset,SIZE_T bytes) {
    if(offset>=A830_SEGMENT_ADDRESS)offset-=A830_SEGMENT_ADDRESS;
    if(offset>A830_SEGMENT_BYTES || bytes>A830_SEGMENT_BYTES-offset)return NULL;
    return a->Memory.Segment.Cpu+(SIZE_T)offset;
}
NTSTATUS APIENTRY A830Paging(HANDLE adapter,DXGKARG_BUILDPAGINGBUFFER *p) {
    A830_ADAPTER *a=adapter;
    if(p->DmaSize<sizeof(ULONG))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if(p->DmaBufferPrivateDataSize<sizeof(A830_DMA))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    A830_DMA *dma=p->pDmaBufferPrivateData;RtlZeroMemory(dma,sizeof(*dma));
    if(p->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT) {
        if(p->MapApertureSegment.SegmentId!=2)return STATUS_INVALID_PARAMETER;
        NTSTATUS status=A830SharedMap(a,p->MapApertureSegment.OffsetInPages,p->MapApertureSegment.NumberOfPages,
            p->MapApertureSegment.pMdl,p->MapApertureSegment.MdlOffset);
        if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        if(p->UnmapApertureSegment.SegmentId!=2)return STATUS_INVALID_PARAMETER;
        NTSTATUS status=A830SharedUnmap(a,p->UnmapApertureSegment.OffsetInPages,p->UnmapApertureSegment.NumberOfPages);
        if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_DISCARD_CONTENT)dma->Operation=0;
    else if(p->Operation==DXGK_OPERATION_FILL) {
        if(p->Fill.FillSize>MAXULONG || (p->Fill.FillSize&3))return STATUS_INVALID_PARAMETER;
        dma->Operation=2;dma->Bytes=(ULONG)p->Fill.FillSize;dma->Value=p->Fill.FillPattern;
        if(p->Fill.Destination.SegmentId==1)dma->Destination=Segment(a,p->Fill.Destination.SegmentAddress.QuadPart,dma->Bytes);
        else if(p->Fill.Destination.SegmentId==2) {
            NTSTATUS status=A830MapSharedCpu(a,p->Fill.Destination.SegmentAddress.QuadPart,dma->Bytes,&dma->Destination,&dma->DestinationMdl);
            if(!NT_SUCCESS(status)){return status;}
        }
        if(!dma->Destination)return STATUS_INVALID_PARAMETER;
    } else if(p->Operation==DXGK_OPERATION_TRANSFER) {
        if(p->Transfer.TransferSize>MAXULONG)return STATUS_INVALID_PARAMETER;
        ULONGLONG allocationOffset=p->Transfer.TransferOffset;
        ULONGLONG source=p->Transfer.Source.SegmentAddress.QuadPart,destination=p->Transfer.Destination.SegmentAddress.QuadPart;
        if((p->Transfer.Source.SegmentId && source>~(ULONGLONG)0-allocationOffset) ||
            (p->Transfer.Destination.SegmentId && destination>~(ULONGLONG)0-allocationOffset))return STATUS_INVALID_PARAMETER;
        dma->Operation=3;dma->Bytes=(ULONG)p->Transfer.TransferSize;
        if(p->Transfer.Source.SegmentId==1)dma->Source=Segment(a,source+allocationOffset,dma->Bytes);
        else if(p->Transfer.Source.SegmentId==2) {
            NTSTATUS status=A830MapSharedCpu(a,source+allocationOffset,dma->Bytes,&dma->Source,&dma->SourceMdl);
            if(!NT_SUCCESS(status)){return status;}
        }
        else if(p->Transfer.Source.SegmentId==0) {
            PMDL mdl=p->Transfer.Source.pMdl;SIZE_T offset=(SIZE_T)p->Transfer.MdlOffset*PAGE_SIZE;
            if(!mdl || offset>MmGetMdlByteCount(mdl) || dma->Bytes>MmGetMdlByteCount(mdl)-offset)return STATUS_INVALID_PARAMETER;
            PUCHAR map=MmGetSystemAddressForMdlSafe(mdl,NormalPagePriority|MdlMappingNoExecute);if(map)dma->Source=map+offset;
        }
        if(p->Transfer.Destination.SegmentId==1)dma->Destination=Segment(a,destination+allocationOffset,dma->Bytes);
        else if(p->Transfer.Destination.SegmentId==2) {
            NTSTATUS status=A830MapSharedCpu(a,destination+allocationOffset,dma->Bytes,&dma->Destination,&dma->DestinationMdl);
            if(!NT_SUCCESS(status)) {
                if(dma->SourceMdl){MmUnmapLockedPages(dma->Source,dma->SourceMdl);IoFreeMdl(dma->SourceMdl);dma->SourceMdl=NULL;}
            }
        }
        else if(p->Transfer.Destination.SegmentId==0) {
            PMDL mdl=p->Transfer.Destination.pMdl;SIZE_T offset=(SIZE_T)p->Transfer.MdlOffset*PAGE_SIZE;
            if(!mdl || offset>MmGetMdlByteCount(mdl) || dma->Bytes>MmGetMdlByteCount(mdl)-offset)return STATUS_INVALID_PARAMETER;
            PUCHAR map=MmGetSystemAddressForMdlSafe(mdl,NormalPagePriority|MdlMappingNoExecute);if(map)dma->Destination=map+offset;
        }
        if(!dma->Source || !dma->Destination) {
            if(dma->SourceMdl){MmUnmapLockedPages(dma->Source,dma->SourceMdl);IoFreeMdl(dma->SourceMdl);dma->SourceMdl=NULL;}
            if(dma->DestinationMdl){MmUnmapLockedPages(dma->Destination,dma->DestinationMdl);IoFreeMdl(dma->DestinationMdl);dma->DestinationMdl=NULL;}
            return STATUS_INVALID_PARAMETER;
        }
    } else {return STATUS_NOT_SUPPORTED;}
    *(PULONG)p->pDmaBuffer=0;p->pDmaBuffer=(PUCHAR)p->pDmaBuffer+sizeof(ULONG);
    p->pDmaBufferPrivateData=(PUCHAR)p->pDmaBufferPrivateData+sizeof(*dma);
    return STATUS_SUCCESS;
}
