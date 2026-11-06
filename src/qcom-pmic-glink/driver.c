#include <ntifs.h>
#include <ntstrsafe.h>
#include "pmic-glink.h"

DRIVER_INITIALIZE DriverEntry;
static VOID QpgThread (PVOID Context);
static VOID QpgUnload (PDRIVER_OBJECT DriverObject);

VOID
QpgLog (
    _In_ PQPG_DEVICE Dev,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    CHAR Text[QPG_LOG_LINE];
    size_t Used = 0;
    va_list Args;
    KIRQL OldIrql;

    RtlStringCchPrintfA (Text, QPG_LOG_LINE, "[%lu] ", (ULONG)(KeQueryInterruptTime () / 10000));
    RtlStringCchLengthA (Text, QPG_LOG_LINE, &Used);
    va_start (Args, Format);
    RtlStringCchVPrintfA (Text + Used, QPG_LOG_LINE - Used, Format, Args);
    va_end (Args);

    KeAcquireSpinLock (&Dev->Lock, &OldIrql);
    RtlStringCchCopyA (Dev->LogRing[((ULONG)Dev->LogHead) % QPG_LOG_RING].Text, QPG_LOG_LINE, Text);
    Dev->LogHead++;
    KeReleaseSpinLock (&Dev->Lock, OldIrql);
}

VOID
QpgLogFlush (
    _In_ PQPG_DEVICE Dev
    )
{
    CHAR Line[QPG_LOG_LINE + 4];
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
        RtlStringCchCopyA (Line, QPG_LOG_LINE, Dev->LogRing[(ULONG)Tail % QPG_LOG_RING].Text);
        Dev->LogTail = Tail + 1;
        KeReleaseSpinLock (&Dev->Lock, OldIrql);

        RtlStringCchCatA (Line, QPG_LOG_LINE + 4, "\r\n");
        RtlStringCchLengthA (Line, QPG_LOG_LINE + 4, &Used);
        (VOID)ZwWriteFile (Dev->LogFile, NULL, NULL, NULL, &Iosb, Line, (ULONG)Used, NULL, NULL);
    }
}

VOID
QpgProtoRx (
    _In_ PQPG_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    PQPG_LINK Link = Dev->Link;
    ULONG Owner;
    ULONG Type;
    ULONG Opcode;
    ULONG Prop;
    ULONG Value;
    ULONG Ret;

    if (Link == NULL) {
        return;
    }
    if (Length < 24) {
        QpgLog (Dev, "rx short len=%u", Length);
        return;
    }
    Owner = *(const ULONG UNALIGNED *)(Data + 0);
    Type = *(const ULONG UNALIGNED *)(Data + 4);
    Opcode = *(const ULONG UNALIGNED *)(Data + 8);
    Prop = *(const ULONG UNALIGNED *)(Data + 12);
    Value = *(const ULONG UNALIGNED *)(Data + 16);
    Ret = *(const ULONG UNALIGNED *)(Data + 20);

    if (Type == QPG_TYPE_NOTIFY) {
        Link->Notifies++;
        QpgLog (Dev, "notify op=0x%03x prop=%u val=0x%x(%d)", Opcode, Prop, Value, (LONG)Value);
        return;
    }
    Link->Replies++;
    if (Ret == 0) {
        if (Opcode == QPG_OP_BATTERY_GET) {
            if (Prop == QPG_BATT_CAPACITY) {
                Link->LastCapacity = Value;
            } else if (Prop == QPG_BATT_VOLT_NOW) {
                Link->LastVoltage = Value;
            } else if (Prop == QPG_BATT_CURR_NOW) {
                Link->LastCurrent = (LONG)Value;
            }
        } else if (Opcode == QPG_OP_USB_GET) {
            if (Prop == QPG_USB_ONLINE) {
                Link->LastUsbOnline = Value;
            } else if (Prop == QPG_USB_TYPE) {
                Link->LastUsbType = Value;
            }
        }
    }
    QpgLog (Dev, "reply owner=%u op=0x%03x prop=%u val=0x%x(%d) ret=%u", Owner, Opcode, Prop, Value,
            (LONG)Value, Ret);
}

