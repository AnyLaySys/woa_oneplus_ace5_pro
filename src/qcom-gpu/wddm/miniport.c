#include "adapter.h"
#include "../regs.h"

static BOOLEAN Barrier(PVOID context){UNREFERENCED_PARAMETER(context);return TRUE;}
static VOID Synchronize(PVOID context) {
    A830_ADAPTER *a=context;BOOLEAN result;
    NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,Barrier,a,0,&result);
    if(!NT_SUCCESS(status))InterlockedExchange(&a->Failed,status);
}
static VOID DisplayWake(PKDPC dpc,PVOID context,PVOID first,PVOID second) {
    UNREFERENCED_PARAMETER(dpc);UNREFERENCED_PARAMETER(first);UNREFERENCED_PARAMETER(second);
    A830_ADAPTER *a=context;KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);
}
static VOID RegisterHardwareInfo(PDEVICE_OBJECT device) {
    HANDLE key;
    if(!NT_SUCCESS(IoOpenDeviceRegistryKey(device,PLUGPLAY_REGKEY_DRIVER,KEY_SET_VALUE,&key)))return;
    static const struct {PCWSTR Name,Value;} strings[]={
        {L"HardwareInformation.AdapterString",L"Qualcomm Adreno 830"},
        {L"HardwareInformation.ChipType",L"Qualcomm Adreno 830"},
        {L"HardwareInformation.DacType",L"Internal"},
        {L"HardwareInformation.BiosString",L"UEFI"}
    };
    UNICODE_STRING name,value;
    for(ULONG i=0;i<RTL_NUMBER_OF(strings);i++) {
        RtlInitUnicodeString(&name,strings[i].Name);RtlInitUnicodeString(&value,strings[i].Value);
        ZwSetValueKey(key,&name,0,REG_SZ,value.Buffer,value.Length+sizeof(WCHAR));
    }
    ULONG bytes=A830_SEGMENT_BYTES;ULONGLONG wideBytes=bytes;
    RtlInitUnicodeString(&name,L"HardwareInformation.MemorySize");
    ZwSetValueKey(key,&name,0,REG_DWORD,&bytes,sizeof(bytes));
    RtlInitUnicodeString(&name,L"HardwareInformation.qwMemorySize");
    ZwSetValueKey(key,&name,0,REG_QWORD,&wideBytes,sizeof(wideBytes));
    ZwClose(key);
}
static NTSTATUS Add(PDEVICE_OBJECT device,PVOID *context) {
    A830_ADAPTER *a=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*a),GPU_TAG);
    if(!a)return STATUS_INSUFFICIENT_RESOURCES;
    RegisterHardwareInfo(device);

    GpuInitialize(&a->Hardware,Synchronize,a);KeInitializeSpinLock(&a->QueueLock);KeInitializeMutex(&a->DisplayPowerLock,0);
    KeInitializeEvent(&a->Wake,SynchronizationEvent,FALSE);InitializeListHead(&a->Pending);InitializeListHead(&a->Free);InitializeListHead(&a->Inflight);InitializeListHead(&a->Contexts);KeInitializeSpinLock(&a->ContextLock);
    KeInitializeDpc(&a->DisplayWake,DisplayWake,a);
    ExInitializeRundownProtection(&a->ScanoutAccess);
    for(ULONG i=0;i<A830_QUEUE_DEPTH;i++)InsertTailList(&a->Free,&a->Jobs[i].Entry);
    *context=a;return STATUS_SUCCESS;
}
static VOID StopWorker(A830_ADAPTER *a) {
    InterlockedIncrement(&a->Generation);InterlockedExchange(&a->Failed,STATUS_DEVICE_NOT_READY);
    InterlockedExchange(&a->Stopping,1);
    KIRQL old;KeAcquireSpinLock(&a->ContextLock,&old);
    for(PLIST_ENTRY e=a->Contexts.Flink;e!=&a->Contexts;e=e->Flink) {
        A830_CONTEXT *c=CONTAINING_RECORD(e,A830_CONTEXT,Entry);A830_COMPLETION *page=(PVOID)c->CompletionBuffer.Cpu;
        InterlockedExchange((volatile LONG *)&page->Status,STATUS_DEVICE_REMOVED);InterlockedIncrement((volatile LONG *)&page->Generation);
        KeSetEvent(c->Completion,IO_NO_INCREMENT,FALSE);
    }
    KeReleaseSpinLock(&a->ContextLock,old);KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);
    if(a->Thread){ZwWaitForSingleObject(a->Thread,FALSE,NULL);ZwClose(a->Thread);a->Thread=NULL;}
    /* Hardware may still read pinned command BOs. Quiesce before dropping references. */
    {
        if(a->BridgeAttached)A830BridgeStopHardware(a);
        else if(a->Hardware.Online)GpuStop(&a->Hardware);
    }
    A830Retire(a);a->IrqRead=a->IrqWrite;
    PLIST_ENTRY entry;
    while((entry=ExInterlockedRemoveHeadList(&a->Pending,&a->QueueLock))) {
        A830FinishJob(CONTAINING_RECORD(entry,A830_JOB,Entry),TRUE);
        A830RecycleJob(a,CONTAINING_RECORD(entry,A830_JOB,Entry));
    }
}
static NTSTATUS StartWorker(A830_ADAPTER *a) {
    OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,NULL,OBJ_KERNEL_HANDLE,NULL,NULL);
    InterlockedExchange(&a->Stopping,0);
    return PsCreateSystemThread(&a->Thread,SYNCHRONIZE,&attributes,NULL,NULL,A830Worker,a);
}
NTSTATUS A830AdapterStop(PVOID context) {
    A830_ADAPTER *a=context;

    StopWorker(a);
    NTSTATUS display=A830DpuStop(a);
    if(!NT_SUCCESS(display))return display;
    NTSTATUS copied=QcomGpuScanoutStop(a);if(!NT_SUCCESS(copied))return copied;
    A830BridgeDetach(a);
    GpuStop(&a->Hardware);A830MemoryStop(a);GpuUnbind(&a->Hardware);A830DisplayStop(a);return STATUS_SUCCESS;
}
static NTSTATUS Start(PVOID context,PDXGK_START_INFO start,PDXGKRNL_INTERFACE dxgk,PULONG sources,PULONG children) {
    UNREFERENCED_PARAMETER(start);
    A830_ADAPTER *a=context;a->Dxgk=*dxgk;*sources=0;*children=0;

    NTSTATUS displayStatus=A830DisplayStart(a);

    if(!NT_SUCCESS(displayStatus))return displayStatus;
    *sources=1;*children=1;
    DXGK_DEVICE_INFO info={0};NTSTATUS status=dxgk->DxgkCbGetDeviceInformation(dxgk->DeviceHandle,&info);
    if(!NT_SUCCESS(status)){A830DisplayStop(a);return status;}
    PHYSICAL_ADDRESS registers={0};ULONG length=0;
    for(ULONG list=0;list<info.TranslatedResourceList->Count;list++) {
        PCM_PARTIAL_RESOURCE_LIST resources=&info.TranslatedResourceList->List[list].PartialResourceList;
        for(ULONG i=0;i<resources->Count;i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR r=resources->PartialDescriptors+i;
            if(r->Type==CmResourceTypeMemory && r->u.Memory.Start.QuadPart==0x03D00000){registers=r->u.Memory.Start;length=r->u.Memory.Length;}
        }
    }

    a->GpuRegisters=registers;a->GpuRegisterSize=length;

    status=GpuBind(&a->Hardware,registers,length);

    if(NT_SUCCESS(status))status=GpuStart(&a->Hardware);

    if(NT_SUCCESS(status))status=A830MemoryStart(a);
    if(NT_SUCCESS(status)) {
        /* Map the two independent fallback frames before DPU can take over.
           Allocation/CreateResource retains an ordinary-BO escape path. */
        QcomGpuScanoutStart(a);
        A830DpuInitialize(a);
    }
    if(NT_SUCCESS(status))status=StartWorker(a);

    if(NT_SUCCESS(status))A830BridgeAttach(a);
    if(!NT_SUCCESS(status))A830AdapterStop(a);

    return status;
}
static NTSTATUS Remove(PVOID context){
    A830_ADAPTER *a=context;KeRemoveQueueDpc(&a->DisplayWake);KeFlushQueuedDpcs();
    A830BridgeRemove();
    ExFreePoolWithTag(a,GPU_TAG);return STATUS_SUCCESS;
}
static BOOLEAN Interrupt(PVOID context,ULONG message) {
    UNREFERENCED_PARAMETER(message);A830_ADAPTER *a=context;BOOLEAN handled=FALSE;

    for(ULONG i=0;i<4+RTL_NUMBER_OF(a->Hardware.Vm);i++)if(GpuInterrupt(&a->Hardware,i))handled=TRUE;
    if(handled && a->Hardware.Fault && !InterlockedCompareExchange(&a->Failed,(LONG)a->Hardware.Fault,0)) {
        KeMemoryBarrier();
        ULONG fence=a->IrqRead!=a->IrqWrite?a->IrqRecords[a->IrqRead%RTL_NUMBER_OF(a->IrqRecords)].Fence:a->LastSubmitted;
        if(fence)InterlockedExchange(&a->FaultFence,(LONG)fence);
    }
    if(handled)A830InterruptComplete(a);
    if(handled)a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);return handled;
}
static VOID Dpc(PVOID context) {
    A830_ADAPTER *a=context;GpuDpc(&a->Hardware);
    KIRQL old;KeAcquireSpinLock(&a->ContextLock,&old);
    for(PLIST_ENTRY e=a->Contexts.Flink;e!=&a->Contexts;e=e->Flink) {
        A830_CONTEXT *c=CONTAINING_RECORD(e,A830_CONTEXT,Entry);
        A830_COMPLETION *page=(PVOID)c->CompletionBuffer.Cpu;
        LONG failed=InterlockedCompareExchange(&a->Failed,0,0);
        if(failed && !InterlockedCompareExchange((volatile LONG *)&page->Status,failed,0))
            InterlockedIncrement((volatile LONG *)&page->Generation);
        ULONGLONG completed=page->Completed;
        if(c->Completion && (completed!=c->NotifiedSequence || page->Status)) {
            c->NotifiedSequence=completed;KeSetEvent(c->Completion,IO_NO_INCREMENT,FALSE);
        }
    }
    KeReleaseSpinLock(&a->ContextLock,old);
    KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);
    if(InterlockedExchange(&a->SchedulerDpcPending,0))a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle);
}
static NTSTATUS Dispatch(PVOID context,ULONG source,PVIDEO_REQUEST_PACKET packet) {
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(source);packet->StatusBlock->Status=1;return STATUS_NOT_SUPPORTED;
}
static NTSTATUS Children(PVOID context,PDXGK_CHILD_DESCRIPTOR descriptors,ULONG bytes) {
    UNREFERENCED_PARAMETER(context);
    if(bytes<2*sizeof(*descriptors))return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(descriptors,bytes);descriptors[0].ChildDeviceType=TypeVideoOutput;
    descriptors[0].ChildCapabilities.HpdAwareness=HpdAwarenessAlwaysConnected;
    descriptors[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology=D3DKMDT_VOT_INTERNAL;
    return STATUS_SUCCESS;
}
static NTSTATUS ChildStatus(PVOID context,PDXGK_CHILD_STATUS status,BOOLEAN nonDestructive) {
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(nonDestructive);
    if(status->ChildUid)return STATUS_INVALID_PARAMETER;
    if(status->Type!=StatusConnection)return STATUS_NOT_SUPPORTED;
    status->HotPlug.Connected=TRUE;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY MonitorLink(HANDLE adapter,DXGKARG_UPDATEMONITORLINKINFO *p) {
    UNREFERENCED_PARAMETER(adapter);if(p->VideoPresentTargetId)return STATUS_INVALID_PARAMETER;
    p->MonitorLinkInfo.Capabilities.Value=0;p->MonitorLinkInfo.DitheringSupport.Value=0;return STATUS_SUCCESS;
}
static NTSTATUS Descriptor(PVOID context,ULONG child,PDXGK_DEVICE_DESCRIPTOR descriptor) {
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(child);UNREFERENCED_PARAMETER(descriptor);return STATUS_MONITOR_NO_DESCRIPTOR;
}
static NTSTATUS Power(PVOID context,ULONG uid,DEVICE_POWER_STATE state,POWER_ACTION action) {
    UNREFERENCED_PARAMETER(action);
    if(state<PowerDeviceD0 || state>PowerDeviceD3)return STATUS_INVALID_PARAMETER;
    return A830DisplayPower(context,uid,state==PowerDeviceD0);
}
static VOID Unload(VOID){A830BridgeDestroy();}
static NTSTATUS APIENTRY Collect(HANDLE adapter,const DXGKARG_COLLECTDBGINFO *p) {
    UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(p);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY StopCapture(HANDLE adapter,const DXGKARG_STOPCAPTURE *p) {
    UNREFERENCED_PARAMETER(adapter);UNREFERENCED_PARAMETER(p);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY StablePower(HANDLE adapter,const DXGKARG_SETSTABLEPOWERSTATE *p) {
    UNREFERENCED_PARAMETER(p);A830_ADAPTER *a=adapter;

    return a->Hardware.Online?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
static NTSTATUS APIENTRY Calibrate(HANDLE adapter,UINT32 node,UINT32 engine,DXGKARG_CALIBRATEGPUCLOCK *p) {
    A830_ADAPTER *a=adapter;if(node || engine)return STATUS_INVALID_PARAMETER;
    ULONGLONG counter;LARGE_INTEGER before=KeQueryPerformanceCounter(NULL);
    NTSTATUS status=a->BridgeAttached?A830BridgeTimestamp(&counter):GpuTimestamp(&a->Hardware,&counter);
    if(!NT_SUCCESS(status))return status;
    LARGE_INTEGER after=KeQueryPerformanceCounter(NULL);RtlZeroMemory(p,sizeof(*p));
    p->GpuFrequency=19200000;p->GpuClockCounter=counter;
    p->CpuClockCounter=(ULONGLONG)before.QuadPart+((ULONGLONG)(after.QuadPart-before.QuadPart)/2);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Present(HANDLE context,DXGKARG_PRESENT *p) {
    return A830Present(context,p);
}
static NTSTATUS APIENTRY ControlInterrupt(HANDLE adapter,DXGK_INTERRUPT_TYPE type,BOOLEAN enabled) {
    A830_ADAPTER *a=adapter;
    if(type==DXGK_INTERRUPT_CRTC_VSYNC) {
        InterlockedExchange(&a->VsyncEnabled,enabled);A830DpuEnableVsync(a,enabled);A830WakeDisplay(a);
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Cancel(HANDLE adapter,const DXGKARG_CANCELCOMMAND *p) {
    UNREFERENCED_PARAMETER(adapter);
    if(p->DmaBufferPrivateDataSize>=sizeof(A830_DMA)) {
        A830_DMA *dma=p->pDmaBufferPrivateData;
        A830CancelRendered(adapter,dma);
    }
    return STATUS_SUCCESS;
}
static VOID ResetDevice(PVOID context) {
    A830_ADAPTER *a=context;InterlockedExchange(&a->Failed,STATUS_DEVICE_HARDWARE_ERROR);
    /* Bugcheck callbacks cannot wait or delete timers. Restore inherited
       framebuffer addresses using the dedicated MMIO-only DPU hook. */
    A830DpuRestoreRegisters(a);
    if(a->Hardware.Powered && a->Hardware.Stage>=6){Gw(&a->Hardware,REG_A8XX_CP_SQE_CNTL,3);Gw(&a->Hardware,REG_A8XX_RBBM_INT_0_MASK,0);}
}
static NTSTATUS Interface(PVOID context,PQUERY_INTERFACE query) {
    UNREFERENCED_PARAMETER(context);UNREFERENCED_PARAMETER(query);return STATUS_NOT_SUPPORTED;
}
static NTSTATUS APIENTRY CreateDevice(HANDLE adapter,DXGKARG_CREATEDEVICE *p) {
    A830_DEVICE *d=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*d),GPU_TAG);if(!d)return STATUS_INSUFFICIENT_RESOURCES;
    d->Adapter=adapter;p->hDevice=d;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DestroyDevice(HANDLE device){ExFreePoolWithTag(device,GPU_TAG);return STATUS_SUCCESS;}
static NTSTATUS APIENTRY CreateContext(HANDLE device,DXGKARG_CREATECONTEXT *p) {

    A830_DEVICE *d=device;if(p->NodeOrdinal || p->EngineAffinity>1 || p->Flags.VirtualAddressing)return STATUS_NOT_SUPPORTED;
    A830_CONTEXT *c=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*c),GPU_TAG);if(!c)return STATUS_INSUFFICIENT_RESOURCES;
    c->Adapter=d->Adapter;c->Process=PsGetCurrentProcess();ObReferenceObject(c->Process);ExInitializeRundownProtection(&c->Jobs);KeInitializeSpinLock(&c->TokenLock);InitializeListHead(&c->Rendered);
    if(p->PrivateDriverDataSize) {
        if(p->PrivateDriverDataSize!=sizeof(A830_CONTEXT_DATA) || !p->pPrivateDriverData) {

            ObDereferenceObject(c->Process);ExFreePoolWithTag(c,GPU_TAG);return STATUS_INVALID_PARAMETER;
        }
        A830_CONTEXT_DATA data;
        __try {RtlCopyMemory(&data,p->pPrivateDriverData,sizeof(data));}
        __except(EXCEPTION_EXECUTE_HANDLER){ObDereferenceObject(c->Process);ExFreePoolWithTag(c,GPU_TAG);return GetExceptionCode();}
        if(data.Version!=A830_ABI_VERSION || data.Reserved || !data.CompletionEvent){ObDereferenceObject(c->Process);ExFreePoolWithTag(c,GPU_TAG);return STATUS_INVALID_PARAMETER;}
        NTSTATUS status=ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)data.CompletionEvent,EVENT_MODIFY_STATE,*ExEventObjectType,
            UserMode,(PVOID *)&c->Completion,NULL);
        if(!NT_SUCCESS(status)){ObDereferenceObject(c->Process);ExFreePoolWithTag(c,GPU_TAG);return status;}
        status=A830ContextStart(c,&data);
        if(!NT_SUCCESS(status)){ObDereferenceObject(c->Completion);ObDereferenceObject(c->Process);ExFreePoolWithTag(c,GPU_TAG);return status;}
        __try {RtlCopyMemory(p->pPrivateDriverData,&data,sizeof(data));}
        __except(EXCEPTION_EXECUTE_HANDLER){A830ContextStop(c);ObDereferenceObject(c->Completion);ObDereferenceObject(c->Process);ExFreePoolWithTag(c,GPU_TAG);return GetExceptionCode();}

    }
    p->hContext=c;RtlZeroMemory(&p->ContextInfo,sizeof(p->ContextInfo));
    p->ContextInfo.DmaBufferSize=A830_DMA_BYTES;p->ContextInfo.DmaBufferPrivateDataSize=sizeof(A830_DMA);
    p->ContextInfo.AllocationListSize=128;p->ContextInfo.PatchLocationListSize=128;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DestroyContext(HANDLE context){A830_CONTEXT *c=context;A830ContextStop(c);if(c->Completion)ObDereferenceObject(c->Completion);ObDereferenceObject(c->Process);ExFreePoolWithTag(context,GPU_TAG);return STATUS_SUCCESS;}
static NTSTATUS APIENTRY Open(HANDLE device,const DXGKARG_OPENALLOCATION *p) {
    A830_DEVICE *d=device;

    for(ULONG i=0;i<p->NumAllocations;i++) {
        DXGKARGCB_GETHANDLEDATA data={0};data.hObject=p->pOpenAllocation[i].hAllocation;data.Type=DXGK_HANDLE_ALLOCATION;
        PVOID allocation=NULL;
        if(d->Adapter->Dxgk.DxgkCbAcquireHandleData && d->Adapter->Dxgk.DxgkCbReleaseHandleData) {
            DXGKARG_RELEASE_HANDLE release=NULL;
            allocation=d->Adapter->Dxgk.DxgkCbAcquireHandleData(&data,&release);
            if(release){DXGKARGCB_RELEASEHANDLEDATA args={release,DXGK_HANDLE_ALLOCATION};d->Adapter->Dxgk.DxgkCbReleaseHandleData(args);}
        } else allocation=d->Adapter->Dxgk.DxgkCbGetHandleData(&data);
        if(!allocation)return STATUS_INVALID_HANDLE;
        p->pOpenAllocation[i].hDeviceSpecificAllocation=allocation;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Close(HANDLE device,const DXGKARG_CLOSEALLOCATION *p) {
    UNREFERENCED_PARAMETER(device);UNREFERENCED_PARAMETER(p);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Describe(HANDLE adapter,DXGKARG_DESCRIBEALLOCATION *p) {
    A830_ALLOCATION *a=p->hAllocation;
    p->Width=a->Resource.Width;p->Height=a->Resource.Height;p->Format=a->Resource.Format;
    p->MultisampleMethod.NumSamples=1;p->MultisampleMethod.NumQualityLevels=0;
    p->RefreshRate.Numerator=A830RefreshRate(adapter);p->RefreshRate.Denominator=1;
    p->Rotation=a->Resource.Rotation?(D3DDDI_ROTATION)a->Resource.Rotation:D3DDDI_ROTATION_IDENTITY;
    p->PrivateDriverFormatAttribute=0;p->Flags.Value=0;
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Fence(HANDLE adapter,DXGKARG_QUERYCURRENTFENCE *p) {
    A830_ADAPTER *a=adapter;if(p->NodeOrdinal || p->EngineOrdinal)return STATUS_INVALID_PARAMETER;
    p->CurrentFence=a->Completed;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Preempt(HANDLE adapter,const DXGKARG_PREEMPTCOMMAND *p) {
    A830_ADAPTER *a=adapter;if(p->NodeOrdinal || p->EngineOrdinal)return STATUS_INVALID_PARAMETER;
    a->Preemption=p->PreemptionFenceId;InterlockedExchange(&a->PreemptPending,1);KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Metadata(HANDLE adapter,UINT node,DXGKARG_GETNODEMETADATA *p) {
    UNREFERENCED_PARAMETER(adapter);if(node)return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(p,sizeof(*p));p->EngineType=DXGK_ENGINE_TYPE_3D;
    return RtlStringCchCopyW(p->FriendlyName,RTL_NUMBER_OF(p->FriendlyName),L"Qualcomm Adreno 830");
}
static NTSTATUS ResetToFence(A830_ADAPTER *a,PULONG aborted) {

    ULONG boundary=A830FreezeFences(a);StopWorker(a);
    NTSTATUS status;
     if(a->BridgeAttached)status=A830BridgeReset();
    else {GpuStop(&a->Hardware);status=GpuStart(&a->Hardware);}
    if(NT_SUCCESS(status))status=A830Map(a,&a->Memory.Segment,A830_SEGMENT_ADDRESS);
    if(NT_SUCCESS(status))status=A830Map(a,&a->Memory.Commands,A830_COMMAND_ADDRESS);
    if(NT_SUCCESS(status))status=A830SharedRestore(a);
    if(NT_SUCCESS(status))status=QcomGpuScanoutAfterReset(a);
    if(NT_SUCCESS(status)) {
        /* All WDDM work up to this snapshot has now been quiesced/aborted.
           Do not use Hardware.Submitted: scanout copies have private fences. */
        a->Completed=boundary;*aborted=boundary;
    }
    InterlockedExchange(&a->Failed,NT_SUCCESS(status)?0:status);InterlockedExchange(&a->PreemptPending,0);

    return status;
}
static NTSTATUS APIENTRY Reset(HANDLE adapter) {
    ULONG aborted=0;return ResetToFence(adapter,&aborted);
}
static NTSTATUS APIENTRY Restart(HANDLE adapter) {
    A830_ADAPTER *a=adapter;if(a->Failed)return (NTSTATUS)a->Failed;return StartWorker(a);
}
static NTSTATUS APIENTRY EngineGroup(HANDLE adapter,DXGKARG_QUERYDEPENDENTENGINEGROUP *p) {
    UNREFERENCED_PARAMETER(adapter);if(p->NodeOrdinal || p->EngineOrdinal)return STATUS_INVALID_PARAMETER;
    p->DependentNodeOrdinalMask=1;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY EngineStatus(HANDLE adapter,DXGKARG_QUERYENGINESTATUS *p) {
    A830_ADAPTER *a=adapter;if(p->NodeOrdinal || p->EngineOrdinal)return STATUS_INVALID_PARAMETER;
    p->EngineStatus.Value=0;p->EngineStatus.Responsive=!a->Failed;return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY ResetEngine(HANDLE adapter,DXGKARG_RESETENGINE *p) {
    A830_ADAPTER *a=adapter;if(p->NodeOrdinal || p->EngineOrdinal)return STATUS_INVALID_PARAMETER;
    ULONG aborted=0;NTSTATUS status=ResetToFence(a,&aborted);
    if(NT_SUCCESS(status))p->LastAbortedFenceId=aborted;
    return NT_SUCCESS(status)?Restart(a):status;
}
DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING path) {

    DRIVER_INITIALIZATION_DATA d={0};d.Version=DXGKDDI_INTERFACE_VERSION;
    d.DxgkDdiAddDevice=Add;d.DxgkDdiStartDevice=Start;d.DxgkDdiStopDevice=A830AdapterStop;d.DxgkDdiRemoveDevice=Remove;
    d.DxgkDdiInterruptRoutine=Interrupt;d.DxgkDdiDpcRoutine=Dpc;d.DxgkDdiDispatchIoRequest=Dispatch;
    d.DxgkDdiQueryChildRelations=Children;d.DxgkDdiQueryChildStatus=ChildStatus;d.DxgkDdiQueryDeviceDescriptor=Descriptor;
    d.DxgkDdiUpdateMonitorLinkInfo=MonitorLink;
    d.DxgkDdiSetPowerState=Power;d.DxgkDdiResetDevice=ResetDevice;d.DxgkDdiUnload=Unload;d.DxgkDdiQueryInterface=Interface;
    d.DxgkDdiCollectDbgInfo=Collect;
    d.DxgkDdiStopCapture=StopCapture;d.DxgkDdiSetStablePowerState=StablePower;
    d.DxgkDdiCalibrateGpuClock=Calibrate;
    d.DxgkDdiPresent=Present;d.DxgkDdiControlInterrupt=ControlInterrupt;d.DxgkDdiCancelCommand=Cancel;
    d.DxgkDdiQueryAdapterInfo=A830Query;d.DxgkDdiCreateDevice=CreateDevice;d.DxgkDdiDestroyDevice=DestroyDevice;
    d.DxgkDdiCreateAllocation=A830CreateAllocation;d.DxgkDdiDestroyAllocation=A830DestroyAllocation;
    d.DxgkDdiOpenAllocation=Open;d.DxgkDdiCloseAllocation=Close;d.DxgkDdiDescribeAllocation=Describe;
    d.DxgkDdiGetStandardAllocationDriverData=A830StandardAllocation;
    d.DxgkDdiCreateContext=CreateContext;d.DxgkDdiDestroyContext=DestroyContext;
    d.DxgkDdiRender=A830Render;d.DxgkDdiRenderKm=A830Render;d.DxgkDdiPatch=A830Patch;d.DxgkDdiSubmitCommand=A830Submit;
    d.DxgkDdiEscape=A830Escape;
    d.DxgkDdiBuildPagingBuffer=A830Paging;d.DxgkDdiPreemptCommand=Preempt;d.DxgkDdiQueryCurrentFence=Fence;
    d.DxgkDdiGetNodeMetadata=Metadata;d.DxgkDdiResetFromTimeout=Reset;d.DxgkDdiRestartFromTimeout=Restart;
    d.DxgkDdiQueryDependentEngineGroup=EngineGroup;d.DxgkDdiQueryEngineStatus=EngineStatus;d.DxgkDdiResetEngine=ResetEngine;
    A830DisplayDdi(&d);

    NTSTATUS status=DxgkInitialize(object,path,&d);
    if(NT_SUCCESS(status))status=A830BridgeInitialize(object);
    return status;
}
