#include "cpupower.h"
SM_CONTEXT Sm;

VOID SmWake(VOID) {
    if(!Sm.Stopping && !Sm.Fault && Sm.RegisteredMask==255 && KeGetCurrentIrql()<=DISPATCH_LEVEL)
        KeSetEvent(&Sm.Wake,0,FALSE);
}
VOID SmRequestLevel(ULONG domain,ULONG ceiling) {
    InterlockedExchange64(&Sm.LastRequest[domain],(LONG64)KeQueryInterruptTime());
    InterlockedExchange(&Sm.Ceiling[domain],(LONG)SmClampLevel(ceiling));SmWake();
}

static NTSTATUS ReadLoad(ULONG *sampled) {
    SM_CPU_TIME times[8];ULONG bytes=0;
    NTSTATUS status=ZwQuerySystemInformation(8,times,sizeof(times),&bytes);
    if(!NT_SUCCESS(status))return status;
    if(bytes!=sizeof(times))return STATUS_INFO_LENGTH_MISMATCH;
    ULONG busyDomain[2]={0};*sampled=0;
    for(ULONG c=0;c<8;c++) {
        SM_CPU_TIME *old=&Sm.Previous[c],*now=&times[c];
        ULONGLONG total=now->Kernel+now->User-old->Kernel-old->User,idle=now->Idle-old->Idle;
        ULONG busy=total?(ULONG)((total-min(idle,total))*100/total):0,domain=SmDomain(c);
        if(total)*sampled|=1u<<domain;
        busyDomain[domain]=max(busyDomain[domain],busy);*old=*now;
    }
    for(ULONG d=0;d<2;d++)if(*sampled&(1u<<d))Sm.Busy[d]=busyDomain[d];
    Sm.LastSample=KeQueryInterruptTime();return STATUS_SUCCESS;
}

