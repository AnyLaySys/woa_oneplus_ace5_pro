#include <ntifs.h>
#include <windef.h>
#include <unknown.h>
#include <ks.h>
#include <ksmedia.h>
#include "../uapi.h"
#include "../frame-clock.h"

DRIVER_INITIALIZE DriverEntry;

typedef struct { volatile LONG Online; } VIDEO_DEVICE;
typedef struct { ULONG FocusPosition,FocusFlags,Zoom; ULONGLONG FocusMode; volatile LONG FocusPending,FocusGeneration; NTSTATUS FocusResult; } VIDEO_FILTER;
typedef struct { ULONG State; QCAM_FRAME_TIME Time; LONGLONG Presentation; } VIDEO_FRAME;
typedef struct {
    HANDLE Parent,Thread; PKSPIN Pin; KMUTEX IoLock; KSPIN_LOCK FrameLock;
    volatile LONG Stopping; PUCHAR Storage; VIDEO_FRAME Slots[3]; NTSTATUS CaptureStatus;
    PIKSREFERENCECLOCK Clock; ULONGLONG Started; LONGLONG ClockStart;
    ULONG Sequence,Dropped; CAM_FRAME_CLOCK Timeline;
} VIDEO_PIN;
#define STATIC_QCAM_NV12 0x3231564e,0,0x0010,0x80,0,0,0xaa,0,0x38,0x9b,0x71
static const GUID Nv12={STATIC_QCAM_NV12};
static const GUID Categories[]={{STATIC_KSCATEGORY_CAPTURE},{STATIC_KSCATEGORY_VIDEO},{STATIC_KSCATEGORY_VIDEO_CAMERA}};
static const LONGLONG FrameInterval=166667;

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
    v->FocusPosition=QCAM_FOCUS_MIN;v->FocusFlags=QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS;v->Zoom=QCAM_ZOOM_MIN;v->FocusMode=KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS|KSCAMERA_EXTENDEDPROP_FOCUS_RANGE_FULLRANGE;filter->Context=v;return STATUS_SUCCESS;
}

