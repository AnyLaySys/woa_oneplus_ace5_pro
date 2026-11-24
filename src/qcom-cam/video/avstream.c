#include <ntifs.h>
#include <windef.h>
#include <unknown.h>
#include <ks.h>
#include <ksmedia.h>
#include "../uapi.h"
#include "../frame-clock.h"

DRIVER_INITIALIZE DriverEntry;

#define VIDEO_RANGE_COUNT 48
typedef struct { volatile LONG Online; ULONG Sensor; KS_DATARANGE_VIDEO2 Range[3][VIDEO_RANGE_COUNT]; PKSDATARANGE Ranges[3][VIDEO_RANGE_COUNT]; ULONG RangeCount[3]; KSPIN_DESCRIPTOR_EX Pins[3]; KSFILTER_DESCRIPTOR Filter; } VIDEO_DEVICE;
typedef struct VIDEO_PIN VIDEO_PIN;
typedef struct { KSPIN_LOCK PhotoLock; VIDEO_PIN *Photo,*Endpoints[3],*Engine; volatile LONG PhotoPending; ULONGLONG PhotoAfter,PhotoDeadline; BOOLEAN PhotoFlash; ULONG FocusPosition,FocusFlags,Zoom,TorchPower,FlashPower; ULONGLONG FocusMode,TorchMode,FlashMode; QCAM_ROI_OPTIONS Roi; KSCAMERA_EXTENDEDPROP_ROI_FOCUS RoiSetting; volatile LONG FocusPending,FocusGeneration; NTSTATUS FocusResult; } VIDEO_FILTER;
typedef struct { ULONG State; QCAM_FRAME_TIME Time; LONGLONG Presentation; } VIDEO_FRAME;
struct VIDEO_PIN {
    EX_RUNDOWN_REF PhotoRundown;
    HANDLE Parent,Thread; PKSPIN Pin; PKSFILTER Filter;
    QCAM_VIDEO_FORMAT_OPTIONS Format; ULONG Bytes,SourceWidth,SourceHeight,ScaleX[QCAM_VIDEO_MAX_WIDTH],ScaleY[QCAM_VIDEO_MAX_HEIGHT]; LONGLONG Interval; ULONGLONG NextSof; KMUTEX IoLock; KSPIN_LOCK FrameLock;
    volatile LONG Stopping; BOOLEAN Resizing; PUCHAR Storage; VIDEO_FRAME Slots[3]; NTSTATUS CaptureStatus;
    PIKSREFERENCECLOCK Clock; ULONGLONG Started; LONGLONG ClockStart;
    ULONG Sequence,Dropped; CAM_FRAME_CLOCK Timeline;
};
#define STATIC_QCAM_NV12 0x3231564e,0,0x0010,0x80,0,0,0xaa,0,0x38,0x9b,0x71
static const GUID Nv12={STATIC_QCAM_NV12};
static const GUID Categories[]={{STATIC_KSCATEGORY_CAPTURE},{STATIC_KSCATEGORY_VIDEO},{STATIC_KSCATEGORY_VIDEO_CAMERA}};
static const LONGLONG FrameInterval=166667;

static VIDEO_PIN *CaptureSource(PKSFILTER filter);
static NTSTATUS StartEngine(PKSFILTER filter);
static VOID StopCapture(VIDEO_PIN *v);

static NTSTATUS ParentIoLocked(VIDEO_PIN *v,ULONG code,PVOID input,ULONG inputBytes,PVOID output,ULONG outputBytes,ULONG_PTR *bytes) {
    if(!v->Parent)return STATUS_DEVICE_NOT_READY;
    IO_STATUS_BLOCK io={0};
    NTSTATUS status=ZwDeviceIoControlFile(v->Parent,NULL,NULL,NULL,&io,code,input,inputBytes,output,outputBytes);
    if(status==STATUS_PENDING) {
        status=ZwWaitForSingleObject(v->Parent,FALSE,NULL);
        if(NT_SUCCESS(status))status=io.Status;
    }
    if(bytes)*bytes=NT_SUCCESS(status)?io.Information:0;
    return status;
}

static NTSTATUS ParentIo(VIDEO_PIN *v,ULONG code,PVOID input,ULONG inputBytes,PVOID output,ULONG outputBytes,ULONG_PTR *bytes) {
    KeWaitForSingleObject(&v->IoLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=ParentIoLocked(v,code,input,inputBytes,output,outputBytes,bytes);
    KeReleaseMutex(&v->IoLock,FALSE);return status;
}

static VOID CloseParent(VIDEO_PIN *v) {
    KeWaitForSingleObject(&v->IoLock,Executive,KernelMode,FALSE,NULL);
    if(v->Parent){ParentIoLocked(v,QCAM_STREAM_STOP,NULL,0,NULL,0,NULL);ZwClose(v->Parent);v->Parent=NULL;}
    KeReleaseMutex(&v->IoLock,FALSE);
}

static NTSTATUS FilterCreate(PKSFILTER filter,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);
    VIDEO_FILTER *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),'fCcQ');
    if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=KsAddItemToObjectBag(filter->Bag,v,NULL);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(v,'fCcQ');return status;}
    v->Engine=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v->Engine),'eCcQ');
    if(!v->Engine)return STATUS_INSUFFICIENT_RESOURCES;
    status=KsAddItemToObjectBag(filter->Bag,v->Engine,NULL);if(!NT_SUCCESS(status)){ExFreePoolWithTag(v->Engine,'eCcQ');return status;}
    v->Engine->Filter=filter;v->Engine->Stopping=1;KeInitializeMutex(&v->Engine->IoLock,0);KeInitializeSpinLock(&v->Engine->FrameLock);
    v->Engine->Storage=ExAllocatePool2(POOL_FLAG_NON_PAGED,QCAM_VIDEO_MAX_BYTES,'sCcQ');if(!v->Engine->Storage)return STATUS_INSUFFICIENT_RESOURCES;
    status=KsAddItemToObjectBag(filter->Bag,v->Engine->Storage,NULL);if(!NT_SUCCESS(status)){ExFreePoolWithTag(v->Engine->Storage,'sCcQ');return status;}
    KeInitializeSpinLock(&v->PhotoLock);v->FocusPosition=QCAM_FOCUS_MIN;v->FocusFlags=QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS;v->Zoom=QCAM_ZOOM_MIN;v->FocusMode=KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS|KSCAMERA_EXTENDEDPROP_FOCUS_RANGE_FULLRANGE;filter->Context=v;return STATUS_SUCCESS;
}

static NTSTATUS SetZoom(PKSFILTER filter,ULONG ratio) {
    if(ratio<QCAM_ZOOM_MIN||ratio>QCAM_ZOOM_MAX)return STATUS_INVALID_PARAMETER;
    VIDEO_FILTER *f=filter->Context;VIDEO_PIN *v=CaptureSource(filter);
    NTSTATUS status=STATUS_SUCCESS;QCAM_ZOOM_OPTIONS zoom={sizeof(zoom),QCAM_ABI,ratio,0};
    if(v&&v->Parent&&!v->Stopping)status=ParentIo(v,QCAM_ZOOM,&zoom,sizeof(zoom),NULL,0,NULL);
    if(NT_SUCCESS(status))f->Zoom=ratio;return status;
}

static NTSTATUS Zoom(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;KSPROPERTY_CAMERACONTROL_S *p=data;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(p->Flags!=KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL)return STATUS_NOT_SUPPORTED;
        if(p->Value<100||p->Value>400)return STATUS_INVALID_PARAMETER;
        return SetZoom(filter,(ULONG)p->Value*QCAM_ZOOM_MIN/100);
    }
    p->Property=*request;p->Value=(LONG)((f->Zoom*100+QCAM_ZOOM_MIN/2)/QCAM_ZOOM_MIN);
    p->Flags=p->Capabilities=KSPROPERTY_CAMERACONTROL_FLAGS_MANUAL;irp->IoStatus.Information=sizeof(*p);return STATUS_SUCCESS;
}

static NTSTATUS ExtendedZoom(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    ULONG size=sizeof(KSCAMERA_EXTENDEDPROP_HEADER)+sizeof(KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING);
    ULONG available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    irp->IoStatus.Information=size;if(!available)return STATUS_BUFFER_OVERFLOW;if(available<size)return STATUS_BUFFER_TOO_SMALL;
    KSCAMERA_EXTENDEDPROP_HEADER *h=data;KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING *s=(PVOID)(h+1);
    if(h->Version!=1||h->PinId!=KSCAMERA_EXTENDEDPROP_FILTERSCOPE||h->Size<size)return STATUS_INVALID_PARAMETER;
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(h->Flags!=KSCAMERA_EXTENDEDPROP_ZOOM_DEFAULT&&h->Flags!=KSCAMERA_EXTENDEDPROP_ZOOM_DIRECT)return STATUS_NOT_SUPPORTED;
        NTSTATUS status=SetZoom(filter,s->VideoProc.Value.ul);irp->IoStatus.Information=0;return status;
    }
    RtlZeroMemory(data,size);h->Version=1;h->PinId=KSCAMERA_EXTENDEDPROP_FILTERSCOPE;h->Size=size;
    h->Flags=h->Capability=KSCAMERA_EXTENDEDPROP_ZOOM_DIRECT;
    s->Min=QCAM_ZOOM_MIN;s->Max=QCAM_ZOOM_MAX;s->Step=256;s->VideoProc.Value.ul=f->Zoom;return STATUS_SUCCESS;
}

static VOID FocusComplete(PKSFILTER filter,LONG generation,NTSTATUS status);