static VOID Worker(PVOID context) {
    LARGE_INTEGER timeout;PLARGE_INTEGER wait=NULL;ULONG down[2]={0};
    ULONGLONG raised[2]={0};
    UNREFERENCED_PARAMETER(context);
    for(;;) {
        KeWaitForSingleObject(&Sm.Wake,Executive,KernelMode,FALSE,wait);wait=NULL;
        if(Sm.Stopping)break;
        if(Sm.RegisteredMask!=255 || Sm.Fault)continue;
        ULONGLONG elapsed=KeQueryInterruptTime()-Sm.LastSample;
        if(elapsed<DVFS_SAMPLE_100NS){timeout.QuadPart=-(LONGLONG)(DVFS_SAMPLE_100NS-elapsed);wait=&timeout;continue;}
        ULONG sampled=0;NTSTATUS status=ReadLoad(&sampled);
        if(!NT_SUCCESS(status))InterlockedExchange(&Sm.Fault,1);
        SmThermalUpdate();
        for(ULONG d=0;d<2;d++) {
            ULONG current=Sm.Applied[d]<0?0:(ULONG)Sm.Applied[d];
            ULONG ceiling=SmEffectiveCeiling(d);
            if(!(sampled&(1u<<d)) && !Sm.Fault && current<=ceiling)continue;
            ULONG sweet=min(ceiling,SmSustainableLevel(d,(ULONG)Sm.SweetBand[d]));
            ULONGLONG lastRequest=(ULONGLONG)InterlockedCompareExchange64(&Sm.LastRequest[d],0,0),now=KeQueryInterruptTime();
            if(Sm.Busy[d]>=63)raised[d]=now;
            BOOLEAN hold=DvfsHold(now,raised[d],Sm.Busy[d]>2,lastRequest);
            ULONG target=Sm.Fault?0:SmDemand(d,current,Sm.Busy[d],ceiling,sweet,hold);
            target=DvfsDelayDown(target,current,Sm.Busy[d],ceiling,&down[d]);
            if((LONG)target==Sm.Applied[d])continue;
            if(!NT_SUCCESS(SmScmiSet(d,target)))InterlockedExchange(&Sm.Fault,1);
            else if(target>current)raised[d]=now;
        }
        if(!Sm.Fault) {
            timeout.QuadPart=(Sm.Busy[0]>2 || Sm.Busy[1]>2 || Sm.Applied[0]>0 || Sm.Applied[1]>0 || Sm.CoolingBand[0] || Sm.CoolingBand[1] || Sm.SweetBand[0] || Sm.SweetBand[1])?-(LONGLONG)DVFS_SAMPLE_100NS:-1000000;
            wait=&timeout;
        }
    }
    for(ULONG d=0;d<2;d++)if(Sm.Applied[d]!=0)SmScmiSet(d,0);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS Finish(PIRP irp,NTSTATUS status) {
    irp->IoStatus.Status=status;irp->IoStatus.Information=0;IoCompleteRequest(irp,0);return status;
}
static NTSTATUS Forward(PDEVICE_OBJECT device,PIRP irp) {
    PDEVICE_OBJECT lower=*(PDEVICE_OBJECT *)device->DeviceExtension;
    if(!lower)return Finish(irp,STATUS_INVALID_DEVICE_REQUEST);
    BOOLEAN power=IoGetCurrentIrpStackLocation(irp)->MajorFunction==IRP_MJ_POWER;
    IoSkipCurrentIrpStackLocation(irp);
    return power?PoCallDriver(lower,irp):IoCallDriver(lower,irp);
}
static NTSTATUS Pnp(PDEVICE_OBJECT device,PIRP irp) {
    switch(IoGetCurrentIrpStackLocation(irp)->MinorFunction) {
    case IRP_MN_QUERY_STOP_DEVICE:case IRP_MN_QUERY_REMOVE_DEVICE:
        return Finish(irp,STATUS_DEVICE_BUSY);
    case IRP_MN_START_DEVICE:case IRP_MN_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:irp->IoStatus.Status=STATUS_SUCCESS;break;
    case IRP_MN_REMOVE_DEVICE: {
        PDEVICE_OBJECT lower=*(PDEVICE_OBJECT *)device->DeviceExtension;
        irp->IoStatus.Status=STATUS_SUCCESS;NTSTATUS status=Forward(device,irp);
        IoDetachDevice(lower);IoDeleteDevice(device);return status;
    }
    }
    return Forward(device,irp);
}
static NTSTATUS AddDevice(PDRIVER_OBJECT driver,PDEVICE_OBJECT physical) {
    PDEVICE_OBJECT device;NTSTATUS status=IoCreateDevice(driver,sizeof(PDEVICE_OBJECT),NULL,FILE_DEVICE_UNKNOWN,0,FALSE,&device);
    if(!NT_SUCCESS(status))return status;
    PDEVICE_OBJECT lower=IoAttachDeviceToDeviceStack(device,physical);
    if(!lower){IoDeleteDevice(device);return STATUS_NO_SUCH_DEVICE;}
    *(PDEVICE_OBJECT *)device->DeviceExtension=lower;
    device->Flags|=DO_POWER_PAGABLE;device->Flags&=~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
static NTSTATUS Shutdown(PDEVICE_OBJECT device,PIRP irp) {
    UNREFERENCED_PARAMETER(device);InterlockedExchange(&Sm.Stopping,1);KeSetEvent(&Sm.Wake,0,FALSE);
    if(Sm.Thread)KeWaitForSingleObject(Sm.Thread,Executive,KernelMode,FALSE,NULL);
    return Finish(irp,STATUS_SUCCESS);
}
static VOID Unload(PDRIVER_OBJECT driver) {
    InterlockedExchange(&Sm.Stopping,1);KeSetEvent(&Sm.Wake,0,FALSE);
    if(Sm.Thread){KeWaitForSingleObject(Sm.Thread,Executive,KernelMode,FALSE,NULL);ObDereferenceObject(Sm.Thread);}
    if(Sm.ShutdownRegistered)IoUnregisterShutdownNotification(driver->DeviceObject);
    if(driver->DeviceObject)IoDeleteDevice(driver->DeviceObject);
    SmScmiClose();
    SmThermalClose();
}
DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path) {
    UNREFERENCED_PARAMETER(path);ULONG midr=(ULONG)_ReadStatusReg(ARM64_SYSREG(3,0,0,0,0));
    if((midr!=0x513f0014 && midr!=0x514f0014) || KeQueryActiveProcessorCountEx(0)!=8)return STATUS_NOT_SUPPORTED;
    RtlZeroMemory(&Sm,sizeof(Sm));KeInitializeEvent(&Sm.Wake,SynchronizationEvent,FALSE);
    for(ULONG c=0;c<8;c++)Sm.Core[c].Number=c;
    for(ULONG i=0;i<=IRP_MJ_MAXIMUM_FUNCTION;i++)driver->MajorFunction[i]=Forward;
    driver->DriverExtension->AddDevice=AddDevice;
    driver->MajorFunction[IRP_MJ_PNP]=Pnp;
    driver->MajorFunction[IRP_MJ_SHUTDOWN]=Shutdown;
    SmThermalInitialize();
    Sm.Retention=SmPsci(0x8400000Au,SM_CPU_SUSPEND)>=0;
    NTSTATUS status=SmScmiInitialize();if(!NT_SUCCESS(status))goto fail;
    SmThermalUpdate();
    PDEVICE_OBJECT device;HANDLE thread;
    status=IoCreateDevice(driver,sizeof(PDEVICE_OBJECT),NULL,FILE_DEVICE_UNKNOWN,0,FALSE,&device);
    if(!NT_SUCCESS(status))goto fail;
    status=IoRegisterShutdownNotification(device);if(!NT_SUCCESS(status))goto fail;Sm.ShutdownRegistered=TRUE;
    status=PsCreateSystemThread(&thread,THREAD_ALL_ACCESS,NULL,NULL,NULL,Worker,NULL);if(!NT_SUCCESS(status))goto fail;
    status=ObReferenceObjectByHandle(thread,SYNCHRONIZE,*PsThreadType,KernelMode,(PVOID*)&Sm.Thread,NULL);
    if(!NT_SUCCESS(status)){InterlockedExchange(&Sm.Stopping,1);KeSetEvent(&Sm.Wake,0,FALSE);ZwWaitForSingleObject(thread,FALSE,NULL);ZwClose(thread);goto fail;}ZwClose(thread);
    PEP_INFORMATION info={0};PEP_KERNEL_INFORMATION kernel={0};
    info.Version=PEP_INFORMATION_VERSION;info.Size=sizeof(info);info.AcceptDeviceNotification=SmDeviceNotification;info.AcceptProcessorNotification=SmProcessorNotification;
    kernel.Version=PEP_KERNEL_INFORMATION_VERSION;kernel.Size=sizeof(kernel);
    status=PoFxRegisterPlugin(&info,&kernel);if(!NT_SUCCESS(status))goto fail;
    /* The OS retains PEP callbacks: a successfully registered PEP cannot unload. */
    device->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
fail:
    Unload(driver);return status;
}
