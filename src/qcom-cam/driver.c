#include "cam.h"
#include <acpiioct.h>
#include "trace-regs.h"

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY Start;
static EVT_WDF_DEVICE_D0_EXIT Stop;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Control;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP Cleanup;

static const ULONG Bases[]={0x00100000,0x0ade0000,0x01760000,0x0ac7b000,0x0adb3000,0x0ad27000,0x0ac86000,0x0ac04000,0x0ad90000,
    0x15000000,0x0f100000,0x0c400000,0x0c42d000,0x0c440000,0x0c500000,0x16520000,0x81c60000,0x0adcb000,0x0ac7f000,0x017d2000,0x017d1000};
static const ULONG Sizes[]={0x100000,0x20000,0x6000,0x1000,0x2000,0x3000,0x10000,0x1000,0x9000,
    0x100000,0x100000,0x3000,0x4000,0x80000,0x400000,0x10000,0x20000,0x5000,0x1000,0x1000,0x1000};
C_ASSERT(RTL_NUMBER_OF(Bases)==CamRegionCount);

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
        c->Base[n]=MmMapIoSpaceEx(address,Sizes[n],(n==CamDb?PAGE_READONLY:PAGE_READWRITE)|PAGE_NOCACHE);
        if(!c->Base[n])return STATUS_INSUFFICIENT_RESOURCES;
    }
    FirmwareStatus(c);
    return STATUS_SUCCESS;
}

static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);CAM *c=CamContext(device);
    InterlockedExchange(&c->Online,0);WdfWaitLockAcquire(c->Lock,NULL);
    NTSTATUS status=CamCaptureStop(c);
    if(NT_SUCCESS(status))status=CamPowerOff(c);
    CamDmaFree(c);
    if(NT_SUCCESS(status)&&!c->DmaActive&&!c->CdmPending&&!c->ScmPending&&!c->CrmPending)
        for(ULONG i=0;i<CamRegionCount;i++)if(c->Base[i]){MmUnmapIoSpace(c->Base[i],Sizes[i]);c->Base[i]=NULL;}
    WdfWaitLockRelease(c->Lock);return status;
}

static NTSTATUS Start(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);CAM *c=CamContext(device);
    if(c->Faulted||c->DmaActive||c->CdmPending)return STATUS_DEVICE_HARDWARE_ERROR;
    InterlockedExchange(&c->Online,1);return STATUS_SUCCESS;
}

static NTSTATUS Stop(WDFDEVICE device,WDF_POWER_DEVICE_STATE next) {
    UNREFERENCED_PARAMETER(next);CAM *c=CamContext(device);
    InterlockedExchange(&c->Online,0);WdfWaitLockAcquire(c->Lock,NULL);
    NTSTATUS status=CamCaptureStop(c);if(NT_SUCCESS(status))status=CamPowerOff(c);
    WdfWaitLockRelease(c->Lock);return status;
}

static VOID Cleanup(WDFOBJECT object) {
    CAM *c=CamContext(object);CamDmaFree(c);
    if(!c->DmaActive&&!c->CdmPending&&c->PowerState){ExFreePoolWithTag(c->PowerState,'pCcQ');c->PowerState=NULL;}
}

