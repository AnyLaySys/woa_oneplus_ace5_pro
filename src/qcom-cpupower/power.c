#include "cpupower.h"
#include <initguid.h>
DEFINE_GUID(GUID_SM_CPUPOWER,0x9f09e67e,0x8c26,0x4fda,0xb2,0x49,0x19,0xb4,0x25,0x88,0x23,0xb0);
SM_CONTEXT Sm;
static UNICODE_STRING Link=RTL_CONSTANT_STRING(L"\\DosDevices\\Sm8750CpuPower");

VOID SmWake(VOID) {
    if(!Sm.Stopping && !Sm.Fault && Sm.RegisteredMask==255 && KeGetCurrentIrql()<=DISPATCH_LEVEL)
        KeSetEvent(&Sm.Wake,0,FALSE);
}
VOID SmRequestLevel(ULONG domain,ULONG ceiling) {
    InterlockedExchange(&Sm.Ceiling[domain],(LONG)SmClampLevel(ceiling));SmWake();
}

static NTSTATUS ReadLoad(VOID) {
    SM_CPU_TIME times[8];ULONG bytes=0;
    NTSTATUS status=ZwQuerySystemInformation(8,times,sizeof(times),&bytes);
    if(!NT_SUCCESS(status))return status;
    if(bytes!=sizeof(times))return STATUS_INFO_LENGTH_MISMATCH;
    Sm.Busy[0]=Sm.Busy[1]=0;
    for(ULONG c=0;c<8;c++) {
        SM_CPU_TIME *old=&Sm.Previous[c],*now=&times[c];
        ULONGLONG total=now->Kernel+now->User-old->Kernel-old->User,idle=now->Idle-old->Idle;
        ULONG busy=total?(ULONG)((total-min(idle,total))*100/total):0,domain=SmDomain(c);
        Sm.Busy[domain]=max(Sm.Busy[domain],busy);*old=*now;
    }
    Sm.LastSample=KeQueryInterruptTime();return STATUS_SUCCESS;
}