static NTSTATUS SetRoi(PKSFILTER filter,const QCAM_ROI_OPTIONS *roi) {
    VIDEO_FILTER *f=filter->Context;VIDEO_PIN *v=CaptureSource(filter);
    NTSTATUS status=STATUS_SUCCESS;
    if(v&&v->Parent&&!v->Stopping) {
        status=ParentIo(v,QCAM_FOCUS_ROI,(PVOID)roi,sizeof(*roi),NULL,0,NULL);
        if(NT_SUCCESS(status)&&roi->Flags) {
            QCAM_FOCUS_OPTIONS focus={sizeof(focus),QCAM_ABI,f->FocusPosition,QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS,0};
            status=ParentIo(v,QCAM_FOCUS,&focus,sizeof(focus),NULL,0,NULL);
        }
    }
    if(NT_SUCCESS(status)) {
        f->Roi=*roi;
        if(roi->Flags){FocusComplete(filter,f->FocusPending,STATUS_CANCELLED);f->FocusFlags=QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS;f->FocusMode=KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS|KSCAMERA_EXTENDEDPROP_FOCUS_RANGE_FULLRANGE;}
    }
    return status;
}

static NTSTATUS ExtendedRoi(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    if(((VIDEO_DEVICE *)KsFilterGetDevice(KsGetFilterFromIrp(irp))->Context)->Sensor)return STATUS_NOT_SUPPORTED;
    VIDEO_FILTER *f=KsGetFilterFromIrp(irp)->Context;
    ULONG base=sizeof(KSCAMERA_EXTENDEDPROP_HEADER),body=request->Id==KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_CONFIGCAPS?
        sizeof(KSCAMERA_EXTENDEDPROP_ROI_CONFIGCAPSHEADER)+sizeof(KSCAMERA_EXTENDEDPROP_ROI_CONFIGCAPS):
        sizeof(KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROLHEADER)+sizeof(KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROL)+sizeof(KSCAMERA_EXTENDEDPROP_ROI_FOCUS);
    ULONG size=base+body,available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    KSCAMERA_EXTENDEDPROP_HEADER *h=data;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(available<base+sizeof(KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROLHEADER))return STATUS_BUFFER_TOO_SMALL;
        if(h->Version!=1||h->PinId!=KSCAMERA_EXTENDEDPROP_FILTERSCOPE||h->Size>available||h->Size<base+sizeof(KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROLHEADER))return STATUS_INVALID_PARAMETER;
        if(h->Flags==KSCAMERA_EXTENDEDPROP_FLAG_CANCELOPERATION){irp->IoStatus.Information=0;return STATUS_SUCCESS;}
        if(h->Flags)return STATUS_INVALID_PARAMETER;
        KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROLHEADER *b=(PVOID)(h+1);
        if(b->Reserved||b->ControlCount>1||b->Size!=h->Size-base)return STATUS_INVALID_PARAMETER;
        QCAM_ROI_OPTIONS roi={sizeof(roi),QCAM_ABI,0,0,0,0,0};KSCAMERA_EXTENDEDPROP_ROI_FOCUS setting={0};
        if(b->ControlCount) {
            if(h->Size<base+sizeof(*b)+sizeof(KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROL))return STATUS_BUFFER_TOO_SMALL;
            KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROL *c=(PVOID)(b+1);
            if(c->ControlId!=KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE||c->ROICount>1||c->Reserved)return STATUS_NOT_SUPPORTED;
            if(c->ROICount) {
                if(h->Size<size)return STATUS_BUFFER_TOO_SMALL;
                setting=*(KSCAMERA_EXTENDEDPROP_ROI_FOCUS *)(c+1);RECT r=setting.ROIInfo.Region;
                if(setting.Reserved||r.left<0||r.top<0||r.left>=r.right||r.top>=r.bottom||setting.ROIInfo.Weight<0||setting.ROIInfo.Weight>100||setting.ROIInfo.RegionOfInterestType<0||setting.ROIInfo.RegionOfInterestType>KSCAMERA_EXTENDEDPROP_ROITYPE_FACE||setting.ROIInfo.Flags)return STATUS_INVALID_PARAMETER;
                roi.Flags=QCAM_ROI_FOCUS;roi.Left=(ULONG)((ULONGLONG)(ULONG)r.left*65536/0x7fffffff);roi.Top=(ULONG)((ULONGLONG)(ULONG)r.top*65536/0x7fffffff);
                roi.Right=(ULONG)(((ULONGLONG)(ULONG)r.right*65536+0x7ffffffe)/0x7fffffff);roi.Bottom=(ULONG)(((ULONGLONG)(ULONG)r.bottom*65536+0x7ffffffe)/0x7fffffff);
            }
        }
        PKSFILTER filter=KsGetFilterFromIrp(irp);NTSTATUS status=SetRoi(filter,&roi);
        if(NT_SUCCESS(status))f->RoiSetting=setting;
        h->Result=(ULONG)status;irp->IoStatus.Information=0;
        KsGenerateEvents(filter,&KSEVENTSETID_ExtendedCameraControl,KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_ISPCONTROL,0,NULL,NULL,NULL);return status;
    }
    if(request->Id==KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_ISPCONTROL&&!f->Roi.Flags){body=sizeof(KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROLHEADER);size=base+body;}
    irp->IoStatus.Information=size;if(!available)return STATUS_BUFFER_OVERFLOW;if(available<size)return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(data,size);h->Version=1;h->PinId=KSCAMERA_EXTENDEDPROP_FILTERSCOPE;h->Size=size;
    h->Capability=request->Id==KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_CONFIGCAPS?0:KSCAMERA_EXTENDEDPROP_CAPS_ASYNCCONTROL|KSCAMERA_EXTENDEDPROP_CAPS_CANCELLABLE;
    if(request->Id==KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_CONFIGCAPS) {
        KSCAMERA_EXTENDEDPROP_ROI_CONFIGCAPSHEADER *b=(PVOID)(h+1);b->Size=body;b->ConfigCapCount=1;
        KSCAMERA_EXTENDEDPROP_ROI_CONFIGCAPS *c=(PVOID)(b+1);c->ControlId=KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE;c->MaxNumberOfROIs=1;c->Capability=KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_AUTO;
    } else {
        KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROLHEADER *b=(PVOID)(h+1);b->Size=body;b->ControlCount=f->Roi.Flags?1:0;
        if(f->Roi.Flags){KSCAMERA_EXTENDEDPROP_ROI_ISPCONTROL *c=(PVOID)(b+1);c->ControlId=KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE;c->ROICount=1;*(KSCAMERA_EXTENDEDPROP_ROI_FOCUS *)(c+1)=f->RoiSetting;}
    }
    return STATUS_SUCCESS;
}

static NTSTATUS ExtendedTorch(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    if(((VIDEO_DEVICE *)KsFilterGetDevice(KsGetFilterFromIrp(irp))->Context)->Sensor==3)return STATUS_NOT_SUPPORTED;
    ULONG size=sizeof(KSCAMERA_EXTENDEDPROP_HEADER)+sizeof(KSCAMERA_EXTENDEDPROP_VALUE);
    ULONG available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    irp->IoStatus.Information=size;if(!available)return STATUS_BUFFER_OVERFLOW;if(available<size)return STATUS_BUFFER_TOO_SMALL;
    KSCAMERA_EXTENDEDPROP_HEADER *h=data;KSCAMERA_EXTENDEDPROP_VALUE *value=(PVOID)(h+1);
    if(h->Version!=1||h->PinId!=KSCAMERA_EXTENDEDPROP_FILTERSCOPE||h->Size<size)return STATUS_INVALID_PARAMETER;
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;VIDEO_PIN *v=CaptureSource(filter);
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(h->Flags!=KSCAMERA_EXTENDEDPROP_VIDEOTORCH_OFF&&h->Flags!=KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON&&h->Flags!=KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON_ADJUSTABLEPOWER)return STATUS_NOT_SUPPORTED;
        if(h->Flags==KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON_ADJUSTABLEPOWER&&value->Value.ull>100)return STATUS_INVALID_PARAMETER;
        ULONG power=h->Flags==KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON_ADJUSTABLEPOWER?max((ULONG)value->Value.ull,1u):40;
        QCAM_FLASH_OPTIONS options={sizeof(options),QCAM_ABI,h->Flags?QCAM_FLASH_TORCH:QCAM_FLASH_OFF,power,0,0,0};NTSTATUS status=STATUS_SUCCESS;
        if(v&&v->Parent)status=ParentIo(v,QCAM_FLASH,&options,sizeof(options),NULL,0,NULL);
        if(NT_SUCCESS(status)){f->TorchMode=h->Flags;f->TorchPower=power;}irp->IoStatus.Information=0;return status;
    }
    RtlZeroMemory(data,size);h->Version=1;h->PinId=KSCAMERA_EXTENDEDPROP_FILTERSCOPE;h->Size=size;
    h->Flags=f->TorchMode;h->Capability=KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON|KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON_ADJUSTABLEPOWER;
    value->Value.ull=f->TorchPower?f->TorchPower:40;return STATUS_SUCCESS;
}

static NTSTATUS ExtendedFlash(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    if(((VIDEO_DEVICE *)KsFilterGetDevice(filter)->Context)->Sensor==3)return STATUS_NOT_SUPPORTED;
    ULONG size=sizeof(KSCAMERA_EXTENDEDPROP_HEADER)+sizeof(KSCAMERA_EXTENDEDPROP_VALUE);
    ULONG available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    irp->IoStatus.Information=size;if(!available)return STATUS_BUFFER_OVERFLOW;if(available<size)return STATUS_BUFFER_TOO_SMALL;
    KSCAMERA_EXTENDEDPROP_HEADER *h=data;KSCAMERA_EXTENDEDPROP_VALUE *value=(PVOID)(h+1);
    if(h->Version!=1||h->PinId!=KSCAMERA_EXTENDEDPROP_FILTERSCOPE||h->Size<size)return STATUS_INVALID_PARAMETER;
    ULONGLONG caps=KSCAMERA_EXTENDEDPROP_FLASH_ON|KSCAMERA_EXTENDEDPROP_FLASH_AUTO|KSCAMERA_EXTENDEDPROP_FLASH_ON_ADJUSTABLEPOWER|KSCAMERA_EXTENDEDPROP_FLASH_AUTO_ADJUSTABLEPOWER;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(h->Flags&&h->Flags!=KSCAMERA_EXTENDEDPROP_FLASH_ON&&h->Flags!=KSCAMERA_EXTENDEDPROP_FLASH_AUTO&&h->Flags!=KSCAMERA_EXTENDEDPROP_FLASH_ON_ADJUSTABLEPOWER&&h->Flags!=KSCAMERA_EXTENDEDPROP_FLASH_AUTO_ADJUSTABLEPOWER)return STATUS_NOT_SUPPORTED;
        BOOLEAN adjustable=(h->Flags&(KSCAMERA_EXTENDEDPROP_FLASH_ON_ADJUSTABLEPOWER|KSCAMERA_EXTENDEDPROP_FLASH_AUTO_ADJUSTABLEPOWER))!=0;
        if(adjustable&&value->Value.ull>100)return STATUS_INVALID_PARAMETER;
        f->FlashMode=h->Flags;f->FlashPower=adjustable?max((ULONG)value->Value.ull,1u):40;irp->IoStatus.Information=0;return STATUS_SUCCESS;
    }
    RtlZeroMemory(data,size);h->Version=1;h->PinId=KSCAMERA_EXTENDEDPROP_FILTERSCOPE;h->Size=size;h->Capability=caps;h->Flags=f->FlashMode;
    value->Value.ull=f->FlashPower?f->FlashPower:40;return STATUS_SUCCESS;
}