static BOOLEAN
QpgSendReq (
    _In_ PQPG_DEVICE Dev,
    _In_ ULONG Opcode,
    _In_ ULONG Prop,
    _In_ ULONG Value
    )
{
    UCHAR Msg[24];

    RtlZeroMemory (Msg, sizeof (Msg));
    *(ULONG UNALIGNED *)(Msg + 0) = QPG_OWNER_BATTMGR;
    *(ULONG UNALIGNED *)(Msg + 4) = QPG_TYPE_REQ_RESP;
    *(ULONG UNALIGNED *)(Msg + 8) = Opcode;
    *(ULONG UNALIGNED *)(Msg + 12) = 0;
    *(ULONG UNALIGNED *)(Msg + 16) = Prop;
    *(ULONG UNALIGNED *)(Msg + 20) = Value;
    return QpgLinkSend (Dev, Msg, sizeof (Msg));
}

static BOOLEAN
QpgNotifyEnable (
    _In_ PQPG_DEVICE Dev
    )
{
    UCHAR Msg[28];

    RtlZeroMemory (Msg, sizeof (Msg));
    *(ULONG UNALIGNED *)(Msg + 0) = QPG_OWNER_BATTMGR;
    *(ULONG UNALIGNED *)(Msg + 4) = QPG_TYPE_NOTIFY;
    *(ULONG UNALIGNED *)(Msg + 8) = QPG_OP_SET_NOTIFY_REQ;
    return QpgLinkSend (Dev, Msg, sizeof (Msg));
}

static VOID
QpgRequestState (
    _In_ PQPG_DEVICE Dev
    )
{
    QpgSendReq (Dev, QPG_OP_BATTERY_GET, QPG_BATT_CAPACITY, 0);
    QpgSendReq (Dev, QPG_OP_BATTERY_GET, QPG_BATT_VOLT_NOW, 0);
    QpgSendReq (Dev, QPG_OP_BATTERY_GET, QPG_BATT_CURR_NOW, 0);
    QpgSendReq (Dev, QPG_OP_BATTERY_GET, QPG_BATT_TEMP, 0);
    QpgSendReq (Dev, QPG_OP_USB_GET, QPG_USB_ONLINE, 0);
    QpgSendReq (Dev, QPG_OP_USB_GET, QPG_USB_TYPE, 0);
}

static VOID
QpgChargeStart (
    _In_ PQPG_DEVICE Dev
    )
{
    ULONG Ma = Dev->ChargeMa != 0 ? Dev->ChargeMa : QPG_CHARGE_MA_DEFAULT;

    if (Dev->ChargeMode == QPG_MODE_CHARGE_OPLUS) {
        QpgSendReq (Dev, QPG_OP_OPLUS_SET, QPG_PROP_USB_INPUT_CURR_LIMIT, Ma);
        QpgSendReq (Dev, QPG_OP_OPLUS_SET, QPG_PROP_CHG_EN, 1);
    } else {
        QpgSendReq (Dev, QPG_OP_USB_SET, QPG_USB_INPUT_LIMIT, Ma);
        QpgSendReq (Dev, QPG_OP_BATTERY_SET, QPG_BATT_CHG_EN, 1);
    }
    QpgLog (Dev, "charge start mode=%u ma=%u", Dev->ChargeMode, Ma);
}

static VOID
QpgUefiCharge (
    _In_ PQPG_DEVICE Dev
    )
{
    QpgSendReq (Dev, QPG_OP_BATTERY_SET, QPG_BATT_UEFI_LOAD_ADSP, 1);
    QpgSendReq (Dev, QPG_OP_BATTERY_SET, QPG_BATT_UEFI_INPUT_CURRENT, 2000);
    QpgSendReq (Dev, QPG_OP_BATTERY_SET, QPG_BATT_UEFI_CHG_EN, 1);
    QpgLog (Dev, "uefi charge sent");
}

static NTSTATUS
QpgOpenLog (
    _In_ PQPG_DEVICE Dev
    )
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    RtlInitUnicodeString (&Name, QPG_LOG_PATH);
    InitializeObjectAttributes (&Attributes, &Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile (&Dev->LogFile, FILE_APPEND_DATA | SYNCHRONIZE, &Attributes, &Iosb, NULL,
                           FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
                           FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS (Status)) {
        Dev->LogFile = NULL;
    }
    return Status;
}