static VOID Control(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    CAM *c=CamContext(WdfIoQueueGetDevice(queue));NTSTATUS status=STATUS_INVALID_DEVICE_REQUEST;ULONG_PTR bytes=0;
    WdfWaitLockAcquire(c->Lock,NULL);
    if(!c->Online){status=STATUS_DEVICE_NOT_READY;goto done;}
    if(code==QCAM_QUERY) {
        QCAM_INFO *info;status=WdfRequestRetrieveOutputBuffer(request,sizeof(*info),(PVOID *)&info,NULL);
        if(NT_SUCCESS(status)){*info=c->Info;bytes=sizeof(*info);}goto done;
    }
    if(code==QCAM_DIAGNOSTICS) {
        QCAM_DIAG *diag;status=WdfRequestRetrieveOutputBuffer(request,sizeof(*diag),(PVOID *)&diag,NULL);
        if(NT_SUCCESS(status)){CamDiagnostics(c,diag);bytes=sizeof(*diag);}goto done;
    }
    if(code==QCAM_TRACE_QUERY) {
        QCAM_TRACE *trace;status=WdfRequestRetrieveOutputBuffer(request,sizeof(*trace),(PVOID *)&trace,NULL);
        if(NT_SUCCESS(status)){*trace=c->Trace;trace->Size=sizeof(*trace);trace->Abi=QCAM_ABI;bytes=sizeof(*trace);}goto done;
    }
    if(code==QCAM_FRAME_DIAGNOSTIC) {
        if(input||c->DmaActive||c->RxActive||c->ScmPending||c->CdmPending){status=STATUS_DEVICE_BUSY;goto done;}
        if(!c->Frame||!c->FrameMdl){status=STATUS_DEVICE_NOT_READY;goto done;}
        PVOID buffer;status=WdfRequestRetrieveOutputBuffer(request,QCAM_FRAME_BYTES,&buffer,NULL);
        if(NT_SUCCESS(status)){KeFlushIoBuffers(c->FrameMdl,TRUE,TRUE);RtlCopyMemory(buffer,c->Frame,QCAM_FRAME_BYTES);bytes=QCAM_FRAME_BYTES;}
        goto done;
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
    if(code==QCAM_CDM_CONFIG) {
        if(input||output){status=STATUS_INVALID_PARAMETER;goto done;}
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=CamCdmConfigure(c);
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
        c->ProbeLimit=0;c->PipelineLimit=1;
        QCAM_CAPTURE_OPTIONS options={sizeof(options),QCAM_ABI,1000,0,4980000000ULL};
        status=CamCaptureStart(c,&options);
        if(NT_SUCCESS(status))status=code==QCAM_SCM_QOS_RAW?CamScmQosRaw(c):
            code==QCAM_SCM_QOS_DIAGNOSTIC?CamScmQosDiagnostic(c):CamScmQos(c);
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
    if(code==QCAM_CAPTURE||code==QCAM_CAPTURE_DIAGNOSTIC) {
        c->ProbeLimit=0;c->PipelineLimit=code==QCAM_CAPTURE_DIAGNOSTIC?6:0;
        QCAM_CAPTURE_OPTIONS *source;PVOID buffer;
        status=WdfRequestRetrieveInputBuffer(request,sizeof(*source),(PVOID *)&source,NULL);
        if(!NT_SUCCESS(status))goto done;
        QCAM_CAPTURE_OPTIONS o=*source;
        if(input!=sizeof(o)||o.Size!=sizeof(o)||o.Abi!=QCAM_ABI||o.TimeoutMs<100||o.TimeoutMs>30000||o.SettleCount>255||o.DataRate<182400000||o.DataRate>13680000000ULL){status=STATUS_INVALID_PARAMETER;goto done;}
        if(code==QCAM_CAPTURE_DIAGNOSTIC&&o.TimeoutMs>1000){status=STATUS_INVALID_PARAMETER;goto done;}
        if(output<QCAM_FRAME_BYTES){status=STATUS_BUFFER_TOO_SMALL;goto done;}
        status=WdfRequestRetrieveOutputBuffer(request,QCAM_FRAME_BYTES,&buffer,NULL);
        if(!NT_SUCCESS(status))goto done;
        status=CamCaptureStart(c,&o);
        if(NT_SUCCESS(status)) {
            ULONGLONG deadline=KeQueryInterruptTime()+(ULONGLONG)o.TimeoutMs*10000;
            for(;;) {
                if(!c->Online||WdfRequestIsCanceled(request)){status=STATUS_CANCELLED;break;}
                if(CamCapturePoll(c,&status))break;
                if(KeQueryInterruptTime()>=deadline){status=STATUS_IO_TIMEOUT;break;}
                CamDelay(1000);
            }
        }
        QCAM_DIAG failed=c->Diag;
        NTSTATUS stopped=CamCaptureStop(c);
        if(!NT_SUCCESS(status))c->Diag=failed;
        if(NT_SUCCESS(status))status=stopped;
        if(NT_SUCCESS(status)) {
            KeFlushIoBuffers(c->FrameMdl,TRUE,TRUE);
            RtlCopyMemory(buffer,c->Frame,QCAM_FRAME_BYTES);bytes=QCAM_FRAME_BYTES;
            c->Info.Flags|=QCAM_FRAME_VALID;c->Info.Frames++;c->Info.Stage=10;
        }
        QCAM_DIAG diag=c->Diag;NTSTATUS off=CamPowerOff(c);
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
    WdfWaitLockRelease(c->Lock);WdfRequestCompleteWithInformation(request,status,bytes);
}

static NTSTATUS AddDevice(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetDeviceType(init,FILE_DEVICE_UNKNOWN);WdfDeviceInitSetExclusive(init,TRUE);
    WdfDeviceInitSetCharacteristics(init,FILE_DEVICE_SECURE_OPEN,FALSE);
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
    PWDFDEVICE_INIT childInit=WdfPdoInitAllocate(device);
    if(!childInit)return STATUS_INSUFFICIENT_RESOURCES;
    UNICODE_STRING id=RTL_CONSTANT_STRING(L"QCOM\\IMX906"),instance=RTL_CONSTANT_STRING(L"0");
    status=WdfPdoInitAssignDeviceID(childInit,&id);
    if(NT_SUCCESS(status))status=WdfPdoInitAddHardwareID(childInit,&id);
    if(NT_SUCCESS(status))status=WdfPdoInitAssignInstanceID(childInit,&instance);
    UNICODE_STRING description=RTL_CONSTANT_STRING(L"Sony IMX906 Main Camera (Qualcomm SM8750)");
    UNICODE_STRING location=RTL_CONSTANT_STRING(L"OnePlus Ace 5 Pro");
    if(NT_SUCCESS(status))status=WdfPdoInitAddDeviceText(childInit,&description,&location,0x409);
    if(!NT_SUCCESS(status)){WdfDeviceInitFree(childInit);return status;}
    WdfPdoInitSetDefaultLocale(childInit,0x409);
    WDFDEVICE child;status=WdfDeviceCreate(&childInit,WDF_NO_OBJECT_ATTRIBUTES,&child);
    if(!NT_SUCCESS(status)){if(childInit)WdfDeviceInitFree(childInit);return status;}
    status=WdfFdoAddStaticChild(device,child);if(!NT_SUCCESS(status)){WdfObjectDelete(child);return status;}
    WDF_IO_QUEUE_CONFIG q;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchSequential);q.EvtIoDeviceControl=Control;
    return WdfIoQueueCreate(device,&q,WDF_NO_OBJECT_ATTRIBUTES,NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,AddDevice);
    return WdfDriverCreate(object,registry,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
