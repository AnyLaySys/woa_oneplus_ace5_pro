#include <ntifs.h>
#include <windef.h>
#include <ks.h>
#include <ksmedia.h>
#include "../uapi.h"
#include "nv12.h"

DRIVER_INITIALIZE DriverEntry;

typedef struct { volatile LONG Online; } VIDEO_DEVICE;
typedef struct { HANDLE Parent; UCHAR *Raw; ULONGLONG Due,Started; LONG Frames; } VIDEO_PIN;
#define STATIC_QCAM_NV12 0x3231564e,0,0x0010,0x80,0,0,0xaa,0,0x38,0x9b,0x71
static const GUID Nv12={STATIC_QCAM_NV12};
static const GUID Categories[]={{STATIC_KSCATEGORY_CAPTURE},{STATIC_KSCATEGORY_VIDEO},{STATIC_KSCATEGORY_VIDEO_CAMERA}};
static const LONGLONG FrameInterval=10000000;

static NTSTATUS ParentIo(VIDEO_PIN *v,ULONG code,PVOID input,ULONG inputBytes,PVOID output,ULONG outputBytes,ULONG_PTR *bytes) {
    IO_STATUS_BLOCK io={0};
    NTSTATUS status=ZwDeviceIoControlFile(v->Parent,NULL,NULL,NULL,&io,code,input,inputBytes,output,outputBytes);
    if(status==STATUS_PENDING) {
        status=ZwWaitForSingleObject(v->Parent,FALSE,NULL);
        if(NT_SUCCESS(status))status=io.Status;
    }
    if(bytes)*bytes=NT_SUCCESS(status)?io.Information:0;
    return status;
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
        h->bmiHeader.biSizeImage==QCAM_VIDEO_BYTES&&h->AvgTimePerFrame==FrameInterval;
}

static NTSTATUS PinCreate(PKSPIN pin,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);
    if(!Format(pin->ConnectionFormat))return STATUS_NO_MATCH;
    VIDEO_PIN *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),'iCcQ');
    if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=KsAddItemToObjectBag(pin->Bag,v,NULL);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(v,'iCcQ');return status;}
    pin->Context=v;pin->StreamHeaderSize=sizeof(KSSTREAM_HEADER)+sizeof(KS_FRAME_INFO);
    v->Raw=ExAllocatePool2(POOL_FLAG_NON_PAGED,QCAM_FRAME_BYTES,'rCcQ');
    return v->Raw?STATUS_SUCCESS:STATUS_INSUFFICIENT_RESOURCES;
}

static NTSTATUS PinClose(PKSPIN pin,PIRP irp) {
    UNREFERENCED_PARAMETER(irp);VIDEO_PIN *v=pin->Context;
    if(v){if(v->Parent){ZwClose(v->Parent);v->Parent=NULL;}if(v->Raw){ExFreePoolWithTag(v->Raw,'rCcQ');v->Raw=NULL;}}
    return STATUS_SUCCESS;
}

static NTSTATUS SetFormat(PKSPIN pin,PKSDATAFORMAT old,PKSMULTIPLE_ITEM oldAttributes,const KSDATARANGE *range,const KSATTRIBUTE_LIST *attributes) {
    UNREFERENCED_PARAMETER(old);UNREFERENCED_PARAMETER(oldAttributes);UNREFERENCED_PARAMETER(range);UNREFERENCED_PARAMETER(attributes);
    return Format(pin->ConnectionFormat)?STATUS_SUCCESS:STATUS_NO_MATCH;
}

static NTSTATUS State(PKSPIN pin,KSSTATE to,KSSTATE from) {
    UNREFERENCED_PARAMETER(from);VIDEO_PIN *v=pin->Context;
    if(to==KSSTATE_STOP) {
        if(v->Parent){ZwClose(v->Parent);v->Parent=NULL;}
        v->Due=0;v->Started=0;v->Frames=0;return STATUS_SUCCESS;
    }
    VIDEO_DEVICE *d=KsPinGetDevice(pin)->Context;
    if(!d->Online)return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=Open(v);
    if(NT_SUCCESS(status)&&to==KSSTATE_RUN){v->Started=KeQueryInterruptTime();v->Due=v->Started;}
    return status;
}