static NTSTATUS PhotoMode(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    ULONG size=sizeof(KSCAMERA_EXTENDEDPROP_HEADER)+sizeof(KSCAMERA_EXTENDEDPROP_PHOTOMODE);
    ULONG available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    irp->IoStatus.Information=size;if(!available)return STATUS_BUFFER_OVERFLOW;if(available<size)return STATUS_BUFFER_TOO_SMALL;
    KSCAMERA_EXTENDEDPROP_HEADER *h=data;KSCAMERA_EXTENDEDPROP_PHOTOMODE *mode=(PVOID)(h+1);
    if(h->Version!=1||h->PinId!=1||h->Size<size)return STATUS_INVALID_PARAMETER;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(h->Flags||mode->RequestedHistoryFrames||mode->SubMode||mode->Reserved)return STATUS_NOT_SUPPORTED;
        h->Result=STATUS_SUCCESS;KsGenerateEvents(KsGetFilterFromIrp(irp),&KSEVENTSETID_ExtendedCameraControl,KSPROPERTY_CAMERACONTROL_EXTENDED_PHOTOMODE,0,NULL,NULL,NULL);return STATUS_SUCCESS;
    }
    RtlZeroMemory(data,size);h->Version=1;h->PinId=1;h->Size=size;h->Capability=KSCAMERA_EXTENDEDPROP_CAPS_ASYNCCONTROL;return STATUS_SUCCESS;
}

static NTSTATUS LegacyFlash(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    if(((VIDEO_DEVICE *)KsFilterGetDevice(filter)->Context)->Sensor==3)return STATUS_NOT_SUPPORTED;
    KSPROPERTY_CAMERACONTROL_FLASH_S *value=data;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(value->Flash>KSPROPERTY_CAMERACONTROL_FLASH_AUTO)return STATUS_INVALID_PARAMETER;
        f->FlashMode=value->Flash==KSPROPERTY_CAMERACONTROL_FLASH_AUTO?KSCAMERA_EXTENDEDPROP_FLASH_AUTO:value->Flash;return STATUS_SUCCESS;
    }
    value->Flash=(f->FlashMode&(KSCAMERA_EXTENDEDPROP_FLASH_AUTO|KSCAMERA_EXTENDEDPROP_FLASH_AUTO_ADJUSTABLEPOWER))?KSPROPERTY_CAMERACONTROL_FLASH_AUTO:f->FlashMode?KSPROPERTY_CAMERACONTROL_FLASH_ON:KSPROPERTY_CAMERACONTROL_FLASH_OFF;
    value->Capabilities=KSPROPERTY_CAMERACONTROL_FLASH_FLAGS_AUTO|KSPROPERTY_CAMERACONTROL_FLASH_FLAGS_MANUAL;irp->IoStatus.Information=sizeof(*value);return STATUS_SUCCESS;
}

static VIDEO_PIN *PhotoReference(VIDEO_FILTER *f) {
    KIRQL irql;KeAcquireSpinLock(&f->PhotoLock,&irql);VIDEO_PIN *photo=f->Photo;
    if(photo&&!ExAcquireRundownProtection(&photo->PhotoRundown))photo=NULL;
    KeReleaseSpinLock(&f->PhotoLock,irql);return photo;
}

static VIDEO_PIN *CaptureSource(PKSFILTER filter) {
    VIDEO_PIN *v=((VIDEO_FILTER *)filter->Context)->Engine;return v&&v->Parent&&v->Thread&&!v->Stopping?v:NULL;
}

static VIDEO_PIN *EndpointReference(VIDEO_FILTER *f,ULONG index) {
    KIRQL irql;KeAcquireSpinLock(&f->PhotoLock,&irql);VIDEO_PIN *v=f->Endpoints[index];
    if(v&&!ExAcquireRundownProtection(&v->PhotoRundown))v=NULL;
    KeReleaseSpinLock(&f->PhotoLock,irql);return v;
}

static VIDEO_PIN *PinContext(PKSPIN pin) {
    if(pin->Id>=3)return NULL;
    VIDEO_FILTER *f=KsPinGetParentFilter(pin)->Context;if(!f)return NULL;
    KIRQL irql;KeAcquireSpinLock(&f->PhotoLock,&irql);
    VIDEO_PIN *v=f->Endpoints[pin->Id];
    if(v!=pin->Context||v&&v->Pin!=pin)v=NULL;
    KeReleaseSpinLock(&f->PhotoLock,irql);return v;
}

static VOID FlashFinish(VIDEO_FILTER *f) {
    if(!f->PhotoFlash)return;VIDEO_PIN *v=f->Engine;
    QCAM_FLASH_OPTIONS flash={sizeof(flash),QCAM_ABI,f->TorchMode?QCAM_FLASH_TORCH:QCAM_FLASH_OFF,f->TorchPower,0,0,0};
    if(v->Parent)ParentIo(v,QCAM_FLASH,&flash,sizeof(flash),NULL,0,NULL);f->PhotoFlash=FALSE;
}


static NTSTATUS VideoControl(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    KSPROPERTY_VIDEOCONTROL_MODE_S *mode=data;
    ULONG size=sizeof(KSPROPERTY_VIDEOCONTROL_MODE_S);
    ULONG available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    irp->IoStatus.Information=size;if(!available)return STATUS_BUFFER_OVERFLOW;if(available<size)return STATUS_BUFFER_TOO_SMALL;
    KSPROPERTY_VIDEOCONTROL_MODE_S input=*mode;
    if(IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.InputBufferLength>=sizeof(input))input=*(KSPROPERTY_VIDEOCONTROL_MODE_S *)request;
    if(input.StreamIndex>2)return STATUS_INVALID_PARAMETER;
    if(request->Id==KSPROPERTY_VIDEOCONTROL_CAPS) {
        KSPROPERTY_VIDEOCONTROL_CAPS_S *caps=data;caps->StreamIndex=input.StreamIndex;
        caps->VideoControlCaps=input.StreamIndex==1?KS_VideoControlFlag_ExternalTriggerEnable|KS_VideoControlFlag_Trigger|KS_VideoControlFlag_IndependentImagePin:0;
        irp->IoStatus.Information=sizeof(*caps);return STATUS_SUCCESS;
    }
    if(!(request->Flags&KSPROPERTY_TYPE_SET)){mode->StreamIndex=input.StreamIndex;mode->Mode=input.StreamIndex==1?KS_VideoControlFlag_ExternalTriggerEnable:0;irp->IoStatus.Information=sizeof(*mode);return STATUS_SUCCESS;}
    mode->StreamIndex=input.StreamIndex;
    if(mode->StreamIndex!=1||mode->Mode&~(KS_VideoControlFlag_ExternalTriggerEnable|KS_VideoControlFlag_Trigger))return mode->Mode?STATUS_NOT_SUPPORTED:STATUS_SUCCESS;
    if(!(mode->Mode&KS_VideoControlFlag_Trigger))return STATUS_SUCCESS;
    VIDEO_PIN *v=CaptureSource(filter),*photo=PhotoReference(f);
    if(!v||!photo||photo->Stopping){if(photo)ExReleaseRundownProtection(&photo->PhotoRundown);return STATUS_INVALID_DEVICE_STATE;}
    if(InterlockedCompareExchange(&f->PhotoPending,-1,0)){ExReleaseRundownProtection(&photo->PhotoRundown);return STATUS_DEVICE_BUSY;}
    BOOLEAN automatic=(f->FlashMode&(KSCAMERA_EXTENDEDPROP_FLASH_AUTO|KSCAMERA_EXTENDEDPROP_FLASH_AUTO_ADJUSTABLEPOWER))!=0;
    QCAM_INFO info={0};NTSTATUS status=ParentIo(v,QCAM_QUERY,NULL,0,&info,sizeof(info),NULL);
    f->PhotoFlash=FALSE;ULONGLONG exposure=0;
    if(NT_SUCCESS(status)&&f->FlashMode&&(!automatic||info.Gain>0x2000||info.Exposure>info.FrameLength/2)) {
        QCAM_FLASH_OPTIONS flash={sizeof(flash),QCAM_ABI,QCAM_FLASH_STROBE,f->FlashPower?f->FlashPower:40,240,0,0};
        status=ParentIo(v,QCAM_FLASH,&flash,sizeof(flash),NULL,0,NULL);
        f->PhotoFlash=NT_SUCCESS(status);
        ULONG sensor=((VIDEO_DEVICE *)KsFilterGetDevice(filter)->Context)->Sensor;
        exposure=info.FrameLength?(ULONGLONG)info.Exposure*(sensor==1||sensor==2?333333:166667)/info.FrameLength:0;
    }
    if(NT_SUCCESS(status)) {
        ULONGLONG qpc;f->PhotoAfter=KeQueryInterruptTimePrecise(&qpc)+exposure;f->PhotoDeadline=f->PhotoAfter+10000000;
        KIRQL irql;KeAcquireSpinLock(&photo->FrameLock,&irql);
        for(ULONG i=0;i<RTL_NUMBER_OF(photo->Slots);i++)if(photo->Slots[i].State==2)photo->Slots[i].State=0;
        KeReleaseSpinLock(&photo->FrameLock,irql);InterlockedExchange(&f->PhotoPending,1);
    } else InterlockedExchange(&f->PhotoPending,0);
    ExReleaseRundownProtection(&photo->PhotoRundown);return status;
}


