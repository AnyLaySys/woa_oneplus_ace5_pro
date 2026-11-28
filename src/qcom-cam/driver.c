#include <ntifs.h>
#include "cam.h"
#include "isp-registers.h"
#include "../platform/lock.h"
#include <acpiioct.h>
#include "trace-regs.h"
#include "csid-commit.h"
#include "dma-address.h"
#include "video/nv12.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY Start;
static EVT_WDF_DEVICE_D0_EXIT Stop;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;
static EVT_WDF_FILE_CLEANUP FileCleanup;

static const ULONG Bases[]={0x00100000,0x0ade0000,0x01760000,0x0ac7b000,0x0adb3000,0x0ad27000,0x0ac86000,0x0ac04000,0x0ad90000,
    0x15000000,0x0f100000,0x0c400000,0x0c42d000,0x0c440000,0x0c500000,0x16520000,0x81c60000,0x0adcb000,0x0ac7f000,0x017d2000,0x017d1000,0x01780000,0x0bbf0000,0x0ac7c000,0x0adab000,0x0adaf000,0x0adb1000};
static const ULONG Sizes[]={0x100000,0x20000,0x6000,0x1000,0x2000,0x3000,0x10000,0x1000,0x9000,
    0x100000,0x100000,0x3000,0x4000,0x80000,0x400000,0x10000,0x20000,0x5000,0x1000,0x1000,0x1000,0x5b800,0x1f00,0x1000,0x2000,0x2000,0x2000};
C_ASSERT(RTL_NUMBER_OF(Bases)==CamRegionCount);

static VOID VideoWorker(PVOID context) {
    CAM_VIDEO_WORKER *w=context;CAM *c=w->Owner;
    GROUP_AFFINITY affinity={0},previous={0};affinity.Mask=(KAFFINITY)1<<(w->Index==0?7:w->Index==1?5:4);
    KeSetSystemGroupAffinityThread(&affinity,&previous);KeSetPriorityThread(KeGetCurrentThread(),15);
    for(;;) {
        KeWaitForSingleObject(&w->Ready,Executive,KernelMode,FALSE,NULL);if(c->VideoStopping)break;
        if(c->VideoCached)CamNv12RowsCachedGains(c->VideoSource,QCAM_FRAME_BYTES*2+256,c->VideoOutput,c->VideoColors,c->VideoWidth==1024?c->VideoLinearGains:NULL,w->Workspace,w->First,w->End,c->VideoDense,c->VideoColumns,c->VideoRows,c->Sensor==3,c->VideoWidth,c->VideoHeight,c->Info.Width/2);
        else CamNv12RowsGeometryGains(c->VideoSource,QCAM_FRAME_BYTES*2+256,c->VideoOutput,c->VideoColors,c->VideoWidth==1024?c->VideoLinearGains:NULL,w->Workspace,w->First,w->End,c->VideoBlocks,c->VideoRows,c->Sensor==3,c->VideoWidth,c->VideoHeight);
        KeSetEvent(&w->Done,IO_NO_INCREMENT,FALSE);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID VideoStop(CAM *c) {
    c->VideoStopping=TRUE;
    for(ULONG i=0;i<RTL_NUMBER_OF(c->VideoWorkers);i++)if(c->VideoWorkers[i].Thread)KeSetEvent(&c->VideoWorkers[i].Ready,IO_NO_INCREMENT,FALSE);
    for(ULONG i=0;i<RTL_NUMBER_OF(c->VideoWorkers);i++)if(c->VideoWorkers[i].Thread) {
        ZwWaitForSingleObject(c->VideoWorkers[i].Thread,FALSE,NULL);ZwClose(c->VideoWorkers[i].Thread);c->VideoWorkers[i].Thread=NULL;
    }
}

static VOID VideoMap(CAM *c) {
    if(!c->VideoWidth){c->VideoWidth=QCAM_VIDEO_WIDTH;c->VideoHeight=QCAM_VIDEO_HEIGHT;c->VideoBytes=QCAM_VIDEO_BYTES;}
    CamRawMapGeometry(c->VideoZoom?c->VideoZoom:QCAM_ZOOM_MIN,c->Info.Width,c->Info.Height,c->VideoWidth,c->VideoHeight,c->VideoBlocks,c->VideoRows);
    CamRawColumns(c->VideoBlocks,c->VideoWidth,c->VideoColumns);
    ULONG rows[2];CamRawMapGeometry(QCAM_ZOOM_MIN,c->Info.Width,2,c->Info.Width/2,1,c->VideoDense,rows);
    c->VideoCached=c->VideoWidth>=c->Info.Width/2&&c->VideoWidth*c->VideoHeight>=c->Info.Width*c->Info.Height/4;
}

static NTSTATUS VideoFormat(CAM *c,const QCAM_VIDEO_FORMAT_OPTIONS *o) {
    if(o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||o->Width<160||o->Height<120||o->Width>QCAM_VIDEO_MAX_WIDTH||o->Height>QCAM_VIDEO_MAX_HEIGHT||(o->Width&7)||(o->Height&1))return STATUS_INVALID_PARAMETER;
    if(c->VideoWidth!=o->Width||c->VideoHeight!=o->Height) {
        c->VideoWidth=o->Width;c->VideoHeight=o->Height;c->VideoBytes=o->Width*o->Height*3/2;VideoMap(c);
    }
    return STATUS_SUCCESS;
}

static VOID VideoConvert(CAM *c,const UCHAR *raw,PUCHAR output) {
    ULONG desired=1,count=0;
    if(KeQueryActiveProcessorCountEx(0)>=8)for(ULONG i=0;i<desired;i++) {
        CAM_VIDEO_WORKER *w=c->VideoWorkers+i;
        if(!w->Thread) {
            w->Owner=c;w->Index=i;KeInitializeEvent(&w->Ready,SynchronizationEvent,FALSE);KeInitializeEvent(&w->Done,SynchronizationEvent,FALSE);c->VideoStopping=FALSE;
            OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
            NTSTATUS status=PsCreateSystemThread(&w->Thread,SYNCHRONIZE,&attributes,NULL,NULL,VideoWorker,w);if(!NT_SUCCESS(status)){w->Thread=NULL;break;}
        }
        count++;
    }
    if(!c->VideoGains[0])c->VideoGains[0]=c->VideoGains[1]=c->VideoGains[2]=256;
    ULONG gains[3];RtlCopyMemory(gains,c->VideoGains,sizeof(gains));CamWhiteBalanceBayer(raw,QCAM_FRAME_BYTES*2+256,c->Info.Width,c->Info.Height,c->Sensor==3,gains);
    for(ULONG i=0;i<3;i++)c->VideoGains[i]=(c->VideoGains[i]*3+gains[i]+2)/4;
    for(ULONG i=0;i<3;i++) {
        ULONG gain=c->VideoGains[c->Sensor==3&&i!=1?2-i:i];c->VideoLinearGains[i]=gain;
        for(ULONG value=0;value<1024;value++)c->VideoColors[i][value]=(UCHAR)CamColor(value<=64?value:64+((value-64)*gain+128)/256);
    }
    c->VideoSource=raw;c->VideoOutput=output;
    for(ULONG i=0;i<count;i++) {
        CAM_VIDEO_WORKER *w=c->VideoWorkers+i;w->First=(c->VideoHeight*(i+1)/(count+1))&~1u;w->End=(c->VideoHeight*(i+2)/(count+1))&~1u;
        KeSetEvent(&w->Ready,IO_NO_INCREMENT,FALSE);
    }
    if(c->VideoCached)CamNv12RowsCachedGains(raw,QCAM_FRAME_BYTES*2+256,output,c->VideoColors,c->VideoWidth==1024?c->VideoLinearGains:NULL,c->VideoWorkspace,0,(c->VideoHeight/(count+1))&~1u,c->VideoDense,c->VideoColumns,c->VideoRows,c->Sensor==3,c->VideoWidth,c->VideoHeight,c->Info.Width/2);
    else CamNv12RowsGeometryGains(raw,QCAM_FRAME_BYTES*2+256,output,c->VideoColors,c->VideoWidth==1024?c->VideoLinearGains:NULL,c->VideoWorkspace,0,(c->VideoHeight/(count+1))&~1u,c->VideoBlocks,c->VideoRows,c->Sensor==3,c->VideoWidth,c->VideoHeight);
    for(ULONG i=0;i<count;i++)KeWaitForSingleObject(&c->VideoWorkers[i].Done,Executive,KernelMode,FALSE,NULL);
    c->VideoSource=NULL;c->VideoOutput=NULL;
}

static BOOLEAN Dedicated(ULONG region) {return region>=CamCc&&region<=CamNoc;}

static VOID FirmwareStatus(CAM *c) {
    ACPI_EVAL_INPUT_BUFFER input={0};
    union { ULONGLONG Alignment; UCHAR Bytes[256]; } output={0};
    input.Signature=ACPI_EVAL_INPUT_BUFFER_SIGNATURE;RtlCopyMemory(input.MethodName,"QSST",4);
    WDF_MEMORY_DESCRIPTOR in,out;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in,&input,sizeof(input));WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out,output.Bytes,sizeof(output.Bytes));
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options,WDF_REQUEST_SEND_OPTION_TIMEOUT);WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options,WDF_REL_TIMEOUT_IN_SEC(2));
    ULONG_PTR bytes=0;c->Trace.FirmwareQos=MAXULONG;
    NTSTATUS status=WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(c->Device),NULL,IOCTL_ACPI_EVAL_METHOD,&in,&out,&options,&bytes);
    ACPI_EVAL_OUTPUT_BUFFER *result=(ACPI_EVAL_OUTPUT_BUFFER *)output.Bytes;
    if(NT_SUCCESS(status)&&bytes>=sizeof(*result)&&result->Signature==ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE&&result->Count==1&&
       result->Argument[0].Type==ACPI_METHOD_ARGUMENT_INTEGER)c->Trace.FirmwareQos=result->Argument[0].Argument;
}

