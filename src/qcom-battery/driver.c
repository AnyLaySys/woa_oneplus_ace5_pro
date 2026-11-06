#include <ntifs.h>
#include <ntstrsafe.h>
#include "battery.h"

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE BatAddDevice;

static NTSTATUS BatCreateClose (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatDeviceControl (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatSystemControl (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatPnp (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatPower (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static NTSTATUS BatUnsupported (PDEVICE_OBJECT DeviceObject, PIRP Irp);
static IO_COMPLETION_ROUTINE BatStartComplete;

VOID
BatLog (
    _In_ PBAT_DEVICE Dev,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    CHAR Text[BAT_LOG_LINE];
    size_t Used = 0;
    va_list Args;
    KIRQL OldIrql;

    RtlStringCchPrintfA (Text, BAT_LOG_LINE, "[%lu] ", (ULONG)(KeQueryInterruptTime () / 10000));
    RtlStringCchLengthA (Text, BAT_LOG_LINE, &Used);
    va_start (Args, Format);
    RtlStringCchVPrintfA (Text + Used, BAT_LOG_LINE - Used, Format, Args);
    va_end (Args);

    KeAcquireSpinLock (&Dev->Lock, &OldIrql);
    RtlStringCchCopyA (Dev->LogRing[((ULONG)Dev->LogHead) % BAT_LOG_RING].Text, BAT_LOG_LINE, Text);
    Dev->LogHead++;
    KeReleaseSpinLock (&Dev->Lock, OldIrql);
}

VOID
BatLogFlush (
    _In_ PBAT_DEVICE Dev
    )
{
    CHAR Line[BAT_LOG_LINE + 4];
    IO_STATUS_BLOCK Iosb;
    size_t Used = 0;
    KIRQL OldIrql;
    LONG Tail;

    if (Dev->LogFile == NULL) {
        return;
    }
    for (;;) {
        KeAcquireSpinLock (&Dev->Lock, &OldIrql);
        if (Dev->LogTail == Dev->LogHead) {
            KeReleaseSpinLock (&Dev->Lock, OldIrql);
            break;
        }
        Tail = Dev->LogTail;
        RtlStringCchCopyA (Line, BAT_LOG_LINE, Dev->LogRing[(ULONG)Tail % BAT_LOG_RING].Text);
        Dev->LogTail = Tail + 1;
        KeReleaseSpinLock (&Dev->Lock, OldIrql);

        RtlStringCchCatA (Line, BAT_LOG_LINE + 4, "\r\n");
        RtlStringCchLengthA (Line, BAT_LOG_LINE + 4, &Used);
        (VOID)ZwWriteFile (Dev->LogFile, NULL, NULL, NULL, &Iosb, Line, (ULONG)Used, NULL, NULL);
    }
}

VOID
BatSnapshotGet (
    _In_ PBAT_DEVICE Dev,
    _Out_ BAT_SNAPSHOT *Snapshot
    )
{
    KIRQL OldIrql;

    KeAcquireSpinLock (&Dev->Lock, &OldIrql);
    *Snapshot = Dev->Now;
    KeReleaseSpinLock (&Dev->Lock, OldIrql);
}

static NTSTATUS
BatOpenLog (
    _In_ PBAT_DEVICE Dev
    )
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    RtlInitUnicodeString (&Name, BAT_LOG_PATH);
    InitializeObjectAttributes (&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile (&Dev->LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &Attributes, &Iosb, NULL,
                           FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                           FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS (Status)) {
        Dev->LogFile = NULL;
    }
    return Status;
}

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    ULONG Index;

    UNREFERENCED_PARAMETER (RegistryPath);

    for (Index = 0; Index <= IRP_MJ_MAXIMUM_FUNCTION; Index++) {
        DriverObject->MajorFunction[Index] = BatUnsupported;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = BatCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = BatCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = BatCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = BatDeviceControl;
    DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = BatSystemControl;
    DriverObject->MajorFunction[IRP_MJ_PNP] = BatPnp;
    DriverObject->MajorFunction[IRP_MJ_POWER] = BatPower;
    DriverObject->DriverExtension->AddDevice = BatAddDevice;
    return STATUS_SUCCESS;
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
    KeInitializeSpinLock (&Dev->PendingLock);
    KeInitializeEvent (&Dev->StopEvent, NotificationEvent, FALSE);
    BatOpenLog (Dev);

    Dev->Lower = IoAttachDeviceToDeviceStack (DeviceObject, PhysicalDeviceObject);
    if (Dev->Lower == NULL) {
        if (Dev->LogFile != NULL) {
            ZwClose (Dev->LogFile);
        }
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
    if (Dev->Now.Valid) {
        Status = BatMiniportInit (Dev);
    }
    BatLog (Dev, "add device pdo=%p valid=%u miniport=0x%08x", PhysicalDeviceObject,
            (ULONG)Dev->Now.Valid, Status);
    BatLogFlush (Dev);
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
    BatLog (Dev, "hw init i2c=0x%08x ok=%u", Status, (ULONG)Dev->Now.Valid);
    BatLogFlush (Dev);

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

    InterlockedExchange (&Dev->Stopping, 0);
    InitializeObjectAttributes (&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread (&Thread, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, BatPollThread, Dev);
    if (!NT_SUCCESS (Status)) {
        BatLog (Dev, "thread create failed 0x%08x", Status);
        BatHardwareStop (Dev);
        return Status;
    }

    Dev->Thread = Thread;
    BatLog (Dev, "polling started");
    return STATUS_SUCCESS;
}

VOID
BatHardwareStop (
    _In_ PBAT_DEVICE Dev
    )
{
    PVOID ThreadObject = NULL;
    LARGE_INTEGER Timeout;

    if (Dev->Thread != NULL) {
        InterlockedExchange (&Dev->Stopping, 1);
        KeSetEvent (&Dev->StopEvent, IO_NO_INCREMENT, FALSE);
        if (NT_SUCCESS (ObReferenceObjectByHandle (Dev->Thread, SYNCHRONIZE, *PsThreadType, KernelMode, &ThreadObject, NULL))) {
            Timeout.QuadPart = -10000LL * BAT_STOP_MS;
            (VOID)KeWaitForSingleObject (ThreadObject, Executive, KernelMode, FALSE, &Timeout);
            ObDereferenceObject (ThreadObject);
        }
        ZwClose (Dev->Thread);
        Dev->Thread = NULL;
    }
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
        BatLogFlush (Dev);
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

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
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

    if ((Code >> 16) == FILE_DEVICE_BATTERY) {
        if (Dev->ClassData != NULL) {
            Status = BatteryClassIoctl (Dev->ClassData, Irp);
        }
        if (Status != STATUS_NOT_SUPPORTED) {
            BatLog (Dev, "ioctl 0x%08x status 0x%08x", Code, Status);
            return Status;
        }
        if (BatPendingAdd (Dev, Irp)) {
            BatLog (Dev, "ioctl 0x%08x pended", Code);
            return STATUS_PENDING;
        }
        BatLog (Dev, "ioctl 0x%08x queue full", Code);
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest (Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    IoSkipCurrentIrpStackLocation (Irp);
    return IoCallDriver (Dev->Lower, Irp);
}

static NTSTATUS
BatSystemControl (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)DeviceObject->DeviceExtension;

    IoSkipCurrentIrpStackLocation (Irp);
    return IoCallDriver (Dev->Lower, Irp);
}

static NTSTATUS
BatUnsupported (
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp
    )
{
    UNREFERENCED_PARAMETER (DeviceObject);

    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest (Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

BOOLEAN
BatPendingAdd (
    _In_ PBAT_DEVICE Dev,
    _In_ PIRP Irp
    )
{
    KIRQL OldIrql;
    ULONG Index;
    BOOLEAN Added = FALSE;

    KeAcquireSpinLock (&Dev->PendingLock, &OldIrql);
    for (Index = 0; Index < BAT_PENDING_MAX; Index++) {
        if (Dev->Pending[Index] == NULL) {
            Dev->Pending[Index] = Irp;
            IoMarkIrpPending (Irp);
            Added = TRUE;
            break;
        }
    }
    KeReleaseSpinLock (&Dev->PendingLock, OldIrql);
    return Added;
}

VOID
BatPendingDrain (
    _In_ PBAT_DEVICE Dev,
    _In_ NTSTATUS Status
    )
{
    PIRP Taken[BAT_PENDING_MAX];
    KIRQL OldIrql;
    ULONG Index;
    ULONG Count = 0;

    KeAcquireSpinLock (&Dev->PendingLock, &OldIrql);
    for (Index = 0; Index < BAT_PENDING_MAX; Index++) {
        if (Dev->Pending[Index] != NULL) {
            Taken[Count++] = Dev->Pending[Index];
            Dev->Pending[Index] = NULL;
        }
    }
    KeReleaseSpinLock (&Dev->PendingLock, OldIrql);

    for (Index = 0; Index < Count; Index++) {
        Taken[Index]->IoStatus.Status = Status;
        Taken[Index]->IoStatus.Information = 0;
        IoCompleteRequest (Taken[Index], IO_NO_INCREMENT);
    }
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

    BatLog (Dev, "pnp minor=0x%02x", Stack->MinorFunction);

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
        if (Dev->LogFile != NULL) {
            ZwClose (Dev->LogFile);
            Dev->LogFile = NULL;
        }
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