static NTSTATUS Focus(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    if(((VIDEO_DEVICE *)KsFilterGetDevice(filter)->Context)->Sensor)return STATUS_NOT_SUPPORTED;
    KSPROPERTY_CAMERACONTROL_S *p=data;
    VIDEO_PIN *v=CaptureSource(filter);QCAM_FOCUS_OPTIONS options={sizeof(options),QCAM_ABI,f->FocusPosition,f->FocusFlags,0};
    NTSTATUS status=STATUS_SUCCESS;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(p->Flags!=QCAM_FOCUS_MANUAL&&p->Flags!=QCAM_FOCUS_AUTO)return STATUS_NOT_SUPPORTED;
        if(p->Flags==QCAM_FOCUS_MANUAL&&(p->Value<(LONG)QCAM_FOCUS_MIN||p->Value>(LONG)QCAM_FOCUS_MAX))return STATUS_INVALID_PARAMETER;
        options.Position=p->Flags==QCAM_FOCUS_MANUAL?(ULONG)p->Value:f->FocusPosition;options.Flags=p->Flags==QCAM_FOCUS_AUTO?QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS:QCAM_FOCUS_MANUAL;
        if(v&&v->Parent&&!v->Stopping)status=ParentIo(v,QCAM_FOCUS,&options,sizeof(options),NULL,0,NULL);
        if(NT_SUCCESS(status)){FocusComplete(filter,f->FocusPending,STATUS_CANCELLED);f->FocusPosition=options.Position;f->FocusFlags=options.Flags;f->FocusMode=p->Flags==QCAM_FOCUS_AUTO?KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS|KSCAMERA_EXTENDEDPROP_FOCUS_RANGE_FULLRANGE:KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_MANUAL;}
    } else {
        if(v&&v->Parent&&!v->Stopping)status=ParentIo(v,QCAM_FOCUS,NULL,0,&options,sizeof(options),NULL);
        if(NT_SUCCESS(status)){p->Property=*request;p->Value=(LONG)options.Position;p->Flags=options.Flags&QCAM_FOCUS_AUTO?QCAM_FOCUS_AUTO:QCAM_FOCUS_MANUAL;p->Capabilities=QCAM_FOCUS_AUTO|QCAM_FOCUS_MANUAL;irp->IoStatus.Information=sizeof(*p);}
    }
    return status;
}

typedef struct { KSCAMERA_EXTENDEDPROP_HEADER Header; KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING Setting; } FOCUS_MODE;
typedef struct { KSCAMERA_EXTENDEDPROP_HEADER Header; KSCAMERA_EXTENDEDPROP_VALUE Value; } FOCUS_STATE;
static const ULONGLONG FocusCaps=KSCAMERA_EXTENDEDPROP_CAPS_ASYNCCONTROL|KSCAMERA_EXTENDEDPROP_CAPS_CANCELLABLE|
    KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_AUTO|KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS|KSCAMERA_EXTENDEDPROP_FOCUS_RANGE_FULLRANGE|KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_MANUAL|KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_LOCK;

static VOID FocusComplete(PKSFILTER filter,LONG generation,NTSTATUS status) {
    VIDEO_FILTER *f=filter->Context;
    if(generation&&InterlockedCompareExchange(&f->FocusPending,0,generation)==generation) {
        f->FocusResult=status;KsGenerateEvents(filter,&KSEVENTSETID_ExtendedCameraControl,KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE,0,NULL,NULL,NULL);
    }
}

static NTSTATUS ExtendedFocus(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    if(((VIDEO_DEVICE *)KsFilterGetDevice(KsGetFilterFromIrp(irp))->Context)->Sensor)return STATUS_NOT_SUPPORTED;
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    VIDEO_PIN *v=CaptureSource(filter);
    BOOLEAN running=v&&v->Parent&&!v->Stopping;
    ULONG size=request->Id==KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE?sizeof(FOCUS_MODE):sizeof(FOCUS_STATE);
    ULONG available=IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    irp->IoStatus.Information=size;
    if(!available)return STATUS_BUFFER_OVERFLOW;
    if(available<size)return STATUS_BUFFER_TOO_SMALL;
    KSCAMERA_EXTENDEDPROP_HEADER *h=data;
    if(h->Version!=1||h->PinId!=KSCAMERA_EXTENDEDPROP_FILTERSCOPE||h->Size<size)return STATUS_INVALID_PARAMETER;
    QCAM_FOCUS_OPTIONS options={sizeof(options),QCAM_ABI,f->FocusPosition,f->FocusFlags,0};NTSTATUS status=STATUS_SUCCESS;
    if(running)status=ParentIo(v,QCAM_FOCUS,NULL,0,&options,sizeof(options),NULL);
    if(!NT_SUCCESS(status))return status;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        ULONGLONG flags=h->Flags,mode=flags&KSCAMERA_EXTENDEDPROP_FOCUS_MODE_MASK;
        if(flags&KSCAMERA_EXTENDEDPROP_FLAG_CANCELOPERATION) {
            options.Flags=QCAM_FOCUS_LOCK;
            if(running)status=ParentIo(v,QCAM_FOCUS,&options,sizeof(options),NULL,0,NULL);
            if(NT_SUCCESS(status)){f->FocusFlags=QCAM_FOCUS_LOCK;f->FocusMode=KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_LOCK;FocusComplete(filter,f->FocusPending,STATUS_CANCELLED);}
            return status;
        }
        if(flags&~(FocusCaps&~KSCAMERA_EXTENDEDPROP_CAPS_MASK))return STATUS_INVALID_PARAMETER;
        if(mode!=KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_MANUAL&&mode!=KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_LOCK&&mode!=KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_AUTO&&mode!=KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS&&mode!=(KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_AUTO|KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_LOCK)&&mode!=(KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_AUTO|KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS))return STATUS_NOT_SUPPORTED;
        options.Flags=mode&(KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_AUTO|KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS)?QCAM_FOCUS_AUTO|((mode&KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_LOCK)?QCAM_FOCUS_LOCK:QCAM_FOCUS_CONTINUOUS):(ULONG)mode;
        if(mode==KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_MANUAL) {
            LONG position=((FOCUS_MODE *)data)->Setting.VideoProc.Value.l;
            if(position<(LONG)QCAM_FOCUS_MIN||position>(LONG)QCAM_FOCUS_MAX)return STATUS_INVALID_PARAMETER;
            options.Position=(ULONG)position;
        }
        FocusComplete(filter,f->FocusPending,STATUS_CANCELLED);
        if(running)status=ParentIo(v,QCAM_FOCUS,&options,sizeof(options),NULL,0,NULL);
        if(NT_SUCCESS(status)) {
            f->FocusPosition=options.Position;f->FocusFlags=options.Flags;f->FocusMode=flags;f->FocusResult=STATUS_SUCCESS;
            LONG generation=InterlockedIncrement(&f->FocusGeneration);InterlockedExchange(&f->FocusPending,generation);
            if(!running||mode==KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_LOCK||(mode&KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS))FocusComplete(filter,generation,STATUS_SUCCESS);
        }
        irp->IoStatus.Information=0;return status;
    }
    RtlZeroMemory(data,size);h->Version=1;h->PinId=KSCAMERA_EXTENDEDPROP_FILTERSCOPE;h->Size=size;
    if(request->Id==KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE) {
        h->Capability=FocusCaps;h->Flags=f->FocusMode;h->Result=(ULONG)f->FocusResult;
        KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING *setting=&((FOCUS_MODE *)data)->Setting;
        setting->Min=QCAM_FOCUS_MIN;setting->Max=QCAM_FOCUS_MAX;setting->Step=1;setting->VideoProc.Value.ul=options.Position;
    } else ((FOCUS_STATE *)data)->Value.Value.ull=options.State;
    return STATUS_SUCCESS;
}

static VOID ScaleFrame(VIDEO_PIN *v,const UCHAR *source,ULONG width,ULONG height,UCHAR *output) {
    ULONG ow=v->Format.Width,oh=v->Format.Height;
    if(width==ow&&height==oh){RtlCopyMemory(output,source,v->Bytes);return;}
    if(v->SourceWidth!=width||v->SourceHeight!=height) {
        ULONG cw=width,ch=height;
        if((ULONGLONG)cw*oh>(ULONGLONG)ch*ow)cw=(ULONG)((ULONGLONG)ch*ow/oh);else ch=(ULONG)((ULONGLONG)cw*oh/ow);
        ULONG left=((width-cw)/2)&~1u,top=((height-ch)/2)&~1u;
        for(ULONG x=0;x<ow;x++)v->ScaleX[x]=left+x*cw/ow;
        for(ULONG y=0;y<oh;y++)v->ScaleY[y]=top+y*ch/oh;
        v->SourceWidth=width;v->SourceHeight=height;
    }
    for(ULONG y=0;y<oh;y++) {
        const UCHAR *row=source+v->ScaleY[y]*width;UCHAR *out=output+y*ow;
        for(ULONG x=0;x<ow;x++)out[x]=row[v->ScaleX[x]];
    }
    const UCHAR *uv=source+width*height;UCHAR *out=output+ow*oh;
    for(ULONG y=0;y<oh;y+=2) {
        const USHORT *row=(const USHORT *)(uv+(v->ScaleY[y]/2)*width);USHORT *target=(USHORT *)(out+(y/2)*ow);
        for(ULONG x=0;x<ow;x+=2)target[x/2]=row[v->ScaleX[x]/2];
    }
}

