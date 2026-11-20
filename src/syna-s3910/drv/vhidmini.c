/* Copyright (C) Microsoft Corporation. All rights reserved. */
#include "vhidmini.h"

static NTSTATUS S3910BringUp(PDEVICE_CONTEXT d);
static NTSTATUS Transfer(PDEVICE_CONTEXT d,const VOID *input,VOID *output,ULONG bytes);
static EVT_WDF_TIMER InitializeTimer,PollTimer;

#define S3910_POINT 0x05,0x0d,0x09,0x22,0xa1,0x02,0x09,0x42,0x25,0x01,0x75,0x01,0x81,0x02,0x09,0x32, \
    0x81,0x02,0x09,0x47,0x81,0x02,0x95,0x05,0x81,0x03,0x75,0x08,0x95,0x01,0x09,0x51, \
    0x81,0x02,0x05,0x01,0x26,0xff,0x7f,0x75,0x10,0x09,0x30,0x81,0x02,0x26,0xff,0x7f, \
    0x09,0x31,0x81,0x02,0xc0
static const UCHAR ReportDescriptor[]={
    0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,0x54,0x09,0x22,0xa1,0x02,0x09,0x42,0x14,0x25,
    0x01,0x75,0x01,0x95,0x01,0x81,0x02,0x09,0x32,0x81,0x02,0x09,0x47,0x81,0x02,0x95,
    0x05,0x81,0x03,0x75,0x08,0x09,0x51,0x95,0x01,0x81,0x02,0x05,0x01,0x26,0xff,0x7f,
    0x75,0x10,0x09,0x30,0x81,0x02,0x26,0xff,0x7f,0x09,0x31,0x81,0x02,0xc0,
    S3910_POINT,S3910_POINT,S3910_POINT,S3910_POINT,S3910_POINT,S3910_POINT,S3910_POINT,S3910_POINT,S3910_POINT,
    0x05,0x0d,0x09,0x54,0x75,0x08,0x25,0x0a,0x81,0x02,0x09,0x55,0xb1,0x02,0xc0,
};
#undef S3910_POINT
static const HID_DESCRIPTOR HidDescriptor={9,0x21,0x0100,0,1,{0x22,sizeof(ReportDescriptor)}};
static const HID_DEVICE_ATTRIBUTES Attributes={sizeof(HID_DEVICE_ATTRIBUTES),0xDEED,0xFEED,0x0101};
typedef struct { UCHAR Id,Points[S3910_CONTACTS][6],Count; } TOUCH_REPORT;
C_ASSERT(sizeof(TOUCH_REPORT)==62);

NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    WDF_DRIVER_CONFIG_INIT(&config,EvtDeviceAdd);
    return WdfDriverCreate(object,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
NTSTATUS EvtDeviceAdd(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware=OnPrepareHardware;
    callbacks.EvtDeviceReleaseHardware=OnReleaseHardware;
    callbacks.EvtDeviceD0Entry=OnD0Entry;callbacks.EvtDeviceD0Exit=OnD0Exit;
    WdfFdoInitSetFilter(init);WdfDeviceInitSetPnpPowerEventCallbacks(init,&callbacks);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,DEVICE_CONTEXT);
    WDFDEVICE device;
    NTSTATUS status=WdfDeviceCreate(&init,&attributes,&device);
    if(!NT_SUCCESS(status))return status;
    PDEVICE_CONTEXT d=GetDeviceContext(device);d->Device=device;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchParallel);
    queue.EvtIoInternalDeviceControl=EvtIoDeviceControl;
    status=WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
    if(!NT_SUCCESS(status))return status;
    WDF_IO_QUEUE_CONFIG_INIT(&queue,WdfIoQueueDispatchManual);
    status=WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,&d->Reports);
    return NT_SUCCESS(status)?WdfIoTargetCreate(device,WDF_NO_OBJECT_ATTRIBUTES,&d->Haptics):status;
}
NTSTATUS OnPrepareHardware(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(raw);
    static const ULONG bases[]={0x00A90000,0x00A14000,0x0F130000,0x00100000,0x16520000,0x81C60000,0x15000000,0x0F1A1000};
    static const ULONG sizes[]={0x4000,0x10000,0x4000,0x60000,0x10000,0x20000,0x1000,0x2000};
    PDEVICE_CONTEXT d=GetDeviceContext(device);d->MappedCount=0;
    for(ULONG i=0;i<WdfCmResourceListGetCount(resources);i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(resources,i);
        if(r->Type!=CmResourceTypeMemory)continue;
        ULONG n=d->MappedCount;
        if(n>=2 || r->u.Memory.Start.QuadPart!=bases[n] || r->u.Memory.Length!=sizes[n])return STATUS_DEVICE_CONFIGURATION_ERROR;
        d->MappedBase[n]=MmMapIoSpaceEx(r->u.Memory.Start,sizes[n],PAGE_READWRITE|PAGE_NOCACHE);
        if(!d->MappedBase[n])return STATUS_INSUFFICIENT_RESOURCES;
        d->MappedSize[n]=sizes[n];d->MappedCount++;
    }
    if(d->MappedCount!=2)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG n=2;n<RTL_NUMBER_OF(bases);n++) {
        PHYSICAL_ADDRESS address;address.QuadPart=bases[n];
        d->MappedBase[n]=MmMapIoSpaceEx(address,sizes[n],(n==5?PAGE_READONLY:PAGE_READWRITE)|PAGE_NOCACHE);
        if(!d->MappedBase[n])return STATUS_INSUFFICIENT_RESOURCES;
        d->MappedSize[n]=sizes[n];d->MappedCount++;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Stop(PDEVICE_CONTEXT d) {
    InterlockedExchange(&d->Stopping,1);
    if(d->InitTimer)WdfTimerStop(d->InitTimer,TRUE);
    if(d->PollTimer)WdfTimerStop(d->PollTimer,TRUE);
    RtlZeroMemory(&d->Tap,sizeof(d->Tap));
    d->Ready=FALSE;
    if(d->Spi) {
        NTSTATUS status=HspDeinit(d->Spi);
        if(!NT_SUCCESS(status))return status;
        ExFreePoolWithTag(d->Spi,'PSh9');d->Spi=NULL;
    }
    return STATUS_SUCCESS;
}
NTSTATUS OnReleaseHardware(WDFDEVICE device,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);
    PDEVICE_CONTEXT d=GetDeviceContext(device);
    NTSTATUS status=Stop(d);if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<d->MappedCount;i++) {
        MmUnmapIoSpace(d->MappedBase[i],d->MappedSize[i]);d->MappedBase[i]=NULL;
    }
    d->MappedCount=0;return STATUS_SUCCESS;
}
NTSTATUS OnD0Exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE state) {
    UNREFERENCED_PARAMETER(state);return Stop(GetDeviceContext(device));
}
static NTSTATUS CreateTimer(PDEVICE_CONTEXT d,PFN_WDF_TIMER callback,BOOLEAN serialized,WDFTIMER *timer) {
    WDF_TIMER_CONFIG config;WDF_TIMER_CONFIG_INIT(&config,callback);config.AutomaticSerialization=serialized;
    config.UseHighResolutionTimer=callback==PollTimer;
    WDF_OBJECT_ATTRIBUTES attributes;WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject=d->Device;attributes.ExecutionLevel=WdfExecutionLevelPassive;
    return WdfTimerCreate(&config,&attributes,timer);
}
NTSTATUS OnD0Entry(WDFDEVICE device,WDF_POWER_DEVICE_STATE state) {
    UNREFERENCED_PARAMETER(state);PDEVICE_CONTEXT d=GetDeviceContext(device);
    d->Ready=FALSE;d->InitAttempts=0;InterlockedExchange(&d->Stopping,0);
    if(!d->InitTimer) {
        NTSTATUS status=CreateTimer(d,InitializeTimer,TRUE,&d->InitTimer);
        if(!NT_SUCCESS(status))return status;
    }
    WdfTimerStart(d->InitTimer,WDF_REL_TIMEOUT_IN_MS(500));return STATUS_SUCCESS;
}
static NTSTATUS CopyReport(WDFREQUEST request,const VOID *source,size_t bytes) {
    WDFMEMORY memory;NTSTATUS status=WdfRequestRetrieveOutputMemory(request,&memory);
    if(!NT_SUCCESS(status))return status;
    size_t size;WdfMemoryGetBuffer(memory,&size);
    if(size<bytes)return STATUS_INVALID_BUFFER_SIZE;
    status=WdfMemoryCopyFromBuffer(memory,0,(PVOID)source,bytes);
    if(NT_SUCCESS(status))WdfRequestSetInformation(request,bytes);
    return status;
}
static NTSTATUS GetFeature(WDFREQUEST request) {
    WDF_REQUEST_PARAMETERS parameters;WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(request,&parameters);
    if(parameters.Parameters.DeviceIoControl.OutputBufferLength<sizeof(HID_XFER_PACKET))return STATUS_BUFFER_TOO_SMALL;
    HID_XFER_PACKET packet;RtlCopyMemory(&packet,WdfRequestWdmGetIrp(request)->UserBuffer,sizeof(packet));
    if(packet.reportId!=S3910_REPORT_ID)return STATUS_INVALID_PARAMETER;
    if(packet.reportBufferLen<2)return STATUS_INVALID_BUFFER_SIZE;
    packet.reportBuffer[0]=S3910_REPORT_ID;packet.reportBuffer[1]=S3910_CONTACTS;
    WdfRequestSetInformation(request,2);return STATUS_SUCCESS;
}
static NTSTATUS GetString(WDFREQUEST request,BOOLEAN indexed) {
    WDF_REQUEST_PARAMETERS parameters;WDF_REQUEST_PARAMETERS_INIT(&parameters);
    WdfRequestGetParameters(request,&parameters);
    ULONG id=PtrToUlong(parameters.Parameters.DeviceIoControl.Type3InputBuffer)&0xffff;
    static const WCHAR manufacturer[]=L"Synaptics",product[]=L"Synaptics S3910 Touchscreen Controller";
    if(indexed)return id==5?CopyReport(request,product,sizeof(product)):STATUS_INVALID_PARAMETER;
    switch(id) {
    case HID_STRING_ID_IMANUFACTURER:return CopyReport(request,manufacturer,sizeof(manufacturer));
    case HID_STRING_ID_IPRODUCT:return CopyReport(request,product,sizeof(product));
    case HID_STRING_ID_ISERIALNUMBER:return STATUS_NOT_SUPPORTED;
    default:return STATUS_INVALID_PARAMETER;
    }
}
VOID EvtIoDeviceControl(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    UNREFERENCED_PARAMETER(output);UNREFERENCED_PARAMETER(input);
    NTSTATUS status;
    switch(code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:status=CopyReport(request,&HidDescriptor,sizeof(HidDescriptor));break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:status=CopyReport(request,&Attributes,sizeof(Attributes));break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:status=CopyReport(request,ReportDescriptor,sizeof(ReportDescriptor));break;
    case IOCTL_HID_READ_REPORT:
        status=WdfRequestForwardToIoQueue(request,GetDeviceContext(WdfIoQueueGetDevice(queue))->Reports);
        if(NT_SUCCESS(status))return;
        break;
    case IOCTL_HID_GET_FEATURE:status=GetFeature(request);break;
    case IOCTL_HID_GET_STRING:status=GetString(request,FALSE);break;
    case IOCTL_HID_GET_INDEXED_STRING:status=GetString(request,TRUE);break;
    default:status=STATUS_NOT_IMPLEMENTED;break;
    }
    WdfRequestComplete(request,status);
}
static VOID InitializeTimer(WDFTIMER timer) {
    PDEVICE_CONTEXT d=GetDeviceContext(WdfTimerGetParentObject(timer));
    if(InterlockedCompareExchange(&d->Stopping,0,0) || d->Ready)return;
    d->InitAttempts++;
    NTSTATUS status=S3910BringUp(d);
    if(!NT_SUCCESS(status) && d->RetryInit && d->InitAttempts<10 && !d->Rpmh.Faulted && !d->Spi &&
        (status==STATUS_INSUFFICIENT_RESOURCES || status==STATUS_IO_TIMEOUT || status==STATUS_DEVICE_BUSY) &&
        !InterlockedCompareExchange(&d->Stopping,0,0))WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(1000));
}
static VOID Pulse(PDEVICE_CONTEXT d,ULONG voltage) {
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"\\Device\\QcomHaptics");WDF_IO_TARGET_OPEN_PARAMS p;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&p,&name,GENERIC_WRITE);p.ShareAccess=FILE_SHARE_READ|FILE_SHARE_WRITE;
    if(!NT_SUCCESS(WdfIoTargetOpen(d->Haptics,&p)))return;
    WDF_REQUEST_SEND_OPTIONS options;WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_MS(10));
    WDF_MEMORY_DESCRIPTOR input;WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input,&voltage,sizeof(voltage));
    WdfIoTargetSendIoctlSynchronously(d->Haptics,NULL,QH_PULSE,&input,NULL,&options,NULL);
    WdfIoTargetClose(d->Haptics);
}
static VOID PollTimer(WDFTIMER timer) {
    PDEVICE_CONTEXT d=GetDeviceContext(WdfTimerGetParentObject(timer));
    if(!d->Ready || InterlockedCompareExchange(&d->Stopping,0,0))return;
    ULONG pressed=0;
    NTSTATUS transfer=Transfer(d,NULL,d->Reply,S3910_TOUCH_BYTES);
    if(NT_SUCCESS(transfer) && d->Reply[1]==0x11) {
        UCHAR count=d->Reply[2];
        if(count>=0x1f && (count-0x1f)%12==0) {
            count=(count-0x1f)/12;
            if(count<=S3910_CONTACTS && (!count ||
                (35+12*(count-1)+4<S3910_TOUCH_BYTES && d->PanelMaxX && d->PanelMaxY))) {
                TOUCH_REPORT report={0};report.Id=S3910_REPORT_ID;report.Count=count?count:1;
                if(!count)report.Points[0][0]=0x06;
                else for(ULONG i=0;i<count;i++) {
                    const UCHAR *point=d->Reply+35+12*i;
                    ULONG x=point[1]|((ULONG)point[2]<<8),y=point[3]|((ULONG)point[4]<<8);
                    x=min(x,d->PanelMaxX)*32767/d->PanelMaxX;y=min(y,d->PanelMaxY)*32767/d->PanelMaxY;
                    report.Points[i][0]=0x07;report.Points[i][1]=point[0]&0x0f;
                    report.Points[i][2]=(UCHAR)x;report.Points[i][3]=(UCHAR)(x>>8);
                    report.Points[i][4]=(UCHAR)y;report.Points[i][5]=(UCHAR)(y>>8);
                }
                pressed=TapUpdate(&d->Tap,KeQueryInterruptTime(),count,report.Points);
                WDFREQUEST request;
                if(NT_SUCCESS(WdfIoQueueRetrieveNextRequest(d->Reports,&request))) {
                    NTSTATUS status=CopyReport(request,&report,sizeof(report));WdfRequestComplete(request,status);
                }
            }
        }
    } else if(!NT_SUCCESS(transfer))d->Tap.Pending=0;
    ULONG held=TapHold(&d->Tap,KeQueryInterruptTime());
    for(; (pressed|held) && !InterlockedCompareExchange(&d->Stopping,0,0);pressed>>=1,held>>=1) {
        if(pressed&1)Pulse(d,800);
        if(held&1)Pulse(d,1200);
    }
    if(d->Ready && !InterlockedCompareExchange(&d->Stopping,0,0))WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(8));
}
static NTSTATUS Transfer(PDEVICE_CONTEXT d,const VOID *input,VOID *output,ULONG bytes) {
    static const struct hsp_cfg config={4,0,0,2,0,0};
    UCHAR tx[HSP_DATA_BYTES],rx[HSP_DATA_BYTES];
    if(!d->Spi || !d->Ready)return STATUS_DEVICE_NOT_READY;
    if(!bytes || bytes>HSP_DATA_BYTES)return STATUS_INVALID_PARAMETER;
    if(input)RtlCopyMemory(tx,input,bytes);else RtlFillMemory(tx,bytes,0xff);
    NTSTATUS status=HspXfer(d->Spi,&config,tx,rx,bytes);
    if(NT_SUCCESS(status) && output)RtlCopyMemory(output,rx,bytes);
    return status;
}
static VOID Delay(LONGLONG milliseconds) {
    LARGE_INTEGER interval;interval.QuadPart=-milliseconds*10000;
    KeDelayExecutionThread(KernelMode,FALSE,&interval);
}
static NTSTATUS Receive(PDEVICE_CONTEXT d,UCHAR event,ULONG bytes) {
    for(ULONG i=0;i<30;i++) {
        NTSTATUS status=Transfer(d,NULL,d->Reply,bytes);
        if(!NT_SUCCESS(status))return status;
        if(d->Reply[0]==0xa5 && d->Reply[1]==event)return STATUS_SUCCESS;
        Delay(10);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS S3910BringUp(PDEVICE_CONTEXT d) {
    static const UCHAR reset[]={4},ack[]={7},application[]={0x20,0,0};
    d->RetryInit=FALSE;if(d->Spi)return STATUS_INVALID_DEVICE_STATE;
    PHSP_CTX spi=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*spi),'PSh9');
    if(!spi)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=HspInit(spi,d->Device,d->MappedBase[0],d->MappedBase[1],d->MappedBase[3],d->MappedBase[6]);
    if(!NT_SUCCESS(status)){ExFreePoolWithTag(spi,'PSh9');return status;}
    d->Spi=spi;
    status=HrpmhInit(&d->Rpmh,d->MappedBase[4],d->MappedBase[5]);if(!NT_SUCCESS(status))goto fail;
    d->RetryInit=TRUE;
    status=HrpmhVoteLdo(&d->Rpmh,4,1800);if(!NT_SUCCESS(status))goto fail;
    status=HrpmhVoteLdo(&d->Rpmh,16,3104);if(!NT_SUCCESS(status))goto fail;
    d->RetryInit=FALSE;
    status=HspClocksOn(spi);if(!NT_SUCCESS(status))goto fail;
    status=HspResetAndConfigurePins(d->MappedBase[2],d->MappedBase[7]);if(!NT_SUCCESS(status))goto fail;
    status=HspConfigureSmmu(spi);if(!NT_SUCCESS(status))goto fail;
    status=HspStartDma(spi);if(!NT_SUCCESS(status))goto fail;
    d->Ready=TRUE;
    status=Transfer(d,reset,NULL,sizeof(reset));if(!NT_SUCCESS(status))goto fail;Delay(20);
    status=Transfer(d,ack,NULL,sizeof(ack));if(!NT_SUCCESS(status))goto fail;Delay(10);
    status=Receive(d,0x10,50);if(!NT_SUCCESS(status))goto fail;
    status=Transfer(d,application,NULL,sizeof(application));if(!NT_SUCCESS(status))goto fail;Delay(10);
    status=Receive(d,1,60);if(!NT_SUCCESS(status))goto fail;
    ULONG length=d->Reply[2]|((ULONG)d->Reply[3]<<8);
    if(length<46 || length+3>60){status=STATUS_DEVICE_PROTOCOL_ERROR;goto fail;}
    ULONG x=d->Reply[36]|((ULONG)d->Reply[37]<<8),y=d->Reply[38]|((ULONG)d->Reply[39]<<8);
    if(!x || !y || x>32767 || y>32767){status=STATUS_DEVICE_PROTOCOL_ERROR;goto fail;}
    d->PanelMaxX=x;d->PanelMaxY=y;
    if(!d->PollTimer){status=CreateTimer(d,PollTimer,FALSE,&d->PollTimer);if(!NT_SUCCESS(status))goto fail;}
    WdfTimerStart(d->PollTimer,WDF_REL_TIMEOUT_IN_MS(100));return STATUS_SUCCESS;
fail:
    d->Ready=FALSE;
    if(NT_SUCCESS(HspDeinit(spi))){ExFreePoolWithTag(spi,'PSh9');d->Spi=NULL;}
    return status;
}