static NTSTATUS Prepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(raw);CAM *c=CamContext(device);ULONG count=0;
    for(ULONG i=0;i<WdfCmResourceListGetCount(resources);i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(resources,i);
        if(r->Type!=CmResourceTypeMemory)continue;
        ULONG n;
        for(n=0;n<CamRegionCount;n++)if(Dedicated(n)&&r->u.Memory.Start.QuadPart==Bases[n]&&r->u.Memory.Length==Sizes[n])break;
        if(n==CamRegionCount||c->Base[n])return STATUS_DEVICE_CONFIGURATION_ERROR;
        c->Base[n]=MmMapIoSpaceEx(r->u.Memory.Start,Sizes[n],PAGE_READWRITE|PAGE_NOCACHE);
        if(!c->Base[n])return STATUS_INSUFFICIENT_RESOURCES;
        count++;
    }
    if(count!=CamNoc-CamCc+1)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG n=0;n<CamRegionCount;n++)if(!Dedicated(n)) {
        PHYSICAL_ADDRESS address;address.QuadPart=Bases[n];
        c->Base[n]=MmMapIoSpaceEx(address,Sizes[n],(n==CamDb||n==CamRpmh?PAGE_READONLY:PAGE_READWRITE)|PAGE_NOCACHE);
        if(!c->Base[n])return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* Stock sun-camera.dtsi pairs IFE/TFE2 with RT-CDM2. */
    const ULONG selectedRegions[]={CamCsid,CamTfe,CamCdm};
    const ULONG selectedBases[]={0x0ad2d000,0x0aca6000,0x0ac81000};
    for(ULONG i=0;i<RTL_NUMBER_OF(selectedRegions);i++) {
        ULONG region=selectedRegions[i];
        MmUnmapIoSpace(c->Base[region],Sizes[region]);c->Base[region]=NULL;
        PHYSICAL_ADDRESS selected;selected.QuadPart=selectedBases[i];
        c->Base[region]=MmMapIoSpaceEx(selected,Sizes[region],PAGE_READWRITE|PAGE_NOCACHE);
        if(!c->Base[region])return STATUS_INSUFFICIENT_RESOURCES;
    }
    FirmwareStatus(c);
    return STATUS_SUCCESS;
}

static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);CAM *c=CamContext(device);
    InterlockedExchange(&c->Online,0);WdfWaitLockAcquire(c->Lock,NULL);
    VideoStop(c);
    NTSTATUS status=CamCaptureStop(c);
    if(NT_SUCCESS(status))status=CamPowerOff(c);
    CamDmaFree(c);
    if(NT_SUCCESS(status)&&!c->DmaActive&&!c->CdmPending&&!c->ScmPending&&!c->CrmPending&&!c->TbuPending&&!c->FrameContextOwned)
        for(ULONG i=0;i<CamRegionCount;i++)if(c->Base[i]){MmUnmapIoSpace(c->Base[i],Sizes[i]);c->Base[i]=NULL;}
    WdfWaitLockRelease(c->Lock);return status;
}

static NTSTATUS Start(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);CAM *c=CamContext(device);
    if(c->Faulted||c->DmaActive||c->CdmPending||c->TbuPending||c->FrameContextOwned)return STATUS_DEVICE_HARDWARE_ERROR;
    InterlockedExchange(&c->Online,1);return STATUS_SUCCESS;
}

static NTSTATUS Stop(WDFDEVICE device,WDF_POWER_DEVICE_STATE next) {
    UNREFERENCED_PARAMETER(next);CAM *c=CamContext(device);
    InterlockedExchange(&c->Online,0);WdfWaitLockAcquire(c->Lock,NULL);
    NTSTATUS status=CamCaptureStop(c);if(NT_SUCCESS(status))status=CamPowerOff(c);
    WdfWaitLockRelease(c->Lock);return status;
}

static VOID Cleanup(WDFOBJECT object) {
    CAM *c=CamContext(object);VideoStop(c);CamDmaFree(c);
    if(!c->DmaActive&&!c->CdmPending&&!c->TbuPending&&!c->FrameContextOwned&&c->PowerState){ExFreePoolWithTag(c->PowerState,'pCcQ');c->PowerState=NULL;}
}

static VOID FileCleanup(WDFFILEOBJECT file) {
    CAM *c=CamContext(WdfFileObjectGetDevice(file));WdfWaitLockAcquire(c->Lock,NULL);
    if(c->StreamFile==file) {
        NTSTATUS status=CamCaptureStop(c);if(NT_SUCCESS(status))status=CamPowerOff(c);
        c->StreamFile=NULL;c->Info.Status=status;
    }
    WdfWaitLockRelease(c->Lock);
}

static VOID WakeFrame(PEX_TIMER timer,PVOID context) {
    UNREFERENCED_PARAMETER(timer);KeSetEvent(context,IO_NO_INCREMENT,FALSE);
}

static NTSTATUS WaitFrame(CAM *c,WDFREQUEST request,ULONG timeout) {
    KEVENT event;KeInitializeEvent(&event,SynchronizationEvent,FALSE);
    PEX_TIMER timer=ExAllocateTimer(WakeFrame,&event,EX_TIMER_HIGH_RESOLUTION);
    if(!timer)return STATUS_INSUFFICIENT_RESOURCES;
    ULONGLONG deadline=KeQueryInterruptTime()+(ULONGLONG)timeout*10000;NTSTATUS status;
    for(;;) {
        if(!c->Online||WdfRequestIsCanceled(request)){status=STATUS_CANCELLED;break;}
        if(CamCapturePoll(c,&status))break;
        if(KeQueryInterruptTime()>=deadline){status=STATUS_IO_TIMEOUT;break;}
        ULONGLONG qpc,now=KeQueryInterruptTimePrecise(&qpc),period=c->Sensor==1||c->Sensor==2?333333:166667;
        if(c->StreamFile&&((c->Sensor==1||c->Sensor==2)||c->FrameTime.Sequence&&now-c->FrameTime.Captured>=period-10000))KeStallExecutionProcessor(8);
        else {ExSetTimer(timer,-5000,0,NULL);KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL);}
    }
    ExDeleteTimer(timer,TRUE,TRUE,NULL);return status;
}

static VOID Merge(PUCHAR raw,ULONG count,ULONG companion) {
    for(ULONG offset=256;offset<count;offset+=512)RtlCopyMemory(raw+offset,raw+companion+offset,256);
}

static VOID Exposure(CAM *c,const UCHAR *raw,ULONG companion) {
    if(!c->StreamFile||c->Info.FrameLength<=48||(c->Info.Frames&3))return;
    ULONGLONG current=(ULONGLONG)c->Info.Exposure*16384/(16384-c->Info.Gain);
    ULONG gains[3]={c->VideoGains[c->Sensor==3?2:0],c->VideoGains[1],c->VideoGains[c->Sensor==3?0:2]};
    ULONGLONG target=CamExposureTargetGeometry(raw,companion,c->Info.Width,c->Info.Height,gains,c->VideoZoom?c->VideoZoom:QCAM_ZOOM_MIN,current);
    ULONG lines=(ULONG)min(target,(ULONGLONG)c->Info.FrameLength-48);
    ULONG gain=target>lines?(ULONG)(16384-16384ULL*lines/target):0;gain=min(gain,0x3f00u);
    if(lines!=c->Info.Exposure||gain!=c->Info.Gain)CamSensorExposure(c,lines,gain);
}