static NTSTATUS SetZoom(PKSFILTER filter,ULONG ratio) {
    if(ratio<QCAM_ZOOM_MIN||ratio>QCAM_ZOOM_MAX)return STATUS_INVALID_PARAMETER;
    VIDEO_FILTER *f=filter->Context;PKSPIN pin=KsFilterGetFirstChildPin(filter,0);VIDEO_PIN *v=pin?pin->Context:NULL;
    NTSTATUS status=STATUS_SUCCESS;QCAM_ZOOM_OPTIONS zoom={sizeof(zoom),QCAM_ABI,ratio,0};
    if(v&&v->Parent&&pin->DeviceState==KSSTATE_RUN)status=ParentIo(v,QCAM_ZOOM,&zoom,sizeof(zoom),NULL,0,NULL);
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

static NTSTATUS Focus(PIRP irp,PKSIDENTIFIER request,PVOID data) {
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    KSPROPERTY_CAMERACONTROL_S *p=data;PKSPIN pin=KsFilterGetFirstChildPin(filter,0);
    VIDEO_PIN *v=pin?pin->Context:NULL;QCAM_FOCUS_OPTIONS options={sizeof(options),QCAM_ABI,f->FocusPosition,f->FocusFlags,0};
    NTSTATUS status=STATUS_SUCCESS;
    if(request->Flags&KSPROPERTY_TYPE_SET) {
        if(p->Flags!=QCAM_FOCUS_MANUAL&&p->Flags!=QCAM_FOCUS_AUTO)return STATUS_NOT_SUPPORTED;
        if(p->Flags==QCAM_FOCUS_MANUAL&&(p->Value<(LONG)QCAM_FOCUS_MIN||p->Value>(LONG)QCAM_FOCUS_MAX))return STATUS_INVALID_PARAMETER;
        options.Position=p->Flags==QCAM_FOCUS_MANUAL?(ULONG)p->Value:f->FocusPosition;options.Flags=p->Flags==QCAM_FOCUS_AUTO?QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS:QCAM_FOCUS_MANUAL;
        if(v&&v->Parent&&pin->DeviceState==KSSTATE_RUN)status=ParentIo(v,QCAM_FOCUS,&options,sizeof(options),NULL,0,NULL);
        if(NT_SUCCESS(status)){FocusComplete(filter,f->FocusPending,STATUS_CANCELLED);f->FocusPosition=options.Position;f->FocusFlags=options.Flags;f->FocusMode=p->Flags==QCAM_FOCUS_AUTO?KSCAMERA_EXTENDEDPROP_FOCUS_CONTINUOUS|KSCAMERA_EXTENDEDPROP_FOCUS_RANGE_FULLRANGE:KSCAMERA_EXTENDEDPROP_VIDEOPROCFLAG_MANUAL;}
    } else {
        if(v&&v->Parent&&pin->DeviceState==KSSTATE_RUN)status=ParentIo(v,QCAM_FOCUS,NULL,0,&options,sizeof(options),NULL);
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
    PKSFILTER filter=KsGetFilterFromIrp(irp);VIDEO_FILTER *f=filter->Context;
    PKSPIN pin=KsFilterGetFirstChildPin(filter,0);VIDEO_PIN *v=pin?pin->Context:NULL;
    BOOLEAN running=v&&v->Parent&&pin->DeviceState==KSSTATE_RUN;
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

static VOID Capture(PVOID context) {
    VIDEO_PIN *v=context;PKSPIN pin=v->Pin;VIDEO_DEVICE *d=KsPinGetDevice(pin)->Context;
    while(!v->Stopping&&d->Online) {
        KIRQL irql;ULONG slot=RTL_NUMBER_OF(v->Slots);
        KeAcquireSpinLock(&v->FrameLock,&irql);
        for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)if(!v->Slots[i].State){slot=i;v->Slots[i].State=1;break;}
        KeReleaseSpinLock(&v->FrameLock,irql);
        if(slot==RTL_NUMBER_OF(v->Slots)){v->CaptureStatus=STATUS_INTERNAL_ERROR;break;}
        VIDEO_FRAME *frame=v->Slots+slot;ULONG_PTR bytes=0;
        NTSTATUS status=ParentIo(v,QCAM_STREAM_NV12,NULL,0,v->Storage+slot*QCAM_VIDEO_BYTES,QCAM_VIDEO_BYTES,&bytes);
        if(NT_SUCCESS(status)&&bytes!=QCAM_VIDEO_BYTES)status=STATUS_DATA_ERROR;
        if(NT_SUCCESS(status)) {
            status=ParentIo(v,QCAM_FRAME_TIME_QUERY,NULL,0,&frame->Time,sizeof(frame->Time),&bytes);
            if(NT_SUCCESS(status)&&(bytes!=sizeof(frame->Time)||!CamFrameClock(&v->Timeline,&frame->Time,v->Started,v->ClockStart,&frame->Presentation)))status=STATUS_DATA_ERROR;
        }
        KeAcquireSpinLock(&v->FrameLock,&irql);
        if(NT_SUCCESS(status)&&!v->Stopping) {
            for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)if(v->Slots[i].State==2)v->Slots[i].State=0;
            frame->State=2;
        } else {frame->State=0;v->CaptureStatus=status;}
        KeReleaseSpinLock(&v->FrameLock,irql);
        if(v->Stopping)break;
        PKSFILTER filter=KsPinGetParentFilter(pin);VIDEO_FILTER *controls=filter->Context;LONG generation=controls->FocusPending;
        if(generation) {
            QCAM_FOCUS_OPTIONS focus={0};NTSTATUS result=ParentIo(v,QCAM_FOCUS,NULL,0,&focus,sizeof(focus),NULL);
            if(!NT_SUCCESS(result)||focus.State!=KSCAMERA_EXTENDEDPROP_FOCUSSTATE_SEARCHING)
                FocusComplete(filter,generation,NT_SUCCESS(result)&&focus.State==KSCAMERA_EXTENDEDPROP_FOCUSSTATE_FAILED?STATUS_IO_DEVICE_ERROR:result);
        }
        KsPinAttemptProcessing(pin,TRUE);
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
    if(NT_SUCCESS(status))status=ParentIo(v,QCAM_PROBE,NULL,0,NULL,0,NULL);
    if(!NT_SUCCESS(status)&&v->Parent){ZwClose(v->Parent);v->Parent=NULL;}
    return status;
}

static NTSTATUS Add(PKSDEVICE device) {
    VIDEO_DEVICE *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),'vCcQ');
    if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=KsAddItemToObjectBag(device->Bag,v,NULL);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(v,'vCcQ');return status;}
    device->Context=v;return STATUS_SUCCESS;
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

