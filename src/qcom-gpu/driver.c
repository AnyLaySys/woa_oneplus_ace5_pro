#include "gpu.h"
#include "uapi.h"
#include <wdf.h>
#include "regs.h"
#include <initguid.h>
DEFINE_GUID(GpuInterface,0x19c45a12,0xc057,0x4903,0xa8,0x30,0x61,0x19,0x45,0x61,0x20,0x26);
C_ASSERT(GPU_MAX_BUFFER_SIZE==GPU_BUFFER_LIMIT);

typedef struct {
    GPU Hardware;
    WDFINTERRUPT Interrupts[29];
    GPU_BO Buffers[GPU_MAX_BUFFERS];
    PVOID Maps[GPU_MAX_BUFFERS];
    PEPROCESS Owner;
    KMUTEX Lock;
    ULONG Irqs;
} GPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GPU_DEVICE,GpuContext)

static EVT_WDF_DRIVER_DEVICE_ADD Add;
static EVT_WDF_DEVICE_PREPARE_HARDWARE Prepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE Release;
static EVT_WDF_DEVICE_D0_ENTRY Enter;
static EVT_WDF_DEVICE_D0_EXIT Exit;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL Io;
static EVT_WDF_DEVICE_FILE_CREATE Create;
static EVT_WDF_FILE_CLEANUP Cleanup;
static EVT_WDF_INTERRUPT_ISR Interrupt;
static EVT_WDF_INTERRUPT_DPC InterruptDpc;
typedef struct { ULONG Index; } GPU_IRQ;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GPU_IRQ,IrqContext)