static VOID Worker(PVOID context) {
    LARGE_INTEGER timeout;PLARGE_INTEGER wait=NULL;ULONG down[2]={0};
    UNREFERENCED_PARAMETER(context);
    for(;;) {
        KeWaitForSingleObject(&Sm.Wake,Executive,KernelMode,FALSE,wait);wait=NULL;
        if(Sm.Stopping)break;
        if(Sm.RegisteredMask!=255 || Sm.Fault)continue;
        ULONGLONG elapsed=KeQueryInterruptTime()-Sm.LastSample;
        if(elapsed<200000){timeout.QuadPart=-(LONGLONG)(200000-elapsed);wait=&timeout;continue;}
        NTSTATUS status=ReadLoad();
        if(!NT_SUCCESS(status))InterlockedExchange(&Sm.Fault,1);
        for(ULONG d=0;d<2;d++) {
            ULONG current=Sm.Applied[d]<0?0:(ULONG)Sm.Applied[d];
            ULONG ceiling=(ULONG)Sm.Ceiling[d];
            ULONG target=Sm.Fault?0:SmDemand(d,current,Sm.Busy[d],ceiling);
            /* Idle and tighter limits take effect immediately. Other downward
             * transitions require two samples; rising demand never waits. */
            if(!Sm.Fault && target<current && Sm.Busy[d]>2 && current<=ceiling && ++down[d]<2)target=current;
            else down[d]=0;
            InterlockedExchange(&Sm.Desired[d],(LONG)target);
            if((LONG)target==Sm.Applied[d])continue;
            if(!NT_SUCCESS(SmScmiSet(d,target)))InterlockedExchange(&Sm.Fault,1);
        }
        /* PEP notifications can stop at a constant OS request, even when the
         * real load changes. Keep a cheap 100 ms idle check to restart DVFS. */
        if(!Sm.Fault) {
            timeout.QuadPart=(Sm.Busy[0]>2 || Sm.Busy[1]>2 || Sm.Applied[0]>0 || Sm.Applied[1]>0)?-200000:-1000000;
            wait=&timeout;
        }
    }
    for(ULONG d=0;d<2;d++)if(Sm.Applied[d]!=0)SmScmiSet(d,0);
    PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS Finish(PIRP irp,NTSTATUS status,ULONG_PTR bytes) {
    irp->IoStatus.Status=status;irp->IoStatus.Information=bytes;IoCompleteRequest(irp,0);return status;
}
static NTSTATUS Reject(PDEVICE_OBJECT device,PIRP irp) {
    UNREFERENCED_PARAMETER(device);return Finish(irp,STATUS_INVALID_DEVICE_REQUEST,0);
}
static NTSTATUS OpenClose(PDEVICE_OBJECT device,PIRP irp) {
    UNREFERENCED_PARAMETER(device);return Finish(irp,STATUS_SUCCESS,0);
}
static NTSTATUS Shutdown(PDEVICE_OBJECT device,PIRP irp) {
    UNREFERENCED_PARAMETER(device);InterlockedExchange(&Sm.Stopping,1);KeSetEvent(&Sm.Wake,0,FALSE);
    if(Sm.Thread)KeWaitForSingleObject(Sm.Thread,Executive,KernelMode,FALSE,NULL);
    return Finish(irp,STATUS_SUCCESS,0);
}
static NTSTATUS Control(PDEVICE_OBJECT device,PIRP irp) {
    PIO_STACK_LOCATION stack=IoGetCurrentIrpStackLocation(irp);UNREFERENCED_PARAMETER(device);
    if(stack->Parameters.DeviceIoControl.IoControlCode!=SM_READ)return Finish(irp,STATUS_INVALID_DEVICE_REQUEST,0);
    if(stack->Parameters.DeviceIoControl.InputBufferLength || stack->Parameters.DeviceIoControl.OutputBufferLength<sizeof(SM_REPORT))return Finish(irp,STATUS_BUFFER_TOO_SMALL,0);
    SM_REPORT *r=irp->AssociatedIrp.SystemBuffer;RtlZeroMemory(r,sizeof(*r));
    r->Magic=SM_MAGIC;r->Version=3;r->Size=sizeof(*r);
    r->Flags=1|(!Sm.Fault?2:4);r->RegisteredMask=(ULONG)Sm.RegisteredMask;r->Retention=(ULONG)Sm.Retention;
    for(ULONG d=0;d<2;d++) {
        r->MaxIndex[d]=(ULONG)Sm.Ceiling[d];r->DesiredIndex[d]=(ULONG)Sm.Desired[d];
        r->AppliedIndex[d]=(ULONG)Sm.Applied[d];r->ScmiStatus[d]=Sm.LastStatus[d];r->Busy[d]=Sm.Busy[d];
        r->Khz[d]=Sm.Applied[d]<0?0:SmFrequencyKhz[d][SmClampLevel((ULONG)Sm.Applied[d])];
    }
    return Finish(irp,STATUS_SUCCESS,sizeof(*r));
}
static VOID Unload(PDRIVER_OBJECT driver) {
    InterlockedExchange(&Sm.Stopping,1);KeSetEvent(&Sm.Wake,0,FALSE);
    if(Sm.Thread){KeWaitForSingleObject(Sm.Thread,Executive,KernelMode,FALSE,NULL);ObDereferenceObject(Sm.Thread);}
    if(Sm.ShutdownRegistered)IoUnregisterShutdownNotification(driver->DeviceObject);
    if(Sm.LinkCreated)IoDeleteSymbolicLink(&Link);
    if(driver->DeviceObject)IoDeleteDevice(driver->DeviceObject);
    SmScmiClose();
}
DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path) {
    UNREFERENCED_PARAMETER(path);ULONG midr=(ULONG)_ReadStatusReg(ARM64_SYSREG(3,0,0,0,0));
    if((midr!=0x513f0014 && midr!=0x514f0014) || KeQueryActiveProcessorCountEx(0)!=8)return STATUS_NOT_SUPPORTED;
    RtlZeroMemory(&Sm,sizeof(Sm));KeInitializeEvent(&Sm.Wake,SynchronizationEvent,FALSE);
    for(ULONG c=0;c<8;c++)Sm.Core[c].Number=c;
    for(ULONG i=0;i<=IRP_MJ_MAXIMUM_FUNCTION;i++)driver->MajorFunction[i]=Reject;
    driver->MajorFunction[IRP_MJ_CREATE]=OpenClose;driver->MajorFunction[IRP_MJ_CLOSE]=OpenClose;
    driver->MajorFunction[IRP_MJ_CLEANUP]=OpenClose;driver->MajorFunction[IRP_MJ_DEVICE_CONTROL]=Control;
    driver->MajorFunction[IRP_MJ_SHUTDOWN]=Shutdown;
    Sm.Retention=SmPsci(0x8400000Au,SM_CPU_SUSPEND)>=0;
    NTSTATUS status=SmScmiInitialize();if(!NT_SUCCESS(status)){SmScmiClose();return status;}
    UNICODE_STRING name=RTL_CONSTANT_STRING(L"\\Device\\Sm8750CpuPower"),sddl=RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    PDEVICE_OBJECT device;HANDLE thread;
    status=IoCreateDeviceSecure(driver,0,&name,FILE_DEVICE_UNKNOWN,FILE_DEVICE_SECURE_OPEN,FALSE,&sddl,&GUID_SM_CPUPOWER,&device);
    if(!NT_SUCCESS(status)){Unload(driver);return status;}
    status=IoCreateSymbolicLink(&Link,&name);if(!NT_SUCCESS(status)){Unload(driver);return status;}Sm.LinkCreated=TRUE;
    status=IoRegisterShutdownNotification(device);if(!NT_SUCCESS(status)){Unload(driver);return status;}Sm.ShutdownRegistered=TRUE;
    status=PsCreateSystemThread(&thread,THREAD_ALL_ACCESS,NULL,NULL,NULL,Worker,NULL);if(!NT_SUCCESS(status)){Unload(driver);return status;}
    status=ObReferenceObjectByHandle(thread,SYNCHRONIZE,*PsThreadType,KernelMode,(PVOID*)&Sm.Thread,NULL);
    if(!NT_SUCCESS(status)){InterlockedExchange(&Sm.Stopping,1);KeSetEvent(&Sm.Wake,0,FALSE);ZwWaitForSingleObject(thread,FALSE,NULL);ZwClose(thread);Unload(driver);return status;}ZwClose(thread);
    PEP_INFORMATION info={0};PEP_KERNEL_INFORMATION kernel={0};
    info.Version=PEP_INFORMATION_VERSION;info.Size=sizeof(info);info.AcceptDeviceNotification=SmDeviceNotification;info.AcceptProcessorNotification=SmProcessorNotification;
    kernel.Version=PEP_KERNEL_INFORMATION_VERSION;kernel.Size=sizeof(kernel);
    status=PoFxRegisterPlugin(&info,&kernel);if(!NT_SUCCESS(status)){Unload(driver);return status;}
    /* The OS retains PEP callbacks: a successfully registered PEP cannot unload. */
    device->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
}
