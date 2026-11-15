#include <ntifs.h>
#include "battery.h"

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE BatAddDevice;

static NTSTATUS BatCreateClose (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatDeviceControl (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatPowerExchange (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatPnp (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatPower (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatUnsupported (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatComplete (PIRP Irp, NTSTATUS Status);
static IO_COMPLETION_ROUTINE BatStartComplete;
static DRIVER_UNLOAD BatUnload;
static DRIVER_DISPATCH BatSystemControl;
static WMI_QUERY_REGINFO_CALLBACK BatWmiInfo;
static WMI_QUERY_DATABLOCK_CALLBACK BatWmiData;
static UNICODE_STRING DriverPath;

VOID
BatSnapshotGet (
    _In_ PBAT_DEVICE Dev,
    _Out_ BAT_SNAPSHOT *Snapshot
    )
{
    KIRQL OldIrql;

    KeAcquireSpinLock (&Dev->Lock, &OldIrql);
    *Snapshot = Dev->Now;
    ULONGLONG Now = KeQueryInterruptTime () / 10000;
    Snapshot->Online = Dev->Source.Valid && Dev->Source.Online && Dev->Source.TimeMs <= Now &&
                       Now - Dev->Source.TimeMs <= QCB_POWER_MAX_AGE_MS;
    if (Snapshot->SampleTimeMs > Now || Now - Snapshot->SampleTimeMs > QCB_POWER_MAX_AGE_MS) {
        Snapshot->Valid = FALSE;
    }
    KeReleaseSpinLock (&Dev->Lock, OldIrql);
}

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    ULONG Index;

    DriverPath.MaximumLength=RegistryPath->Length+sizeof(WCHAR);
    DriverPath.Buffer=ExAllocatePool2(POOL_FLAG_NON_PAGED,DriverPath.MaximumLength,'rtPB');
    if(!DriverPath.Buffer)return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyUnicodeString(&DriverPath,RegistryPath);

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
        DriverObject->MajorFunction[Index] = BatUnsupported;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = BatCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = BatCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = BatCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = BatDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = BatPowerExchange;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = BatSystemControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = BatPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = BatPower;
    DriverObject->DriverExtension->AddDevice = BatAddDevice;
    DriverObject->DriverUnload = BatUnload;
    return STATUS_SUCCESS;
}

static VOID
BatUnload (PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER (DriverObject);
    ExFreePoolWithTag(DriverPath.Buffer,'rtPB');
}

NTSTATUS
BatAddDevice (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
{
    PDEVICE_OBJECT DeviceObject = NULL;
    PBAT_DEVICE Dev;
    NTSTATUS Status;

    Status = IoCreateDevice (DriverObject, sizeof (BAT_DEVICE), NULL, FILE_DEVICE_BATTERY,
                             FILE_DEVICE_SECURE_OPEN, FALSE, &DeviceObject);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    Dev = (PBAT_DEVICE)DeviceObject->DeviceExtension;
    RtlZeroMemory (Dev, sizeof (*Dev));
    Dev->Self = DeviceObject;
    Dev->Pdo = PhysicalDeviceObject;
    KeInitializeSpinLock (&Dev->Lock);
    KeInitializeEvent (&Dev->StopEvent, NotificationEvent, FALSE);

    Dev->Lower = IoAttachDeviceToDeviceStack (DeviceObject, PhysicalDeviceObject);
    if (Dev->Lower == NULL) {

        IoDeleteDevice (DeviceObject);
        return STATUS_DEVICE_REMOVED;
    }

    DeviceObject->Flags |= DO_POWER_PAGABLE;
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = BatHardwareInit (Dev);
    if (!NT_SUCCESS (Status)) {
        KeStallExecutionProcessor (20000);
        Status = BatHardwareInit (Dev);
    }
    Status=BatMiniportInit(Dev);
    if(!NT_SUCCESS(Status)){
        BatHardwareStop(Dev);IoDetachDevice(Dev->Lower);IoDeleteDevice(DeviceObject);return Status;
    }
    Dev->Wmi.QueryWmiRegInfo=BatWmiInfo;
    Dev->Wmi.QueryWmiDataBlock=BatWmiData;
    Dev->WmiRegistered=NT_SUCCESS(IoWMIRegistrationControl(DeviceObject,WMIREG_ACTION_REGISTER));

    return STATUS_SUCCESS;
}

NTSTATUS
BatHardwareInit (
    _In_ PBAT_DEVICE Dev
    )
{
    PHYSICAL_ADDRESS Phys;
    NTSTATUS Status;

    if (Dev->Tlmm == NULL) {
        Phys.QuadPart = BAT_TLMM_PHYS;
        Dev->Tlmm = (PUCHAR)MmMapIoSpaceEx (Phys, BAT_TLMM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (Dev->Tlmm == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    Status = BatI2cInit (Dev);
    Dev->I2cReady = NT_SUCCESS (Status);
    (VOID)BatGaugeSample (Dev);

    return Status;
}

NTSTATUS
BatHardwareStart (
    _In_ PBAT_DEVICE Dev
    )
{
    OBJECT_ATTRIBUTES Attributes;
    HANDLE Thread = NULL;
    NTSTATUS Status;

    Status = BatHardwareInit (Dev);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    KeClearEvent (&Dev->StopEvent);
    InterlockedExchange (&Dev->Stopping, 0);
    InitializeObjectAttributes (&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread (&Thread, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, BatPollThread, Dev);
    if (!NT_SUCCESS (Status)) {

        BatHardwareStop (Dev);
        return Status;
    }

    Dev->Thread = Thread;

    return STATUS_SUCCESS;
}

VOID
BatHardwareStop (
    _In_ PBAT_DEVICE Dev
    )
{
    if (Dev->Thread != NULL) {
        InterlockedExchange (&Dev->Stopping, 1);
        KeSetEvent (&Dev->StopEvent, IO_NO_INCREMENT, FALSE);
        (VOID)ZwWaitForSingleObject (Dev->Thread, FALSE, NULL);
        ZwClose (Dev->Thread);
        Dev->Thread = NULL;
    }
    KIRQL Irql;
    KeAcquireSpinLock (&Dev->Lock, &Irql);
    Dev->Now.Valid = FALSE;
    Dev->Source.Valid = 0;
    KeReleaseSpinLock (&Dev->Lock, Irql);
    if (Dev->Tlmm != NULL) {
        MmUnmapIoSpace (Dev->Tlmm, BAT_TLMM_SIZE);
        Dev->Tlmm = NULL;
    }
}

static NTSTATUS
BatStartComplete (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)Context;

    UNREFERENCED_PARAMETER (DeviceObject);

    if (NT_SUCCESS (Irp->IoStatus.Status)) {
        (VOID)BatHardwareStart (Dev);

    }
    if (Irp->PendingReturned) {
        IoMarkIrpPending (Irp);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
BatCreateClose (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER (DeviceObject);

    return BatComplete (Irp, STATUS_SUCCESS);
}

static NTSTATUS
BatWmiInfo(PDEVICE_OBJECT object,PULONG flags,PUNICODE_STRING instance,PUNICODE_STRING *path,
    PUNICODE_STRING mof,PDEVICE_OBJECT *pdo)
{
    UNREFERENCED_PARAMETER(instance);UNREFERENCED_PARAMETER(mof);
    PBAT_DEVICE dev=object->DeviceExtension;
    *flags=WMIREG_FLAG_INSTANCE_PDO;*path=&DriverPath;*pdo=dev->Pdo;return STATUS_SUCCESS;
}
static NTSTATUS
BatWmiData(PDEVICE_OBJECT object,PIRP irp,ULONG guid,ULONG index,ULONG count,PULONG lengths,ULONG size,PUCHAR buffer)
{
    PBAT_DEVICE dev=object->DeviceExtension;
    if(index || count!=1)return WmiCompleteRequest(object,irp,STATUS_WMI_INSTANCE_NOT_FOUND,0,IO_NO_INCREMENT);
    NTSTATUS status=BatteryClassQueryWmiDataBlock(dev->ClassData,object,irp,guid,lengths,size,buffer);
    return status==STATUS_WMI_GUID_NOT_FOUND?WmiCompleteRequest(object,irp,status,0,IO_NO_INCREMENT):status;
}
static NTSTATUS
BatSystemControl(PDEVICE_OBJECT object,PIRP irp)
{
    PBAT_DEVICE dev=object->DeviceExtension;
    SYSCTL_IRP_DISPOSITION disposition=IrpForward;NTSTATUS status=STATUS_NOT_SUPPORTED;
    if(dev->ClassData)status=BatteryClassSystemControl(dev->ClassData,&dev->Wmi,object,irp,&disposition);
    if(disposition==IrpProcessed)return status;
    if(disposition==IrpNotCompleted){IoCompleteRequest(irp,IO_NO_INCREMENT);return status;}
    IoSkipCurrentIrpStackLocation(irp);return IoCallDriver(dev->Lower,irp);
}

static NTSTATUS
BatDeviceControl (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation (Irp);
    ULONG Code = Stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS Status = STATUS_NOT_SUPPORTED;

    if (Stack->MajorFunction == IRP_MJ_DEVICE_CONTROL && (Code >> 16) == FILE_DEVICE_BATTERY) {
        if (Dev->ClassData != NULL) {
            Status = BatteryClassIoctl (Dev->ClassData, Irp);
        }
        if (Status != STATUS_NOT_SUPPORTED) {

            return Status;
        }
        return BatComplete (Irp, STATUS_INVALID_DEVICE_REQUEST);
    }

    IoSkipCurrentIrpStackLocation (Irp);
    return IoCallDriver (Dev->Lower, Irp);
}

static NTSTATUS
BatPowerExchange (PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation (Irp);
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Bytes = 0;

    if (Stack->Parameters.DeviceIoControl.IoControlCode == QCB_POWER_EXCHANGE &&
        Irp->RequestorMode == KernelMode && Dev->Stopping == 0 &&
        Stack->Parameters.DeviceIoControl.InputBufferLength == sizeof (QCB_SOURCE) &&
        Stack->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(QCB_POWER)) {
        QCB_SOURCE Source = *(QCB_SOURCE *)Irp->AssociatedIrp.SystemBuffer;
        QCB_POWER *Power = (QCB_POWER *)Irp->AssociatedIrp.SystemBuffer;
        ULONGLONG Now = KeQueryInterruptTime () / 10000;
        KIRQL Irql;
        BOOLEAN Changed;
        BAT_SNAPSHOT S;

        if (Source.Valid > 1 || Source.Online > 1 || Source.TimeMs > Now ||
            Now - Source.TimeMs > QCB_POWER_MAX_AGE_MS) Source.Valid = 0;
        KeAcquireSpinLock (&Dev->Lock, &Irql);
        Changed = Dev->Now.Online != (Source.Valid && Source.Online);
        Dev->Source = Source;
        Dev->Now.Online = Source.Valid && Source.Online;
        KeReleaseSpinLock (&Dev->Lock, Irql);
        BatSnapshotGet (Dev, &S);
        RtlZeroMemory (Power, sizeof (*Power));
        Power->SampleTimeMs = S.SampleTimeMs;
        Power->Valid = S.Valid;
        Power->Soc = S.Soc;
        Power->TemperatureDeciK = S.TempDeciK;
        Power->VoltageMv = S.VoltageMv;
        Power->CurrentMa = S.CurrentMa;
        Power->RemainingMah = S.RemainingMah;
        if (Changed && Dev->ClassData != NULL) (VOID)BatteryClassStatusNotify (Dev->ClassData);
        Status = STATUS_SUCCESS;
        Bytes = sizeof (*Power);
        Power->CellValid=S.CellValid;Power->Cell1Mv=S.Cell1Mv;Power->Cell2Mv=S.Cell2Mv;
        Power->BatteryStatus=S.BatteryStatus;Power->ChargingVoltageMv=S.ChargingVoltageMv;
        Power->ChargingCurrentMa=S.ChargingCurrentMa;
    }
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Bytes;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
BatUnsupported (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER (DeviceObject);

    return BatComplete (Irp, STATUS_INVALID_DEVICE_REQUEST);
}

static NTSTATUS
BatComplete (PIRP Irp, NTSTATUS Status)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS
BatPnp (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)DeviceObject->DeviceExtension;
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation (Irp);
    NTSTATUS Status;

    switch (Stack->MinorFunction) {
    case IRP_MN_START_DEVICE:
        IoCopyCurrentIrpStackLocationToNext (Irp);
        IoSetCompletionRoutine (Irp, BatStartComplete, Dev, TRUE, TRUE, TRUE);
        return IoCallDriver (Dev->Lower, Irp);

    case IRP_MN_STOP_DEVICE:
        BatHardwareStop (Dev);
        break;

    case IRP_MN_REMOVE_DEVICE:
        BatHardwareStop (Dev);
        BatMiniportStop (Dev);
        IoSkipCurrentIrpStackLocation (Irp);
        Status = IoCallDriver (Dev->Lower, Irp);
        IoDetachDevice (Dev->Lower);

        IoDeleteDevice (DeviceObject);
        return Status;

    case IRP_MN_SURPRISE_REMOVAL:
        BatHardwareStop (Dev);
        BatMiniportStop (Dev);
        break;

    default:
        break;
    }

    IoSkipCurrentIrpStackLocation (Irp);
    return IoCallDriver (Dev->Lower, Irp);
}

static NTSTATUS
BatPower (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation (Irp);
    return PoCallDriver (Dev->Lower, Irp);
}
