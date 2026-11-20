typedef struct { WDFDEVICE Parent; } HAPTIC_CONTROL;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HAPTIC_CONTROL,HapticContext);
static NTSTATUS HapticRead(KEYS *k,ULONG module,UCHAR reg,UCHAR *value) {
    return k->HapticObserve[module]?Transfer(k->HapticObserve[module],reg,value,FALSE):STATUS_DEVICE_NOT_READY;
}
static NTSTATUS HapticWrite(KEYS *k,ULONG module,UCHAR reg,UCHAR value) {
    return k->HapticChannel[module]?Transfer(k->HapticChannel[module],reg,&value,TRUE):STATUS_ACCESS_DENIED;
}
static NTSTATUS HapticSet(KEYS *k,ULONG module,UCHAR reg,UCHAR mask,UCHAR value) {
    UCHAR old;NTSTATUS status=HapticRead(k,module,reg,&old);
    return NT_SUCCESS(status)?HapticWrite(k,module,reg,(old&~mask)|(value&mask)):status;
}
static NTSTATUS HapticOff(KEYS *k) {
    NTSTATUS status=HapticWrite(k,0,0x4c,1),off=HapticWrite(k,0,0x46,0);
    k->HapticPhase=0;return NT_SUCCESS(status)?off:status;
}
static VOID HapticPrepare(KEYS *k) {
    k->HapticDue=0;k->HapticPhase=k->HapticRepeat=0;
    for(ULONG i=0;i<2;i++)if(k->HapticChannel[i]&&k->HapticInterrupt[i])Write(k->HapticChannel[i],0x100,0);
    UCHAR cfg=0,ptn=0;
    NTSTATUS status=HapticRead(k,0,1,&cfg);
    if(NT_SUCCESS(status))status=HapticRead(k,1,1,&ptn);
    if(NT_SUCCESS(status)) {
        if(cfg!=5 || ptn!=5)status=STATUS_NOT_SUPPORTED;
        else if(!k->HapticChannel[0] || !k->HapticChannel[1])status=STATUS_ACCESS_DENIED;
    }
    k->HapticStatus=status;
}
static VOID HapticPoll(WDFTIMER timer) {
    KEYS *k=Context(WdfTimerGetParentObject(timer));if(!k->Online)return;
    NTSTATUS status=STATUS_SUCCESS;UCHAR value=0;
    if(k->HapticPhase==1) {
        const UCHAR settings[][4]={
            {0,0x48,0xff,k->HapticVoltage},{0,0x49,0x13,1},{0,0x4b,0x10,0x10},
            {0,0x5c,0x0f,4},{0,0x5d,0xff,0x98},{0,0x63,0x80,0},
            {0,0x60,0xf8,0xd0},{0,0x72,0x0c,4},{0,0x67,0x7f,30},{1,0x26,0xff,0xff}
        };
        status=HapticWrite(k,0,0x4c,1);
        if(NT_SUCCESS(status))status=HapticWrite(k,0,0x46,0);
        for(ULONG i=0;NT_SUCCESS(status) && i<RTL_NUMBER_OF(settings);i++)status=HapticSet(k,settings[i][0],settings[i][1],settings[i][2],settings[i][3]);
        if(NT_SUCCESS(status))status=HapticRead(k,1,0x46,&value);
        if(NT_SUCCESS(status) && (value&0x80)) {
            status=HapticWrite(k,1,0x68,0xff);
            if(NT_SUCCESS(status))status=HapticWrite(k,1,0x69,0xff);
        }
        if(NT_SUCCESS(status))status=HapticWrite(k,0,0x66,0x17);
        KeStallExecutionProcessor(500);
        if(NT_SUCCESS(status))status=HapticWrite(k,0,0x46,0x80);
        if(NT_SUCCESS(status))status=HapticWrite(k,0,0x4c,0x81);
        if(NT_SUCCESS(status)){k->HapticPhase=2;WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(18));return;}
    } else if(k->HapticPhase==2) {
        status=HapticRead(k,0,0x0c,&value);
        if(NT_SUCCESS(status) && (value&7))status=STATUS_IO_DEVICE_ERROR;
        k->HapticStatus=status;status=HapticWrite(k,0,0x4c,1);
        if(NT_SUCCESS(status)){k->HapticPhase=3;k->HapticDue=KeQueryInterruptTime()+300000;WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(1));return;}
    } else if(k->HapticPhase==3) {
        status=HapticRead(k,0,0x0b,&value);
        if(NT_SUCCESS(status) && (value&3) && KeQueryInterruptTime()<k->HapticDue){WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(1));return;}
        if(NT_SUCCESS(status) && (value&3))status=STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(500);
    } else return;
    if(!NT_SUCCESS(status))k->HapticStatus=status;
    status=HapticOff(k);if(!NT_SUCCESS(status))k->HapticStatus=status;
    k->HapticDue=KeQueryInterruptTime()+270000;
    if(k->HapticRepeat && NT_SUCCESS(k->HapticStatus)){k->HapticRepeat--;k->HapticPhase=1;WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(27));}
    else k->HapticRepeat=0;
}
static NTSTATUS HapticPulse(KEYS *k,ULONG voltage,ULONG pulses) {
    if(voltage!=800 && voltage!=1200)return STATUS_INVALID_PARAMETER;
    if(!k->Online || !k->DisplayOn)return STATUS_DEVICE_NOT_READY;
    if(!NT_SUCCESS(k->HapticStatus))return k->HapticStatus;
    if(k->HapticPhase || KeQueryInterruptTime()<k->HapticDue)return STATUS_DEVICE_BUSY;
    k->HapticVoltage=(UCHAR)(voltage/50);k->HapticRepeat=(UCHAR)(pulses-1);k->HapticPhase=1;
    WdfTimerStart(k->HapticTimer,WDF_REL_TIMEOUT_IN_MS(1));return STATUS_SUCCESS;
}
static NTSTATUS HapticDisplay(LPCGUID setting,PVOID value,ULONG bytes,PVOID context) {
    UNREFERENCED_PARAMETER(setting);if(bytes!=sizeof(ULONG))return STATUS_INVALID_PARAMETER;
    WDFDEVICE device=context;KEYS *k=Context(device);
    WdfObjectAcquireLock(device);k->DisplayOn=*(ULONG UNALIGNED*)value!=0;
    if(!k->DisplayOn) {
        WdfTimerStop(k->HapticTimer,FALSE);k->HapticRepeat=0;
        if(k->Online && k->HapticPhase){NTSTATUS status=HapticOff(k);if(!NT_SUCCESS(status))k->HapticStatus=status;}
    }
    WdfObjectReleaseLock(device);return STATUS_SUCCESS;
}
static VOID HapticControl(WDFQUEUE queue,WDFREQUEST request,size_t output,size_t input,ULONG code) {
    WDFDEVICE parent=HapticContext(WdfIoQueueGetDevice(queue))->Parent;KEYS *k=Context(parent);
    NTSTATUS status=code==QH_PULSE?STATUS_INVALID_PARAMETER:STATUS_INVALID_DEVICE_REQUEST;
    WdfObjectAcquireLock(parent);
    if(code==QH_PULSE && input==sizeof(ULONG) && !output) {
        ULONG *voltage;status=WdfRequestRetrieveInputBuffer(request,sizeof(*voltage),(PVOID*)&voltage,NULL);
        if(NT_SUCCESS(status))status=HapticPulse(k,*voltage,1);
    }
    WdfObjectReleaseLock(parent);WdfRequestComplete(request,status);
}
static VOID HapticCleanup(WDFOBJECT object) {
    KEYS *k=Context(object);
    if(k->HapticDevice){WDFQUEUE queue=WdfDeviceGetDefaultQueue(k->HapticDevice);if(queue)WdfIoQueuePurgeSynchronously(queue);WdfObjectDelete(k->HapticDevice);}
}
static NTSTATUS HapticCreate(WDFDEVICE parent) {
    KEYS *k=Context(parent);
    WDF_TIMER_CONFIG t;WDF_TIMER_CONFIG_INIT(&t,HapticPoll);t.UseHighResolutionTimer=TRUE;WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject=parent;a.ExecutionLevel=WdfExecutionLevelPassive;
    NTSTATUS status=WdfTimerCreate(&t,&a,&k->HapticTimer);if(!NT_SUCCESS(status))return status;
    UNICODE_STRING sddl=RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)"),name=RTL_CONSTANT_STRING(L"\\Device\\QcomHaptics"),link=RTL_CONSTANT_STRING(L"\\DosDevices\\QcomHaptics");
    PWDFDEVICE_INIT init=WdfControlDeviceInitAllocate(WdfDeviceGetDriver(parent),&sddl);if(!init)return STATUS_INSUFFICIENT_RESOURCES;
    status=WdfDeviceInitAssignName(init,&name);if(!NT_SUCCESS(status)){WdfDeviceInitFree(init);return status;}
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,HAPTIC_CONTROL);a.ExecutionLevel=WdfExecutionLevelPassive;
    status=WdfDeviceCreate(&init,&a,&k->HapticDevice);if(!NT_SUCCESS(status)){if(init)WdfDeviceInitFree(init);return status;}
    HapticContext(k->HapticDevice)->Parent=parent;
    WDF_IO_QUEUE_CONFIG q;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q,WdfIoQueueDispatchSequential);q.EvtIoDeviceControl=HapticControl;
    status=WdfIoQueueCreate(k->HapticDevice,&q,WDF_NO_OBJECT_ATTRIBUTES,WDF_NO_HANDLE);
    if(NT_SUCCESS(status))status=WdfDeviceCreateSymbolicLink(k->HapticDevice,&link);
    if(NT_SUCCESS(status))WdfControlFinishInitializing(k->HapticDevice);
    return status;
}
