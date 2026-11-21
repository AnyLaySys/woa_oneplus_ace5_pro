#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include "haptics.h"
#include "feedback.h"
#include "../qcom-sensors/abi.h"
#define SENSOR_HARDWARE
#include "../qcom-sensors/hall.inc"
#undef SENSOR_HARDWARE

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY Start;
static EVT_WDF_DEVICE_D0_EXIT Stop;
static EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL Control;
static EVT_WDF_TIMER Poll;
static EVT_WDF_INTERRUPT_ISR Isr;

typedef struct { UCHAR Id,Value; } REPORT;
typedef struct {
    PUCHAR Observe[2],Channel[2],Irq[2];
    WDFQUEUE Reads;
    WDFTIMER Timer;
    volatile LONG Online;
    UCHAR State,Candidate,Debounce[3],Failures,Head,Count;
    REPORT Pending[16];
    PUCHAR HapticObserve[2],HapticChannel[2];
    BOOLEAN HapticInterrupt[2];
    WDFDEVICE HapticDevice;
    WDFTIMER HapticTimer;
    NTSTATUS HapticStatus;
    ULONGLONG HapticDue;
    UCHAR HapticVoltage,HapticRepeat,HapticPhase;
    PVOID DisplayCallback;
    BOOLEAN DisplayOn;
    WDFIOTARGET Audio;
    KEY_FEEDBACK Feedback;
    HALL Hall;
} KEYS;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(KEYS,Context);
static const UCHAR Masks[]={0xc0,1};
static const UCHAR Descriptor[]={
    0x05,0x01,0x09,0x0d,0xa1,0x01,0x85,0x01,0x09,0x81,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x01,0x81,0x02,0x05,0x0c,0x09,0xe9,0x09,0xea,
    0x95,0x02,0x81,0x02,0x95,0x05,0x81,0x03,0xc0
};
static const HID_DESCRIPTOR Hid={9,0x21,0x0111,0,1,{0x22,sizeof(Descriptor)}};
static const HID_DEVICE_ATTRIBUTES Attributes={sizeof(HID_DEVICE_ATTRIBUTES),0x17cb,0x8750,0x0100};