static ULONG
QpgReadDword (
    _In_ PUNICODE_STRING Key,
    _In_ PCWSTR Name,
    _In_ ULONG Default
    )
{
    WCHAR Path[256];
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = Default;

    if (Key == NULL || Key->Buffer == NULL || Key->Length >= sizeof (Path)) {
        return Default;
    }
    RtlZeroMemory (Path, sizeof (Path));
    RtlCopyMemory (Path, Key->Buffer, Key->Length);
    RtlZeroMemory (Table, sizeof (Table));
    Table[0].Name = (PWSTR)Name;
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof (ULONG);
    if (!NT_SUCCESS (RtlQueryRegistryValues (RTL_REGISTRY_ABSOLUTE, Path, Table, NULL, NULL))) {
        return Default;
    }
    return Value;
}

static VOID
QpgThread (
    _In_ PVOID Context
    )
{
    PQPG_DEVICE Dev = (PQPG_DEVICE)Context;
    LARGE_INTEGER Timeout;
    NTSTATUS Status = STATUS_DEVICE_NOT_READY;
    ULONG Tries;
    ULONG Tick = 0;

    for (Tries = 0; Tries < 8; Tries++) {
        Status = QpgLinkStart (Dev);
        QpgLog (Dev, "link start try=%u status=0x%08x rcid=%u intents=%u frames=%u foreign=%u", Tries, Status,
                Dev->Link != NULL ? Dev->Link->Rcid : 0, Dev->Link != NULL ? Dev->Link->IntentCount : 0,
                Dev->Link != NULL ? Dev->Link->Frames : 0, Dev->Link != NULL ? Dev->Link->Foreign : 0);
        QpgLogFlush (Dev);
        if (NT_SUCCESS (Status)) {
            break;
        }
        QpgLinkStop (Dev);
        Timeout.QuadPart = -10000LL * 500;
        KeWaitForSingleObject (&Dev->StopEvent, Executive, KernelMode, FALSE, &Timeout);
        if (InterlockedCompareExchange (&Dev->Stopping, 0, 0) != 0) {
            break;
        }
    }

    if (NT_SUCCESS (Status)) {
        QpgLog (Dev, "notify enable=%u mode=%u ma=%u", (ULONG)QpgNotifyEnable (Dev), Dev->ChargeMode,
                Dev->ChargeMa);
        QpgUefiCharge (Dev);
        if (Dev->ChargeMode != QPG_MODE_READONLY) {
            QpgChargeStart (Dev);
        }
        QpgLogFlush (Dev);
    }

    for (;;) {
        if (NT_SUCCESS (Status)) {
            QpgLinkPoll (Dev);
            if (Dev->Link != NULL && Dev->Link->OpenAcked && (Tick % 4) == 0) {
                ULONG Tail = 0;
                ULONG Head = 0;
                ULONG Pending = 0;

                if (Dev->Link->Desc != NULL) {
                    Tail = READ_REGISTER_ULONG ((volatile ULONG *)Dev->Link->Desc);
                    Head = READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Link->Desc + 4));
                    Pending = (Head - Tail + Dev->Link->TxLength) % Dev->Link->TxLength;
                }
                if (Pending > 512) {
                    QpgLinkKick (Dev);
                } else {
                    QpgRequestState (Dev);
                }
                QpgLog (Dev, "state cap=%u v=%u i=%d usb=%u type=%u rep=%u nt=%u fr=%u fo=%u tx=%u/%u rx=%u/%u pend=%u",
                        Dev->Link->LastCapacity, Dev->Link->LastVoltage, Dev->Link->LastCurrent,
                        Dev->Link->LastUsbOnline, Dev->Link->LastUsbType, Dev->Link->Replies,
                        Dev->Link->Notifies, Dev->Link->Frames, Dev->Link->Foreign, Tail, Head,
                        Dev->Link->Desc != NULL ? READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Link->Desc + 8)) : 0,
                        Dev->Link->Desc != NULL ? READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Link->Desc + 12)) : 0,
                        Pending);
            }
            Tick++;
        }
        QpgLogFlush (Dev);
        Timeout.QuadPart = -10000LL * 500;
        KeWaitForSingleObject (&Dev->StopEvent, Executive, KernelMode, FALSE, &Timeout);
        if (InterlockedCompareExchange (&Dev->Stopping, 0, 0) != 0) {
            break;
        }
    }

    QpgLinkStop (Dev);
    QpgLogFlush (Dev);
    PsTerminateSystemThread (STATUS_SUCCESS);
}

