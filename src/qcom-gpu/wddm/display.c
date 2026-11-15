#include "adapter.h"
#include "dpu.h"

NTSTATUS A830DisplayStart(A830_ADAPTER *a) {
    /* Read once at PASSIVE startup; mode enumeration and current-mode queries
       use the same bounded value. The inherited panel is already 120 Hz. */
    a->RefreshRate=60;
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Services\\a830_native");
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"RefreshRate");OBJECT_ATTRIBUTES attributes;HANDLE key;
    InitializeObjectAttributes(&attributes,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    if(NT_SUCCESS(ZwOpenKey(&key,KEY_QUERY_VALUE,&attributes))) {
        UCHAR storage[sizeof(KEY_VALUE_PARTIAL_INFORMATION)+sizeof(ULONG)];ULONG used=0;
        PKEY_VALUE_PARTIAL_INFORMATION value=(PVOID)storage;
        NTSTATUS read=ZwQueryValueKey(key,&name,KeyValuePartialInformation,storage,sizeof(storage),&used);ZwClose(key);
        if(NT_SUCCESS(read) && value->Type==REG_DWORD && value->DataLength==sizeof(ULONG) && *(PULONG)value->Data==120)a->RefreshRate=120;
    }
    NTSTATUS status=a->Dxgk.DxgkCbAcquirePostDisplayOwnership(a->Dxgk.DeviceHandle,&a->DisplayInfo);
    if(!NT_SUCCESS(status))return status;
    DXGK_DISPLAY_INFORMATION *d=&a->DisplayInfo;
    status=A830DisplayLayout(d,&a->FramebufferSize);if(!NT_SUCCESS(status))return status;
    A830Record(L"DisplayWidth",d->Width);A830Record(L"DisplayHeight",d->Height);A830Record(L"DisplayPitch",d->Pitch);
    a->Framebuffer=MmMapIoSpaceEx(d->PhysicAddress,a->FramebufferSize,PAGE_READWRITE|PAGE_WRITECOMBINE);
    if(!a->Framebuffer)return STATUS_INSUFFICIENT_RESOURCES;
    a->Visible=TRUE;return STATUS_SUCCESS;
}
VOID A830DisplayStop(A830_ADAPTER *a) {
    if(!NT_SUCCESS(A830DpuStop(a)))return;
    if(a->Framebuffer)MmUnmapIoSpace(a->Framebuffer,a->FramebufferSize);
    a->Framebuffer=NULL;a->FramebufferSize=0;
}
static NTSTATUS Interface(A830_ADAPTER *a,D3DKMDT_HVIDPN h,const DXGK_VIDPN_INTERFACE **v) {
    return a->Dxgk.DxgkCbQueryVidPnInterface(h,DXGK_VIDPN_INTERFACE_VERSION_V1,v);
}
static NTSTATUS SourceModes(A830_ADAPTER *a,D3DKMDT_HVIDPN h,const DXGK_VIDPN_INTERFACE *v) {
    D3DKMDT_HVIDPNSOURCEMODESET set;const DXGK_VIDPNSOURCEMODESET_INTERFACE *i;
    NTSTATUS acquired=v->pfnAcquireSourceModeSet(h,0,&set,&i);
    if(NT_SUCCESS(acquired)) {
        const D3DKMDT_VIDPN_SOURCE_MODE *pinned=NULL;NTSTATUS pinnedStatus=i->pfnAcquirePinnedModeInfo(set,&pinned);
        if(NT_SUCCESS(pinnedStatus) && pinned) {
            BOOLEAN valid=pinned->Type==D3DKMDT_RMT_GRAPHICS && pinned->Format.Graphics.PrimSurfSize.cx==a->DisplayInfo.Width &&
                pinned->Format.Graphics.PrimSurfSize.cy==a->DisplayInfo.Height && pinned->Format.Graphics.PixelFormat==a->DisplayInfo.ColorFormat;
            i->pfnReleaseModeInfo(set,pinned);v->pfnReleaseSourceModeSet(h,set);
            return valid?STATUS_SUCCESS:STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
        }
        v->pfnReleaseSourceModeSet(h,set);
        if(!NT_SUCCESS(pinnedStatus))return pinnedStatus;
    }
    NTSTATUS status=v->pfnCreateNewSourceModeSet(h,0,&set,&i);if(!NT_SUCCESS(status))return status;
    D3DKMDT_VIDPN_SOURCE_MODE *m=NULL;status=i->pfnCreateNewModeInfo(set,&m);
    if(NT_SUCCESS(status)) {
        m->Type=D3DKMDT_RMT_GRAPHICS;m->Format.Graphics.PrimSurfSize.cx=a->DisplayInfo.Width;
        m->Format.Graphics.PrimSurfSize.cy=a->DisplayInfo.Height;m->Format.Graphics.VisibleRegionSize=m->Format.Graphics.PrimSurfSize;
        m->Format.Graphics.Stride=a->DisplayOnly?a->DisplayInfo.Pitch:(a->DisplayInfo.Width*4+255)&~255u;m->Format.Graphics.PixelFormat=a->DisplayInfo.ColorFormat;
        m->Format.Graphics.ColorBasis=D3DKMDT_CB_SCRGB;m->Format.Graphics.PixelValueAccessMode=D3DKMDT_PVAM_DIRECT;
        status=i->pfnAddMode(set,m);if(!NT_SUCCESS(status))i->pfnReleaseModeInfo(set,m);
    }
    if(NT_SUCCESS(status))status=v->pfnAssignSourceModeSet(h,0,set);
    if(!NT_SUCCESS(status))v->pfnReleaseSourceModeSet(h,set);return status;
}
static NTSTATUS TargetModes(A830_ADAPTER *a,D3DKMDT_HVIDPN h,const DXGK_VIDPN_INTERFACE *v) {
    D3DKMDT_HVIDPNTARGETMODESET set;const DXGK_VIDPNTARGETMODESET_INTERFACE *i;
    NTSTATUS acquired=v->pfnAcquireTargetModeSet(h,0,&set,&i);
    if(NT_SUCCESS(acquired)) {
        const D3DKMDT_VIDPN_TARGET_MODE *pinned=NULL;NTSTATUS pinnedStatus=i->pfnAcquirePinnedModeInfo(set,&pinned);
        if(NT_SUCCESS(pinnedStatus) && pinned) {
            BOOLEAN valid=pinned->VideoSignalInfo.ActiveSize.cx==a->DisplayInfo.Width && pinned->VideoSignalInfo.ActiveSize.cy==a->DisplayInfo.Height;
            i->pfnReleaseModeInfo(set,pinned);v->pfnReleaseTargetModeSet(h,set);
            return valid?STATUS_SUCCESS:STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET_MODE;
        }
        v->pfnReleaseTargetModeSet(h,set);
        if(!NT_SUCCESS(pinnedStatus))return pinnedStatus;
    }
    NTSTATUS status=v->pfnCreateNewTargetModeSet(h,0,&set,&i);if(!NT_SUCCESS(status))return status;
    D3DKMDT_VIDPN_TARGET_MODE *m=NULL;status=i->pfnCreateNewModeInfo(set,&m);
    if(NT_SUCCESS(status)) {
        m->VideoSignalInfo=A830Signal(a);m->Preference=D3DKMDT_MP_PREFERRED;
        status=i->pfnAddMode(set,m);if(!NT_SUCCESS(status))i->pfnReleaseModeInfo(set,m);
    }
    if(NT_SUCCESS(status))status=v->pfnAssignTargetModeSet(h,0,set);
    if(!NT_SUCCESS(status))v->pfnReleaseTargetModeSet(h,set);return status;
}
static NTSTATUS APIENTRY Supported(HANDLE adapter,DXGKARG_ISSUPPORTEDVIDPN *p) {
    A830_ADAPTER *a=adapter;p->IsVidPnSupported=FALSE;if(!p->hDesiredVidPn){p->IsVidPnSupported=TRUE;return STATUS_SUCCESS;}
    const DXGK_VIDPN_INTERFACE *v;NTSTATUS status=Interface(a,p->hDesiredVidPn,&v);if(!NT_SUCCESS(status))return status;
    D3DKMDT_HVIDPNTOPOLOGY topology;const DXGK_VIDPNTOPOLOGY_INTERFACE *t;
    status=v->pfnGetTopology(p->hDesiredVidPn,&topology,&t);if(!NT_SUCCESS(status))return status;
    SIZE_T count=0;status=t->pfnGetNumPaths(topology,&count);if(!NT_SUCCESS(status))return status;
    if(!count){p->IsVidPnSupported=TRUE;return STATUS_SUCCESS;}if(count!=1)return STATUS_SUCCESS;
    const D3DKMDT_VIDPN_PRESENT_PATH *path;
    status=t->pfnAcquireFirstPathInfo(topology,&path);if(!NT_SUCCESS(status))return status;
    p->IsVidPnSupported=!path->VidPnSourceId && !path->VidPnTargetId &&
        (A830RotationSupported(a,path->ContentTransformation.Rotation) || path->ContentTransformation.Rotation==D3DKMDT_VPPR_UNPINNED) &&
        (path->ContentTransformation.Scaling==D3DKMDT_VPPS_IDENTITY || path->ContentTransformation.Scaling==D3DKMDT_VPPS_UNPINNED);
    A830Record(L"SupportedRotation",path->ContentTransformation.Rotation);A830Record(L"SupportedScaling",path->ContentTransformation.Scaling);
    A830Record(L"VidPnSupported",p->IsVidPnSupported);
    t->pfnReleasePathInfo(topology,path);return STATUS_SUCCESS;
}
static NTSTATUS CofuncModes(HANDLE adapter,const DXGKARG_ENUMVIDPNCOFUNCMODALITY *p) {
    A830_ADAPTER *a=adapter;const DXGK_VIDPN_INTERFACE *v;
    NTSTATUS status=Interface(a,p->hConstrainingVidPn,&v);if(!NT_SUCCESS(status))return status;
    if(p->EnumPivotType!=D3DKMDT_EPT_VIDPNSOURCE){status=SourceModes(a,p->hConstrainingVidPn,v);A830Record(L"SourceModesStatus",status);}
    if(NT_SUCCESS(status) && p->EnumPivotType!=D3DKMDT_EPT_VIDPNTARGET){status=TargetModes(a,p->hConstrainingVidPn,v);A830Record(L"TargetModesStatus",status);}
    if(NT_SUCCESS(status)) {
        D3DKMDT_HVIDPNTOPOLOGY topology;const DXGK_VIDPNTOPOLOGY_INTERFACE *t;
        status=v->pfnGetTopology(p->hConstrainingVidPn,&topology,&t);if(!NT_SUCCESS(status))return status;
        const D3DKMDT_VIDPN_PRESENT_PATH *path;
        status=t->pfnAcquireFirstPathInfo(topology,&path);
        if(status==STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET || status==STATUS_GRAPHICS_DATASET_IS_EMPTY)return STATUS_SUCCESS;
        if(!NT_SUCCESS(status))return status;
        D3DKMDT_VIDPN_PRESENT_PATH supported=*path;
        if(A830PathSupport(a,&supported,p))status=t->pfnUpdatePathSupportInfo(topology,&supported);
        t->pfnReleasePathInfo(topology,path);
    }
    return status;
}
static NTSTATUS APIENTRY Cofunc(HANDLE adapter,const DXGKARG_ENUMVIDPNCOFUNCMODALITY *p) {
    NTSTATUS status=CofuncModes(adapter,p);A830Record(L"CofuncPivot",p->EnumPivotType);A830Record(L"CofuncStatus",status);return status;
}
static NTSTATUS APIENTRY Recommend(HANDLE adapter,const DXGKARG_RECOMMENDFUNCTIONALVIDPN *p) {
    UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(p);return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}