static BOOLEAN Format(PKSDATAFORMAT format) {
    if(!format||format->FormatSize<sizeof(KS_DATAFORMAT_VIDEOINFOHEADER2)||
       !IsEqualGUID(&format->MajorFormat,&KSDATAFORMAT_TYPE_VIDEO)||
       !IsEqualGUID(&format->SubFormat,&Nv12)||
       !IsEqualGUID(&format->Specifier,&KSDATAFORMAT_SPECIFIER_VIDEOINFO2))return FALSE;
    KS_VIDEOINFOHEADER2 *h=&((PKS_DATAFORMAT_VIDEOINFOHEADER2)format)->VideoInfoHeader2;
    return h->bmiHeader.biWidth==QCAM_VIDEO_WIDTH&&h->bmiHeader.biHeight==QCAM_VIDEO_HEIGHT&&
        h->bmiHeader.biCompression==0x3231564e&&h->bmiHeader.biPlanes==1&&h->bmiHeader.biBitCount==12&&
        h->bmiHeader.biSizeImage==QCAM_VIDEO_BYTES&&(h->AvgTimePerFrame==166666||h->AvgTimePerFrame==FrameInterval);
}

static NTSTATUS PinCreate(PKSPIN pin,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);
    if(!Format(pin->ConnectionFormat))return STATUS_NO_MATCH;
    VIDEO_PIN *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),'iCcQ');
    if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=KsAddItemToObjectBag(pin->Bag,v,NULL);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(v,'iCcQ');return status;}
    v->Storage=ExAllocatePool2(POOL_FLAG_NON_PAGED,QCAM_VIDEO_BYTES*RTL_NUMBER_OF(v->Slots),'bCcQ');
    if(!v->Storage)return STATUS_INSUFFICIENT_RESOURCES;
    status=KsAddItemToObjectBag(pin->Bag,v->Storage,NULL);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(v->Storage,'bCcQ');v->Storage=NULL;return status;}
    v->Pin=pin;KeInitializeMutex(&v->IoLock,0);KeInitializeSpinLock(&v->FrameLock);v->Stopping=1;
    pin->Context=v;pin->StreamHeaderSize=sizeof(KSSTREAM_HEADER)+sizeof(KS_FRAME_INFO);
    return STATUS_SUCCESS;
}

static NTSTATUS PinClose(PKSPIN pin,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);VIDEO_PIN *v=pin->Context;
    if(v) {
        StopCapture(v);
        FocusComplete(KsPinGetParentFilter(pin),((VIDEO_FILTER *)KsPinGetParentFilter(pin)->Context)->FocusPending,STATUS_CANCELLED);
        if(v->Clock){v->Clock->lpVtbl->Release(v->Clock);v->Clock=NULL;}
        CloseParent(v);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS SetFormat(PKSPIN pin,PKSDATAFORMAT old,PKSMULTIPLE_ITEM oldAttributes,const KSDATARANGE *range,const KSATTRIBUTE_LIST *attributes) {
    UNREFERENCED_PARAMETER(old);UNREFERENCED_PARAMETER(oldAttributes);UNREFERENCED_PARAMETER(range);UNREFERENCED_PARAMETER(attributes);
    return Format(pin->ConnectionFormat)?STATUS_SUCCESS:STATUS_NO_MATCH;
}

static NTSTATUS State(PKSPIN pin,KSSTATE to,KSSTATE from) {
    VIDEO_PIN *v=pin->Context;
    if(to!=KSSTATE_RUN&&(from==KSSTATE_RUN||to==KSSTATE_STOP)) {
        StopCapture(v);
        PKSFILTER filter=KsPinGetParentFilter(pin);FocusComplete(filter,((VIDEO_FILTER *)filter->Context)->FocusPending,STATUS_CANCELLED);
        if(v->Clock){v->Clock->lpVtbl->Release(v->Clock);v->Clock=NULL;}
        CloseParent(v);v->Started=0;v->ClockStart=0;v->Sequence=0;v->Dropped=0;
        RtlZeroMemory(&v->Timeline,sizeof(v->Timeline));
    }
    if(to==KSSTATE_STOP)return STATUS_SUCCESS;
    VIDEO_DEVICE *d=KsPinGetDevice(pin)->Context;
    if(!d->Online)return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=Open(v);
    if(NT_SUCCESS(status)&&to==KSSTATE_RUN&&!v->Thread) {
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,5000,0,4980000000ULL};
        status=ParentIo(v,QCAM_STREAM_START,&options,sizeof(options),NULL,0,NULL);
        if(!NT_SUCCESS(status))return status;
        VIDEO_FILTER *f=KsPinGetParentFilter(pin)->Context;
        QCAM_FOCUS_OPTIONS focus={sizeof(focus),QCAM_ABI,f->FocusPosition,f->FocusFlags,0};
        status=ParentIo(v,QCAM_FOCUS,&focus,sizeof(focus),NULL,0,NULL);
        if(NT_SUCCESS(status)){QCAM_ZOOM_OPTIONS zoom={sizeof(zoom),QCAM_ABI,f->Zoom,0};status=ParentIo(v,QCAM_ZOOM,&zoom,sizeof(zoom),NULL,0,NULL);}
        if(!NT_SUCCESS(status)){CloseParent(v);return status;}
        RtlZeroMemory(&v->Timeline,sizeof(v->Timeline));v->Sequence=v->Dropped=0;v->CaptureStatus=STATUS_SUCCESS;
        if(!v->Clock)KsPinGetReferenceClockInterface(pin,&v->Clock);
        LARGE_INTEGER frequency,counter=KeQueryPerformanceCounter(&frequency);
        ULONGLONG qpc;v->Started=KeQueryInterruptTimePrecise(&qpc);
        v->ClockStart=v->Clock?v->Clock->lpVtbl->GetTime(v->Clock):counter.QuadPart*10000000/frequency.QuadPart;
        OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
        InterlockedExchange(&v->Stopping,0);
        status=PsCreateSystemThread(&v->Thread,SYNCHRONIZE,&attributes,NULL,NULL,Capture,v);
        if(!NT_SUCCESS(status)){InterlockedExchange(&v->Stopping,1);CloseParent(v);}
    }
    return status;
}