static ULONG Read(PUCHAR base,ULONG offset) { return READ_REGISTER_ULONG((PULONG)(base+offset)); }
static VOID Write(PUCHAR base,ULONG offset,ULONG value) { WRITE_REGISTER_ULONG((PULONG)(base+offset),value); }
static NTSTATUS Transfer(PUCHAR base,UCHAR reg,UCHAR *value,BOOLEAN write) {
    if(write)Write(base,0x10,*value);
    Write(base,0,(write?0:0x08000000)|((ULONG)reg<<4));
    for(ULONG i=0;i<1000;i++) {
        ULONG status=Read(base,8);
        if(status&1) {
            if(status&0x0e)return STATUS_IO_DEVICE_ERROR;
            if(!write)*value=(UCHAR)Read(base,0x18);
            return STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}
#include "haptics.inc"
static NTSTATUS SetInterrupts(KEYS *k,BOOLEAN on) {
    NTSTATUS result=STATUS_SUCCESS;
    for(ULONG i=0;i<2;i++) {
        if(!k->Channel[i]||!k->Irq[i])continue;
        UCHAR value=0xff;
        Write(k->Irq[i],0x100,0);
        NTSTATUS status=Transfer(k->Channel[i],0x16,&value,TRUE);
        if(!on){if(!NT_SUCCESS(status))result=status;continue;}
        if(!NT_SUCCESS(status))return status;
        for(UCHAR reg=0x11;reg<=0x13;reg++) {
            status=Transfer(k->Observe[i],reg,&value,FALSE);
            if(!NT_SUCCESS(status))return status;
            value|=Masks[i];status=Transfer(k->Channel[i],reg,&value,TRUE);
            if(!NT_SUCCESS(status))return status;
        }
        value=Masks[i];status=Transfer(k->Channel[i],0x14,&value,TRUE);
        if(!NT_SUCCESS(status))return status;
        Write(k->Irq[i],0x108,value);Write(k->Irq[i],0x100,1);
        status=Transfer(k->Channel[i],0x15,&value,TRUE);
        if(!NT_SUCCESS(status))return status;
    }
    return result;
}
static NTSTATUS Copy(WDFREQUEST request,const VOID *data,size_t size) {
    WDFMEMORY memory;
    NTSTATUS status=WdfRequestRetrieveOutputMemory(request,&memory);
    if(NT_SUCCESS(status))status=WdfMemoryCopyFromBuffer(memory,0,(PVOID)data,size);
    if(NT_SUCCESS(status))WdfRequestSetInformation(request,size);
    return status;
}
static VOID Flush(KEYS *k) {
    WDFREQUEST request;
    while(k->Count&&NT_SUCCESS(WdfIoQueueRetrieveNextRequest(k->Reads,&request))) {
        NTSTATUS status=Copy(request,&k->Pending[k->Head],sizeof(REPORT));
        if(NT_SUCCESS(status)){k->Head=(k->Head+1)&15;k->Count--;}
        WdfRequestComplete(request,status);
    }
}
static VOID Put(KEYS *k,UCHAR id,UCHAR value) {
    REPORT *r=&k->Pending[(k->Head+k->Count)&15];
    r->Id=id;r->Value=value;k->Count++;
}
static VOID Update(KEYS *k,UCHAR state,BOOLEAN repeat) {
    Flush(k);
    if(k->Count>RTL_NUMBER_OF(k->Pending)-2)return;
    if(k->State!=state || repeat){Put(k,1,state);if(state==2 || state==4)Put(k,1,0);}
    k->State=state;Flush(k);
}
static ULONG VolumeLimits(KEYS *k) {
    ULONG limits=0;UNICODE_STRING name=RTL_CONSTANT_STRING(L"\\Device\\AlsCharge");WDF_IO_TARGET_OPEN_PARAMS p;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&p,&name,GENERIC_READ);p.ShareAccess=FILE_SHARE_READ|FILE_SHARE_WRITE;
    if(!NT_SUCCESS(WdfIoTargetOpen(k->Audio,&p)))return 0;
    WDF_MEMORY_DESCRIPTOR output;WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output,&limits,sizeof(limits));
    WDF_REQUEST_SEND_OPTIONS options;WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_MS(10));
    NTSTATUS status=WdfIoTargetSendIoctlSynchronously(k->Audio,NULL,QH_VOLUME,NULL,&output,&options,NULL);
    WdfIoTargetClose(k->Audio);return NT_SUCCESS(status)?limits:0;
}
static VOID Poll(WDFTIMER timer) {
    KEYS *k=Context(WdfTimerGetParentObject(timer));
    if(!k->Online)return;
    UCHAR pon,gpio;
    if((!k->Failures||NT_SUCCESS(SetInterrupts(k,TRUE)))&&NT_SUCCESS(Transfer(k->Observe[0],0x10,&pon,FALSE))&&NT_SUCCESS(Transfer(k->Observe[1],0x10,&gpio,FALSE))) {
        UCHAR sample=(UCHAR)(((pon>>7)&1)|((pon>>5)&2)|((~gpio&1)<<2)),state=k->State;
        k->Failures=0;
        for(UCHAR i=0;i<3;i++) {
            UCHAR bit=(UCHAR)(1<<i);
            if((sample^k->Candidate)&bit)k->Debounce[i]=0;
            if(k->Debounce[i]<4)k->Debounce[i]++;
            if(k->Debounce[i]==4)state=(state&~bit)|(sample&bit);
        }
        k->Candidate=sample;Update(k,state,FALSE);
    } else {
        RtlZeroMemory(k->Debounce,sizeof(k->Debounce));
        if(k->Failures<4)k->Failures++;
        if(k->Failures==4)Update(k,0,FALSE);
    }
    if(k->Failures)RtlZeroMemory(&k->Feedback,sizeof(k->Feedback));
    ULONGLONG now=KeQueryInterruptTime();ULONG limits=0;
    if(k->Feedback.Volume && now>=k->Feedback.VolumeAt && now-k->Feedback.VolumeAt>=2700000)limits=VolumeLimits(k);
    ULONG action=k->Failures?0:KeyFeedback(&k->Feedback,k->State,now,limits);
    if(action&4)Update(k,k->State,TRUE);
    if(action&2){if(NT_SUCCESS(HapticPulse(k,1200,2)))k->Feedback.Limited=2;}
    else if(action&1)HapticPulse(k,800,1);
    if(k->Online&&(k->State||k->Failures||k->State!=k->Candidate||k->Debounce[0]<4||k->Debounce[1]<4||k->Debounce[2]<4)) {
        ULONG delay=k->Failures?100:10;now=KeQueryInterruptTime();
        if(k->Feedback.Volume && !k->Feedback.Limited && now<k->Feedback.RepeatDue)delay=min(delay,(ULONG)((k->Feedback.RepeatDue-now+9999)/10000));
        WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(delay));
    }
}
static BOOLEAN Isr(WDFINTERRUPT interrupt,ULONG message) {
    UNREFERENCED_PARAMETER(message);
    WDFDEVICE device=WdfInterruptGetDevice(interrupt);KEYS *k=Context(device);BOOLEAN handled=FALSE;
    WdfObjectAcquireLock(device);
    if(k->Online) {
        for(ULONG i=0;i<2;i++) {
            UCHAR pending=(UCHAR)Read(k->Irq[i],0x104)&Masks[i];
            if(!pending)continue;
            Write(k->Irq[i],0x108,pending);
            if(!NT_SUCCESS(Transfer(k->Channel[i],0x14,&pending,TRUE))){Write(k->Irq[i],0x100,0);k->Failures=4;}
            handled=TRUE;
        }
        if(handled){RtlZeroMemory(k->Debounce,sizeof(k->Debounce));WdfTimerStart(k->Timer,WDF_REL_TIMEOUT_IN_MS(10));}
    }
    WdfObjectReleaseLock(device);return handled;
}
static NTSTATUS Prepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST resources) {
    static const GUID display={0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
    KEYS *k=Context(device);
    WDF_INTERRUPT_CONFIG irq;WDF_INTERRUPT_CONFIG_INIT(&irq,Isr,NULL);irq.PassiveHandling=TRUE;
    for(ULONG i=0;i<WdfCmResourceListGetCount(resources);i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(resources,i);
        if(r->Type==CmResourceTypeMemory && r->u.Memory.Start.QuadPart==0x0f150000 && r->u.Memory.Length==0x2000){if(k->Hall.Base)return STATUS_DEVICE_CONFIGURATION_ERROR;k->Hall.Base=MmMapIoSpaceEx(r->u.Memory.Start,0x2000,PAGE_READWRITE|PAGE_NOCACHE);if(!k->Hall.Base)return STATUS_INSUFFICIENT_RESOURCES;}
        if(r->Type==CmResourceTypeInterrupt){if(irq.InterruptTranslated)return STATUS_DEVICE_CONFIGURATION_ERROR;irq.InterruptRaw=WdfCmResourceListGetDescriptor(raw,i);irq.InterruptTranslated=r;}
    }
    if(!irq.InterruptTranslated)return STATUS_DEVICE_CONFIGURATION_ERROR;
    PHYSICAL_ADDRESS address;address.QuadPart=0x0c400000;
    PUCHAR core=MmMapIoSpaceEx(address,0x3000,PAGE_READONLY|PAGE_NOCACHE);if(!core)return STATUS_INSUFFICIENT_RESOURCES;
    address.QuadPart=0x0c42d000;PUCHAR owners=MmMapIoSpaceEx(address,0x4000,PAGE_READONLY|PAGE_NOCACHE);
    if(!owners){MmUnmapIoSpace(core,0x3000);return STATUS_INSUFFICIENT_RESOURCES;}
    NTSTATUS status=STATUS_SUCCESS;ULONG count=Read(core,4)&0x7ff,version=Read(core,0);
    if(version<0x70000000||version>=0x80000000||!count||count>1024)status=STATUS_NOT_SUPPORTED;
    ULONG observe[4]={MAXULONG,MAXULONG,MAXULONG,MAXULONG},channel[4]={MAXULONG,MAXULONG,MAXULONG,MAXULONG},interrupt[2]={MAXULONG,MAXULONG};
    for(ULONG i=0;i<count&&NT_SUCCESS(status);i++) {
        ULONG map=Read(core,0x2000+4*i),ppid=(map>>8)&0xfff,owner=Read(owners,4*i)&7,n;
        if(ppid==0x013||ppid==0x18d)n=ppid==0x013?0:1;
        else if(ppid==0x7f0||ppid==0x7f1)n=ppid-0x7f0+2;
        else continue;
        if(!owner){observe[n]=channel[n]=i;if(n>=2)k->HapticInterrupt[n-2]=(map&0x1000000)!=0;}
        else if(n>=2)observe[n]=i;
        if(n<2&&(map&0x1000000))interrupt[n]=owner?MAXULONG:i;
    }
    MmUnmapIoSpace(owners,0x4000);MmUnmapIoSpace(core,0x3000);
    if(!NT_SUCCESS(status))return status;
    if(channel[0]==MAXULONG||channel[1]==MAXULONG||interrupt[0]==MAXULONG||interrupt[1]==MAXULONG)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG i=0;i<4;i++) {
        PUCHAR *o=i<2?&k->Observe[i]:&k->HapticObserve[i-2],*c=i<2?&k->Channel[i]:&k->HapticChannel[i-2];
        if(observe[i]!=MAXULONG){address.QuadPart=0x0c440000+0x20*observe[i];*o=MmMapIoSpaceEx(address,0x20,PAGE_READWRITE|PAGE_NOCACHE);if(!*o)return STATUS_INSUFFICIENT_RESOURCES;}
        if(channel[i]!=MAXULONG){address.QuadPart=0x0c500000+0x1000*channel[i];*c=MmMapIoSpaceEx(address,0x1000,PAGE_READWRITE|PAGE_NOCACHE);if(!*c)return STATUS_INSUFFICIENT_RESOURCES;}
        if(i<2){if(interrupt[i]==channel[i])k->Irq[i]=*c;else{address.QuadPart=0x0c500000+0x1000*interrupt[i];k->Irq[i]=MmMapIoSpaceEx(address,0x1000,PAGE_READWRITE|PAGE_NOCACHE);if(!k->Irq[i])return STATUS_INSUFFICIENT_RESOURCES;}}
    }
    WDFINTERRUPT handle;status=WdfInterruptCreate(device,&irq,WDF_NO_OBJECT_ATTRIBUTES,&handle);
    return NT_SUCCESS(status)?PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(device),&display,HapticDisplay,device,&k->DisplayCallback):status;
}
static NTSTATUS Start(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    KEYS *k=Context(device);
    static const UCHAR regs[]={0x40,0x41,0x42,0x43,0x46},values[]={0,1,0,0,0x80};
    for(ULONG i=0;i<RTL_NUMBER_OF(regs);i++) {
        UCHAR value=values[i];
        NTSTATUS status=Transfer(k->Channel[1],regs[i],&value,TRUE);
        if(!NT_SUCCESS(status))return status;
    }
    UCHAR value;
    NTSTATUS status=Transfer(k->Observe[0],0x10,&value,FALSE);
    if(NT_SUCCESS(status))status=Transfer(k->Observe[1],0x10,&value,FALSE);
    if(!NT_SUCCESS(status))return status;
    status=SetInterrupts(k,TRUE);
    if(!NT_SUCCESS(status)){SetInterrupts(k,FALSE);return status;}
    k->Candidate=0;k->Failures=0;RtlZeroMemory(k->Debounce,sizeof(k->Debounce));
    RtlZeroMemory(&k->Feedback,sizeof(k->Feedback));
    HapticPrepare(k);HallStart(&k->Hall);
    InterlockedExchange(&k->Online,1);
    WdfTimerStart(k->Timer,WDF_REL_TIMEOUT_IN_MS(10));return STATUS_SUCCESS;
}
static NTSTATUS Stop(WDFDEVICE device,WDF_POWER_DEVICE_STATE next) {
    UNREFERENCED_PARAMETER(next);
    KEYS *k=Context(device);
    InterlockedExchange(&k->Online,0);WdfTimerStop(k->Timer,TRUE);
    WdfTimerStop(k->HapticTimer,TRUE);
    WdfObjectAcquireLock(device);
    if(k->HapticPhase)HapticOff(k);
    HallStop(&k->Hall);SetInterrupts(k,FALSE);
    k->Head=0;k->Count=0;k->State=0;Put(k,1,0);Flush(k);
    RtlZeroMemory(&k->Feedback,sizeof(k->Feedback));k->HapticRepeat=0;
    WdfObjectReleaseLock(device);return STATUS_SUCCESS;
}
static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);
    KEYS *k=Context(device);
    if(k->DisplayCallback){PoUnregisterPowerSettingCallback(k->DisplayCallback);k->DisplayCallback=NULL;}k->DisplayOn=FALSE;
    SetInterrupts(k,FALSE);HallStop(&k->Hall);if(k->Hall.Base){MmUnmapIoSpace(k->Hall.Base,0x2000);k->Hall.Base=NULL;}
    for(ULONG i=0;i<2;i++) {
        if(k->Observe[i])MmUnmapIoSpace(k->Observe[i],0x20);
        if(k->Irq[i]&&k->Irq[i]!=k->Channel[i])MmUnmapIoSpace(k->Irq[i],0x1000);
        if(k->Channel[i])MmUnmapIoSpace(k->Channel[i],0x1000);
        if(k->HapticObserve[i])MmUnmapIoSpace(k->HapticObserve[i],0x20);
        if(k->HapticChannel[i])MmUnmapIoSpace(k->HapticChannel[i],0x1000);
        k->Observe[i]=k->Channel[i]=k->Irq[i]=k->HapticObserve[i]=k->HapticChannel[i]=NULL;k->HapticInterrupt[i]=FALSE;
    }
    return STATUS_SUCCESS;
}
static VOID Control(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    UNREFERENCED_PARAMETER(output);UNREFERENCED_PARAMETER(input);
    KEYS *k=Context(WdfIoQueueGetDevice(queue));
    NTSTATUS status;
    switch(code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:status=Copy(request,&Hid,sizeof(Hid));break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:status=Copy(request,&Attributes,sizeof(Attributes));break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:status=Copy(request,Descriptor,sizeof(Descriptor));break;
    case IOCTL_HID_READ_REPORT:
        status=WdfRequestForwardToIoQueue(request,k->Reads);
        if(NT_SUCCESS(status)){Flush(k);return;}
        break;
    case IOCTL_HID_GET_STRING: {
        WDF_REQUEST_PARAMETERS p;WDF_REQUEST_PARAMETERS_INIT(&p);WdfRequestGetParameters(request,&p);
        ULONG id=PtrToUlong(p.Parameters.DeviceIoControl.Type3InputBuffer)&0xffff;
        static const WCHAR vendor[]=L"Qualcomm",product[]=L"Qualcomm Buttons";
        status=id==HID_STRING_ID_IMANUFACTURER?Copy(request,vendor,sizeof(vendor)):
            id==HID_STRING_ID_IPRODUCT?Copy(request,product,sizeof(product)):STATUS_NOT_SUPPORTED;
        break;
    }
    default:status=STATUS_NOT_SUPPORTED;break;
    }
    WdfRequestComplete(request,status);
}
static NTSTATUS AddDevice(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS p;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDevicePrepareHardware=Prepare;p.EvtDeviceReleaseHardware=Release;p.EvtDeviceD0Entry=Start;p.EvtDeviceD0Exit=Stop;
    WdfFdoInitSetFilter(init);WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,KEYS);
    a.EvtCleanupCallback=HapticCleanup;
    a.SynchronizationScope=WdfSynchronizationScopeDevice;a.ExecutionLevel=WdfExecutionLevelPassive;
    WDFDEVICE device;NTSTATUS status=WdfDeviceCreate(&init,&a,&device);
    if(!NT_SUCCESS(status))return status;
    KEYS *k=Context(device);
    WDF_IO_QUEUE_CONFIG q;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchParallel);
    q.PowerManaged=WdfFalse;q.EvtIoInternalDeviceControl=Control;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
    if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT(&q,WdfIoQueueDispatchManual);q.PowerManaged=WdfFalse;
    status=WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,&k->Reads);
    if(!NT_SUCCESS(status))return status;
    WDF_TIMER_CONFIG t;WDF_TIMER_CONFIG_INIT(&t,Poll);
    t.UseHighResolutionTimer=TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;a.ExecutionLevel=WdfExecutionLevelPassive;
    status=WdfTimerCreate(&t,&a,&k->Timer);
    if(NT_SUCCESS(status))status=WdfIoTargetCreate(device,WDF_NO_OBJECT_ATTRIBUTES,&k->Audio);
    return NT_SUCCESS(status)?HapticCreate(device):status;
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,AddDevice);
    return WdfDriverCreate(object,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