static VOID Control(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    CAM *c=CamContext(WdfIoQueueGetDevice(queue));NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;ULONG_PTR bytes=0;
    GROUP_AFFINITY previousAffinity={0};BOOLEAN bound=FALSE;KPRIORITY previousPriority=0;
    WdfWaitLockAcquire(c->Lock,NULL);
    if(!c->Online){status=STATUS_DEVICE_NOT_READY;goto done;}
    if(c->StreamFile&&c->StreamFile!=WdfRequestGetFileObject(request)&&code!=QCAM_QUERY&&code!=QCAM_DIAGNOSTICS&&code!=QCAM_TRACE_QUERY){status=STATUS_DEVICE_BUSY;goto done;}
    if(code==QCAM_CAMERA) {
        QCAM_CAMERA_OPTIONS *o;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);if(!NT_SUCCESS(status))goto done;
        if(input!=sizeof(*o)||output||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||o->Index>3||o->Probe>1||c->StreamFile){status=STATUS_INVALID_PARAMETER;goto done;}
        status=CamCaptureStop(c);if(NT_SUCCESS(status))status=CamPowerOff(c);if(!NT_SUCCESS(status))goto done;
        c->Sensor=o->Index;c->Info.SensorId=0;c->Info.Flags=0;c->Info.Status=STATUS_SUCCESS;c->Info.Exposure=1000;c->Info.Gain=0x1334;c->FocusCalibration.Valid=FALSE;c->VideoZoom=QCAM_ZOOM_MIN;
        RtlZeroMemory(c->VideoGains,sizeof(c->VideoGains));
        c->Info.Width=c->Sensor==1?3264:c->Sensor==2?1600:c->Sensor==3?2304:QCAM_WIDTH;
        c->Info.Height=c->Sensor==1?2448:c->Sensor==2?1200:c->Sensor==3?1728:QCAM_HEIGHT;
        VideoMap(c);
        if(o->Probe) {
            c->ProbeLimit=0;status=CamPowerOn(c);QCAM_DIAG saved=c->Diag;NTSTATUS off=CamPowerOff(c);c->Diag=saved;if(NT_SUCCESS(status))status=off;
        }
        goto done;
    }
    if(code==QCAM_VIDEO_FORMAT) {
        QCAM_VIDEO_FORMAT_OPTIONS *o;
        if(input) {
            status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(NT_SUCCESS(status))status=input!=sizeof(*o)||output?STATUS_INVALID_PARAMETER:VideoFormat(c,o);
        } else {
            status=WdfRequestRetrieveOutputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(NT_SUCCESS(status)){o->Size=sizeof(*o);o->Abi=QCAM_ABI;o->Width=c->VideoWidth?c->VideoWidth:QCAM_VIDEO_WIDTH;o->Height=c->VideoHeight?c->VideoHeight:QCAM_VIDEO_HEIGHT;bytes=sizeof(*o);}
        }
        goto done;
    }
    if(code==QCAM_FOCUS_ROI) {
        QCAM_ROI_OPTIONS *o;
        if(input) {
            status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);if(!NT_SUCCESS(status))goto done;
            if(input!=sizeof(*o)||output||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||o->Flags>QCAM_ROI_FOCUS||
                (o->Flags&&(o->Left>=o->Right||o->Top>=o->Bottom||o->Right>65536||o->Bottom>65536))){status=STATUS_INVALID_PARAMETER;goto done;}
            c->FocusRoi=*o;c->FocusWindowValid=FALSE;c->FocusComplete=FALSE;c->FocusStable=c->FocusSamples=c->FocusMisses=0;c->PdafFocusCopied=FALSE;status=STATUS_SUCCESS;
        } else {
            status=WdfRequestRetrieveOutputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(NT_SUCCESS(status)){*o=c->FocusRoi;o->Size=sizeof(*o);o->Abi=QCAM_ABI;bytes=sizeof(*o);}
        }
        goto done;
    }
    if(code==QCAM_FLASH) {
        QCAM_FLASH_OPTIONS *o;
        if(input) {
            status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);if(!NT_SUCCESS(status))goto done;
            if(input!=sizeof(*o)||output||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||o->Available||o->Fault){status=STATUS_INVALID_PARAMETER;goto done;}
            status=CamFlashSet(c,o);
        } else {
            status=WdfRequestRetrieveOutputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(NT_SUCCESS(status)){o->Size=sizeof(*o);o->Abi=QCAM_ABI;o->Mode=c->FlashMode;o->Power=c->FlashPower;o->TimeoutMs=c->FlashTimeout;o->Available=c->FlashAvailable;o->Fault=0;bytes=sizeof(*o);}
        }
        goto done;
    }
    if(code==QCAM_ZOOM) {
        QCAM_ZOOM_OPTIONS *o;
        if(input) {
            status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);if(!NT_SUCCESS(status))goto done;
            if(input!=sizeof(*o)||output||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||o->Reserved||o->Ratio<QCAM_ZOOM_MIN||o->Ratio>QCAM_ZOOM_MAX){status=STATUS_INVALID_PARAMETER;goto done;}
            if(c->VideoZoom!=o->Ratio) {
                c->VideoZoom=o->Ratio;VideoMap(c);
                c->FocusWindowValid=FALSE;c->FocusComplete=FALSE;c->FocusStable=c->FocusSamples=c->FocusMisses=0;
            }
            status=STATUS_SUCCESS;
        } else {
            status=WdfRequestRetrieveOutputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(NT_SUCCESS(status)){o->Size=sizeof(*o);o->Abi=QCAM_ABI;o->Ratio=c->VideoZoom?c->VideoZoom:QCAM_ZOOM_MIN;o->Reserved=0;bytes=sizeof(*o);}
        }
        goto done;
    }
    if(code==QCAM_CALIBRATION) {
        PUCHAR buffer;
        if(input||c->StreamFile){status=STATUS_INVALID_DEVICE_STATE;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,32768,(PVOID *)&buffer,NULL);if(!NT_SUCCESS(status))goto done;
        status=CamPowerOn(c);
        for(ULONG offset=0;offset<32768&&NT_SUCCESS(status);offset+=32)status=CamCciRead(c,0x50,(USHORT)offset,buffer+offset,32);
        NTSTATUS off=CamPowerOff(c);if(NT_SUCCESS(status))status=off;
        if(NT_SUCCESS(status))bytes=32768;goto done;
    }
    if(code==QCAM_FOCUS) {
        QCAM_FOCUS_OPTIONS *o;
        if(input) {
            status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);if(!NT_SUCCESS(status))goto done;
            if(input!=sizeof(*o)||output||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI){status=STATUS_INVALID_PARAMETER;goto done;}
            status=CamFocusSet(c,o);
        } else {
            status=WdfRequestRetrieveOutputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(NT_SUCCESS(status)){o->Size=sizeof(*o);o->Abi=QCAM_ABI;o->Position=c->FocusPosition;o->Flags=c->FocusFlags;o->State=CamFocusState(c);bytes=sizeof(*o);}
        }
        goto done;
    }
    if(code==QCAM_STREAM_STOP) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        status=CamFlashStop(c);if(NT_SUCCESS(status))status=CamCaptureStop(c);if(NT_SUCCESS(status))status=CamPowerOff(c);
        if(NT_SUCCESS(status))c->StreamFile=NULL;goto done;
    }
    if(code==QCAM_STREAM_START||code==QCAM_STREAM_START_PDAF) {
        QCAM_CAPTURE_OPTIONS *o;status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
        if(!NT_SUCCESS(status))goto done;
        if(input!=sizeof(*o)||output||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||o->TimeoutMs<100||o->TimeoutMs>30000||o->SettleCount>255||o->DataRate<182400000||o->DataRate>13680000000ULL){status=STATUS_INVALID_PARAMETER;goto done;}
        if(c->StreamFile){status=STATUS_SUCCESS;goto done;}
        c->ProbeLimit=c->PipelineLimit=c->FrameSlot=c->FrameStamp=0;
        RtlZeroMemory(&c->FrameTime,sizeof(c->FrameTime));
        RtlZeroMemory(c->Trace.FrameTimings,sizeof(c->Trace.FrameTimings));c->Trace.FrameTimingCount=0;c->Trace.FramePollCount=c->Trace.PairWaitCount=0;c->Trace.PairEofGap=c->Trace.PairEofGapMax=0;
        if(code==QCAM_STREAM_START_PDAF)c->PipelineLimit=13;
        status=CamCaptureStart(c,o);
        if(NT_SUCCESS(status))c->StreamFile=WdfRequestGetFileObject(request);
        else {CamCaptureStop(c);CamPowerOff(c);}
        c->Info.Status=status;goto done;
    }
    if(code==QCAM_STREAM_PDAF) {
        PUCHAR buffer;
        if(input||!c->StreamFile||!c->PdafEnabled){status=STATUS_INVALID_DEVICE_STATE;goto done;}
        status=CamPdafCopy(c);if(!NT_SUCCESS(status))goto done;
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(c->PdafInfo)+QCAM_PDAF_BYTES,(PVOID *)&buffer,NULL);
        if(NT_SUCCESS(status)) {
            RtlCopyMemory(buffer,&c->PdafInfo,sizeof(c->PdafInfo));
            RtlCopyMemory(buffer+sizeof(c->PdafInfo),c->PdafLatest,QCAM_PDAF_BYTES);bytes=sizeof(c->PdafInfo)+QCAM_PDAF_BYTES;
        }
        goto done;
    }
    if(code==QCAM_STREAM_FRAME||code==QCAM_STREAM_NV12) {
        PVOID buffer;if(!c->StreamFile||input&&(code!=QCAM_STREAM_NV12||input!=sizeof(QCAM_VIDEO_FORMAT_OPTIONS))){status=STATUS_INVALID_DEVICE_STATE;goto done;}
        if(code==QCAM_STREAM_NV12) {
            if(input) {QCAM_VIDEO_FORMAT_OPTIONS *o;status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);if(!NT_SUCCESS(status))goto done;status=VideoFormat(c,o);if(!NT_SUCCESS(status))goto done;}
            else if(!c->VideoWidth)VideoMap(c);
        }
        if(code==QCAM_STREAM_NV12&&c->PipelineLimit==13){status=STATUS_INVALID_DEVICE_REQUEST;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,code==QCAM_STREAM_NV12?c->VideoBytes:QCAM_FRAME_BYTES,&buffer,NULL);if(!NT_SUCCESS(status))goto done;
        if(code==QCAM_STREAM_NV12&&KeQueryActiveProcessorCountEx(0)>=8) {
            GROUP_AFFINITY affinity={0};affinity.Mask=0x40;KeSetSystemGroupAffinityThread(&affinity,&previousAffinity);bound=TRUE;
            previousPriority=KeSetPriorityThread(KeGetCurrentThread(),15);
        }
        LARGE_INTEGER timingFrequency;ULONGLONG timings[7];timings[0]=KeQueryPerformanceCounter(&timingFrequency).QuadPart;
        PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
        status=WaitFrame(c,request,2000);
        if(!NT_SUCCESS(status)&&c->PdafPending)CamPdafFrame(c);
        if(NT_SUCCESS(status)) {
            timings[1]=KeQueryPerformanceCounter(NULL).QuadPart;
            ULONG address=CamRead(t,0x2490);
            if(address==CamDmaImageAddress(c->FrameAddress.QuadPart))c->FrameSlot=0;
            else if(address==CamDmaImageAddress(c->FrameAddress.QuadPart+QCAM_FRAME_BYTES))c->FrameSlot=1;
            else {status=STATUS_DATA_ERROR;c->Info.Status=status;goto done;}
            if(CamRead(t,0x2590)!=CamDmaImageAddress(c->FrameAddress.QuadPart+(c->FrameSlot+2)*QCAM_FRAME_BYTES+256)){status=STATUS_DATA_ERROR;c->Info.Status=status;goto done;}
            PUCHAR source=(PUCHAR)c->Frame+c->FrameSlot*QCAM_FRAME_BYTES;
            CamWrite(s,0x11c,8);CamWrite(s,0x12c,8);CamWrite(s,0xac,0x30000);CamWrite(s,0x14,1);c->Info.TfeBusIrq&=~0x30000u;
            c->FrameSlot^=1;CamWrite(t,0x2404,CamDmaImageAddress(c->FrameAddress.QuadPart+c->FrameSlot*QCAM_FRAME_BYTES));
            CamWrite(t,0x2504,CamDmaImageAddress(c->FrameAddress.QuadPart+(c->FrameSlot+2)*QCAM_FRAME_BYTES+256));
            CamCsidCommit(s,0x300);
            timings[2]=KeQueryPerformanceCounter(NULL).QuadPart;
            status=CamPdafFrame(c);if(!NT_SUCCESS(status)){c->Info.Status=status;goto done;}
            timings[3]=KeQueryPerformanceCounter(NULL).QuadPart;
            if(code==QCAM_STREAM_NV12) {
                ULONG previous=MAXULONG;
                for(ULONG y=0;y<c->VideoHeight;y++)if(c->VideoRows[y]!=previous) {
                    previous=c->VideoRows[y];PUCHAR row=source+previous;
                    CamDmaInvalidate(row,QCAM_STRIDE*2,QCAM_STRIDE*2,1,0);
                    CamDmaInvalidate(row+QCAM_FRAME_BYTES*2+256,QCAM_STRIDE*2,QCAM_STRIDE*2,1,256);
                }
                ULONG samples=(c->Info.Height+63)/64;
                CamDmaInvalidate(source,QCAM_STRIDE*2,QCAM_STRIDE*64,samples,0);
                CamDmaInvalidate(source+QCAM_FRAME_BYTES*2+256,QCAM_STRIDE*2,QCAM_STRIDE*64,samples,256);
            } else {
                KeFlushIoBuffers(c->FrameMdl[c->FrameSlot^1],TRUE,TRUE);KeFlushIoBuffers(c->FrameMdl[(c->FrameSlot^1)+2],TRUE,TRUE);
            }
            KeMemoryBarrier();
            timings[4]=KeQueryPerformanceCounter(NULL).QuadPart;
            if(code==QCAM_STREAM_NV12){VideoConvert(c,source,buffer);bytes=c->VideoBytes;Exposure(c,source,QCAM_FRAME_BYTES*2+256);}
            else {Merge(source,c->PipelineLimit==13?QCAM_PDAF_BYTES:QCAM_FRAME_BYTES,QCAM_FRAME_BYTES*2+256);RtlCopyMemory(buffer,source,QCAM_FRAME_BYTES);bytes=QCAM_FRAME_BYTES;}
            timings[5]=KeQueryPerformanceCounter(NULL).QuadPart;
            CamFocusFrame(c);CamFlashFrame(c);timings[6]=KeQueryPerformanceCounter(NULL).QuadPart;
            for(ULONG i=0;i<6;i++)c->Trace.FrameTimings[i]+=(timings[i+1]-timings[i])*1000000/(ULONGLONG)timingFrequency.QuadPart;c->Trace.FrameTimingCount++;
            c->Info.Flags|=QCAM_FRAME_VALID;c->Info.Frames++;c->Info.Stage=10;
        }
        c->Info.Status=status;goto done;
    }
    if(code==QCAM_FRAME_TIME_QUERY) {
        QCAM_FRAME_TIME *frame;
        if(input||!c->StreamFile||!c->FrameTime.Sequence){status=STATUS_INVALID_DEVICE_STATE;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*frame),(PVOID *)&frame,NULL);
        if(NT_SUCCESS(status)){*frame=c->FrameTime;bytes=sizeof(*frame);}goto done;
    }
    if(code==QCAM_QUERY) {
        QCAM_INFO *info;status=WdfRequestRetrieveOutputBuffer(request,sizeof(*info),(PVOID *)&info,NULL);
        if(NT_SUCCESS(status)){*info=c->Info;bytes=sizeof(*info);}goto done;
    }
    if(code==QCAM_DIAGNOSTICS) {
        QCAM_DIAG *diag;status=WdfRequestRetrieveOutputBuffer(request,sizeof(*diag),(PVOID *)&diag,NULL);
        if(NT_SUCCESS(status)){CamDiagnostics(c,diag);bytes=sizeof(*diag);}goto done;
    }
    if(code==QCAM_TRACE_QUERY) {
        c->Trace.FocusPhase=c->FocusResult.Phase;c->Trace.FocusDefocus=c->FocusResult.Defocus;c->Trace.FocusConfidence=c->FocusResult.Confidence;c->Trace.FocusWindow=c->FocusResult.Window;
        c->Trace.PdafInfo=c->PdafInfo;c->Trace.PdafAddress=c->PdafAddress.QuadPart;
        if(c->Powered&&c->PdafEnabled)for(ULONG n=0;n<2;n++) {
            const ULONG offsets[]={0,4,0x10,0x94,0xa0,0xa4,0xb0,0xb4};
            for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++)c->Trace.PdafCsid[n][i]=CamRead(c->Base[CamCsid],0x1200+n*0x200+offsets[i]);
            c->Trace.PdafWriter[n][0]=CamRead(c->Base[CamTfe],0x2600+n*0x100);
            c->Trace.PdafWriter[n][1]=CamRead(c->Base[CamTfe],0x260c+n*0x100);
            c->Trace.PdafWriter[n][2]=CamRead(c->Base[CamTfe],0x2690+n*0x100);
            c->Trace.PdafWriter[n][3]=CamRead(c->Base[CamCsid],0x134+n*0x10);
        }
        QCAM_TRACE *trace;status=WdfRequestRetrieveOutputBuffer(request,sizeof(*trace),(PVOID *)&trace,NULL);
        if(NT_SUCCESS(status)){*trace=c->Trace;trace->Size=sizeof(*trace);trace->Abi=QCAM_ABI;bytes=sizeof(*trace);}goto done;
    }
    if(code==QCAM_CONTEXT_READ) {
        QCAM_CONTEXT_REGISTER *reg;ULONG *value;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*reg),(PVOID *)&reg,NULL);
        if(!NT_SUCCESS(status))goto done;
        ULONG bank=reg->Bank,offset=reg->Offset;
        if(input!=sizeof(*reg)||output!=sizeof(*value)||bank>=4||
           (offset&&offset!=0x58&&offset!=0x60&&offset!=0x64&&offset!=0x68)) {
            status=STATUS_INVALID_PARAMETER;goto done;
        }
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*value),(PVOID *)&value,NULL);
        if(!NT_SUCCESS(status))goto done;
        PLATFORM_LOCK_GUARD guard={0};status=PlatformLockAcquire(&guard,TRUE);
        if(status!=STATUS_SUCCESS)goto done;
        BOOLEAN active=FALSE;PUCHAR base=c->Base[CamSmmu];
        if(CamRead(base,0x20)==0x4c017e84&&CamRead(base,0x24)==0x60000053) {
            for(ULONG i=0;i<128;i++)if((CamRead(base,0x800+i*4)&0x80000000u)&&
               (CamRead(base,0xc00+i*4)&0x300ff)==bank){active=TRUE;break;}
        }
        status=active?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
        if(active) {
            c->Diag.Step=170;c->Diag.Region=CamSmmu;c->Diag.Offset=0x80000+bank*4096+offset;
            *value=CamRead(base,c->Diag.Offset);c->Diag.Observed=*value;bytes=sizeof(*value);
        }
        NTSTATUS released=PlatformLockRelease(&guard);if(NT_SUCCESS(status))status=released;
        goto done;
    }
    if(code==QCAM_FRAME_DIAGNOSTIC||code==QCAM_ARENA_DIAGNOSTIC) {
        if(input||c->ScmPending||c->CdmPending||c->CrmPending||c->TbuPending){status=STATUS_DEVICE_BUSY;goto done;}
        if(c->DmaActive||c->RxActive) {
            if(!c->Powered||c->BusOn||c->PhyOn||
               ((CamRead(c->Base[CamTfe],0x2400)|CamRead(c->Base[CamTfe],0x2500)|CamRead(c->Base[CamTfe],0x2600)|CamRead(c->Base[CamTfe],0x2700))&1)||CamRead(c->Base[CamCsid],0xe04)||CamRead(c->Base[CamCsid],0x1004)||CamRead(c->Base[CamCsid],0x1204)||CamRead(c->Base[CamCsid],0x1404)||
               ((CamRead(c->Base[CamCpas],0xf0)|CamRead(c->Base[CamCpas],0xf8))&1)) {
                status=STATUS_DEVICE_BUSY;goto done;
            }
            const ULONG tbus[]={CamHfTbu,CamSfTbu};
            for(ULONG i=0;i<RTL_NUMBER_OF(tbus);i++)
                if(CamRead64(c->Base[tbus[i]],0xc10)||CamRead64(c->Base[tbus[i]],0xc20)) {
                    status=STATUS_DEVICE_BUSY;goto done;
                }
        }
        if(!c->Frame||!c->FrameMdl[0]){status=STATUS_DEVICE_NOT_READY;goto done;}
        ULONG count=code==QCAM_ARENA_DIAGNOSTIC?QCAM_ARENA_BYTES:QCAM_FRAME_BYTES;
        PVOID source=code==QCAM_ARENA_DIAGNOSTIC?c->FrameAllocation:c->Frame;
        if(!source){status=STATUS_DEVICE_NOT_READY;goto done;}
        PVOID buffer;status=WdfRequestRetrieveOutputBuffer(request,count,&buffer,NULL);
        if(NT_SUCCESS(status)) {
            KeMemoryBarrier();
            for(ULONG offset=0;offset<count;offset+=sizeof(ULONG))
                *(ULONG *)((PUCHAR)buffer+offset)=*(volatile ULONG *)((PUCHAR)source+offset);
            bytes=count;
        }
        goto done;
    }
    if(code==QCAM_ISP_FD_CAPTURE_NATIVE_DISCARD||code==QCAM_ISP_FD_CAPTURE_NATIVE_SETTLED||code==QCAM_ISP_FD_CAPTURE_NATIVE_BLACKLEVEL||code==QCAM_ISP_FD_CAPTURE_COMMON_BUFFER32||code==QCAM_ISP_FD_LUT_PROBE||code==QCAM_ISP_FD_PROBE||code==QCAM_ISP_FD_CAPTURE||code==QCAM_ISP_FD_CAPTURE_HALF||code==QCAM_ISP_FD_CAPTURE_EXACT||code==QCAM_ISP_FD_CAPTURE_BYTE||code==QCAM_ISP_FD_CAPTURE_UNCACHED||code==QCAM_ISP_FD_CAPTURE_LOW||code==QCAM_ISP_FD_CAPTURE_OFFSET||code==QCAM_ISP_FD_CAPTURE_CONSTANT||code==QCAM_ISP_FD_CAPTURE_CHROMA||code==QCAM_ISP_FD_CAPTURE_CST_BIAS||code==QCAM_ISP_FD_CAPTURE_COMPAND||code==QCAM_ISP_FD_CAPTURE_GAMMA||code==QCAM_ISP_FD_CAPTURE_CC_BYPASS||code==QCAM_ISP_FD_CAPTURE_BLS_BYPASS||code==QCAM_ISP_FD_CAPTURE_WB_BYPASS||code==QCAM_ISP_FD_CAPTURE_LUMA_BIAS||code==QCAM_ISP_FD_CAPTURE_BDS2||code==QCAM_ISP_FD_CAPTURE_GTM||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA_BDS2||code==QCAM_ISP_FD_CAPTURE_CB_LUMA||code==QCAM_ISP_FD_CAPTURE_INVERSE_LUMA||code==QCAM_ISP_FD_CAPTURE_NATIVE_LUMA_BIAS||code==QCAM_ISP_FD_CAPTURE_POSITIVE_GREEN||code==QCAM_ISP_FD_CAPTURE_NEGATIVE_GREEN||code==QCAM_ISP_FD_CAPTURE_PRECISION_CHAIN||code==QCAM_ISP_FD_CAPTURE_PRECISION_CHAIN_BDS2||code==QCAM_ISP_FD_CAPTURE_LUMA_CLAMP||code==QCAM_ISP_FD_CAPTURE_ZERO_CLAMP||code==QCAM_ISP_FD_CAPTURE_CST_BYPASS||code==QCAM_ISP_FD_CAPTURE_COHERENT||code==QCAM_ISP_FD_CAPTURE_CST_16BIT||code==QCAM_ISP_FD_CAPTURE_CST_14BIT||code==QCAM_ISP_FD_CAPTURE_COMMON_BUFFER||code==QCAM_ISP_FD_CAPTURE_NATIVE_DMI64||code==QCAM_ISP_FD_CAPTURE_OFFSET128) {
        QCAM_ISP_FD_STATE *state;
        BOOLEAN lutProbe=code==QCAM_ISP_FD_LUT_PROBE;
        BOOLEAN capture=code!=QCAM_ISP_FD_PROBE&&!lutProbe;
        ULONG capacity=sizeof(*state)+(capture?QCAM_ISP_FD_BYTES:0);
        if(input){status=STATUS_INVALID_PARAMETER;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,capacity,(PVOID *)&state,NULL);if(!NT_SUCCESS(status))goto done;
        RtlZeroMemory(state,sizeof(*state));state->Size=sizeof(*state);state->Abi=QCAM_ABI;
        if(c->IspFd&&!c->IspFdOwned)RtlZeroMemory(c->IspFd,sizeof(*c->IspFd));
        c->IspFdHalf=code==QCAM_ISP_FD_CAPTURE_HALF;
        c->IspFdExact=code==QCAM_ISP_FD_CAPTURE_EXACT;
        c->IspFdByte=code==QCAM_ISP_FD_CAPTURE_BYTE;
        c->IspFdOffset=code==QCAM_ISP_FD_CAPTURE_OFFSET||code==QCAM_ISP_FD_CAPTURE_OFFSET128;
        c->IspFdAddressOffset=code==QCAM_ISP_FD_CAPTURE_OFFSET128?128:c->IspFdOffset?256:0;
        c->IspFdConstant=code==QCAM_ISP_FD_CAPTURE_CONSTANT;
        c->IspFdChroma=code==QCAM_ISP_FD_CAPTURE_CHROMA;
        c->IspFdCstBias=code==QCAM_ISP_FD_CAPTURE_CST_BIAS;
        c->IspFdCompand=code==QCAM_ISP_FD_CAPTURE_COMPAND;
        c->IspFdCoherent=code==QCAM_ISP_FD_CAPTURE_COHERENT;
        c->IspFdDmi64=code==QCAM_ISP_FD_CAPTURE_NATIVE_DMI64;
        c->IspFdSettled=code==QCAM_ISP_FD_CAPTURE_NATIVE_SETTLED;
        c->IspFdDiscard=code==QCAM_ISP_FD_CAPTURE_NATIVE_DISCARD;
        c->IspFdCommonBuffer32=c->IspFdDiscard||c->IspFdSettled||code==QCAM_ISP_FD_CAPTURE_COMMON_BUFFER32||code==QCAM_ISP_FD_CAPTURE_NATIVE_BLACKLEVEL;
        c->IspFdCommonBuffer=c->IspFdCommonBuffer32||lutProbe||c->IspFdDmi64||code==QCAM_ISP_FD_CAPTURE_COMMON_BUFFER||code==QCAM_ISP_FD_CAPTURE_OFFSET128;
        c->IspFdPrecisionChain=code==QCAM_ISP_FD_CAPTURE_PRECISION_CHAIN||code==QCAM_ISP_FD_CAPTURE_PRECISION_CHAIN_BDS2;
        c->IspFdLumaProfile=(c->IspFdDiscard||c->IspFdSettled||code==QCAM_ISP_FD_CAPTURE_NATIVE_BLACKLEVEL)?33:code==QCAM_ISP_FD_CAPTURE_OFFSET128?32:c->IspFdDmi64?31:c->IspFdCommonBuffer?30:code==QCAM_ISP_FD_CAPTURE_COHERENT?27:code==QCAM_ISP_FD_CAPTURE_CST_16BIT?28:code==QCAM_ISP_FD_CAPTURE_CST_14BIT?29:code==QCAM_ISP_FD_CAPTURE_CST_BYPASS?26:code==QCAM_ISP_FD_CAPTURE_LUMA_CLAMP?24:code==QCAM_ISP_FD_CAPTURE_ZERO_CLAMP?25:code==QCAM_ISP_FD_CAPTURE_NATIVE_LUMA_BIAS?19:code==QCAM_ISP_FD_CAPTURE_POSITIVE_GREEN?20:code==QCAM_ISP_FD_CAPTURE_NEGATIVE_GREEN?21:0;
        c->IspFdCbLuma=code==QCAM_ISP_FD_CAPTURE_CB_LUMA;
        c->IspFdInverseLuma=code==QCAM_ISP_FD_CAPTURE_INVERSE_LUMA;
        c->IspFdGamma=c->IspFdPrecisionChain||c->IspFdLumaProfile||c->IspFdCbLuma||c->IspFdInverseLuma||code==QCAM_ISP_FD_CAPTURE_GAMMA||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA_BDS2;
        c->IspFdCcBypass=code==QCAM_ISP_FD_CAPTURE_CC_BYPASS;
        c->IspFdBlsBypass=code==QCAM_ISP_FD_CAPTURE_BLS_BYPASS;
        c->IspFdWbBypass=code==QCAM_ISP_FD_CAPTURE_WB_BYPASS;
        c->IspFdLumaBias=code==QCAM_ISP_FD_CAPTURE_LUMA_BIAS;
        c->IspFdBds2=code==QCAM_ISP_FD_CAPTURE_PRECISION_CHAIN_BDS2||code==QCAM_ISP_FD_CAPTURE_BDS2||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA_BDS2;
        c->IspFdGtm=c->IspFdPrecisionChain||c->IspFdLumaProfile||c->IspFdCbLuma||c->IspFdInverseLuma||code==QCAM_ISP_FD_CAPTURE_GTM||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA||code==QCAM_ISP_FD_CAPTURE_GTM_GAMMA_BDS2;
        c->IspFdUncached=!c->IspFdCoherent&&(code==QCAM_ISP_FD_CAPTURE_UNCACHED||c->IspFdOffset||c->IspFdConstant||c->IspFdChroma||c->IspFdCstBias||c->IspFdCompand||c->IspFdGamma||c->IspFdCcBypass||c->IspFdBlsBypass||c->IspFdWbBypass||c->IspFdLumaBias||c->IspFdBds2||c->IspFdGtm);
        c->IspFdLow=code==QCAM_ISP_FD_CAPTURE_LOW;
        c->ProbeLimit=0;c->PipelineLimit=capture?14:15;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status)&&!capture)status=CamIspFdConfigure(c,NULL);
        if(NT_SUCCESS(status)&&capture)status=WaitFrame(c,request,options.TimeoutMs);
        QCAM_DIAG diag=c->Diag;
        NTSTATUS stopped=CamCaptureStop(c);
        if(!NT_SUCCESS(status))c->Diag=diag;
        if(NT_SUCCESS(status))status=stopped;
        if(c->IspFd&&c->IspFd->Size==sizeof(*state))RtlCopyMemory(state,c->IspFd,sizeof(*state));
        state->Status=(ULONG)status;bytes=sizeof(*state);
        if(NT_SUCCESS(status)&&capture) {
            if(!(state->Stage&16)||state->GuardChanged||!state->ImageChanged)status=STATUS_DATA_ERROR;
            else if(!state->ImageBytes||state->ImageBytes>QCAM_ISP_FD_BYTES)status=STATUS_INVALID_BUFFER_SIZE;
            else {RtlCopyMemory(state+1,(PUCHAR)c->Frame+state->BufferOffset,state->ImageBytes);bytes+=state->ImageBytes;}
        }
        diag=c->Diag;
        NTSTATUS off=CamPowerOff(c);
        if(!NT_SUCCESS(status))c->Diag=diag;
        if(NT_SUCCESS(status))status=off;
        state->Status=(ULONG)status;if(!NT_SUCCESS(status))bytes=sizeof(*state);
        c->Info.Status=status;c->Diag.Status=status;c->PipelineLimit=0;
        /* Return diagnostic evidence even when the native check fails. */
        status=STATUS_SUCCESS;goto done;
    }
    if(code==QCAM_ISP_GAMMA_PROBE) {
        QCAM_ISP_GAMMA_STATE *state;
        if(input||output!=sizeof(*state)||c->StreamFile){status=STATUS_INVALID_PARAMETER;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*state),(PVOID *)&state,NULL);if(!NT_SUCCESS(status))goto done;
        RtlZeroMemory(state,sizeof(*state));
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=CamCdmGamma(c,state);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=c->CdmPending?STATUS_DEVICE_BUSY:CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;
        c->Diag.Status=status;
        if(state->Size==sizeof(*state)){state->Status=(ULONG)status;bytes=sizeof(*state);status=STATUS_SUCCESS;}
        goto done;
    }
    if(code==QCAM_ISP_PIXEL_PROBE) {
        QCAM_ISP_PIXEL_STATE *state;
        if(input||output!=sizeof(*state)||c->StreamFile){status=STATUS_INVALID_PARAMETER;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*state),(PVOID *)&state,NULL);if(!NT_SUCCESS(status))goto done;
        RtlZeroMemory(state,sizeof(*state));
        c->ProbeLimit=0;c->PipelineLimit=1;
        status=CamPowerOn(c);
        if(NT_SUCCESS(status))status=CamBusOn(c);
        if(NT_SUCCESS(status))status=CamIspPixelCheck(c,state);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;
        c->Diag.Status=status;
        if(state->Size==sizeof(*state)){state->Status=(ULONG)status;bytes=sizeof(*state);status=STATUS_SUCCESS;}
        goto done;
    }
    if(code==QCAM_ISP_INPUT_PROBE) {
        QCAM_ISP_INPUT_STATE *state;
        if(input||output!=sizeof(*state)||c->StreamFile){status=STATUS_INVALID_PARAMETER;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*state),(PVOID *)&state,NULL);if(!NT_SUCCESS(status))goto done;
        RtlZeroMemory(state,sizeof(*state));
        c->ProbeLimit=0;c->PipelineLimit=1;
        status=CamPowerOn(c);
        if(NT_SUCCESS(status))status=CamBusOn(c);
        if(NT_SUCCESS(status))status=CamIspInputCheck(c,state);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;
        c->Diag.Status=status;
        if(state->Size==sizeof(*state)){state->Status=(ULONG)status;bytes=sizeof(*state);status=STATUS_SUCCESS;}
        goto done;
    }
    if(code==QCAM_ISP_PROBE) {
        QCAM_ISP_STATE *state;
        if(input||output!=sizeof(*state)||c->StreamFile){status=STATUS_INVALID_PARAMETER;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*state),(PVOID *)&state,NULL);if(!NT_SUCCESS(status))goto done;
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status)) {
            RtlZeroMemory(state,sizeof(*state));state->Size=sizeof(*state);state->Abi=QCAM_ABI;
            for(ULONG i=0;i<RTL_NUMBER_OF(CamIspCsidRegs);i++)state->Csid[i]=CamRead(c->Base[CamCsid],CamIspCsidRegs[i]);
            for(ULONG i=0;i<RTL_NUMBER_OF(CamIspTfeRegs);i++)state->Tfe[i]=CamRead(c->Base[CamTfe],CamIspTfeRegs[i]);
        }
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;
        if(NT_SUCCESS(status))bytes=sizeof(*state);
        c->Diag.Status=status;goto done;
    }
    if(code==QCAM_TBU_PROBE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status)) {
            const ULONG regions[]={CamHfTbu,CamSfTbu};
            for(ULONG n=0;n<2;n++) {
                PUCHAR base=c->Base[regions[n]];
                for(ULONG i=0;i<RTL_NUMBER_OF(CamTbuTraceRegs);i++)
                    c->Trace.Tbu[n][i]=i?CamRead64(base,CamTbuTraceRegs[i]):CamRead(base,0);
            }
            c->Diag.Step=167;c->Diag.Region=CamHfTbu;c->Diag.Offset=0;
            c->Diag.Mask=MAXULONG;c->Diag.Observed=(ULONG)c->Trace.Tbu[0][0];
        }
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_CLOCK_MEASURE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=CamMeasureClocks(c);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_PREPARE_RELEASE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        status=CamCaptureStop(c);if(NT_SUCCESS(status))status=CamPowerOff(c);
        if(NT_SUCCESS(status))c->PipelineLimit=0;goto done;
    }
    if(code==QCAM_NOC_REGISTERS) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        if(c->Powered){status=CamNocRegisters(c);c->Diag.Status=status;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=CamNocRegisters(c);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_SCM_NOC_FAULTS||code==QCAM_SCM_NOC_FAULTS32||code==QCAM_CAMERA_BUS_PROBE||code==QCAM_FRAME_ROUTE_PROBE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=code==QCAM_FRAME_ROUTE_PROBE?CamFrameRoutePrepare(c):code==QCAM_CAMERA_BUS_PROBE?CamCameraBusProbe(c):
            code==QCAM_SCM_NOC_FAULTS32?CamScmNocFaults32(c):CamScmNocFaults(c);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_BUS_SNAPSHOT) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        if(!c->Powered||!c->BusOn){status=STATUS_DEVICE_NOT_READY;goto done;}
        if(c->ScmPending||c->CdmPending||c->TbuPending||c->CrmPending||c->RxActive||c->DmaActive){status=STATUS_DEVICE_BUSY;goto done;}
        CamBusSnapshot(c);status=STATUS_SUCCESS;goto done;
    }
    if(code==QCAM_SCM_QOS_SMART||code==QCAM_SCM_QOS_RDI||code==QCAM_SCM_QOS_CDM) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status)){status=code==QCAM_SCM_QOS_CDM?CamScmQosCdm(c):code==QCAM_SCM_QOS_RDI?CamScmQosRdi(c):CamScmQosSmart(c);c->Diag.Status=status;goto done;}
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_MNOC_PROBE||code==QCAM_MNOC_QOS_HOLD) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=CamMnocQos(c,code==QCAM_MNOC_QOS_HOLD);
        if(code==QCAM_MNOC_QOS_HOLD&&NT_SUCCESS(status)){c->Diag.Status=status;goto done;}
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_TBU_TRANSLATE||code==QCAM_FRAME_TRANSLATE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=code==QCAM_FRAME_TRANSLATE?CamFrameTranslate(c):CamCdmTranslate(c);
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_CDM_CONFIG||code==QCAM_CDM_FETCH||code==QCAM_CDM_PREPARE||code==QCAM_CDM_PREPARE_HOLD) {
        ULONG limit=4;
        if(output||(input&&((code!=QCAM_CDM_PREPARE&&code!=QCAM_CDM_PREPARE_HOLD)||input!=sizeof(ULONG)))){status=STATUS_INVALID_PARAMETER;goto done;}
        if(input) {
            ULONG *p;status=WdfRequestRetrieveInputBuffer(request,sizeof(*p),(PVOID *)&p,NULL);
            if(!NT_SUCCESS(status))goto done;
            limit=*p;if(limit<1||limit>4){status=STATUS_INVALID_PARAMETER;goto done;}
        }
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=code==QCAM_CDM_CONFIG?CamCdmConfigure(c):code==QCAM_CDM_FETCH?CamCdmFetch(c):CamCdmPrepare(c,limit);
        if(code==QCAM_CDM_PREPARE_HOLD&&NT_SUCCESS(status)){c->Diag.Status=status;goto done;}
        QCAM_DIAG diag=c->Diag;NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;c->PipelineLimit=0;
        if(NT_SUCCESS(status))status=stopped;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_CDM_PROBE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;status=CamPowerOn(c);
        if(NT_SUCCESS(status)) {
            for(ULONG i=0;i<RTL_NUMBER_OF(CamCdmTraceRegs);i++)c->Trace.Cdm[i]=CamRead(c->Base[CamCdm],CamCdmTraceRegs[i]);
            c->Diag.Step=160;c->Diag.Region=CamCdm;c->Diag.Offset=0;c->Diag.Observed=c->Trace.Cdm[0];
            if(!c->Trace.Cdm[0]||c->Trace.Cdm[0]==MAXULONG)status=STATUS_REVISION_MISMATCH;
        }
        QCAM_DIAG diag=c->Diag;NTSTATUS off=CamPowerOff(c);c->Diag=diag;
        if(NT_SUCCESS(status))status=off;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_SCM_WAITQ_PROBE||code==QCAM_SCM_WAITQ_CONTEXT) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        status=code==QCAM_SCM_WAITQ_PROBE?CamScmWaitqProbe(c):CamScmWaitqContext(c);goto done;
    }
    if(code==QCAM_SCM_CONTINUE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        status=CamScmContinue(c);goto done;
    }
    if(code==QCAM_SCM_PROBE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        status=CamScmProbe(c);goto done;
    }
    if(code==QCAM_SCM_IO_PROBE) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;status=CamPowerOn(c);
        if(NT_SUCCESS(status))status=CamScmIoProbe(c);
        QCAM_DIAG diag=c->Diag;NTSTATUS off=CamPowerOff(c);c->Diag=diag;
        if(NT_SUCCESS(status))status=off;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_SCM_QOS_PROBE||code==QCAM_SCM_QOS_DIAGNOSTIC||code==QCAM_SCM_QOS_RAW) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        if(c->ScmPending||c->CdmPending||c->TbuPending||c->CrmPending||c->DmaActive||c->RxActive||c->FrameRouteOwned){status=STATUS_DEVICE_BUSY;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=code==QCAM_SCM_QOS_RAW?CamScmQosRaw(c):
            code==QCAM_SCM_QOS_DIAGNOSTIC?CamScmQosDiagnostic(c):CamScmQos(c);
        if(code==QCAM_SCM_QOS_RAW){c->Diag.Status=status;goto done;}
        if(c->Powered&&!c->ScmPending)CamBusSnapshot(c);
        QCAM_DIAG diag=c->Diag;
        NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(stopped))stopped=CamPowerOff(c);
        c->Diag=diag;
        if(NT_SUCCESS(status))status=stopped;c->PipelineLimit=0;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_DMA_ROUTES) {
        QCAM_DMA_ROUTE_INFO *routes;
        status=WdfRequestRetrieveOutputBuffer(request,sizeof(*routes),(PVOID *)&routes,NULL);
        if(NT_SUCCESS(status)) {
            RtlZeroMemory(routes,sizeof(*routes));routes->Size=sizeof(*routes);routes->Abi=QCAM_ABI;
            PUCHAR b=c->Base[CamSmmu];routes->Id0=CamRead(b,0x20);routes->Id1=CamRead(b,0x24);routes->Global=CamRead(b,0);
            routes->FaultStatus=CamRead(b,0x48);
            for(ULONG i=0;i<3;i++)routes->FaultSyndrome[i]=CamRead(b,0x50+i*4);
            if(!(routes->Id0&(1u<<27))||(routes->Id0&0x100))status=STATUS_NOT_SUPPORTED;
            else {
                routes->Count=min(routes->Id0&255,RTL_NUMBER_OF(routes->Smr));
                for(ULONG i=0;i<routes->Count;i++) {
                    routes->Smr[i]=CamRead(b,0x800+4*i);routes->S2cr[i]=CamRead(b,0xc00+4*i);
                    ULONG bank=routes->S2cr[i]&255;
                    if((routes->Smr[i]&0x80000000u)&&!(routes->S2cr[i]&0x30000)&&
                       bank<(routes->Id1&255)&&!(routes->Id1&0x80000000u)) {
                        ULONG context=1u<<(((routes->Id1>>28)&7)+1);
                        if(context*4096+bank*4096<Sizes[CamSmmu]) {
                            routes->Cbar[i]=CamRead(b,0x1000+4*bank);
                            routes->Sctlr[i]=CamRead(b,context*4096+bank*4096);
                        }
                    }
                }
                bytes=sizeof(*routes);
            }
        }
        goto done;
    }
    if(code==QCAM_EXPOSURE) {
        QCAM_EXPOSURE_OPTIONS *source;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*source),(PVOID *)&source,NULL);
        if(!NT_SUCCESS(status))goto done;
        QCAM_EXPOSURE_OPTIONS o=*source;
        if(input!=sizeof(o)||o.Size!=sizeof(o)||o.Abi!=QCAM_ABI||!o.Lines||o.Lines>0x1d68-48||o.Gain>0x3fff){status=STATUS_INVALID_PARAMETER;goto done;}
        c->Info.Exposure=o.Lines;c->Info.Gain=o.Gain;status=STATUS_SUCCESS;goto done;
    }
    if(code==QCAM_PROBE||code==QCAM_PLATFORM_PROBE) {
        c->ProbeLimit=0;
        if(code==QCAM_PROBE&&input){status=STATUS_INVALID_PARAMETER;goto done;}
        if(code==QCAM_PLATFORM_PROBE) {
            QCAM_PROBE_OPTIONS *o;
            status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
            if(!NT_SUCCESS(status))goto done;
            if(input!=sizeof(*o)||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||!o->Limit||o->Limit>10){status=STATUS_INVALID_PARAMETER;goto done;}
            c->ProbeLimit=o->Limit;
        }
        status=CamPowerOn(c);QCAM_DIAG diag=c->Diag;NTSTATUS off=CamPowerOff(c);
        if(!NT_SUCCESS(status)||NT_SUCCESS(off))c->Diag=diag;c->ProbeLimit=0;
        if(NT_SUCCESS(status))status=off;c->Info.Status=status;c->Diag.Status=status;goto done;
    }
    if(code==QCAM_CAPTURE||code==QCAM_CAPTURE_PDAF||code==QCAM_CAPTURE_DIAGNOSTIC||code==QCAM_CAPTURE_FRAME_DIAGNOSTIC||code==QCAM_CAPTURE_AUTO_DIAGNOSTIC||code==QCAM_CAPTURE_PREFETCH_DIAGNOSTIC||code==QCAM_CAPTURE_PRIMED_DIAGNOSTIC||code==QCAM_CAPTURE_CONTEXT_DIAGNOSTIC||code==QCAM_CAPTURE_PACKED_DIAGNOSTIC) {
        c->ProbeLimit=0;c->PipelineLimit=code==QCAM_CAPTURE_PDAF?13:code==QCAM_CAPTURE_PACKED_DIAGNOSTIC?12:code==QCAM_CAPTURE_CONTEXT_DIAGNOSTIC?11:code==QCAM_CAPTURE_PRIMED_DIAGNOSTIC?10:code==QCAM_CAPTURE_PREFETCH_DIAGNOSTIC?9:code==QCAM_CAPTURE_AUTO_DIAGNOSTIC?8:code==QCAM_CAPTURE_FRAME_DIAGNOSTIC?7:code==QCAM_CAPTURE_DIAGNOSTIC?6:0;
        QCAM_CAPTURE_OPTIONS *source;PVOID buffer;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*source),(PVOID *)&source,NULL);
        if(!NT_SUCCESS(status))goto done;
        QCAM_CAPTURE_OPTIONS o=*source;
        if(input!=sizeof(o)||o.Size!=sizeof(o)||o.Abi!=QCAM_ABI||o.TimeoutMs<100||o.TimeoutMs>30000||o.SettleCount>255||o.DataRate<182400000||o.DataRate>13680000000ULL){status=STATUS_INVALID_PARAMETER;goto done;}
        if(code!=QCAM_CAPTURE&&o.TimeoutMs>1000){status=STATUS_INVALID_PARAMETER;goto done;}
        if(output<QCAM_FRAME_BYTES){status=STATUS_BUFFER_TOO_SMALL;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,QCAM_FRAME_BYTES,&buffer,NULL);
        if(!NT_SUCCESS(status))goto done;
        status=CamCaptureStart(c,&o);
        if(NT_SUCCESS(status)) {
            status=WaitFrame(c,request,o.TimeoutMs);
        }
        QCAM_DIAG failed=c->Diag;
        NTSTATUS stopped=CamCaptureStop(c);
        if(!NT_SUCCESS(status))c->Diag=failed;
        if(NT_SUCCESS(status))status=stopped;
        if(NT_SUCCESS(status)) {
            KeMemoryBarrier();
            if(code==QCAM_CAPTURE||code==QCAM_CAPTURE_PDAF)Merge(c->Frame,code==QCAM_CAPTURE_PDAF?QCAM_PDAF_BYTES:QCAM_FRAME_BYTES,QCAM_FRAME_BYTES*2+256);
            bytes=code==QCAM_CAPTURE_PDAF?QCAM_PDAF_BYTES:QCAM_FRAME_BYTES;RtlCopyMemory(buffer,c->Frame,bytes);
            c->Info.Flags|=QCAM_FRAME_VALID;c->Info.Frames++;c->Info.Stage=10;
        }
        QCAM_DIAG diag=c->Diag;
        if(code!=QCAM_CAPTURE&&NT_SUCCESS(stopped)&&c->Powered&&c->ScmNocReadable)CamScmNocFaults(c);
        c->Diag=diag;NTSTATUS off=CamPowerOff(c);
        if(!NT_SUCCESS(status))c->Diag=diag;
        if(NT_SUCCESS(status)&&!NT_SUCCESS(off)){status=off;bytes=0;}
        c->Info.Status=status;
        c->Diag.Status=status;
        c->PipelineLimit=0;
    }
    if(code==QCAM_PIPELINE_PROBE) {
        QCAM_PROBE_OPTIONS *o;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*o),(PVOID *)&o,NULL);
        if(!NT_SUCCESS(status))goto done;
        if(input!=sizeof(*o)||o->Size!=sizeof(*o)||o->Abi!=QCAM_ABI||!o->Limit||o->Limit>5){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=o->Limit;
        if(c->PipelineLimit==3)c->ReceiverVerified=FALSE;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,5000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status)&&c->PipelineLimit==3) {
            CamDelay(100000);CamSnapshot(c);
            if(!c->Info.Packets)status=STATUS_IO_TIMEOUT;
            else if(c->Info.CsidRxIrq&0x25fff000u)status=STATUS_DATA_ERROR;
            c->ReceiverVerified=NT_SUCCESS(status);
        }
        NTSTATUS stopped=CamCaptureStop(c);
        if(NT_SUCCESS(status))status=stopped;
        QCAM_DIAG diag=c->Diag;NTSTATUS off=CamPowerOff(c);
        if(!NT_SUCCESS(status)||NT_SUCCESS(off))c->Diag=diag;
        if(NT_SUCCESS(status))status=off;c->PipelineLimit=0;c->Info.Status=status;c->Diag.Status=status;
    }