static NTSTATUS Process(PKSPIN pin) {
    VIDEO_PIN *v=pin->Context;VIDEO_DEVICE *d=KsPinGetDevice(pin)->Context;
    if(v->Stopping||!d->Online)return STATUS_DEVICE_NOT_READY;
    PKSSTREAM_POINTER p=KsPinGetLeadingEdgeStreamPointer(pin,KSSTREAM_POINTER_STATE_LOCKED);
    if(!p)return STATUS_PENDING;
    KIRQL irql;ULONG slot=RTL_NUMBER_OF(v->Slots);NTSTATUS status;
    KeAcquireSpinLock(&v->FrameLock,&irql);
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Slots);i++)if(v->Slots[i].State==2){slot=i;v->Slots[i].State=3;break;}
    status=v->CaptureStatus;
    KeReleaseSpinLock(&v->FrameLock,irql);
    if(slot==RTL_NUMBER_OF(v->Slots)) {
        if(NT_SUCCESS(status)){KsStreamPointerUnlock(p,FALSE);return STATUS_PENDING;}
    } else {
        status=STATUS_BUFFER_TOO_SMALL;
        if(p->OffsetOut.Remaining>=QCAM_VIDEO_BYTES) {
            RtlCopyMemory(p->OffsetOut.Data,v->Storage+slot*QCAM_VIDEO_BYTES,QCAM_VIDEO_BYTES);status=STATUS_SUCCESS;
        }
    }
    KSSTREAM_HEADER *h=p->StreamHeader;
    if(NT_SUCCESS(status)) {
        VIDEO_FRAME *frame=v->Slots+slot;h->DataUsed=QCAM_VIDEO_BYTES;
        h->OptionsFlags&=~(KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM|KSSTREAM_HEADER_OPTIONSF_TIMEVALID|KSSTREAM_HEADER_OPTIONSF_DATADISCONTINUITY);
        if(frame->Time.Dropped!=v->Dropped||frame->Time.Sequence!=v->Sequence+1)h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_DATADISCONTINUITY;
        h->PresentationTime.Time=frame->Presentation;
        h->PresentationTime.Numerator=1;h->PresentationTime.Denominator=1;
        h->Duration=FrameInterval;h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_DURATIONVALID|KSSTREAM_HEADER_OPTIONSF_TIMEVALID;
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
    KsStreamPointerAdvanceOffsetsAndUnlock(p,0,NT_SUCCESS(status)?QCAM_VIDEO_BYTES:0,TRUE);
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
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(irp);UNREFERENCED_PARAMETER(pin);UNREFERENCED_PARAMETER(supported);
    if(!IsEqualGUID(&requested->MajorFormat,&KSDATAFORMAT_TYPE_VIDEO)||!IsEqualGUID(&requested->SubFormat,&Nv12)||
       !IsEqualGUID(&requested->Specifier,&KSDATAFORMAT_SPECIFIER_VIDEOINFO2))return STATUS_NO_MATCH;
    *dataSize=sizeof(KS_DATAFORMAT_VIDEOINFOHEADER2);
    if(!size)return STATUS_BUFFER_OVERFLOW;
    if(size<*dataSize)return STATUS_BUFFER_TOO_SMALL;
    PKS_DATAFORMAT_VIDEOINFOHEADER2 f=data;f->DataFormat=VideoRange.DataRange;f->DataFormat.FormatSize=*dataSize;
    f->VideoInfoHeader2=VideoRange.VideoInfoHeader;return STATUS_SUCCESS;
}