static NTSTATUS Process(PKSPIN pin) {
    VIDEO_PIN *v=pin->Context;VIDEO_DEVICE *d=KsPinGetDevice(pin)->Context;
    if(!v->Parent||!d->Online)return STATUS_DEVICE_NOT_READY;
    PKSSTREAM_POINTER p=KsPinGetLeadingEdgeStreamPointer(pin,KSSTREAM_POINTER_STATE_LOCKED);
    if(!p)return STATUS_PENDING;
    NTSTATUS status=STATUS_BUFFER_TOO_SMALL;ULONG_PTR bytes=0;
    if(p->OffsetOut.Remaining>=QCAM_VIDEO_BYTES) {
        ULONGLONG now=KeQueryInterruptTime();
        if(now<v->Due) {
            LARGE_INTEGER wait;wait.QuadPart=-(LONGLONG)(v->Due-now);KeDelayExecutionThread(KernelMode,FALSE,&wait);
        }
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,5000,0,4980000000ULL};
        status=d->Online?ParentIo(v,QCAM_CAPTURE,&options,sizeof(options),v->Raw,QCAM_FRAME_BYTES,&bytes):STATUS_DEVICE_NOT_READY;
        if(NT_SUCCESS(status)&&(bytes!=QCAM_FRAME_BYTES||!CamNv12(v->Raw,(ULONG)bytes,p->OffsetOut.Data,p->OffsetOut.Remaining)))status=STATUS_DATA_ERROR;
    }
    KSSTREAM_HEADER *h=p->StreamHeader;
    if(NT_SUCCESS(status)) {
        ULONGLONG now=KeQueryInterruptTime();h->DataUsed=QCAM_VIDEO_BYTES;
        h->PresentationTime.Time=(LONGLONG)(now-v->Started);h->PresentationTime.Numerator=1;h->PresentationTime.Denominator=1;
        h->Duration=FrameInterval;h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_TIMEVALID|KSSTREAM_HEADER_OPTIONSF_DURATIONVALID;
        if(h->Size>=sizeof(*h)+sizeof(KS_FRAME_INFO)) {
            KS_FRAME_INFO *f=(KS_FRAME_INFO *)(h+1);f->ExtendedHeaderSize=sizeof(*f);f->PictureNumber=++v->Frames;
        }
        v->Due+=FrameInterval;
    } else {
        h->DataUsed=0;h->OptionsFlags|=KSSTREAM_HEADER_OPTIONSF_ENDOFSTREAM;
        KsStreamPointerSetStatusCode(p,status);
    }
    KsStreamPointerAdvanceOffsetsAndUnlock(p,0,NT_SUCCESS(status)?QCAM_VIDEO_BYTES:0,TRUE);
    return status;
}

static const KS_DATARANGE_VIDEO2 VideoRange={
    {sizeof(KS_DATARANGE_VIDEO2),0,QCAM_VIDEO_BYTES,0,{STATIC_KSDATAFORMAT_TYPE_VIDEO},{STATIC_QCAM_NV12},{STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO2}},
    TRUE,FALSE,KS_VIDEOSTREAM_CAPTURE,0,
    {{STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO2},0,{QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},
        {QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},{QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},1,1,1,1,
        {QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},{QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},1,1,0,0,0,0,10000000,10000000,QCAM_VIDEO_BYTES*8,QCAM_VIDEO_BYTES*8},
    {{0,0,QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},{0,0,QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT},QCAM_VIDEO_BYTES*8,0,10000000,
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
    KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY|KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY,4,0,QCAM_VIDEO_BYTES,QCAM_VIDEO_BYTES);

static const KSPIN_DISPATCH PinDispatch={PinCreate,PinClose,Process,NULL,SetFormat,State,NULL,NULL,NULL,NULL};
static const KSPIN_DESCRIPTOR_EX Pins[]={
    {&PinDispatch,NULL,{0,NULL,0,NULL,RTL_NUMBER_OF(Ranges),Ranges,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_BOTH,
        &PINNAME_VIDEO_CAPTURE,NULL,0},KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY,1,0,&Framing,Intersect}
};
static const KSFILTER_DESCRIPTOR Filter={NULL,NULL,KSFILTER_DESCRIPTOR_VERSION,0,&KSNAME_Filter,
    RTL_NUMBER_OF(Pins),sizeof(Pins[0]),Pins,RTL_NUMBER_OF(Categories),Categories,0,sizeof(KSNODE_DESCRIPTOR),NULL,0,NULL,NULL};
static const KSFILTER_DESCRIPTOR *const Filters[]={&Filter};
static const KSDEVICE_DISPATCH DeviceDispatch={Add,Start,NULL,NULL,NULL,Stop,NULL,NULL,Stop,NULL,Stop,NULL,Power,NULL};
static const KSDEVICE_DESCRIPTOR DeviceDescriptor={&DeviceDispatch,RTL_NUMBER_OF(Filters),Filters,KSDEVICE_DESCRIPTOR_VERSION_2,0};

NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry) {return KsInitializeDriver(object,registry,&DeviceDescriptor);}