done:
    if(bound){KeSetPriorityThread(KeGetCurrentThread(),previousPriority);KeRevertToUserGroupAffinityThread(&previousAffinity);}
    WdfWaitLockRelease(c->Lock);WdfRequestCompleteWithInformation(request,status,bytes);
}

static NTSTATUS AddDevice(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init,FILE_DEVICE_UNKNOWN);WdfDeviceInitSetExclusive(init,TRUE);
    WdfDeviceInitSetCharacteristics(init,FILE_DEVICE_SECURE_OPEN,FALSE);
    WDF_FILEOBJECT_CONFIG files;WDF_FILEOBJECT_CONFIG_INIT(&files,NULL,NULL,FileCleanup);
    WdfDeviceInitSetFileObjectConfig(init,&files,WDF_NO_OBJECT_ATTRIBUTES);
    UNICODE_STRING deviceName=RTL_CONSTANT_STRING(L"\\Device\\QcomCam");
    NTSTATUS status=WdfDeviceInitAssignName(init,&deviceName);if(!NT_SUCCESS(status))return status;
    UNICODE_STRING security=RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    status=WdfDeviceInitAssignSDDLString(init,&security);if(!NT_SUCCESS(status))return status;
    WDF_PNPPOWER_EVENT_CALLBACKS p;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDevicePrepareHardware=Prepare;p.EvtDeviceReleaseHardware=Release;p.EvtDeviceD0Entry=Start;p.EvtDeviceD0Exit=Stop;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,CAM);
    a.ExecutionLevel=WdfExecutionLevelPassive;a.SynchronizationScope=WdfSynchronizationScopeNone;a.EvtCleanupCallback=Cleanup;
    WDFDEVICE device;status=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(status))return status;
    CAM *c=CamContext(device);c->Device=device;
    c->Info.Size=sizeof(QCAM_INFO);c->Info.Abi=QCAM_ABI;c->Info.Width=QCAM_WIDTH;c->Info.Height=QCAM_HEIGHT;
    c->Info.Stride=QCAM_STRIDE;c->Info.FrameBytes=QCAM_FRAME_BYTES;c->Info.Exposure=1000;c->Info.Gain=0x1334;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;
    status=WdfWaitLockCreate(&a,&c->Lock);if(!NT_SUCCESS(status))return status;
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"\\DosDevices\\qcom-cam");
    status=WdfDeviceCreateSymbolicLink(device,&name);if(!NT_SUCCESS(status))return status;
    static const GUID bus={0x83e80906,0x5a57,0x4e38,{0xa9,0x00,0x87,0x50,0x09,0x06,0x00,0x02}};
    PNP_BUS_INFORMATION information={bus,PNPBus,0};WdfDeviceSetBusInformationForChildren(device,&information);
    static const PCWSTR ids[]={L"QCOM\\IMX906",L"QCOM\\IMX355",L"QCOM\\OV02B10",L"QCOM\\IMX471"};
    static const PCWSTR descriptions[]={L"Sony IMX906 Main Camera",L"Sony IMX355 Ultrawide Camera",L"OmniVision OV02B10 Macro Camera",L"Sony IMX471 Front Camera"};
    for(ULONG index=0;index<RTL_NUMBER_OF(ids);index++) {
        PWDFDEVICE_INIT childInit=WdfPdoInitAllocate(device);if(!childInit)return STATUS_INSUFFICIENT_RESOURCES;
        UNICODE_STRING id,instance=RTL_CONSTANT_STRING(L"0"),description,location=RTL_CONSTANT_STRING(L"OnePlus Ace 5 Pro");
        RtlInitUnicodeString(&id,ids[index]);RtlInitUnicodeString(&description,descriptions[index]);
        status=WdfPdoInitAssignDeviceID(childInit,&id);
        if(NT_SUCCESS(status))status=WdfPdoInitAddHardwareID(childInit,&id);
        if(NT_SUCCESS(status))status=WdfPdoInitAssignInstanceID(childInit,&instance);
        if(NT_SUCCESS(status))status=WdfPdoInitAddDeviceText(childInit,&description,&location,0x409);
        if(!NT_SUCCESS(status)){WdfDeviceInitFree(childInit);return status;}
        WdfPdoInitSetDefaultLocale(childInit,0x409);
        WDFDEVICE child;status=WdfDeviceCreate(&childInit,WDF_NO_OBJECT_ATTRIBUTES,&child);
        if(!NT_SUCCESS(status)){if(childInit)WdfDeviceInitFree(childInit);return status;}
        status=WdfFdoAddStaticChild(device,child);if(!NT_SUCCESS(status)){WdfObjectDelete(child);return status;}
    }
    WDF_IO_QUEUE_CONFIG q;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchSequential);q.EvtIoDeviceControl=Control;
    return WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,AddDevice);
    return WdfDriverCreate(object,registry,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