static NTSTATUS APIENTRY Commit(HANDLE adapter,const DXGKARG_COMMITVIDPN *p) {
    A830_ADAPTER *a=adapter;if(p->AffectedVidPnSourceId && p->AffectedVidPnSourceId!=D3DDDI_ID_ALL)return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    const DXGK_VIDPN_INTERFACE *v;NTSTATUS status=Interface(a,p->hFunctionalVidPn,&v);if(!NT_SUCCESS(status))return status;
    D3DKMDT_HVIDPNTOPOLOGY topology;const DXGK_VIDPNTOPOLOGY_INTERFACE *t;
    status=v->pfnGetTopology(p->hFunctionalVidPn,&topology,&t);if(!NT_SUCCESS(status))return status;
    SIZE_T count=0;status=t->pfnGetNumPaths(topology,&count);if(!NT_SUCCESS(status))return status;
    A830Record(L"CommitPaths",(ULONG)count);
    if(!count){a->Visible=FALSE;a->ScanoutPitch=0;InterlockedExchange(&a->FlipPending,0);A830Record(L"CommitStatus",STATUS_SUCCESS);return STATUS_SUCCESS;}
    const D3DKMDT_VIDPN_PRESENT_PATH *path=NULL;
    status=t->pfnAcquireFirstPathInfo(topology,&path);if(!NT_SUCCESS(status))return status;
    BOOLEAN valid=A830RotationSupported(a,path->ContentTransformation.Rotation);
    A830Record(L"ActiveRotation",path->ContentTransformation.Rotation);
    t->pfnReleasePathInfo(topology,path);
    if(!valid)return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    D3DKMDT_HVIDPNSOURCEMODESET set;const DXGK_VIDPNSOURCEMODESET_INTERFACE *i;
    status=v->pfnAcquireSourceModeSet(p->hFunctionalVidPn,0,&set,&i);if(!NT_SUCCESS(status))return status;
    const D3DKMDT_VIDPN_SOURCE_MODE *m=NULL;status=i->pfnAcquirePinnedModeInfo(set,&m);
    if(status==STATUS_GRAPHICS_MODE_NOT_PINNED && !m)status=STATUS_SUCCESS;
    if(NT_SUCCESS(status) && m) {
        if(m->Type!=D3DKMDT_RMT_GRAPHICS || m->Format.Graphics.PrimSurfSize.cx!=a->DisplayInfo.Width ||
            m->Format.Graphics.PrimSurfSize.cy!=a->DisplayInfo.Height || m->Format.Graphics.PixelFormat!=a->DisplayInfo.ColorFormat)
            status=STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
        i->pfnReleaseModeInfo(set,m);
    }
    v->pfnReleaseSourceModeSet(p->hFunctionalVidPn,set);A830Record(L"CommitStatus",status);return status;
}
static NTSTATUS APIENTRY Address(HANDLE adapter,const DXGKARG_SETVIDPNSOURCEADDRESS *p) {
    A830_ADAPTER *a=adapter;
    A830Count(&a->Telemetry.AddressCalls,1);
    if(KeGetCurrentIrql()>DISPATCH_LEVEL)A830Count(&a->Telemetry.AddressDirql,1);
    for(ULONG flags=p->Flags.Value,i=0;flags;flags>>=1,i++)if(flags&1)A830Count(a->Telemetry.AddressFlags+i,1);
    if(p->VidPnSourceId || !a->Framebuffer || !a->Memory.Segment.Cpu)return STATUS_INVALID_PARAMETER;
    A830_ALLOCATION *allocation=p->hAllocation;
    if(allocation && ((allocation->Resource.Format!=D3DDDIFMT_X8R8G8B8 && allocation->Resource.Format!=D3DDDIFMT_A8R8G8B8) || allocation->Resource.Pitch<a->DisplayInfo.Width*4 || allocation->Resource.Width!=a->DisplayInfo.Width ||
        allocation->Resource.Height!=a->DisplayInfo.Height || allocation->Resource.Size<(SIZE_T)allocation->Resource.Pitch*a->DisplayInfo.Height)) {
        A830Count(&a->Telemetry.AddressRejected,1);
        InterlockedExchange64((volatile LONG64 *)&a->Telemetry.RejectedWidth,allocation->Resource.Width);
        InterlockedExchange64((volatile LONG64 *)&a->Telemetry.RejectedHeight,allocation->Resource.Height);
        InterlockedExchange64((volatile LONG64 *)&a->Telemetry.RejectedPitch,allocation->Resource.Pitch);
        return STATUS_INVALID_PARAMETER;
    }
    ULONG pitch=allocation?allocation->Resource.Pitch:(a->DisplayInfo.Width*4+255)&~255u;
    SIZE_T bytes=(SIZE_T)pitch*a->DisplayInfo.Height;
    ULONGLONG offset=(ULONGLONG)p->PrimaryAddress.QuadPart;
    if(allocation && allocation->Buffer.Mdl) {
        if(A830DpuEnabled(a)) {
            if(A830GpuScanoutBusy(a) || !allocation->Buffer.Contiguous || allocation->Buffer.Physical>MAXULONG ||
               bytes-1>MAXULONG-allocation->Buffer.Physical)
                return A830GpuScanoutQueue(a,allocation,0,pitch,(ULONGLONG)p->PrimaryAddress.QuadPart);
            /* Bridge teardown sees either this transient pin or the published
               pending/active physical address; there is no unowned window. */
            InterlockedIncrement(&allocation->ScanoutReferences);
            NTSTATUS status=A830DpuQueue(a,allocation->Buffer.Physical,pitch,(ULONGLONG)p->PrimaryAddress.QuadPart);
            InterlockedDecrement(&allocation->ScanoutReferences);return status;
        }
        if(A830GpuScanoutEnabled(a)) {
            a->ScanoutPitch=pitch;
            return A830GpuScanoutQueue(a,allocation,0,pitch,(ULONGLONG)p->PrimaryAddress.QuadPart);
        }
        a->ScanoutOffset=0;a->ScanoutPitch=pitch;a->ScanoutAddress=(ULONGLONG)p->PrimaryAddress.QuadPart;
        a->ScanoutCpu=allocation->Buffer.Cpu;
        InterlockedExchange(&a->FlipPending,1);A830WakeDisplay(a);return STATUS_SUCCESS;
    }
    if(offset>=A830_SEGMENT_ADDRESS)offset-=A830_SEGMENT_ADDRESS;
    if(p->PrimarySegment!=1 || offset>A830_SEGMENT_BYTES || bytes>A830_SEGMENT_BYTES-offset)return STATUS_INVALID_PARAMETER;
    if(A830DpuEnabled(a)) {
        ULONGLONG physical=a->Memory.Segment.Physical+offset;
        if(A830GpuScanoutBusy(a) || physical>MAXULONG || bytes-1>MAXULONG-physical)
            return A830GpuScanoutQueue(a,NULL,offset,pitch,(ULONGLONG)p->PrimaryAddress.QuadPart);
        return A830DpuQueue(a,physical,pitch,(ULONGLONG)p->PrimaryAddress.QuadPart);
    }
    if(A830GpuScanoutEnabled(a)) {
        a->ScanoutPitch=pitch;
        return A830GpuScanoutQueue(a,NULL,offset,pitch,(ULONGLONG)p->PrimaryAddress.QuadPart);
    }
    a->ScanoutOffset=offset;a->ScanoutPitch=pitch;a->ScanoutAddress=(ULONGLONG)p->PrimaryAddress.QuadPart;
    a->ScanoutCpu=a->Memory.Segment.Cpu+(SIZE_T)offset;
    InterlockedExchange(&a->FlipPending,1);A830WakeDisplay(a);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Visible(HANDLE adapter,const DXGKARG_SETVIDPNSOURCEVISIBILITY *p) {
    A830_ADAPTER *a=adapter;if(p->VidPnSourceId && p->VidPnSourceId!=D3DDDI_ID_ALL)return STATUS_INVALID_PARAMETER;
    a->Visible=p->Visible;A830Record(L"SourceVisible",p->Visible);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Update(HANDLE adapter,const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH *p) {
    A830_ADAPTER *a=adapter;
    if(p->VidPnPresentPathInfo.VidPnSourceId || p->VidPnPresentPathInfo.VidPnTargetId)return STATUS_INVALID_PARAMETER;
    if(!A830RotationSupported(a,p->VidPnPresentPathInfo.ContentTransformation.Rotation) ||
        p->VidPnPresentPathInfo.ContentTransformation.Scaling!=D3DKMDT_VPPS_IDENTITY)return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    A830Record(L"ActiveRotation",p->VidPnPresentPathInfo.ContentTransformation.Rotation);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY MonitorModes(HANDLE adapter,const DXGKARG_RECOMMENDMONITORMODES *p) {
    A830_ADAPTER *a=adapter;if(p->VideoPresentTargetId)return STATUS_INVALID_PARAMETER;
    D3DKMDT_MONITOR_SOURCE_MODE *m;const DXGK_MONITORSOURCEMODESET_INTERFACE *i=p->pMonitorSourceModeSetInterface;
    NTSTATUS status=i->pfnCreateNewModeInfo(p->hMonitorSourceModeSet,&m);if(!NT_SUCCESS(status))return status;
    m->VideoSignalInfo=A830Signal(a);m->Origin=D3DKMDT_MCO_DRIVER;m->Preference=D3DKMDT_MP_PREFERRED;
    m->ColorBasis=D3DKMDT_CB_SRGB;m->ColorCoeffDynamicRanges.FirstChannel=8;m->ColorCoeffDynamicRanges.SecondChannel=8;
    m->ColorCoeffDynamicRanges.ThirdChannel=8;m->ColorCoeffDynamicRanges.FourthChannel=8;
    status=i->pfnAddMode(p->hMonitorSourceModeSet,m);A830Record(L"MonitorModesStatus",status);if(!NT_SUCCESS(status))i->pfnReleaseModeInfo(p->hMonitorSourceModeSet,m);return status;
}
static NTSTATUS APIENTRY HardwareCaps(HANDLE adapter,DXGKARG_QUERYVIDPNHWCAPABILITY *p) {
    A830_ADAPTER *a=adapter;if(p->SourceId || p->TargetId)return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&p->VidPnHWCaps,sizeof(p->VidPnHWCaps));p->VidPnHWCaps.DriverRotation=!a->DisplayOnly && !a->RenderOnly;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Palette(HANDLE adapter,const DXGKARG_SETPALETTE *p) {
    UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(p);return STATUS_NOT_SUPPORTED;
}
static NTSTATUS APIENTRY PointerPosition(HANDLE adapter,const DXGKARG_SETPOINTERPOSITION *p) {
    UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(p);return STATUS_NOT_SUPPORTED;
}
static NTSTATUS APIENTRY PointerShape(HANDLE adapter,const DXGKARG_SETPOINTERSHAPE *p) {
    UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(p);return STATUS_NOT_SUPPORTED;
}
static NTSTATUS APIENTRY ScanLine(HANDLE adapter,DXGKARG_GETSCANLINE *p) {
    UNREFERENCED_PARAMETER(adapter);
    if(p->VidPnTargetId)return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS StopOwnership(PVOID context,D3DDDI_VIDEO_PRESENT_TARGET_ID target,DXGK_DISPLAY_INFORMATION *display) {
    A830_ADAPTER *a=context;if(target && target!=D3DDDI_ID_UNINITIALIZED)return STATUS_INVALID_PARAMETER;
    *display=a->DisplayInfo;return A830AdapterStop(a);
}
static NTSTATUS SystemEnable(PVOID context,D3DDDI_VIDEO_PRESENT_TARGET_ID target,PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS flags,UINT *width,UINT *height,D3DDDIFORMAT *format) {
    UNREFERENCED_PARAMETER(flags);A830_ADAPTER *a=context;
    if((target && target!=D3DDDI_ID_UNINITIALIZED) || !a->Framebuffer)return STATUS_DEVICE_NOT_READY;
    *width=a->DisplayInfo.Width;*height=a->DisplayInfo.Height;*format=a->DisplayInfo.ColorFormat;return STATUS_SUCCESS;
}
static VOID SystemWrite(PVOID context,PVOID source,UINT width,UINT height,UINT stride,UINT x,UINT y) {
    A830_ADAPTER *a=context;
    if(!a->Framebuffer || !source || x>=a->DisplayInfo.Width || y>=a->DisplayInfo.Height || width>stride/4)return;
    width=min(width,a->DisplayInfo.Width-x);height=min(height,a->DisplayInfo.Height-y);
    for(UINT row=0;row<height;row++)RtlCopyMemory(a->Framebuffer+(SIZE_T)(row+y)*a->DisplayInfo.Pitch+(SIZE_T)x*4,
        (PUCHAR)source+(SIZE_T)row*stride,(SIZE_T)width*4);
    KeMemoryBarrier();
}
VOID A830DisplayDdi(DRIVER_INITIALIZATION_DATA *d) {
    d->DxgkDdiSetPalette=Palette;d->DxgkDdiSetPointerPosition=PointerPosition;d->DxgkDdiSetPointerShape=PointerShape;
    d->DxgkDdiGetScanLine=ScanLine;
    d->DxgkDdiIsSupportedVidPn=Supported;d->DxgkDdiRecommendFunctionalVidPn=Recommend;
    d->DxgkDdiEnumVidPnCofuncModality=Cofunc;d->DxgkDdiCommitVidPn=Commit;
    d->DxgkDdiSetVidPnSourceAddress=Address;d->DxgkDdiSetVidPnSourceVisibility=Visible;
    d->DxgkDdiUpdateActiveVidPnPresentPath=Update;d->DxgkDdiRecommendMonitorModes=MonitorModes;
    d->DxgkDdiQueryVidPnHWCapability=HardwareCaps;d->DxgkDdiStopDeviceAndReleasePostDisplayOwnership=StopOwnership;
    d->DxgkDdiSystemDisplayEnable=SystemEnable;d->DxgkDdiSystemDisplayWrite=SystemWrite;
}
static NTSTATUS APIENTRY PresentDisplayOnly(HANDLE adapter,const DXGKARG_PRESENT_DISPLAYONLY *p) {
    A830_ADAPTER *a=adapter;if(p->VidPnSourceId || !a->Framebuffer || p->BytesPerPixel!=4)return STATUS_INVALID_PARAMETER;
    if(!a->Visible)return STATUS_SUCCESS;
    NTSTATUS status=STATUS_SUCCESS;DXGK_DISPLAY_INFORMATION *d=&a->DisplayInfo;
    __try {
        for(ULONG i=0;i<p->NumMoves && NT_SUCCESS(status);i++)status=A830MoveFrame(a->Framebuffer,d->Pitch,d->Width,d->Height,&p->pMoves[i].SourcePoint,&p->pMoves[i].DestRect);
        for(ULONG i=0;i<p->NumDirtyRects && NT_SUCCESS(status);i++)status=A830BlitFrame(a->Framebuffer,d->Pitch,d->Width,d->Height,p->pSource,p->Pitch,p->pDirtyRect+i);
        if(!p->NumMoves && !p->NumDirtyRects) {
            RECT full={0,0,(LONG)d->Width,(LONG)d->Height};status=A830BlitFrame(a->Framebuffer,d->Pitch,d->Width,d->Height,p->pSource,p->Pitch,&full);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER){status=GetExceptionCode();}
    KeMemoryBarrier();ULONG count=(ULONG)InterlockedIncrement((volatile LONG *)&a->PresentCount);
    if(count==1 || !NT_SUCCESS(status)){A830Record(L"PresentStatus",status);A830Record(L"PresentCount",count);}
    return status;
}
VOID A830DisplayOnlyDdi(KMDDOD_INITIALIZATION_DATA *d) {
    d->DxgkDdiSetPalette=Palette;d->DxgkDdiSetPointerPosition=PointerPosition;d->DxgkDdiSetPointerShape=PointerShape;
    d->DxgkDdiIsSupportedVidPn=Supported;d->DxgkDdiRecommendFunctionalVidPn=Recommend;
    d->DxgkDdiEnumVidPnCofuncModality=Cofunc;d->DxgkDdiCommitVidPn=Commit;
    d->DxgkDdiSetVidPnSourceVisibility=Visible;d->DxgkDdiUpdateActiveVidPnPresentPath=Update;
    d->DxgkDdiRecommendMonitorModes=MonitorModes;d->DxgkDdiQueryVidPnHWCapability=HardwareCaps;
    d->DxgkDdiPresentDisplayOnly=PresentDisplayOnly;d->DxgkDdiStopDeviceAndReleasePostDisplayOwnership=StopOwnership;
    d->DxgkDdiSystemDisplayEnable=SystemEnable;d->DxgkDdiSystemDisplayWrite=SystemWrite;
}
