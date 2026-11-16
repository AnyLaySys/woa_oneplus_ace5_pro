#include "adapter.h"

ULONG A830RefreshRate(const A830_ADAPTER *a) {return a->RefreshRate==120?120u:60u;}
D3DKMDT_VIDEO_SIGNAL_INFO A830Signal(const A830_ADAPTER *a) {
    D3DKMDT_VIDEO_SIGNAL_INFO s={0};s.VideoStandard=D3DKMDT_VSS_OTHER;
    s.TotalSize.cx=s.ActiveSize.cx=a->DisplayInfo.Width;s.TotalSize.cy=s.ActiveSize.cy=a->DisplayInfo.Height;
    ULONG hz=A830RefreshRate(a);
    s.PixelRate=(UINT64)s.TotalSize.cx*s.TotalSize.cy*hz;s.VSyncFreq.Numerator=hz;s.VSyncFreq.Denominator=1;
    s.HSyncFreq.Numerator=s.TotalSize.cy*hz;s.HSyncFreq.Denominator=1;s.ScanLineOrdering=D3DDDI_VSSLO_PROGRESSIVE;

    return s;
}

BOOLEAN A830RotationSupported(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation) {
    if(rotation==D3DKMDT_VPPR_IDENTITY)return TRUE;
    return rotation>=D3DKMDT_VPPR_ROTATE90 && rotation<=D3DKMDT_VPPR_ROTATE270;
}
BOOLEAN A830PathSupport(D3DKMDT_VIDPN_PRESENT_PATH *path,const DXGKARG_ENUMVIDPNCOFUNCMODALITY *args) {
    BOOLEAN changed=FALSE;
    BOOLEAN pivot=args->EnumPivot.VidPnSourceId==path->VidPnSourceId && args->EnumPivot.VidPnTargetId==path->VidPnTargetId;
    if(!(pivot && args->EnumPivotType==D3DKMDT_EPT_SCALING) && path->ContentTransformation.Scaling==D3DKMDT_VPPS_UNPINNED) {
        RtlZeroMemory(&path->ContentTransformation.ScalingSupport,sizeof(path->ContentTransformation.ScalingSupport));
        path->ContentTransformation.ScalingSupport.Identity=1;changed=TRUE;
    }
    if(!(pivot && args->EnumPivotType==D3DKMDT_EPT_ROTATION) && path->ContentTransformation.Rotation==D3DKMDT_VPPR_UNPINNED) {
        RtlZeroMemory(&path->ContentTransformation.RotationSupport,sizeof(path->ContentTransformation.RotationSupport));
        path->ContentTransformation.RotationSupport.Identity=1;path->ContentTransformation.RotationSupport.Offset0=1;changed=TRUE;
        {path->ContentTransformation.RotationSupport.Rotate90=1;path->ContentTransformation.RotationSupport.Rotate180=1;path->ContentTransformation.RotationSupport.Rotate270=1;}
    }
    return changed;
}
NTSTATUS A830DisplayLayout(const DXGK_DISPLAY_INFORMATION *d,SIZE_T *bytes) {
    if(!d->Width || !d->Height || d->Width>16384 || d->Height>16384 || d->Pitch<d->Width*4 ||
        !d->PhysicAddress.QuadPart || (d->ColorFormat!=D3DDDIFMT_A8R8G8B8 && d->ColorFormat!=D3DDDIFMT_X8R8G8B8) ||
        d->Pitch>A830_SEGMENT_BYTES/d->Height)return STATUS_DEVICE_CONFIGURATION_ERROR;
    *bytes=(SIZE_T)d->Pitch*d->Height;return STATUS_SUCCESS;
}
NTSTATUS APIENTRY A830Query(HANDLE adapter,const DXGKARG_QUERYADAPTERINFO *q) {
    A830_ADAPTER *a=adapter;
    if(!q->pOutputData)return STATUS_INVALID_PARAMETER;
    if(q->Type==DXGKQAITYPE_DRIVERCAPS) {
        if(q->OutputDataSize<sizeof(DXGK_DRIVERCAPS))return STATUS_BUFFER_TOO_SMALL;
        DXGK_DRIVERCAPS *c=q->pOutputData;RtlZeroMemory(c,sizeof(*c));

        c->HighestAcceptableAddress.QuadPart=0xFFFFFFFFFFF;
        c->MemoryManagementCaps.NonCpuVisiblePrimary=1;
        c->FlipCaps.FlipOnVSyncMmIo=1;
        c->MaxQueuedFlipOnVSync=1;
        c->MaxAllocationListSlotId=127;c->SchedulingCaps.MultiEngineAware=1;
        c->SchedulingCaps.PreemptionAware=1;c->SchedulingCaps.CancelCommandAware=1;
        c->GpuEngineTopology.NbAsymetricProcessingNodes=1;c->WDDMVersion=DXGKDDI_WDDMv2;
        c->PreemptionCaps.GraphicsPreemptionGranularity=D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->PreemptionCaps.ComputePreemptionGranularity=D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->SupportNonVGA=TRUE;c->SupportPerEngineTDR=TRUE;c->SupportSmoothRotation=TRUE;return STATUS_SUCCESS;
    }
    if(q->Type==DXGKQAITYPE_WDDMDEVICECAPS) {
        if(q->OutputDataSize<sizeof(DXGK_WDDMDEVICECAPS))return STATUS_BUFFER_TOO_SMALL;
        DXGK_WDDMDEVICECAPS *c=q->pOutputData;RtlZeroMemory(c,sizeof(*c));c->WDDMVersion=DXGKDDI_WDDMv2;return STATUS_SUCCESS;
    }
    if(q->Type==DXGKQAITYPE_PHYSICALADAPTERCAPS) {
        if(q->OutputDataSize<sizeof(DXGK_PHYSICALADAPTERCAPS))return STATUS_BUFFER_TOO_SMALL;
        if(q->InputDataSize!=sizeof(DXGK_QUERYPHYSICALADAPTERCAPSIN) || !q->pInputData ||
            ((DXGK_QUERYPHYSICALADAPTERCAPSIN *)q->pInputData)->PhysicalAdapterIndex)return STATUS_INVALID_PARAMETER;
        if(!a->Dxgk.DeviceHandle)return STATUS_DEVICE_NOT_READY;
        DXGK_PHYSICALADAPTERCAPS *c=q->pOutputData;RtlZeroMemory(c,sizeof(*c));c->NumExecutionNodes=1;
        c->DxgkPhysicalAdapterHandle=a->Dxgk.DeviceHandle;
        return STATUS_SUCCESS;
    }
    if(q->Type==DXGKQAITYPE_QUERYSEGMENT4) {
        if(q->OutputDataSize<sizeof(DXGK_QUERYSEGMENTOUT4))return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT4 *s=q->pOutputData;
        ULONG segments=2;
        if(!s->pSegmentDescriptor){s->NbSegment=segments;return STATUS_SUCCESS;}
        if(s->NbSegment<segments || s->SegmentDescriptorStride<sizeof(DXGK_SEGMENTDESCRIPTOR4))return STATUS_INVALID_PARAMETER;
        DXGK_SEGMENTDESCRIPTOR4 *d=(PVOID)s->pSegmentDescriptor;RtlZeroMemory(d,sizeof(*d));
        d->Flags.CpuVisible=1;
        d->Flags.PopulatedFromSystemMemory=1;
        d->BaseAddress.QuadPart=A830_SEGMENT_ADDRESS;d->CpuTranslatedAddress.QuadPart=a->Memory.Segment.Physical;
        d->Size=A830_SEGMENT_BYTES;d->CommitLimit=A830_SEGMENT_BYTES;
         {
            DXGK_SEGMENTDESCRIPTOR4 *aperture=(PVOID)((PUCHAR)s->pSegmentDescriptor+s->SegmentDescriptorStride);
            RtlZeroMemory(aperture,sizeof(*aperture));aperture->Flags.Aperture=1;
            aperture->Flags.CacheCoherent=1;
            aperture->BaseAddress.QuadPart=A830_SHARED_ADDRESS;
            aperture->Size=aperture->CommitLimit=a->Memory.Shared.Bytes;
        }
        s->NbSegment=segments;s->PagingBufferSegmentId=0;s->PagingBufferSize=A830_DMA_BYTES;s->PagingBufferPrivateDataSize=sizeof(A830_DMA);
        return STATUS_SUCCESS;
    }
    if(q->Type==DXGKQAITYPE_UMDRIVERPRIVATE) {
        if(q->OutputDataSize<sizeof(ULONG))return STATUS_BUFFER_TOO_SMALL;
        if(q->OutputDataSize>=sizeof(A830_ADAPTER_INFO)) {
            A830_ADAPTER_INFO info={A830_ABI_VERSION,(3u)|((ULONG)a->Generation<<16),(ULONG)a->Failed,0x44050001};
            RtlCopyMemory(q->pOutputData,&info,sizeof(info));return STATUS_SUCCESS;
        }
        *(PULONG)q->pOutputData=A830_ABI_VERSION;return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}
NTSTATUS APIENTRY A830StandardAllocation(HANDLE adapter,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *p) {

    A830_RESOURCE r={A830_ABI_VERSION,0,0,0,0,0};UINT *pitch=NULL;
    if(p->StandardAllocationType==D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE && p->pCreateStagingSurfaceData) {
        D3DKMDT_STAGINGSURFACEDATA *s=p->pCreateStagingSurfaceData;
        r.Width=s->Width;r.Height=s->Height;r.Format=D3DDDIFMT_X8R8G8B8;pitch=&s->Pitch;
    } else if(p->StandardAllocationType==D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE && p->pCreateShadowSurfaceData) {
        D3DKMDT_SHADOWSURFACEDATA *s=p->pCreateShadowSurfaceData;
        r.Width=s->Width;r.Height=s->Height;r.Format=s->Format;pitch=&s->Pitch;
    } else if(p->StandardAllocationType==D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE && p->pCreateSharedPrimarySurfaceData) {
        D3DKMDT_SHAREDPRIMARYSURFACEDATA *s=p->pCreateSharedPrimarySurfaceData;
        if(s->VidPnSourceId)return STATUS_INVALID_PARAMETER;
        r.Width=s->Width;r.Height=s->Height;r.Format=s->Format;
    } else return STATUS_NOT_SUPPORTED;
    if(adapter &&
        p->StandardAllocationType!=D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE)r.Flags=1;
    if(r.Format!=D3DDDIFMT_X8R8G8B8 && r.Format!=D3DDDIFMT_A8R8G8B8)return STATUS_NOT_SUPPORTED;
    ULONGLONG limit=(r.Flags&1)?GPU_BUFFER_LIMIT:A830_SEGMENT_BYTES;
    if(adapter && (r.Flags&1) && ((A830_ADAPTER *)adapter)->Memory.Shared.Bytes<limit)
        limit=((A830_ADAPTER *)adapter)->Memory.Shared.Bytes;
    if(!r.Width || !r.Height || r.Width>limit/4)return STATUS_INVALID_PARAMETER;
    r.Pitch=(r.Width*4+255)&~255u;
    if(r.Height>limit/r.Pitch)return STATUS_INVALID_PARAMETER;
    r.Size=r.Pitch*r.Height;

    if(p->pAllocationPrivateDriverData && p->AllocationPrivateDriverDataSize<sizeof(r))return STATUS_BUFFER_TOO_SMALL;
    p->AllocationPrivateDriverDataSize=sizeof(r);p->ResourcePrivateDriverDataSize=0;
    if(p->pAllocationPrivateDriverData){RtlCopyMemory(p->pAllocationPrivateDriverData,&r,sizeof(r));if(pitch)*pitch=r.Pitch;}
    return STATUS_SUCCESS;
}