static VOID Deliver(VIDEO_PIN *v,VIDEO_PIN *engine,const QCAM_FRAME_TIME *time,ULONG width,ULONG height,NTSTATUS status) {
    if(v->Stopping)return;
    if(NT_SUCCESS(status)&&(time->Captured<v->Started||time->Captured-v->Started<CamFrameTicks(time->Eof-time->Sof,time->ClockHz)))return;
    if(NT_SUCCESS(status)&&v->Pin->Id!=1&&v->NextSof&&time->Sof<v->NextSof)return;
    KIRQL irql;ULONG slot=RTL_NUMBER_OF(v->Slots);
    KeAcquireSpinLock(&v->FrameLock,&irql);
    if(NT_SUCCESS(status))for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)if(!v->Slots[i].State){slot=i;v->Slots[i].State=1;break;}
    if(!NT_SUCCESS(status))v->CaptureStatus=status;
    KeReleaseSpinLock(&v->FrameLock,irql);
    if(slot<RTL_NUMBER_OF(v->Slots)) {
        VIDEO_FRAME *frame=v->Slots+slot;frame->Time=*time;
        if(!CamFrameClock(&v->Timeline,time,v->Started,v->ClockStart,&frame->Presentation))status=STATUS_DATA_ERROR;
        if(NT_SUCCESS(status))ScaleFrame(v,engine->Storage,width,height,v->Storage+slot*v->Bytes);
        KeAcquireSpinLock(&v->FrameLock,&irql);
        if(NT_SUCCESS(status)&&!v->Stopping) {
            for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)if(v->Slots[i].State==2)v->Slots[i].State=0;
            frame->State=2;v->NextSof=time->Sof+(ULONGLONG)v->Interval*time->ClockHz/10000000-time->ClockHz/1000;
        } else {frame->State=0;v->CaptureStatus=status;}
        KeReleaseSpinLock(&v->FrameLock,irql);
    }
    KsPinAttemptProcessing(v->Pin,TRUE);
}

static VOID Capture(PVOID context) {
    VIDEO_PIN *engine=context;PKSFILTER filter=engine->Filter;VIDEO_FILTER *f=filter->Context;VIDEO_DEVICE *d=KsFilterGetDevice(filter)->Context;
    if(KeQueryActiveProcessorCountEx(0)>=8){GROUP_AFFINITY affinity={0},previous={0};affinity.Mask=0x40;KeSetSystemGroupAffinityThread(&affinity,&previous);}
    KeSetPriorityThread(KeGetCurrentThread(),15);
    while(!engine->Stopping&&d->Online) {
        VIDEO_PIN *pins[3]={0};QCAM_VIDEO_FORMAT_OPTIONS format={sizeof(format),QCAM_ABI,QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT};ULONG pixels=0;
        for(ULONG i=0;i<3;i++) {
            pins[i]=EndpointReference(f,i);
            if(pins[i]&&!pins[i]->Stopping&&i!=1&&pins[i]->Bytes>pixels){format=pins[i]->Format;pixels=pins[i]->Bytes;}
        }
        BOOLEAN pending=InterlockedCompareExchange(&f->PhotoPending,1,1)==1;
        if(pending&&pins[1]&&!pins[1]->Stopping&&(!pixels||KeQueryInterruptTime()>=f->PhotoAfter+333333))format=pins[1]->Format;
        ULONG_PTR bytes=0;NTSTATUS status=ParentIo(engine,QCAM_STREAM_NV12,&format,sizeof(format),engine->Storage,format.Width*format.Height*3/2,&bytes);
        if(NT_SUCCESS(status)&&bytes!=format.Width*format.Height*3/2)status=STATUS_DATA_ERROR;
        QCAM_FRAME_TIME time={0};if(NT_SUCCESS(status))status=ParentIo(engine,QCAM_FRAME_TIME_QUERY,NULL,0,&time,sizeof(time),NULL);
        BOOLEAN photo=FALSE;
        if(pending&&pins[1]&&!pins[1]->Stopping&&NT_SUCCESS(status)) {
            ULONGLONG readout=(time.Eof-time.Sof)*10000000/time.ClockHz;
            photo=time.Captured>=readout&&time.Captured-readout>=f->PhotoAfter&&format.Width==pins[1]->Format.Width&&format.Height==pins[1]->Format.Height;
            if(time.Captured>f->PhotoDeadline){status=STATUS_IO_TIMEOUT;photo=FALSE;}
        }
        if(photo||!NT_SUCCESS(status)&&pending){FlashFinish(f);InterlockedExchange(&f->PhotoPending,0);}
        for(ULONG i=0;i<3;i++)if(pins[i]) {
            if(i!=1||photo||pending&&!NT_SUCCESS(status))Deliver(pins[i],engine,&time,format.Width,format.Height,status);
            ExReleaseRundownProtection(&pins[i]->PhotoRundown);
        }
        LONG generation=f->FocusPending;
        if(generation) {
            QCAM_FOCUS_OPTIONS focus={0};NTSTATUS result=ParentIo(engine,QCAM_FOCUS,NULL,0,&focus,sizeof(focus),NULL);
            if(!NT_SUCCESS(result)||focus.State!=KSCAMERA_EXTENDEDPROP_FOCUSSTATE_SEARCHING)
                FocusComplete(filter,generation,NT_SUCCESS(result)&&focus.State==KSCAMERA_EXTENDEDPROP_FOCUSSTATE_FAILED?STATUS_IO_DEVICE_ERROR:result);
        }
        if(!NT_SUCCESS(status))break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID StopCapture(VIDEO_PIN *v) {
    InterlockedExchange(&v->Stopping,1);
    if(v->Thread){ZwWaitForSingleObject(v->Thread,FALSE,NULL);ZwClose(v->Thread);v->Thread=NULL;}
    KIRQL irql;KeAcquireSpinLock(&v->FrameLock,&irql);
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)v->Slots[i].State=0;
    KeReleaseSpinLock(&v->FrameLock,irql);
}

static NTSTATUS Open(VIDEO_PIN *v) {
    if(v->Parent)return STATUS_SUCCESS;
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"\\Device\\QcomCam");OBJECT_ATTRIBUTES a;
    InitializeObjectAttributes(&a,&name,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    IO_STATUS_BLOCK io;
    NTSTATUS status=ZwCreateFile(&v->Parent,GENERIC_READ|GENERIC_WRITE|SYNCHRONIZE,&a,&io,NULL,0,0,FILE_OPEN,
        FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,NULL,0);

    if(!NT_SUCCESS(status)&&v->Parent){ZwClose(v->Parent);v->Parent=NULL;}
    return status;
}

static NTSTATUS CreateFilter(PKSDEVICE device);

static NTSTATUS Add(PKSDEVICE device) {
    VIDEO_DEVICE *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),'vCcQ');
    if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=KsAddItemToObjectBag(device->Bag,v,NULL);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(v,'vCcQ');return status;}
    HANDLE key;status=IoOpenDeviceRegistryKey(device->PhysicalDeviceObject,PLUGPLAY_REGKEY_DRIVER,KEY_READ,&key);
    if(NT_SUCCESS(status)) {
        UNICODE_STRING name=RTL_CONSTANT_STRING(L"CameraIndex");UCHAR storage[sizeof(KEY_VALUE_PARTIAL_INFORMATION)+sizeof(ULONG)]={0};ULONG size=0;
        status=ZwQueryValueKey(key,&name,KeyValuePartialInformation,storage,sizeof(storage),&size);ZwClose(key);
        KEY_VALUE_PARTIAL_INFORMATION *value=(PVOID)storage;
        if(NT_SUCCESS(status)&&value->Type==REG_DWORD&&value->DataLength==sizeof(ULONG))RtlCopyMemory(&v->Sensor,value->Data,sizeof(ULONG));
    }
    if(v->Sensor>3)return STATUS_DEVICE_CONFIGURATION_ERROR;
    device->Context=v;return CreateFilter(device);
}

static NTSTATUS Start(PKSDEVICE device,PIRP irp,PCM_RESOURCE_LIST translated,PCM_RESOURCE_LIST raw) {
    UNREFERENCED_PARAMETER(irp);UNREFERENCED_PARAMETER(translated);UNREFERENCED_PARAMETER(raw);
    VIDEO_DEVICE *v=device->Context;InterlockedExchange(&v->Online,1);return STATUS_SUCCESS;
}

static VOID Stop(PKSDEVICE device,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);VIDEO_DEVICE *v=device->Context;InterlockedExchange(&v->Online,0);
}

static VOID Power(PKSDEVICE device,PIRP irp,DEVICE_POWER_STATE to,DEVICE_POWER_STATE from) {
    UNREFERENCED_PARAMETER(irp);UNREFERENCED_PARAMETER(from);
    VIDEO_DEVICE *v=device->Context;InterlockedExchange(&v->Online,to==PowerDeviceD0?1:0);
}

static BOOLEAN Format(PKSDATAFORMAT format,ULONG sensor) {
    if(!format||format->FormatSize<sizeof(KS_DATAFORMAT_VIDEOINFOHEADER2)||
       !IsEqualGUID(&format->MajorFormat,&KSDATAFORMAT_TYPE_VIDEO)||!IsEqualGUID(&format->SubFormat,&Nv12)||
       !IsEqualGUID(&format->Specifier,&KSDATAFORMAT_SPECIFIER_VIDEOINFO2))return FALSE;
    KS_VIDEOINFOHEADER2 *h=&((PKS_DATAFORMAT_VIDEOINFOHEADER2)format)->VideoInfoHeader2;
    ULONG w=(ULONG)h->bmiHeader.biWidth,height=(ULONG)h->bmiHeader.biHeight;
    BOOLEAN slow=sensor==1||sensor==2||w*height>1920*1080;
    return w>=160&&w<=QCAM_VIDEO_MAX_WIDTH&&height>=120&&height<=QCAM_VIDEO_MAX_HEIGHT&&!(w&7)&&!(height&1)&&
        h->bmiHeader.biCompression==0x3231564e&&h->bmiHeader.biPlanes==1&&h->bmiHeader.biBitCount==12&&
        h->bmiHeader.biSizeImage==w*height*3/2&&(h->AvgTimePerFrame==333333||h->AvgTimePerFrame==333334||
        !slow&&(h->AvgTimePerFrame==166666||h->AvgTimePerFrame==FrameInterval));
}

