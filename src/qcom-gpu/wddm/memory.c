#include "adapter.h"
#include "gpuva.h"

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
    a->PagingRoot=0;
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
        if((info->PrivateDriverDataSize!=sizeof(A830_RESOURCE) && info->PrivateDriverDataSize!=sizeof(A830_DX12_RESOURCE)) || !info->pPrivateDriverData)break;
        A830_RESOURCE r=*(A830_RESOURCE *)info->pPrivateDriverData;
        BOOLEAN native=(r.Flags&A830_RESOURCE_NATIVE)!=0;
        if(info->PrivateDriverDataSize==sizeof(A830_DX12_RESOURCE) && !native)break;
        BOOLEAN shared=(r.Flags&1) || ((r.Flags&A830_RESOURCE_OWNED) && !(r.Flags&A830_RESOURCE_PRIMARY));
        ULONGLONG limit=shared || native?min(((A830_ADAPTER *)adapter)->Memory.Shared.Bytes,(ULONGLONG)GPU_BUFFER_LIMIT):A830_SEGMENT_BYTES;
        if(r.Version!=A830_ABI_VERSION || !r.Size || r.Size>limit || (r.Flags&~15u) ||
           (native?(r.Rotation>2 || (r.Flags&(1u|A830_RESOURCE_OWNED))):r.Rotation>D3DDDI_ROTATION_270))break;
        if(native && (r.Flags&A830_RESOURCE_PRIMARY)) {
            ULONG bytes=A830PixelBytes(r.Format);
            if(!bytes || !r.Width || !r.Height || r.Size>A830_SEGMENT_BYTES || r.Width>MAXULONG/bytes ||
               r.Pitch<r.Width*bytes || r.Height>r.Size/r.Pitch)break;
        }
        if(!native && (r.Flags&A830_RESOURCE_OWNED)) {
            ULONG bytes=A830PixelBytes(r.Format);
            if(!bytes || !r.Width || !r.Height || r.Width>MAXULONG/bytes ||
               r.Pitch<r.Width*bytes || r.Height>r.Size/r.Pitch)break;
        }
        A830_ALLOCATION *allocation=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*allocation),GPU_TAG);
        if(!allocation){status=STATUS_INSUFFICIENT_RESOURCES;break;}
        allocation->Resource=r;info->hAllocation=allocation;info->Size=(r.Size+4095)&~4095u;
        if(!native && (r.Flags&A830_RESOURCE_OWNED)) {
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
        if(native) {
            BOOLEAN primary=(r.Flags&A830_RESOURCE_PRIMARY)!=0;
            info->Alignment=65536;info->PreferredSegment.SegmentId0=primary?1:2;
            info->SupportedReadSegmentSet=info->SupportedWriteSegmentSet=primary?1:2;
            info->FlagsWddm2.AccessedPhysically=0;info->FlagsWddm2.CpuVisible=1;
            info->FlagsWddm2.Cached=r.Rotation==2;
            info->FlagsWddm2.ExplicitResidencyNotification=1;
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
static NTSTATUS ReadFence(A830_ADAPTER *a,A830_CONTEXT *c,A830_FENCE_QUERY *query) {
    if(query->Version!=A830_ABI_VERSION || query->Reserved || c->Adapter!=a || !c->AddressSpace ||
       c->Process!=PsGetCurrentProcess() || (query->Address&7) || query->Address<GPU_CLIENT_VA_BASE || query->Address>>39)return STATUS_INVALID_PARAMETER;
    A830_PROCESS *process=c->AddressSpace;KeWaitForSingleObject(&process->Lock,Executive,KernelMode,FALSE,NULL);
    ULONGLONG physical;NTSTATUS status=A830ResolveGpuAddress(a,process->Root,query->Address,&physical);
    if(NT_SUCCESS(status)) {
        MM_COPY_ADDRESS source;source.PhysicalAddress.QuadPart=physical;SIZE_T copied=0;
        status=MmCopyMemory(&query->Value,source,sizeof(query->Value),MM_COPY_MEMORY_PHYSICAL,&copied);
        if(NT_SUCCESS(status) && copied!=sizeof(query->Value))status=STATUS_PARTIAL_COPY;
    }
    KeReleaseMutex(&process->Lock,FALSE);return status;
}
NTSTATUS APIENTRY A830Escape(HANDLE adapter,const DXGKARG_ESCAPE *p) {
    A830_ADAPTER *a=adapter;
    if(p->hContext && p->PrivateDriverDataSize==sizeof(A830_FENCE_QUERY) && p->pPrivateDriverData)
        return ReadFence(a,p->hContext,p->pPrivateDriverData);
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
    if(!allocation || (!allocation->Buffer.Mdl && (!(allocation->Resource.Flags&A830_RESOURCE_NATIVE) || !allocation->Resident.SegmentId))) {
        if(release){DXGKARGCB_RELEASEHANDLEDATA drop={release,DXGK_HANDLE_ALLOCATION};a->Dxgk.DxgkCbReleaseHandleData(drop);}return STATUS_INVALID_HANDLE;
    }
    address->Address=allocation->Buffer.Mdl?allocation->Buffer.Address:
        (allocation->Resident.SegmentId==1?A830_SEGMENT_ADDRESS:A830_SHARED_ADDRESS)+allocation->Resident.SegmentOffset;
    address->Size=allocation->Buffer.Mdl?allocation->Buffer.Size:allocation->Resource.Size;
    address->Pitch=allocation->Resource.Pitch;address->Format=allocation->Resource.Format;
    if(release){DXGKARGCB_RELEASEHANDLEDATA drop={release,DXGK_HANDLE_ALLOCATION};a->Dxgk.DxgkCbReleaseHandleData(drop);}
    return STATUS_SUCCESS;
}
static PVOID Segment(A830_ADAPTER *a,ULONGLONG offset,SIZE_T bytes) {
    if(offset>=A830_SEGMENT_ADDRESS)offset-=A830_SEGMENT_ADDRESS;
    if(offset>A830_SEGMENT_BYTES || bytes>A830_SEGMENT_BYTES-offset)return NULL;
    return a->Memory.Segment.Cpu+(SIZE_T)offset;
}
static NTSTATUS MapVirtualCpu(A830_ADAPTER *a,ULONGLONG address,SIZE_T bytes,PVOID *map,PMDL *mapping) {
    *map=NULL;*mapping=NULL;
    if(!bytes || bytes>GPU_MAX_BUFFER_SIZE || address>=0x8000000000ULL || bytes>0x8000000000ULL-address)return STATUS_INVALID_PARAMETER;
    ULONG offset=(ULONG)(address&4095);SIZE_T pages=(offset+bytes+4095)/PAGE_SIZE;
    PMDL mdl=IoAllocateMdl((PVOID)(ULONG_PTR)offset,(ULONG)bytes,FALSE,FALSE,NULL);
    if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
    PPFN_NUMBER pfns=MmGetMdlPfnArray(mdl);NTSTATUS status=STATUS_SUCCESS;MEMORY_CACHING_TYPE cache=MmCached;
    ULONGLONG root=(ULONGLONG)InterlockedCompareExchange64((volatile LONG64 *)&a->PagingRoot,0,0);
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
    if(NT_SUCCESS(status) && process && p->PageTableLevel==2) {
        ULONGLONG physical=MmGetPhysicalAddress(p->PageTableAddress.CpuVirtual).QuadPart;
        if(process->System)InterlockedExchange64((volatile LONG64 *)&a->PagingRoot,physical);
        else if(p->Flags.InitialUpdate || !process->Root)process->Root=physical;
    }
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
        dma->Operation=2;dma->Bytes=(ULONG)bytes;dma->Value=p->FillVirtual.FillPattern;
        NTSTATUS status=MapVirtualCpu(a,p->FillVirtual.DestinationVirtualAddress,(SIZE_T)bytes,&dma->Destination,&dma->DestinationMdl);
        if(!NT_SUCCESS(status))return status;
    } else if(p->Operation==DXGK_OPERATION_VIRTUAL_TRANSFER) {
        const DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL *v=&p->TransferVirtual;A830_ALLOCATION *allocation=v->hAllocation;
        if(!v->TransferSizeInBytes || v->TransferSizeInBytes>MAXULONG ||
           (allocation && (v->AllocationOffsetInBytes>allocation->Resource.Size || v->TransferSizeInBytes>allocation->Resource.Size-v->AllocationOffsetInBytes)))return STATUS_INVALID_PARAMETER;
        dma->Operation=3;dma->Bytes=(ULONG)v->TransferSizeInBytes;
        NTSTATUS status=MapVirtualCpu(a,v->SourceVirtualAddress,dma->Bytes,&dma->Source,&dma->SourceMdl);
        if(NT_SUCCESS(status))status=MapVirtualCpu(a,v->DestinationVirtualAddress,dma->Bytes,&dma->Destination,&dma->DestinationMdl);
        if(!NT_SUCCESS(status)){A830ReleasePresent(dma);return status;}
    } else if(p->Operation==DXGK_OPERATION_SIGNAL_MONITORED_FENCE) {
        if(p->SignalMonitoredFence.MonitoredFenceGpuVa&7)return STATUS_INVALID_PARAMETER;
        dma->Operation=8;dma->SourceShared=p->SignalMonitoredFence.MonitoredFenceValue;
        NTSTATUS status=MapVirtualCpu(a,p->SignalMonitoredFence.MonitoredFenceGpuVa,8,&dma->Destination,&dma->DestinationMdl);
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