DECLARE_SIMPLE_FRAMING_EX(Framing,STATIC_KSMEMORY_TYPE_KERNEL_NONPAGED,
    KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY|KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,2,0,QCAM_VIDEO_BYTES,QCAM_VIDEO_BYTES);

static const KSPIN_DISPATCH PinDispatch={PinCreate,PinClose,Process,NULL,SetFormat,State,NULL,NULL,NULL,NULL};
static const KSPIN_DESCRIPTOR_EX Pins[]={
    {&PinDispatch,NULL,{0,NULL,0,NULL,RTL_NUMBER_OF(Ranges),Ranges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_BOTH,
        &PINNAME_VIDEO_CAPTURE,NULL,0},KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY,1,0,&Framing,Intersect}
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
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE,ExtendedFocus,sizeof(KSPROPERTY),0,ExtendedFocus,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSSTATE,ExtendedFocus,sizeof(KSPROPERTY),0,NULL,NULL,0,NULL,NULL,0),
    DEFINE_KSPROPERTY_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_ZOOM,ExtendedZoom,sizeof(KSPROPERTY),0,ExtendedZoom,NULL,0,NULL,NULL,0)
};
static const KSEVENT_ITEM FocusEvents[]={DEFINE_KSEVENT_ITEM(KSPROPERTY_CAMERACONTROL_EXTENDED_FOCUSMODE,sizeof(KSEVENTDATA),0,NULL,NULL,NULL)};
static const KSEVENT_SET EventSets[]={{&KSEVENTSETID_ExtendedCameraControl,RTL_NUMBER_OF(FocusEvents),FocusEvents}};
static const KSPROPERTY_SET PropertySets[]={
    {&PROPSETID_VIDCAP_CAMERACONTROL,RTL_NUMBER_OF(FocusProperties),FocusProperties,0,NULL},
    {&KSPROPERTYSETID_ExtendedCameraControl,RTL_NUMBER_OF(ExtendedProperties),ExtendedProperties,0,NULL}
};
static const KSAUTOMATION_TABLE Automation={DEFINE_KSAUTOMATION_PROPERTIES(PropertySets),DEFINE_KSAUTOMATION_METHODS_NULL,DEFINE_KSAUTOMATION_EVENTS(EventSets)};
static const KSFILTER_DISPATCH FilterDispatch={FilterCreate,NULL,NULL,NULL};
static const KSFILTER_DESCRIPTOR Filter={&FilterDispatch,&Automation,KSFILTER_DESCRIPTOR_VERSION,0,&KSNAME_Filter,
    RTL_NUMBER_OF(Pins),sizeof(Pins[0]),Pins,RTL_NUMBER_OF(Categories),Categories,0,sizeof(KSNODE_DESCRIPTOR),NULL,0,NULL,NULL};
static const KSFILTER_DESCRIPTOR *const Filters[]={&Filter};
static const KSDEVICE_DISPATCH DeviceDispatch={Add,Start,NULL,NULL,NULL,Stop,NULL,NULL,Stop,NULL,Stop,NULL,Power,NULL};
static const KSDEVICE_DESCRIPTOR DeviceDescriptor={&DeviceDispatch,RTL_NUMBER_OF(Filters),Filters,KSDEVICE_DESCRIPTOR_VERSION_2,0};

NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry) {return KsInitializeDriver(object,registry,&DeviceDescriptor);}