static NTSTATUS ApplyFormat(PKSPIN pin,VIDEO_PIN *v) {
    KS_VIDEOINFOHEADER2 *h=&((PKS_DATAFORMAT_VIDEOINFOHEADER2)pin->ConnectionFormat)->VideoInfoHeader2;
    ULONG bytes=h->bmiHeader.biSizeImage;
    NTSTATUS edited=KsEdit(pin,&pin->Descriptor,'pCcQ');if(!NT_SUCCESS(edited))return edited;
    edited=KsEdit(pin,&pin->Descriptor->AllocatorFraming,'aCcQ');if(!NT_SUCCESS(edited))return edited;
    PKSALLOCATOR_FRAMING_EX framing=(PKSALLOCATOR_FRAMING_EX)pin->Descriptor->AllocatorFraming;
    framing->FramingItem[0].Frames=3;framing->FramingItem[0].PhysicalRange.MinFrameSize=framing->FramingItem[0].PhysicalRange.MaxFrameSize=bytes;
    framing->FramingItem[0].FramingRange.Range.MinFrameSize=framing->FramingItem[0].FramingRange.Range.MaxFrameSize=bytes;
    if(bytes!=v->Bytes) {
        PUCHAR storage=ExAllocatePool2(POOL_FLAG_NON_PAGED,bytes*RTL_NUMBER_OF(v->Slots),'bCcQ');if(!storage)return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS status=KsAddItemToObjectBag(pin->Bag,storage,NULL);if(!NT_SUCCESS(status)){ExFreePoolWithTag(storage,'bCcQ');return status;}
        if(v->Storage)KsRemoveItemFromObjectBag(pin->Bag,v->Storage,TRUE);v->Storage=storage;v->Bytes=bytes;
    }
    v->Format.Size=sizeof(v->Format);v->Format.Abi=QCAM_ABI;v->Format.Width=h->bmiHeader.biWidth;v->Format.Height=h->bmiHeader.biHeight;v->Interval=h->AvgTimePerFrame;
    v->SourceWidth=v->SourceHeight=0;return STATUS_SUCCESS;
}

static NTSTATUS PinCreate(PKSPIN pin,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);
    pin->Context=NULL;
    if(!Format(pin->ConnectionFormat,((VIDEO_DEVICE *)KsPinGetDevice(pin)->Context)->Sensor))return STATUS_NO_MATCH;
    VIDEO_PIN *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),'iCcQ');if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=KsAddItemToObjectBag(pin->Bag,v,NULL);if(!NT_SUCCESS(status)){ExFreePoolWithTag(v,'iCcQ');return status;}
    status=ApplyFormat(pin,v);if(!NT_SUCCESS(status))return status;
    ExInitializeRundownProtection(&v->PhotoRundown);v->Pin=pin;v->Filter=KsPinGetParentFilter(pin);KeInitializeMutex(&v->IoLock,0);KeInitializeSpinLock(&v->FrameLock);v->Stopping=1;
    VIDEO_FILTER *f=v->Filter->Context;KIRQL irql;KeAcquireSpinLock(&f->PhotoLock,&irql);f->Endpoints[pin->Id]=v;if(pin->Id==1)f->Photo=v;KeReleaseSpinLock(&f->PhotoLock,irql);
    pin->Context=v;pin->StreamHeaderSize=sizeof(KSSTREAM_HEADER)+sizeof(KS_FRAME_INFO);return STATUS_SUCCESS;
}

static VOID EndpointClock(VIDEO_PIN *v) {
    if(!v->Clock)KsPinGetReferenceClockInterface(v->Pin,&v->Clock);
    LARGE_INTEGER frequency,counter=KeQueryPerformanceCounter(&frequency);ULONGLONG qpc;
    v->Started=KeQueryInterruptTimePrecise(&qpc);v->ClockStart=v->Clock?v->Clock->lpVtbl->GetTime(v->Clock):counter.QuadPart*10000000/frequency.QuadPart;
    RtlZeroMemory(&v->Timeline,sizeof(v->Timeline));v->Sequence=v->Dropped=0;v->NextSof=0;
}

static ULONG Running(VIDEO_FILTER *f) {
    ULONG count=0;KIRQL irql;KeAcquireSpinLock(&f->PhotoLock,&irql);
    for(ULONG i=0;i<3;i++)if(f->Endpoints[i]&&!f->Endpoints[i]->Stopping)count++;
    KeReleaseSpinLock(&f->PhotoLock,irql);return count;
}

static NTSTATUS StartEngine(PKSFILTER filter) {
    VIDEO_FILTER *f=filter->Context;VIDEO_PIN *v=f->Engine;VIDEO_DEVICE *d=KsFilterGetDevice(filter)->Context;
    if(v->Thread&&!v->Stopping)return STATUS_SUCCESS;
    NTSTATUS status=Open(v);if(!NT_SUCCESS(status))return status;
    QCAM_CAMERA_OPTIONS camera={sizeof(camera),QCAM_ABI,d->Sensor,0};status=ParentIo(v,QCAM_CAMERA,&camera,sizeof(camera),NULL,0,NULL);
    QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,5000,0,4980000000ULL};if(NT_SUCCESS(status))status=ParentIo(v,QCAM_STREAM_START,&options,sizeof(options),NULL,0,NULL);
    QCAM_FOCUS_OPTIONS focus={sizeof(focus),QCAM_ABI,f->FocusPosition,f->FocusFlags,0};if(NT_SUCCESS(status)&&!d->Sensor)status=ParentIo(v,QCAM_FOCUS,&focus,sizeof(focus),NULL,0,NULL);
    if(NT_SUCCESS(status)){QCAM_ZOOM_OPTIONS zoom={sizeof(zoom),QCAM_ABI,f->Zoom,0};status=ParentIo(v,QCAM_ZOOM,&zoom,sizeof(zoom),NULL,0,NULL);}
    if(NT_SUCCESS(status)&&!d->Sensor){QCAM_ROI_OPTIONS roi=f->Roi;roi.Size=sizeof(roi);roi.Abi=QCAM_ABI;status=ParentIo(v,QCAM_FOCUS_ROI,&roi,sizeof(roi),NULL,0,NULL);}
    if(NT_SUCCESS(status)&&f->TorchMode){QCAM_FLASH_OPTIONS flash={sizeof(flash),QCAM_ABI,QCAM_FLASH_TORCH,f->TorchPower,0,0,0};status=ParentIo(v,QCAM_FLASH,&flash,sizeof(flash),NULL,0,NULL);}
    if(!NT_SUCCESS(status)){CloseParent(v);return status;}
    OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);InterlockedExchange(&v->Stopping,0);
    status=PsCreateSystemThread(&v->Thread,SYNCHRONIZE,&attributes,NULL,NULL,Capture,v);if(!NT_SUCCESS(status)){InterlockedExchange(&v->Stopping,1);CloseParent(v);}return status;
}

static NTSTATUS PinClose(PKSPIN pin,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);VIDEO_PIN *v=PinContext(pin);if(!v)return STATUS_SUCCESS;
    VIDEO_FILTER *f=v->Filter->Context;InterlockedExchange(&v->Stopping,1);KIRQL irql;KeAcquireSpinLock(&f->PhotoLock,&irql);f->Endpoints[pin->Id]=NULL;if(pin->Id==1)f->Photo=NULL;KeReleaseSpinLock(&f->PhotoLock,irql);
    ExWaitForRundownProtectionRelease(&v->PhotoRundown);
    if(pin->Id==1){InterlockedExchange(&f->PhotoPending,0);FlashFinish(f);}
    if(!Running(f)){StopCapture(f->Engine);CloseParent(f->Engine);FocusComplete(v->Filter,f->FocusPending,STATUS_CANCELLED);}
    if(v->Clock){v->Clock->lpVtbl->Release(v->Clock);v->Clock=NULL;}pin->Context=NULL;return STATUS_SUCCESS;
}

static NTSTATUS SetFormat(PKSPIN pin,PKSDATAFORMAT old,PKSMULTIPLE_ITEM oldAttributes,const KSDATARANGE *range,const KSATTRIBUTE_LIST *attributes) {
    UNREFERENCED_PARAMETER(oldAttributes);UNREFERENCED_PARAMETER(range);UNREFERENCED_PARAMETER(attributes);
    if(!Format(pin->ConnectionFormat,((VIDEO_DEVICE *)KsPinGetDevice(pin)->Context)->Sensor))return STATUS_NO_MATCH;
    if(!old)return STATUS_SUCCESS;
    VIDEO_PIN *v=PinContext(pin);if(!v)return STATUS_INVALID_DEVICE_STATE;
    KS_VIDEOINFOHEADER2 *h=&((PKS_DATAFORMAT_VIDEOINFOHEADER2)pin->ConnectionFormat)->VideoInfoHeader2;
    if(v->Format.Width==(ULONG)h->bmiHeader.biWidth&&v->Format.Height==(ULONG)h->bmiHeader.biHeight&&
       v->Interval>=h->AvgTimePerFrame-1&&v->Interval<=h->AvgTimePerFrame+1)return STATUS_SUCCESS;
    BOOLEAN running=!v->Stopping;InterlockedExchange(&v->Stopping,1);ExWaitForRundownProtectionRelease(&v->PhotoRundown);
    NTSTATUS status=ApplyFormat(pin,v);KIRQL irql;KeAcquireSpinLock(&v->FrameLock,&irql);
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)v->Slots[i].State=0;KeReleaseSpinLock(&v->FrameLock,irql);
    ExReInitializeRundownProtection(&v->PhotoRundown);
    if(running&&NT_SUCCESS(status)){v->Resizing=TRUE;EndpointClock(v);InterlockedExchange(&v->Stopping,0);}return status;
}