static VOID
QpgUnload (
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    PDEVICE_OBJECT DeviceObject = DriverObject->DeviceObject;
    PQPG_DEVICE Dev;
    PVOID ThreadObject = NULL;
    LARGE_INTEGER Timeout;

    if (DeviceObject == NULL) {
        return;
    }
    Dev = (PQPG_DEVICE)DeviceObject->DeviceExtension;

    if (Dev->Thread != NULL) {
        InterlockedExchange (&Dev->Stopping, 1);
        KeSetEvent (&Dev->StopEvent, IO_NO_INCREMENT, FALSE);
        if (NT_SUCCESS (ObReferenceObjectByHandle (Dev->Thread, SYNCHRONIZE, *PsThreadType, KernelMode,
                                                  &ThreadObject, NULL))) {
            Timeout.QuadPart = -10000LL * 5000;
            (VOID)KeWaitForSingleObject (ThreadObject, Executive, KernelMode, FALSE, &Timeout);
            ObDereferenceObject (ThreadObject);
        }
        ZwClose (Dev->Thread);
        Dev->Thread = NULL;
    }
    if (Dev->Smem != NULL) {
        MmUnmapIoSpace (Dev->Smem, QPG_SMEM_SIZE);
        Dev->Smem = NULL;
    }
    if (Dev->Ipcc != NULL) {
        MmUnmapIoSpace (Dev->Ipcc, QPG_IPCC_SIZE);
        Dev->Ipcc = NULL;
    }
    if (Dev->LogFile != NULL) {
        ZwClose (Dev->LogFile);
        Dev->LogFile = NULL;
    }
    IoDeleteDevice (DeviceObject);
}

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    PDEVICE_OBJECT DeviceObject = NULL;
    PQPG_DEVICE Dev;
    PHYSICAL_ADDRESS Phys;
    HANDLE Thread = NULL;
    OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;

    Status = IoCreateDevice (DriverObject, sizeof (QPG_DEVICE), NULL, FILE_DEVICE_UNKNOWN,
                             FILE_DEVICE_SECURE_OPEN, FALSE, &DeviceObject);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    DriverObject->DriverUnload = QpgUnload;

    Dev = (PQPG_DEVICE)DeviceObject->DeviceExtension;
    RtlZeroMemory (Dev, sizeof (*Dev));
    Dev->Self = DeviceObject;
    Dev->ChargeMode = QpgReadDword (RegistryPath, QPG_REG_CHARGE_MODE, QPG_MODE_READONLY);
    Dev->ChargeMa = QpgReadDword (RegistryPath, QPG_REG_CHARGE_MA, QPG_CHARGE_MA_DEFAULT);
    KeInitializeSpinLock (&Dev->Lock);
    KeInitializeEvent (&Dev->StopEvent, NotificationEvent, FALSE);
    QpgOpenLog (Dev);

    Phys.QuadPart = QPG_SMEM_PHYS;
    Dev->Smem = (PUCHAR)MmMapIoSpaceEx (Phys, QPG_SMEM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Phys.QuadPart = QPG_IPCC_PHYS;
    Dev->Ipcc = (PUCHAR)MmMapIoSpaceEx (Phys, QPG_IPCC_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Dev->Smem == NULL || Dev->Ipcc == NULL) {
        QpgLog (Dev, "map failed smem=%p ipcc=%p", Dev->Smem, Dev->Ipcc);
        QpgLogFlush (Dev);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    QpgLog (Dev, "mapped smem=%p ipcc=%p init=%u mode=%u ma=%u", Dev->Smem, Dev->Ipcc,
            READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Smem + QPG_HDR_INITIALIZED)), Dev->ChargeMode,
            Dev->ChargeMa);

    InitializeObjectAttributes (&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = PsCreateSystemThread (&Thread, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, QpgThread, Dev);
    if (!NT_SUCCESS (Status)) {
        QpgLog (Dev, "thread failed 0x%08x", Status);
        QpgLogFlush (Dev);
        return Status;
    }

    Dev->Thread = Thread;
    return STATUS_SUCCESS;
}