static VOID DestroyInterrupts(GPU_DEVICE *g);
static NTSTATUS Prepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated) {
    GPU_DEVICE *g=GpuContext(device);
    PHYSICAL_ADDRESS registers={0};ULONG length=0;
    g->Irqs=0;
    for(ULONG i=0;i<WdfCmResourceListGetCount(translated);i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(translated,i);
        if(r->Type==CmResourceTypeMemory && r->u.Memory.Start.QuadPart==0x03D00000) {
            registers=r->u.Memory.Start;length=r->u.Memory.Length;
        }
        if(r->Type==CmResourceTypeInterrupt)g->Irqs++;
    }
    if(g->Irqs!=29)return STATUS_DEVICE_CONFIGURATION_ERROR;
    NTSTATUS status=GpuBind(&g->Hardware,registers,length);if(!NT_SUCCESS(status))return status;
    ULONG irq=0;
    for(ULONG i=0;i<WdfCmResourceListGetCount(translated);i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(translated,i);
        if(r->Type!=CmResourceTypeInterrupt)continue;
        WDF_INTERRUPT_CONFIG config;WDF_INTERRUPT_CONFIG_INIT(&config,Interrupt,InterruptDpc);
        config.AutomaticSerialization=FALSE;
        config.InterruptRaw=WdfCmResourceListGetDescriptor(raw,i);config.InterruptTranslated=r;
        WDF_OBJECT_ATTRIBUTES attributes;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,GPU_IRQ);
        status=WdfInterruptCreate(device,&config,&attributes,g->Interrupts+irq);
        if(!NT_SUCCESS(status)){DestroyInterrupts(g);GpuUnbind(&g->Hardware);return status;}
        IrqContext(g->Interrupts[irq])->Index=irq;irq++;
    }
    g->Hardware.Stage=1;
    return STATUS_SUCCESS;
}
static VOID DestroyInterrupts(GPU_DEVICE *g) {
    for(ULONG i=0;i<g->Irqs;i++)if(g->Interrupts[i]){WdfObjectDelete(g->Interrupts[i]);g->Interrupts[i]=NULL;}
}
static VOID Synchronize(PVOID context) {
    GPU_DEVICE *g=context;
    for(ULONG i=0;i<g->Irqs;i++)if(g->Interrupts[i]){WdfInterruptAcquireLock(g->Interrupts[i]);WdfInterruptReleaseLock(g->Interrupts[i]);}
}
static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);GPU_DEVICE *g=GpuContext(device);
    GpuStop(&g->Hardware);DestroyInterrupts(g);GpuUnbind(&g->Hardware);return STATUS_SUCCESS;
}
static NTSTATUS Enter(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);GPU_DEVICE *g=GpuContext(device);g->Hardware.Fault=0;g->Hardware.Stage=1;return STATUS_SUCCESS;
}
static NTSTATUS Exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);GpuStop(&GpuContext(device)->Hardware);return STATUS_SUCCESS;
}
static BOOLEAN Interrupt(WDFINTERRUPT interrupt,ULONG message) {
    UNREFERENCED_PARAMETER(message);
    GPU_DEVICE *g=GpuContext(WdfInterruptGetDevice(interrupt));
    if(!GpuInterrupt(&g->Hardware,IrqContext(interrupt)->Index))return FALSE;
    WdfInterruptQueueDpcForIsr(interrupt);return TRUE;
}
static VOID InterruptDpc(WDFINTERRUPT interrupt,WDFOBJECT device) {
    UNREFERENCED_PARAMETER(device);
    GpuDpc(&GpuContext(WdfInterruptGetDevice(interrupt))->Hardware);
}
static VOID Create(WDFDEVICE device,WDFREQUEST request,WDFFILEOBJECT file) {
    UNREFERENCED_PARAMETER(file);
    GPU_DEVICE *g=GpuContext(device);
    g->Owner=IoGetRequestorProcess(WdfRequestWdmGetIrp(request));
    if(g->Owner)ObReferenceObject(g->Owner);
    WdfRequestComplete(request,g->Owner?STATUS_SUCCESS:STATUS_INVALID_PARAMETER);
}
static VOID Cleanup(WDFFILEOBJECT file) {
    GPU_DEVICE *g=GpuContext(WdfFileObjectGetDevice(file));
    if(!g->Owner)return;
    KeWaitForSingleObject(&g->Lock,Executive,KernelMode,FALSE,NULL);
    if(!NT_SUCCESS(GpuWait(&g->Hardware,g->Hardware.Submitted,10000000)))GpuStop(&g->Hardware);
    KAPC_STATE state;BOOLEAN attach=PsGetCurrentProcess()!=g->Owner;
    if(attach)KeStackAttachProcess(g->Owner,&state);
    for(ULONG i=0;i<RTL_NUMBER_OF(g->Buffers);i++) {
        GPU_BO *bo=g->Buffers+i;
        if(g->Hardware.Online)GpuUnmap(&g->Hardware,0,bo);
        if(g->Maps[i])MmUnmapLockedPages(g->Maps[i],bo->Mdl);
        g->Maps[i]=NULL;
        GpuFree(bo);
    }
    if(attach)KeUnstackDetachProcess(&state);
    ObDereferenceObject(g->Owner);g->Owner=NULL;
    KeReleaseMutex(&g->Lock,FALSE);
}
static ULONGLONG Address(GPU_DEVICE *g,ULONG size) {
    ULONGLONG address=GPU_CLIENT_VA_BASE;
    for(ULONG i=0;i<RTL_NUMBER_OF(g->Buffers);i++) {
        GPU_BO *b=g->Buffers+i;
        if(b->Address && address<b->Address+b->Size && address+size>b->Address) {
            address=b->Address+b->Size;i=MAXULONG;
        }
    }
    return address+size<=GPU_CLIENT_VA_END?address:0;
}
static NTSTATUS Submit(GPU_DEVICE *g,const GPU_SUBMIT *entries,ULONG count) {
    GPU_COMMAND commands[GPU_MAX_COMMANDS];
    if(!count || count>RTL_NUMBER_OF(commands))return STATUS_INVALID_PARAMETER;
    for(ULONG i=0;i<count;i++) {
        const GPU_SUBMIT *s=entries+i;
        if(!s->Handle || s->Handle>RTL_NUMBER_OF(g->Buffers) || s->Reserved)return STATUS_INVALID_PARAMETER;
        commands[i]=(GPU_COMMAND){g->Buffers+s->Handle-1,s->Offset,s->Words};
    }
    return GpuSubmitBatch(&g->Hardware,commands,count);
}
static VOID Io(WDFQUEUE queue,WDFREQUEST request,size_t outLength,size_t inLength,ULONG code) {
    GPU_DEVICE *g=GpuContext(WdfIoQueueGetDevice(queue));
    PVOID in=NULL,out=NULL;ULONG_PTR bytes=0;NTSTATUS status=STATUS_INVALID_PARAMETER;
    if(inLength && !NT_SUCCESS(WdfRequestRetrieveInputBuffer(request,inLength,&in,NULL)))goto done;
    if(outLength && !NT_SUCCESS(WdfRequestRetrieveOutputBuffer(request,outLength,&out,NULL)))goto done;
    KeWaitForSingleObject(&g->Lock,Executive,KernelMode,FALSE,NULL);
    if(code==GPU_IOCTL_ALLOC && !g->Hardware.Online) {
        if(inLength!=sizeof(ULONG) || outLength<sizeof(GPU_ALLOCATION) || !*(PULONG)in || *(PULONG)in>GPU_MAX_BUFFER_SIZE)goto unlock;
        status=GpuStart(&g->Hardware);if(!NT_SUCCESS(status))goto unlock;
    }
    if(code==GPU_IOCTL_QUERY && outLength>=sizeof(GPU_INFO)) {
        if(g->Hardware.Online){g->Hardware.Snapshot[2]=Gr(&g->Hardware,REG_A6XX_CP_RB_RPTR);g->Hardware.Snapshot[3]=g->Hardware.WriteIndex&8191;}
        GPU_INFO info={2,0x44050001,g->Hardware.Stage,g->Hardware.Fault,g->Hardware.Submitted,0,g->Hardware.GmuVersion,g->Irqs};
        info.Step=g->Hardware.Step;RtlCopyMemory(info.Snapshot,g->Hardware.Snapshot,sizeof(info.Snapshot));
        if(g->Hardware.Memory.Cpu)info.Completed=*(volatile ULONGLONG *)g->Hardware.Memory.Cpu;
        RtlCopyMemory(out,&info,sizeof(info));bytes=sizeof(info);status=STATUS_SUCCESS;
    } else if(!g->Hardware.Online)status=STATUS_DEVICE_NOT_READY;
    else if(code==GPU_IOCTL_ALLOC && inLength==sizeof(ULONG) && outLength>=sizeof(GPU_ALLOCATION)) {
        ULONG size=*(PULONG)in;
        status=STATUS_INSUFFICIENT_RESOURCES;
        for(ULONG i=0;i<RTL_NUMBER_OF(g->Buffers);i++)if(!g->Buffers[i].Mdl) {
            GPU_BO *bo=g->Buffers+i;
            status=GpuAllocate(bo,size);
            if(!NT_SUCCESS(status))break;
            ULONGLONG address=Address(g,bo->Size);
            if(!address){GpuFree(bo);status=STATUS_INSUFFICIENT_RESOURCES;break;}
            status=GpuMap(&g->Hardware,0,bo,address);
            if(!NT_SUCCESS(status)){GpuStop(&g->Hardware);GpuFree(bo);break;}
            KAPC_STATE state;BOOLEAN attach=PsGetCurrentProcess()!=g->Owner;
            if(attach)KeStackAttachProcess(g->Owner,&state);
            __try { g->Maps[i]=MmMapLockedPagesSpecifyCache(bo->Mdl,UserMode,MmWriteCombined,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute); }
            __except(EXCEPTION_EXECUTE_HANDLER) { status=GetExceptionCode(); }
            if(attach)KeUnstackDetachProcess(&state);
            if(!g->Maps[i]){GpuUnmap(&g->Hardware,0,bo);GpuFree(bo);if(NT_SUCCESS(status))status=STATUS_INSUFFICIENT_RESOURCES;break;}
            GPU_ALLOCATION allocation={i+1,bo->Size,bo->Address,(ULONGLONG)g->Maps[i]};
            RtlCopyMemory(out,&allocation,sizeof(allocation));bytes=sizeof(allocation);break;
        }
    } else if(code==GPU_IOCTL_FREE && inLength==sizeof(ULONG)) {
        ULONG id=*(PULONG)in;
        if(id && id<=RTL_NUMBER_OF(g->Buffers)) {
            GPU_BO *bo=g->Buffers+id-1;
            status=GpuWait(&g->Hardware,g->Hardware.Submitted,10000000);
            if(NT_SUCCESS(status) && bo->Mdl) {
                KAPC_STATE state;BOOLEAN attach=PsGetCurrentProcess()!=g->Owner;
                if(attach)KeStackAttachProcess(g->Owner,&state);
                MmUnmapLockedPages(g->Maps[id-1],bo->Mdl);g->Maps[id-1]=NULL;
                if(attach)KeUnstackDetachProcess(&state);
                GpuUnmap(&g->Hardware,0,bo);
                GpuFree(bo);
            }
        }
    } else if(code==GPU_IOCTL_SUBMIT && inLength==sizeof(GPU_SUBMIT) && outLength>=sizeof(ULONGLONG)) {
        GPU_SUBMIT s=*(GPU_SUBMIT *)in;
        if(s.Handle && s.Handle<=RTL_NUMBER_OF(g->Buffers) && !s.Reserved) {
            status=GpuSubmit(&g->Hardware,g->Buffers+s.Handle-1,s.Offset,s.Words);
            if(NT_SUCCESS(status)){*(PULONGLONG)out=g->Hardware.Submitted;bytes=sizeof(ULONGLONG);}
        }
    } else if(code==GPU_IOCTL_SUBMIT_BATCH && inLength && !(inLength%sizeof(GPU_SUBMIT)) && inLength/sizeof(GPU_SUBMIT)<=256 && outLength>=sizeof(ULONGLONG)) {
        status=Submit(g,(GPU_SUBMIT *)in,(ULONG)(inLength/sizeof(GPU_SUBMIT)));
        if(NT_SUCCESS(status)){*(PULONGLONG)out=g->Hardware.Submitted;bytes=sizeof(ULONGLONG);}
    } else if(code==GPU_IOCTL_WAIT && inLength==sizeof(GPU_WAIT)) {
        GPU_WAIT w=*(GPU_WAIT *)in;status=GpuWait(&g->Hardware,w.Fence,w.Timeout);
    }
unlock:
    KeReleaseMutex(&g->Lock,FALSE);
done:
    WdfRequestCompleteWithInformation(request,status,bytes);
}
static NTSTATUS Add(WDFDRIVER driver,PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WdfDeviceInitSetExclusive(init,TRUE);
    WdfDeviceInitSetCharacteristics(init,FILE_AUTOGENERATED_DEVICE_NAME,FALSE);
    DECLARE_CONST_UNICODE_STRING(sddl,L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    NTSTATUS status=WdfDeviceInitAssignSDDLString(init,&sddl);if(!NT_SUCCESS(status))return status;
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware=Prepare;callbacks.EvtDeviceReleaseHardware=Release;
    callbacks.EvtDeviceD0Entry=Enter;callbacks.EvtDeviceD0Exit=Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init,&callbacks);
    WDF_FILEOBJECT_CONFIG files;WDF_FILEOBJECT_CONFIG_INIT(&files,Create,WDF_NO_EVENT_CALLBACK,Cleanup);
    WdfDeviceInitSetFileObjectConfig(init,&files,WDF_NO_OBJECT_ATTRIBUTES);
    WDF_OBJECT_ATTRIBUTES attributes;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,GPU_DEVICE);
    attributes.ExecutionLevel=WdfExecutionLevelPassive;
    WDFDEVICE device;status=WdfDeviceCreate(&init,&attributes,&device);if(!NT_SUCCESS(status))return status;
    GPU_DEVICE *g=GpuContext(device);GpuInitialize(&g->Hardware,Synchronize,g);KeInitializeMutex(&g->Lock,0);
    WDF_IO_QUEUE_CONFIG config;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config,WdfIoQueueDispatchSequential);
    config.EvtIoDeviceControl=Io;
    status=WdfIoQueueCreate(device,&config,WDF_NO_OBJECT_ATTRIBUTES,NULL);
    if(NT_SUCCESS(status))status=WdfDeviceCreateDeviceInterface(device,&GpuInterface,NULL);
    return status;
}
DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,Add);
    return WdfDriverCreate(object,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);
}