static NTSTATUS State(PKSPIN pin,KSSTATE to,KSSTATE from) {
    UNREFERENCED_PARAMETER(from);VIDEO_PIN *v=PinContext(pin);if(!v)return to==KSSTATE_RUN?STATUS_INVALID_DEVICE_STATE:STATUS_SUCCESS;
    VIDEO_FILTER *f=v->Filter->Context;
    if(to!=KSSTATE_RUN) {
        LONG stopping=InterlockedExchange(&v->Stopping,1);
        if(!stopping)ExWaitForRundownProtectionRelease(&v->PhotoRundown);
        if(pin->Id==1){InterlockedExchange(&f->PhotoPending,0);FlashFinish(f);}
        if(!Running(f)){StopCapture(f->Engine);CloseParent(f->Engine);FocusComplete(v->Filter,f->FocusPending,STATUS_CANCELLED);}
        KIRQL irql;KeAcquireSpinLock(&v->FrameLock,&irql);for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)v->Slots[i].State=0;KeReleaseSpinLock(&v->FrameLock,irql);
        if(!stopping)ExReInitializeRundownProtection(&v->PhotoRundown);
        return STATUS_SUCCESS;
    }
    if(!((VIDEO_DEVICE *)KsPinGetDevice(pin)->Context)->Online)return STATUS_DEVICE_NOT_READY;
    EndpointClock(v);v->CaptureStatus=STATUS_SUCCESS;InterlockedExchange(&v->Stopping,0);
    NTSTATUS status=StartEngine(v->Filter);if(!NT_SUCCESS(status))InterlockedExchange(&v->Stopping,1);return status;
}

static NTSTATUS Process(PKSPIN pin) {
    if(pin->Id>=3)return STATUS_INVALID_DEVICE_STATE;
    VIDEO_FILTER *filter=KsPinGetParentFilter(pin)->Context;
    VIDEO_PIN *v=EndpointReference(filter,pin->Id);VIDEO_DEVICE *d=KsPinGetDevice(pin)->Context;
    if(!v)return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=STATUS_DEVICE_NOT_READY;
    if(v!=pin->Context||v->Pin!=pin||v->Stopping||!d->Online)goto done;
    PKSSTREAM_POINTER p=KsPinGetLeadingEdgeStreamPointer(pin,KSSTREAM_POINTER_STATE_LOCKED);
    if(!p){status=STATUS_PENDING;goto done;}
    if(v->Resizing&&p->OffsetOut.Remaining<v->Bytes) {p->StreamHeader->DataUsed=0;KsStreamPointerAdvanceOffsetsAndUnlock(p,0,0,TRUE);status=STATUS_SUCCESS;goto done;}
    v->Resizing=FALSE;
    KIRQL irql;ULONG slot=RTL_NUMBER_OF(v->Slots);
    KeAcquireSpinLock(&v->FrameLock,&irql);
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)if(v->Slots[i].State==2){slot=i;v->Slots[i].State=3;break;}
    status=v->CaptureStatus;
    KeReleaseSpinLock(&v->FrameLock,irql);
    if(slot==RTL_NUMBER_OF(v->Slots)) {
        if(NT_SUCCESS(status)){KsStreamPointerUnlock(p,FALSE);status=STATUS_PENDING;goto done;}
    } else {
        status=STATUS_BUFFER_TOO_SMALL;
        if(p->OffsetOut.Remaining>=v->Bytes) {
            RtlCopyMemory(p->OffsetOut.Data,v->Storage+slot*v->Bytes,v->Bytes);status=STATUS_SUCCESS;
        }
    }
    KSSTREAM_HEADER *h=p->StreamHeader;
    if(NT_SUCCESS(status)) {
        VIDEO_FRAME *frame=v->Slots+slot;h->DataUsed=v->Bytes;
        h->OptionsFlags&=~(KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM|KSSTREAM_HEADER_OPTIONSF_TIMEVALID|KSSTREAM_HEADER_OPTIONSF_DATADISCONTINUITY);
        if(!v->Sequence)h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_DATADISCONTINUITY;
        h->PresentationTime.Time=frame->Presentation;
        h->PresentationTime.Numerator=1;h->PresentationTime.Denominator=1;
        h->Duration=v->Interval;h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_DURATIONVALID|KSSTREAM_HEADER_OPTIONSF_TIMEVALID;
        if(h->Size>=sizeof(*h)+sizeof(KS_FRAME_INFO)) {
            KS_FRAME_INFO *f=(KS_FRAME_INFO *)(h+1);RtlZeroMemory(f,sizeof(*f));
            f->ExtendedHeaderSize=sizeof(*f);f->PictureNumber=frame->Time.Sequence+frame->Time.Dropped;
            h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_FRAMEINFO;
        }
        v->Dropped=frame->Time.Dropped;v->Sequence=frame->Time.Sequence;
    } else {
        h->DataUsed=0;h->OptionsFlags&=~KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM;
        KsStreamPointerSetStatusCode(p,status);
    }
    if(slot!=RTL_NUMBER_OF(v->Slots)) {
        KeAcquireSpinLock(&v->FrameLock,&irql);v->Slots[slot].State=0;KeReleaseSpinLock(&v->FrameLock,irql);
    }
    KsStreamPointerAdvanceOffsetsAndUnlock(p,0,NT_SUCCESS(status)?v->Bytes:0,TRUE);
done:
    ExReleaseRundownProtection(&v->PhotoRundown);
    return status;
}

static const KS_DATARANGE_VIDEO2 VideoRange={
    {sizeof(KS_DATARANGE_VIDEO2),0,QCAM_VIDEO_BYTES,0,{STATIC_KSDATAFORMAT_TYPE_VIDEO},{STATIC_QCAM_NV12},{STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO2}},
    TRUE,FALSE,KS_VIDEOSTREAM_CAPTURE,0,
    {{STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO2},0,{QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},
        {QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},{QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},1,1,1,1,
        {QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},{QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},1,1,0,0,0,0,166666,166667,QCAM_VIDEO_BYTES*8*60,QCAM_VIDEO_BYTES*8*60},
    {{0,0,QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},{0,0,QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},QCAM_VIDEO_BYTES*8*60,0,166667,
        0,0,4,3,0,0,{sizeof(KS_BITMAPINFOHEADER),QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT,1,12,0x3231564e,QCAM_VIDEO_BYTES,0,0,0,0}}
};
static const PKSDATARANGE Ranges[]={(PKSDATARANGE)&VideoRange.DataRange};

static NTSTATUS Intersect(PVOID context,PIRP irp,PKSP_PIN pin,PKSDATARANGE requested,PKSDATARANGE supported,ULONG size,PVOID data,PULONG dataSize) {
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(irp);UNREFERENCED_PARAMETER(pin);
    if(!IsEqualGUID(&requested->MajorFormat,&KSDATAFORMAT_TYPE_VIDEO)||!IsEqualGUID(&requested->SubFormat,&Nv12)||
       !IsEqualGUID(&requested->Specifier,&KSDATAFORMAT_SPECIFIER_VIDEOINFO2))return STATUS_NO_MATCH;
    if(requested->FormatSize>=sizeof(KS_DATARANGE_VIDEO2)) {
        KS_VIDEOINFOHEADER2 *r=&((KS_DATARANGE_VIDEO2 *)requested)->VideoInfoHeader,*h=&((KS_DATARANGE_VIDEO2 *)supported)->VideoInfoHeader;
        if(r->bmiHeader.biWidth&&r->bmiHeader.biWidth!=h->bmiHeader.biWidth||r->bmiHeader.biHeight&&r->bmiHeader.biHeight!=h->bmiHeader.biHeight)return STATUS_NO_MATCH;
        if(r->AvgTimePerFrame&&(r->AvgTimePerFrame<((KS_DATARANGE_VIDEO2 *)supported)->ConfigCaps.MinFrameInterval||r->AvgTimePerFrame>((KS_DATARANGE_VIDEO2 *)supported)->ConfigCaps.MaxFrameInterval))return STATUS_NO_MATCH;
    }
    *dataSize=sizeof(KS_DATAFORMAT_VIDEOINFOHEADER2);
    if(!size)return STATUS_BUFFER_OVERFLOW;
    if(size<*dataSize)return STATUS_BUFFER_TOO_SMALL;
    PKS_DATAFORMAT_VIDEOINFOHEADER2 f=data;f->DataFormat=*supported;f->DataFormat.FormatSize=*dataSize;
    f->VideoInfoHeader2=((KS_DATARANGE_VIDEO2 *)supported)->VideoInfoHeader;return STATUS_SUCCESS;
}

DECLARE_SIMPLE_FRAMING_EX(Framing,STATIC_KSMEMORY_TYPE_KERNEL_NONPAGED,
    KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY|KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,3,0,160*120*3/2,QCAM_VIDEO_MAX_BYTES);

