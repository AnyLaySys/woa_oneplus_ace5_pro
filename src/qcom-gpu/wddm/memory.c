#include "adapter.h"
#include "gpuva.h"

NTSTATUS A830MemoryStart(A830_ADAPTER *a) {
    GPU_BO *b=&a->Memory.Segment;
    PHYSICAL_ADDRESS low={0},high,skip={0};high.QuadPart=MAXULONG;
    b->Size=A830_SEGMENT_BYTES;
    b->Cpu=MmAllocateContiguousMemorySpecifyCache(b->Size,low,high,skip,MmWriteCombined);
    if(!b->Cpu){high.QuadPart=0xFFFFFFFFFFF;b->Cpu=MmAllocateContiguousMemorySpecifyCache(b->Size,low,high,skip,MmWriteCombined);}
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
NTSTATUS A830CopyKernelMappings(A830_ADAPTER *a,ULONGLONG physical) {
    if(!physical)return STATUS_DEVICE_NOT_READY;
    if((physical&4095) || physical>>44)return STATUS_INVALID_ADDRESS;
    PULONGLONG root;PMDL mdl=NULL;
    if(a->Memory.Segment.Cpu && physical>=a->Memory.Segment.Physical && physical-a->Memory.Segment.Physical<=A830_SEGMENT_BYTES-PAGE_SIZE)
        root=(PVOID)(a->Memory.Segment.Cpu+(SIZE_T)(physical-a->Memory.Segment.Physical));
    else {
        mdl=IoAllocateMdl(NULL,PAGE_SIZE,FALSE,FALSE,NULL);if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
        *MmGetMdlPfnArray(mdl)=(PFN_NUMBER)(physical>>12);mdl->MdlFlags|=MDL_PAGES_LOCKED;
        __try {root=MmMapLockedPagesSpecifyCache(mdl,KernelMode,MmCached,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute);}
        __except(EXCEPTION_EXECUTE_HANDLER){IoFreeMdl(mdl);return GetExceptionCode();}
        if(!root){IoFreeMdl(mdl);return STATUS_INSUFFICIENT_RESOURCES;}
    }
    KeWaitForSingleObject(&a->Hardware.MappingLock,Executive,KernelMode,FALSE,NULL);
    PULONGLONG global=(PULONGLONG)a->Hardware.Vm[0].Tables.Cpu;
    NTSTATUS status=!global?STATUS_DEVICE_NOT_READY:global[0]?STATUS_DEVICE_CONFIGURATION_ERROR:STATUS_SUCCESS;
    for(ULONG i=1;NT_SUCCESS(status) && i<4;i++)if(root[i] && root[i]!=global[i])status=STATUS_CONFLICTING_ADDRESSES;
    if(NT_SUCCESS(status)) {
        for(ULONG i=1;i<4;i++)root[i]=global[i];KeMemoryBarrier();A830FlushPageTable(root);
    }
    KeReleaseMutex(&a->Hardware.MappingLock,FALSE);
    if(mdl){MmUnmapLockedPages(root,mdl);IoFreeMdl(mdl);}return status;
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
            if((r.Format==A830_FORMAT_NV12 || r.Format==A830_FORMAT_P010) && (((r.Width|r.Height|r.Pitch)&1) ||
               (r.Flags&A830_RESOURCE_PRIMARY) || (ULONGLONG)r.Pitch*(r.Height+r.Height/2)>r.Size))break;
        }
        A830_ALLOCATION *allocation=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*allocation),GPU_TAG);
        if(!allocation){status=STATUS_INSUFFICIENT_RESOURCES;break;}
        allocation->Resource=r;info->hAllocation=allocation;info->Size=(r.Size+4095)&~4095u;
        if(r.Flags&A830_RESOURCE_OWNED) {
            status=A830SharedCreate(allocation);
            if(!NT_SUCCESS(status)){ExFreePoolWithTag(allocation,GPU_TAG);info->hAllocation=NULL;break;}
            /* SharedCreate owns and maps the complete pixel BO independently
               of the VidMm segment. This allocation is only its handle token,
               including for primaries. Charging the pixels here a second time
               fills the 64 MiB segment when DWM keeps native and IDD swapchains
               resident together during a mode change. Resource.Size and the
               scanout BO retain the full image extent. */
            info->Size=4096;
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
VOID A830ReleasePaging(A830_DMA *dma) {
    if(dma->SourceMdl){MmUnmapLockedPages(dma->Source,dma->SourceMdl);IoFreeMdl(dma->SourceMdl);dma->SourceMdl=NULL;dma->Source=NULL;}
    if(dma->DestinationMdl){MmUnmapLockedPages(dma->Destination,dma->DestinationMdl);IoFreeMdl(dma->DestinationMdl);dma->DestinationMdl=NULL;dma->Destination=NULL;}
}
static PVOID Segment(A830_ADAPTER *a,ULONGLONG offset,SIZE_T bytes) {
    if(offset>=A830_SEGMENT_ADDRESS)offset-=A830_SEGMENT_ADDRESS;
    if(offset>A830_SEGMENT_BYTES || bytes>A830_SEGMENT_BYTES-offset)return NULL;
    return a->Memory.Segment.Cpu+(SIZE_T)offset;
}
static NTSTATUS MapVirtualCpu(A830_ADAPTER *a,ULONGLONG root,ULONGLONG address,SIZE_T bytes,PVOID *map,PMDL *mapping) {
    *map=NULL;*mapping=NULL;
    if(!bytes || bytes>GPU_MAX_BUFFER_SIZE || address>=0x8000000000ULL || bytes>0x8000000000ULL-address)return STATUS_INVALID_PARAMETER;
    ULONG offset=(ULONG)(address&4095);SIZE_T pages=(offset+bytes+4095)/PAGE_SIZE;
    PMDL mdl=IoAllocateMdl((PVOID)(ULONG_PTR)offset,(ULONG)bytes,FALSE,FALSE,NULL);
    if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
    PPFN_NUMBER pfns=MmGetMdlPfnArray(mdl);NTSTATUS status=STATUS_SUCCESS;MEMORY_CACHING_TYPE cache=MmCached;
    for(SIZE_T i=0;i<pages;i++) {
        ULONGLONG physical;status=A830ResolveGpuAddress(a,root,(address&~4095ULL)+i*PAGE_SIZE,&physical);if(!NT_SUCCESS(status))break;
        MEMORY_CACHING_TYPE type=physical>=a->Memory.Segment.Physical && physical-a->Memory.Segment.Physical<A830_SEGMENT_BYTES?MmWriteCombined:MmCached;
        if(!i)cache=type;else if(cache!=type){status=STATUS_NOT_SUPPORTED;break;}
        pfns[i]=(PFN_NUMBER)(physical>>12);
    }
    if(NT_SUCCESS(status)) {
        mdl->MdlFlags|=MDL_PAGES_LOCKED;
        __try {*map=MmMapLockedPagesSpecifyCache(mdl,KernelMode,cache,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute);}
        __except(EXCEPTION_EXECUTE_HANDLER){status=GetExceptionCode();}
        if(!*map && NT_SUCCESS(status))status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if(NT_SUCCESS(status))*mapping=mdl;else IoFreeMdl(mdl);return status;
}
NTSTATUS A830ResolvePaging(A830_ADAPTER *a,A830_DMA *dma) {
    ULONGLONG source=(ULONG_PTR)dma->Source,destination=(ULONG_PTR)dma->Destination;
    dma->Source=dma->Destination=NULL;
    NTSTATUS status=STATUS_SUCCESS;
    if(dma->Operation==10)status=MapVirtualCpu(a,dma->Root,source,dma->Bytes,&dma->Source,&dma->SourceMdl);
    if(NT_SUCCESS(status))status=MapVirtualCpu(a,dma->Root,destination,dma->Bytes,&dma->Destination,&dma->DestinationMdl);
    if(NT_SUCCESS(status))dma->Operation=dma->Operation==9?2:dma->Operation==10?3:8;
    return status;
}
static NTSTATUS UpdatePageTableLocked(A830_ADAPTER *a,const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *p) {
    if(p->UpdateMode!=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL || !p->PageTableAddress.CpuVirtual ||
       ((ULONG_PTR)p->PageTableAddress.CpuVirtual&4095) || !p->pPageTableEntries || p->PageTableLevel>2 ||
       !p->NumPageTableEntries || p->StartIndex>=512 || p->NumPageTableEntries>512-p->StartIndex ||
       p->Flags.Use64KBPages || p->Flags.NotifyEviction || p->DriverProtection || p->pPageTableEntries64KB)
        return STATUS_NOT_SUPPORTED;
    ULONGLONG entries[512];
    A830_PROCESS *process=p->hProcess;
    BOOLEAN root=process && !process->System && p->PageTableLevel==2 && process->Root &&
        (ULONGLONG)MmGetPhysicalAddress(p->PageTableAddress.CpuVirtual).QuadPart==process->Root;
    for(ULONG i=0;i<p->NumPageTableEntries;i++) {
        const DXGK_PTE *entry=p->pPageTableEntries+(p->Flags.Repeat?0:i);
        if(root && p->StartIndex+i>=1 && p->StartIndex+i<4) {
            if(entry->Valid || entry->Zero)return STATUS_CONFLICTING_ADDRESSES;
            continue;
        }
        ULONGLONG physical=0;
        if(entry->Valid) {
            if(entry->PageAddress>>32)return STATUS_NOT_SUPPORTED;
            NTSTATUS status=A830PhysicalAddress(a,(ULONG)entry->Segment,entry->PageAddress<<12,&physical);
            if(!NT_SUCCESS(status))return status;
        }
        if(!A830GpuPte(entry,p->PageTableLevel,physical,entries+i))return STATUS_NOT_SUPPORTED;
    }
    /* Validate the entire update before changing any live descriptor. */
    if(root)KeWaitForSingleObject(&a->Hardware.MappingLock,Executive,KernelMode,FALSE,NULL);
    PULONGLONG global=(PULONGLONG)a->Hardware.Vm[0].Tables.Cpu;
    if(root && !global){KeReleaseMutex(&a->Hardware.MappingLock,FALSE);return STATUS_DEVICE_NOT_READY;}
    KeMemoryBarrier();
    volatile ULONGLONG *table=(PULONGLONG)p->PageTableAddress.CpuVirtual;
    for(ULONG i=0;i<p->NumPageTableEntries;i++)table[p->StartIndex+i]=root && p->StartIndex+i>=1 && p->StartIndex+i<4?global[p->StartIndex+i]:entries[i];
    KeMemoryBarrier();A830FlushPageTable(p->PageTableAddress.CpuVirtual);if(root)KeReleaseMutex(&a->Hardware.MappingLock,FALSE);return STATUS_SUCCESS;
}
static NTSTATUS UpdatePageTable(A830_ADAPTER *a,const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *p) {
    A830_PROCESS *process=p->hProcess;
    if(process)KeWaitForSingleObject(&process->Lock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=UpdatePageTableLocked(a,p);
    if(NT_SUCCESS(status) && process && !process->System && p->PageTableLevel==2 && (p->Flags.InitialUpdate || !process->Root))
        process->Root=MmGetPhysicalAddress(p->PageTableAddress.CpuVirtual).QuadPart;
    if(process)KeReleaseMutex(&process->Lock,FALSE);return status;
}
NTSTATUS APIENTRY A830Paging(HANDLE adapter,DXGKARG_BUILDPAGINGBUFFER *p) {
    A830_ADAPTER *a=adapter;
    if(p->Operation==DXGK_OPERATION_UPDATE_PAGE_TABLE && !p->pDmaBuffer)return UpdatePageTable(a,&p->UpdatePageTable);
    if(p->DmaSize<sizeof(ULONG))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if(p->DmaBufferPrivateDataSize<sizeof(A830_DMA))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    A830_DMA *dma=p->pDmaBufferPrivateData;RtlZeroMemory(dma,sizeof(*dma));
    if(p->Operation==DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        NTSTATUS status=UpdatePageTable(a,&p->UpdatePageTable);if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_FLUSH_TLB) {
        const D3DGPU_PHYSICAL_ADDRESS *root=&p->FlushTlb.RootPageTableAddress;
        if(root->SegmentId>1 || (root->SegmentOffset&4095) || (root->SegmentOffset>>44) ||
           (root->SegmentId==1 && root->SegmentOffset>A830_SEGMENT_BYTES-PAGE_SIZE))return STATUS_INVALID_PARAMETER;
        NTSTATUS status=GpuFlushMappings(&a->Hardware,0);if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT) {
        if(p->MapApertureSegment.SegmentId!=2)return STATUS_INVALID_PARAMETER;
        NTSTATUS status=A830SharedMap(a,p->MapApertureSegment.OffsetInPages,p->MapApertureSegment.NumberOfPages,
            p->MapApertureSegment.pMdl,p->MapApertureSegment.MdlOffset);
        if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        if(p->UnmapApertureSegment.SegmentId!=2)return STATUS_INVALID_PARAMETER;
        NTSTATUS status=A830SharedUnmap(a,p->UnmapApertureSegment.OffsetInPages,p->UnmapApertureSegment.NumberOfPages);
        if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_NOTIFY_RESIDENCY) {
        A830_ALLOCATION *allocation=p->NotifyResidency.hAllocation;
        if(!allocation)return STATUS_INVALID_PARAMETER;
        if(p->NotifyResidency.Resident)allocation->Resident=p->NotifyResidency.PhysicalAddress;
        else RtlZeroMemory(&allocation->Resident,sizeof(allocation->Resident));
    } else if(p->Operation==DXGK_OPERATION_VIRTUAL_FILL) {
        A830_ALLOCATION *allocation=p->FillVirtual.hAllocation;ULONGLONG offset=p->FillVirtual.AllocationOffsetInBytes,bytes=p->FillVirtual.FillSizeInBytes;
        if(!bytes || bytes>MAXULONG || ((offset|bytes|p->FillVirtual.DestinationVirtualAddress)&3) ||
           (allocation && (offset>allocation->Resource.Size || bytes>allocation->Resource.Size-offset)))return STATUS_INVALID_PARAMETER;
        dma->Operation=9;dma->Bytes=(ULONG)bytes;dma->Value=p->FillVirtual.FillPattern;
        dma->Destination=(PVOID)(ULONG_PTR)p->FillVirtual.DestinationVirtualAddress;
    } else if(p->Operation==DXGK_OPERATION_VIRTUAL_TRANSFER) {
        const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL *v=&p->TransferVirtual;A830_ALLOCATION *allocation=v->hAllocation;
        if(!v->TransferSizeInBytes || v->TransferSizeInBytes>MAXULONG ||
           (allocation && (v->AllocationOffsetInBytes>allocation->Resource.Size || v->TransferSizeInBytes>allocation->Resource.Size-v->AllocationOffsetInBytes)))return STATUS_INVALID_PARAMETER;
        dma->Operation=10;dma->Bytes=(ULONG)v->TransferSizeInBytes;
        dma->Source=(PVOID)(ULONG_PTR)v->SourceVirtualAddress;dma->Destination=(PVOID)(ULONG_PTR)v->DestinationVirtualAddress;
    } else if(p->Operation==DXGK_OPERATION_SIGNAL_MONITORED_FENCE) {
        if(p->SignalMonitoredFence.MonitoredFenceGpuVa&7)return STATUS_INVALID_PARAMETER;
        dma->Operation=11;dma->Bytes=8;dma->FenceValue=p->SignalMonitoredFence.MonitoredFenceValue;
        dma->Destination=(PVOID)(ULONG_PTR)p->SignalMonitoredFence.MonitoredFenceGpuVa;
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