static const KSPIN_DISPATCH PinDispatch={PinCreate,PinClose,Process,NULL,SetFormat,State,NULL,NULL,NULL,NULL};
static const KSPIN_DESCRIPTOR_EX Pins[]={
    {&PinDispatch,NULL,{0,NULL,0,NULL,RTL_NUMBER_OF(Ranges),Ranges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_BOTH,
        &PINNAME_VIDEO_CAPTURE,NULL,0},KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY,1,0,&Framing,Intersect},
    {&PinDispatch,NULL,{0,NULL,0,NULL,RTL_NUMBER_OF(Ranges),Ranges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_BOTH,
        &PINNAME_IMAGE,&PINNAME_IMAGE,0},KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY,1,0,&Framing,Intersect},
    {&PinDispatch,NULL,{0,NULL,0,NULL,RTL_NUMBER_OF(Ranges),Ranges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_BOTH,
        &PINNAME_VIDEO_PREVIEW,&PINNAME_VIDEO_PREVIEW,0},KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY,1,0,&Framing,Intersect}
};
static const KSPROPERTY_STEPPING_LONG FocusRange={1,0,{QCAM_FOCUS_MIN,QCAM_FOCUS_MAX}};
static const LONG FocusDefault=QCAM_FOCUS_MIN;
static const KSPROPERTY_MEMBERSLIST FocusMembers[]={
    {{KSPROPERTY_MEMBER_STEPPEDRANGES,sizeof(FocusRange),1,0},&FocusRange},
    {{KSPROPERTY_MEMBER_VALUES,sizeof(FocusDefault),1,KSPROPERTY_MEMBER_FLAG_DEFAULT},&FocusDefault}
};
static const KSPROPERTY_VALUES FocusValues={.PropTypeSet={.Set={STATIC_KSPROPTYPESETID_General},.Id=VT_I4},.MembersListCount=RTL_NUMBER_OF(FocusMembers),.MembersList=FocusMembers};
static const KSPROPERTY_STEPPING_LONG ZoomRange={1,0,{100,400}};
static const LONG ZoomDefault=100;
static const KSPROPERTY_MEMBERSLIST ZoomMembers[]={
    {{KSPROPERTY_MEMBER_STEPPEDRANGES,sizeof(ZoomRange),1,0},&ZoomRange},
    {{KSPROPERTY_MEMBER_VALUES,sizeof(ZoomDefault),1,KSPROPERTY_MEMBER_FLAG_DEFAULT},&ZoomDefault}
};
static const KSPROPERTY_VALUES ZoomValues={.PropTypeSet={.Set={STATIC_KSPROPTYPESETID_General},.Id=VT_I4},.MembersListCount=RTL_NUMBER_OF(ZoomMembers),.MembersList=ZoomMembers};
static const KSPROPERTY_ITEM FocusProperties[]={
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_FOCUS,Focus,sizeof(KSPROPERTY_CAMERACONTROL_S),sizeof(KSPROPERTY_CAMERACONTROL_S),Focus,&FocusValues,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_ZOOM,Zoom,sizeof(KSPROPERTY_CAMERACONTROL_S),sizeof(KSPROPERTY_CAMERACONTROL_S),Zoom,&ZoomValues,0,NULL,NULL,0)
};
static const KSPROPERTY_ITEM ExtendedProperties[]={
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_PHOTOMODE,PhotoMode,sizeof(KSPROPERTY),0,PhotoMode,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FLASHMODE,ExtendedFlash,sizeof(KSPROPERTY),0,ExtendedFlash,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_CONFIGCAPS,ExtendedRoi,sizeof(KSPROPERTY),0,NULL,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_ISPCONTROL,ExtendedRoi,sizeof(KSPROPERTY),0,ExtendedRoi,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_TORCHMODE,ExtendedTorch,sizeof(KSPROPERTY),0,ExtendedTorch,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE,ExtendedFocus,sizeof(KSPROPERTY),0,ExtendedFocus,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSSTATE,ExtendedFocus,sizeof(KSPROPERTY),0,NULL,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_ZOOM,ExtendedZoom,sizeof(KSPROPERTY),0,ExtendedZoom,NULL,0,NULL,NULL,0)
};
static const KSEVENT_ITEM FocusEvents[]={DEFINE_KSEVENT_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_PHOTOMODE,sizeof(KSEVENTDATA),0,NULL,NULL,NULL),DEFINE_KSEVENT_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE,sizeof(KSEVENTDATA),0,NULL,NULL,NULL),DEFINE_KSEVENT_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_ROI_ISPCONTROL,sizeof(KSEVENTDATA),0,NULL,NULL,NULL)};
static const KSEVENT_SET EventSets[]={{&KSEVENTSETID_ExtendedCameraControl,RTL_NUMBER_OF(FocusEvents),FocusEvents}};
static const KSPROPERTY_ITEM FlashProperties[]={DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_FLASH_PROPERTY_ID,LegacyFlash,sizeof(KSPROPERTY),sizeof(KSPROPERTY_CAMERACONTROL_FLASH_S),LegacyFlash,NULL,0,NULL,NULL,0)};
static const KSPROPERTY_ITEM VideoProperties[]={
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_VIDEOCONTROL_CAPS,VideoControl,sizeof(KSPROPERTY),0,NULL,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_VIDEOCONTROL_MODE,VideoControl,sizeof(KSPROPERTY),0,VideoControl,NULL,0,NULL,NULL,0)
};
static const KSPROPERTY_SET PropertySets[]={
    {&PROPSETID_VIDCAP_CAMERACONTROL_FLASH,RTL_NUMBER_OF(FlashProperties),FlashProperties,0,NULL},
    {&PROPSETID_VIDCAP_VIDEOCONTROL,RTL_NUMBER_OF(VideoProperties),VideoProperties,0,NULL},
    {&PROPSETID_VIDCAP_CAMERACONTROL,RTL_NUMBER_OF(FocusProperties),FocusProperties,0,NULL},
    {&KSPROPERTYSETID_ExtendedCameraControl,RTL_NUMBER_OF(ExtendedProperties),ExtendedProperties,0,NULL}
};
static const KSAUTOMATION_TABLE Automation={DEFINE_KSAUTOMATION_PROPERTIES(PropertySets),DEFINE_KSAUTOMATION_METHODS_NULL,DEFINE_KSAUTOMATION_EVENTS(EventSets)};
static NTSTATUS FilterClose(PKSFILTER filter,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);VIDEO_FILTER *f=filter->Context;StopCapture(f->Engine);CloseParent(f->Engine);return STATUS_SUCCESS;
}
static const KSFILTER_DISPATCH FilterDispatch={FilterCreate,FilterClose,NULL,NULL};
static const KSFILTER_DESCRIPTOR Filter={&FilterDispatch,&Automation,KSFILTER_DESCRIPTOR_VERSION,0,&KSNAME_Filter,
    RTL_NUMBER_OF(Pins),sizeof(Pins[0]),Pins,RTL_NUMBER_OF(Categories),Categories,0,sizeof(KSNODE_DESCRIPTOR),NULL,0,NULL,NULL};
static NTSTATUS CreateFilter(PKSDEVICE device) {
    VIDEO_DEVICE *v=device->Context;
    static const ULONG dimensions[][2]={{160,120},{320,240},{640,360},{640,480},{800,600},{960,540},{1024,768},
        {1280,720},{1280,960},{1600,900},{1600,1200},{1920,1080},{1920,1440},{2048,1536},{2304,1728},
        {2560,1440},{2560,1920},{3264,2448},{3840,2160},{4096,2304},{4096,3072}};
    for(ULONG pin=0;pin<3;pin++) {
        ULONG preferredW=pin==2?640:pin==1?(v->Sensor==1?3264:v->Sensor==2?1600:v->Sensor==3?2304:4096):1280;
        ULONG preferredH=pin==2?480:pin==1?(v->Sensor==1?2448:v->Sensor==2?1200:v->Sensor==3?1728:3072):720;
        for(ULONG n=0;n<=RTL_NUMBER_OF(dimensions);n++) {
            ULONG w=n?dimensions[n-1][0]:preferredW,h=n?dimensions[n-1][1]:preferredH;if(n&&w==preferredW&&h==preferredH)continue;
            BOOLEAN slow=v->Sensor==1||v->Sensor==2||w*h>1920*1080;
            for(ULONG fps=slow?30:60;fps>=30;fps-=30) {
                ULONG index=v->RangeCount[pin]++;if(index>=VIDEO_RANGE_COUNT)return STATUS_INSUFFICIENT_RESOURCES;
                KS_DATARANGE_VIDEO2 *r=&v->Range[pin][index];*r=VideoRange;ULONG bytes=w*h*3/2;LONGLONG interval=fps==60?166667:333333;
                r->DataRange.SampleSize=bytes;r->StreamDescriptionFlags=pin==1?KS_VIDEOSTREAM_STILL:pin==2?KS_VIDEOSTREAM_PREVIEW:KS_VIDEOSTREAM_CAPTURE;
                r->ConfigCaps.InputSize.cx=r->ConfigCaps.MinCroppingSize.cx=r->ConfigCaps.MaxCroppingSize.cx=r->ConfigCaps.MinOutputSize.cx=r->ConfigCaps.MaxOutputSize.cx=w;
                r->ConfigCaps.InputSize.cy=r->ConfigCaps.MinCroppingSize.cy=r->ConfigCaps.MaxCroppingSize.cy=r->ConfigCaps.MinOutputSize.cy=r->ConfigCaps.MaxOutputSize.cy=h;
                r->ConfigCaps.MinFrameInterval=interval-1;r->ConfigCaps.MaxFrameInterval=interval;r->ConfigCaps.MinBitsPerSecond=r->ConfigCaps.MaxBitsPerSecond=(LONG)min((ULONGLONG)bytes*8*fps,2147483647ULL);
                r->VideoInfoHeader.rcSource.right=r->VideoInfoHeader.rcTarget.right=w;r->VideoInfoHeader.rcSource.bottom=r->VideoInfoHeader.rcTarget.bottom=h;
                r->VideoInfoHeader.dwBitRate=(ULONG)min((ULONGLONG)bytes*8*fps,4294967295ULL);r->VideoInfoHeader.AvgTimePerFrame=interval;r->VideoInfoHeader.dwPictAspectRatioX=w;r->VideoInfoHeader.dwPictAspectRatioY=h;
                r->VideoInfoHeader.bmiHeader.biWidth=w;r->VideoInfoHeader.bmiHeader.biHeight=h;r->VideoInfoHeader.bmiHeader.biSizeImage=bytes;
                v->Ranges[pin][index]=(PKSDATARANGE)r;
            }
        }
        v->Pins[pin]=Pins[pin];v->Pins[pin].PinDescriptor.DataRanges=v->Ranges[pin];v->Pins[pin].PinDescriptor.DataRangesCount=v->RangeCount[pin];
    }
    v->Filter=Filter;v->Filter.PinDescriptors=v->Pins;
    return KsCreateFilterFactory(device->FunctionalDeviceObject,&v->Filter,NULL,NULL,0,NULL,NULL,NULL);
}
static const KSDEVICE_DISPATCH DeviceDispatch={Add,Start,NULL,NULL,NULL,Stop,NULL,NULL,Stop,NULL,Stop,NULL,Power,NULL};
static const KSDEVICE_DESCRIPTOR DeviceDescriptor={&DeviceDispatch,0,NULL,KSDEVICE_DESCRIPTOR_VERSION_2,0};

NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry) {return KsInitializeDriver(object,registry,&DeviceDescriptor);}
