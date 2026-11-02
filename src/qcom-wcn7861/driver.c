/*
 * KMDF glue for the WLN0 (HMRW7861) node: resources, the iMSI interrupt, a
 * passive-level bring-up thread and a plain-text log at \SystemRoot\wcn7861.log
 * (the PE runs flat from the als partition, so the log survives a reboot).
 */
#include "version.h"
#include <ntifs.h> /* documented ZwWaitForSingleObject for thread-create rollback */
#include "wcn7861.h"

#define WCN_LOG_PATH        L"\\SystemRoot\\wcn7861.log"
#define WCN_FIRMWARE_PATH   L"\\SystemRoot\\System32\\drivers\\wcn7861\\amss.bin"

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD WcnEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE WcnEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE WcnEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY_POST_INTERRUPTS_ENABLED WcnEvtD0EntryPostInterruptsEnabled;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED WcnEvtD0ExitPreInterruptsDisabled;
EVT_WDF_INTERRUPT_ISR WcnEvtIsr;
EVT_WDF_INTERRUPT_DPC WcnEvtDpc;
KSTART_ROUTINE WcnThread;

VOID
WcnLog (
    _In_ PWCN_DEVICE Dev,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    CHAR Line[512];
    size_t Length;
    va_list Args;
    LONGLONG Elapsed;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    if (KeGetCurrentIrql () != PASSIVE_LEVEL) {
        return;
    }

    if (Dev->LogFile == NULL) {
        OBJECT_ATTRIBUTES Attributes;
        UNICODE_STRING Path;

        RtlInitUnicodeString (&Path, WCN_LOG_PATH);
        InitializeObjectAttributes (&Attributes, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = ZwCreateFile (&Dev->LogFile,
                               FILE_APPEND_DATA | SYNCHRONIZE,
                               &Attributes,
                               &Iosb,
                               NULL,
                               FILE_ATTRIBUTE_NORMAL,
                               FILE_SHARE_READ,
                               FILE_OVERWRITE_IF,
                               FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH,
                               NULL,
                               0);
        if (!NT_SUCCESS (Status)) {
            Dev->LogFile = NULL;
            return;
        }
        Dev->LogStart = (LONGLONG)KeQueryInterruptTime ();
    }

    Elapsed = ((LONGLONG)KeQueryInterruptTime () - Dev->LogStart) / 10000;
    Status = RtlStringCbPrintfA (Line, sizeof (Line), "[%5lld.%03lld] ", Elapsed / 1000, Elapsed % 1000);
    if (!NT_SUCCESS (Status)) {
        return;
    }
    RtlStringCbLengthA (Line, sizeof (Line), &Length);

    va_start (Args, Format);
    RtlStringCbVPrintfA (Line + Length, sizeof (Line) - Length - 2, Format, Args);
    va_end (Args);
    RtlStringCbCatA (Line, sizeof (Line), "\r\n");
    RtlStringCbLengthA (Line, sizeof (Line), &Length);

    ZwWriteFile (Dev->LogFile, NULL, NULL, NULL, &Iosb, Line, (ULONG)Length, NULL, NULL);
}

LONGLONG
WcnNowMs (
    VOID
    )
{
    return (LONGLONG)(KeQueryInterruptTime () / 10000);
}

/* Up to 256 bytes, 32 per line. */
VOID
WcnLogHex (
    _In_ PWCN_DEVICE Dev,
    _In_z_ PCSTR Prefix,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    static const CHAR Digits[] = "0123456789abcdef";
    CHAR Line[3 * 32 + 1];
    ULONG Offset;
    ULONG Index;
    ULONG Count;

    if (Length > 256) {
        Length = 256;
    }
    for (Offset = 0; Offset < Length; Offset += 32) {
        Count = Length - Offset;
        if (Count > 32) {
            Count = 32;
        }
        for (Index = 0; Index < Count; Index++) {
            Line[Index * 3] = Digits[Data[Offset + Index] >> 4];
            Line[Index * 3 + 1] = Digits[Data[Offset + Index] & 0xF];
            Line[Index * 3 + 2] = ' ';
        }
        Line[Count * 3] = 0;
        WcnLog (Dev, "%s+%03x: %s", Prefix, Offset, Line);
    }
}

VOID
WcnSleep (
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread (KernelMode, FALSE, &Interval);
}

/* Waits for an interrupt (or a stop request); returns FALSE on timeout. */
BOOLEAN
WcnWait (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER Timeout;

    Timeout.QuadPart = -10000LL * Milliseconds;
    return KeWaitForSingleObject (&Dev->WorkEvent, Executive, KernelMode, FALSE, &Timeout) == STATUS_SUCCESS;
}

NTSTATUS
WcnReadFile (
    _In_ PCWSTR Path,
    _Outptr_ PUCHAR *Data,
    _Out_ SIZE_T *Size
    )
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Name;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Info;
    HANDLE File;
    PUCHAR Buffer;
    NTSTATUS Status;

    *Data = NULL;
    *Size = 0;

    RtlInitUnicodeString (&Name, Path);
    InitializeObjectAttributes (&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile (&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                           FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    Status = ZwQueryInformationFile (File, &Iosb, &Info, sizeof (Info), FileStandardInformation);
    if (NT_SUCCESS (Status) && (Info.EndOfFile.QuadPart == 0 || Info.EndOfFile.QuadPart > 64 * 1024 * 1024)) {
        Status = STATUS_INVALID_IMAGE_FORMAT;
    }
    if (NT_SUCCESS (Status)) {
        Buffer = ExAllocatePool2 (POOL_FLAG_PAGED, (SIZE_T)Info.EndOfFile.QuadPart, WCN_TAG);
        if (Buffer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            Status = ZwReadFile (File, NULL, NULL, NULL, &Iosb, Buffer, Info.EndOfFile.LowPart, NULL, NULL);
            if (NT_SUCCESS (Status) && Iosb.Information != Info.EndOfFile.LowPart) {
                Status = STATUS_END_OF_FILE;
            }
            if (NT_SUCCESS (Status)) {
                *Data = Buffer;
                *Size = Info.EndOfFile.LowPart;
            } else {
                ExFreePoolWithTag (Buffer, WCN_TAG);
            }
        }
    }

    ZwClose (File);
    return Status;
}

NTSTATUS
WcnDmaAllocAligned (
    _In_ PWCN_DEVICE Dev,
    _In_ SIZE_T Length,
    _In_ ULONG AlignmentMask,
    _Out_ PWCN_DMA Dma
    )
{
    NTSTATUS Status;
    WDF_COMMON_BUFFER_CONFIG Config;

    RtlZeroMemory (Dma, sizeof (*Dma));
    WDF_COMMON_BUFFER_CONFIG_INIT (&Config, AlignmentMask);
    Status = WdfCommonBufferCreateWithConfig (Dev->DmaEnabler, Length, &Config,
                                             WDF_NO_OBJECT_ATTRIBUTES, &Dma->Buffer);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }
    Dma->Va = WdfCommonBufferGetAlignedVirtualAddress (Dma->Buffer);
    Dma->La = (ULONG64)WdfCommonBufferGetAlignedLogicalAddress (Dma->Buffer).QuadPart;
    Dma->Length = Length;
    /* WDF_COMMON_BUFFER_CONFIG: for alignment above PAGE_SIZE only the
     * device logical address is guaranteed to meet that alignment. The CPU
     * virtual address may merely be page-aligned (e.g. QMI 8-MiB chunks).
     * Keep full logical alignment and 32-bit range checks, not a VA==LA rule.
     */
    if (!Length || Dma->La > MAXULONG || Length - 1 > MAXULONG - Dma->La ||
        (Dma->La & AlignmentMask) != 0 ||
        (AlignmentMask < PAGE_SIZE && ((ULONG_PTR)Dma->Va & AlignmentMask) != 0)) {
        WcnLog (Dev, "dma: invalid common buffer LA 0x%llx VA %p len %Iu mask 0x%x",
                Dma->La, Dma->Va, Length, AlignmentMask);
        WdfObjectDelete (Dma->Buffer);
        RtlZeroMemory (Dma, sizeof (*Dma));
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory (Dma->Va, Length);
    return STATUS_SUCCESS;
}

NTSTATUS
WcnDmaAlloc (
    _In_ PWCN_DEVICE Dev,
    _In_ SIZE_T Length,
    _Out_ PWCN_DMA Dma
    )
{
    return WcnDmaAllocAligned (Dev, Length, FILE_OCTA_ALIGNMENT, Dma);
}

NTSTATUS
WcnWriteDump (
    _In_ PCWSTR Path,
    _In_reads_ (Count) PWCN_DMA Segments,
    _In_ ULONG Count
    )
{
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Name;
    IO_STATUS_BLOCK Iosb;
    HANDLE File;
    ULONG Index;
    NTSTATUS Status;

    RtlInitUnicodeString (&Name, Path);
    InitializeObjectAttributes (&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile (&File, FILE_WRITE_DATA | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL, 0,
                           FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }
    for (Index = 0; Index < Count && NT_SUCCESS (Status); Index++) {
        Status = ZwWriteFile (File, NULL, NULL, NULL, &Iosb, Segments[Index].Va, (ULONG)Segments[Index].Length, NULL, NULL);
    }
    ZwClose (File);
    return Status;
}

VOID
WcnThread (
    _In_ PVOID Context
    )
{
    PWCN_DEVICE Dev = Context;
    NTSTATUS Status;
    ULONG Beat;

    WcnLog (Dev, WCN_DRIVER_BANNER ": BAR0 0x%llx (+0x%x) cfg %p dbi %p parf %p",
            Dev->BarPhysical.QuadPart, Dev->BarLength, Dev->Config, Dev->Dbi, Dev->Parf);

    Status = WcnPcieSetup (Dev);
    if (NT_SUCCESS (Status)) {
        Status = WcnPcieConfigSid (Dev);
    }
    if (NT_SUCCESS (Status)) {
        Status = WcnSmmuSetup (Dev);
        WcnLog (Dev, "smmu setup: 0x%08x", Status);
    }
    if (NT_SUCCESS (Status)) {
        Status = WcnReadFile (WCN_FIRMWARE_PATH, &Dev->Firmware, &Dev->FirmwareSize);
        WcnLog (Dev, "firmware amss.bin: %Iu bytes, status 0x%08x", Dev->FirmwareSize, Status);
    }
    if (NT_SUCCESS (Status)) {
        WcnPcieSwReset (Dev, TRUE);
        WcnPcieMsiEnable (Dev, TRUE);
        Status = WcnMhiPrepare (Dev);
    }
    if (NT_SUCCESS (Status)) {
        Status = WcnMhiPowerUp (Dev);
    }
    if (NT_SUCCESS (Status)) {
        Status = WcnMhiStartChannels (Dev);
    }
    WcnLog (Dev, "bring-up finished: status 0x%08x, isr %ld, msi seen 0x%08lx",
            Status, Dev->IsrCount, Dev->MsiSeen);

    if (Dev->Firmware != NULL) {
        ExFreePoolWithTag (Dev->Firmware, WCN_TAG);
        Dev->Firmware = NULL;
    }

    for (Beat = 0; !Dev->Stop; Beat++) {
        WcnWait (Dev, 100);
        if (Dev->Stop) {
            break;
        }
        WcnMhiService (Dev);
        WcnCeService (Dev);
        WcnDpService (Dev);
        WcnStage6Service (Dev);
        WcnStage8Service (Dev);
        WcnStage7Service (Dev);
        if (Beat % 50 == 0 && Beat < 600 && Dev->Mhi.Bhi != 0) {
            WcnLog (Dev, "heartbeat: mhi status 0x%08x ee %u isr %ld msi 0x%08lx, ipcr tx %u rx %u, qmi %s%s",
                    WcnBarRead (Dev, MHI_STATUS), WcnBarRead (Dev, Dev->Mhi.Bhi + BHI_EXECENV), Dev->IsrCount, Dev->MsiSeen,
                    Dev->Mhi.Chan[0].Packets, Dev->Mhi.Chan[1].Packets, WcnQmiStepName (Dev),
                    Dev->Qmi.Failed ? " (failed)" : "");
        }
    }

    PsTerminateSystemThread (STATUS_SUCCESS);
}

BOOLEAN
WcnEvtIsr (
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID
    )
{
    PWCN_DEVICE Dev = WcnGetDevice (WdfInterruptGetDevice (Interrupt));
    ULONG Pending;

    UNREFERENCED_PARAMETER (MessageID);

    Pending = WcnDbiRead (Dev, DBI_MSI_INTR0_STATUS);
    if (Pending == 0 || Pending == MAXULONG) {
        return FALSE;
    }
    WcnDbiWrite (Dev, DBI_MSI_INTR0_STATUS, Pending);
    InterlockedOr (&Dev->MsiPending, (LONG)Pending);
    InterlockedOr (&Dev->MsiSeen, (LONG)Pending);
    InterlockedIncrement (&Dev->IsrCount);
    WdfInterruptQueueDpcForIsr (Interrupt);
    return TRUE;
}

VOID
WcnEvtDpc (
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject
    )
{
    PWCN_DEVICE Dev = WcnGetDevice (WdfInterruptGetDevice (Interrupt));

    UNREFERENCED_PARAMETER (AssociatedObject);
    KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
}

NTSTATUS
WcnEvtPrepareHardware (
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PWCN_DEVICE Dev = WcnGetDevice (Device);
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc;
    ULONG Index;
    ULONG Count;
    PVOID Mapped;

    UNREFERENCED_PARAMETER (ResourcesRaw);

    Count = WdfCmResourceListGetCount (ResourcesTranslated);
    for (Index = 0; Index < Count; Index++) {
        Desc = WdfCmResourceListGetDescriptor (ResourcesTranslated, Index);
        if (Desc == NULL || Desc->Type != CmResourceTypeMemory) {
            continue;
        }
        Mapped = MmMapIoSpaceEx (Desc->u.Memory.Start, Desc->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        if (Mapped == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        switch (Desc->u.Memory.Start.QuadPart) {
        case 0x40400000:
            Dev->Bar = Mapped;
            Dev->BarLength = Desc->u.Memory.Length;
            Dev->BarPhysical = Desc->u.Memory.Start;
            break;
        case 0x40100000:
            Dev->Config = Mapped;
            Dev->ConfigLength = Desc->u.Memory.Length;
            break;
        case 0x40000000:
            Dev->Dbi = Mapped;
            Dev->DbiLength = Desc->u.Memory.Length;
            break;
        case 0x01C00000:
            Dev->Parf = Mapped;
            Dev->ParfLength = Desc->u.Memory.Length;
            break;
        default:
            MmUnmapIoSpace (Mapped, Desc->u.Memory.Length);
            break;
        }
    }

    if (Dev->Bar == NULL || Dev->Config == NULL || Dev->Dbi == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
WcnEvtReleaseHardware (
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PWCN_DEVICE Dev = WcnGetDevice (Device);

    UNREFERENCED_PARAMETER (ResourcesTranslated);

    WcnStage7Cleanup (Dev);
    WcnDpCleanup (Dev);
    if (Dev->Bar != NULL) {
        MmUnmapIoSpace (Dev->Bar, Dev->BarLength);
        Dev->Bar = NULL;
    }
    if (Dev->Config != NULL) {
        MmUnmapIoSpace (Dev->Config, Dev->ConfigLength);
        Dev->Config = NULL;
    }
    if (Dev->Dbi != NULL) {
        MmUnmapIoSpace (Dev->Dbi, Dev->DbiLength);
        Dev->Dbi = NULL;
    }
    if (Dev->Parf != NULL) {
        MmUnmapIoSpace (Dev->Parf, Dev->ParfLength);
        Dev->Parf = NULL;
    }
    if (Dev->LogFile != NULL) {
        ZwClose (Dev->LogFile);
        Dev->LogFile = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
WcnEvtD0EntryPostInterruptsEnabled (
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PWCN_DEVICE Dev = WcnGetDevice (Device);
    HANDLE Thread;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER (PreviousState);

    Dev->Stop = FALSE;
    Status = PsCreateSystemThread (&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, WcnThread, Dev);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }
    Status = ObReferenceObjectByHandle (Thread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode, (PVOID *)&Dev->Thread, NULL);
    if (!NT_SUCCESS (Status)) {
        Dev->Stop = TRUE;
        KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
        ZwWaitForSingleObject (Thread, FALSE, NULL);
        ZwClose (Thread);
        WcnMhiPowerDown (Dev);
        return Status;
    }
    ZwClose (Thread);
    Status = WcnStage8Ready (Dev);
    if (!NT_SUCCESS (Status)) {
        Dev->Stop = TRUE;
        KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject (Dev->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject (Dev->Thread);
        Dev->Thread = NULL;
        WcnStage8Stop (Dev);
        WcnMhiPowerDown (Dev);
    }
    return Status;
}

NTSTATUS
WcnEvtD0ExitPreInterruptsDisabled (
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PWCN_DEVICE Dev = WcnGetDevice (Device);

    UNREFERENCED_PARAMETER (TargetState);

    if (Dev->Thread != NULL) {
        Dev->Stop = TRUE;
        KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject (Dev->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject (Dev->Thread);
        Dev->Thread = NULL;
    }
    WcnStage8Stop (Dev);
    WcnMhiPowerDown (Dev);
    return STATUS_SUCCESS;
}

NTSTATUS
WcnEvtDeviceAdd (
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPower;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_DMA_ENABLER_CONFIG DmaConfig;
    WDFDEVICE Device;
    PWCN_DEVICE Dev;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER (Driver);

    Status = WcnStage8Init (DeviceInit);
    if (!NT_SUCCESS (Status)) return Status;

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT (&PnpPower);
    PnpPower.EvtDevicePrepareHardware = WcnEvtPrepareHardware;
    PnpPower.EvtDeviceReleaseHardware = WcnEvtReleaseHardware;
    PnpPower.EvtDeviceD0EntryPostInterruptsEnabled = WcnEvtD0EntryPostInterruptsEnabled;
    PnpPower.EvtDeviceD0ExitPreInterruptsDisabled = WcnEvtD0ExitPreInterruptsDisabled;
    WdfDeviceInitSetPnpPowerEventCallbacks (DeviceInit, &PnpPower);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE (&Attributes, WCN_DEVICE);
    Status = WdfDeviceCreate (&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    Dev = WcnGetDevice (Device);
    Dev->Device = Device;
    KeInitializeSpinLock (&Dev->WindowLock);
    KeInitializeEvent (&Dev->WorkEvent, SynchronizationEvent, FALSE);
    Status = WcnStage8Create (Dev);
    if (!NT_SUCCESS (Status)) return Status;

    WDF_INTERRUPT_CONFIG_INIT (&InterruptConfig, WcnEvtIsr, WcnEvtDpc);
    Status = WdfInterruptCreate (Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Interrupt);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    WdfDeviceSetAlignmentRequirement (Device, FILE_OCTA_ALIGNMENT);
    WDF_DMA_ENABLER_CONFIG_INIT (&DmaConfig, WdfDmaProfileScatterGather64, MHI_SEG_LEN);
    /* below 4 GB, like the s3910 GPI buffers and Linux iommu-dma's PCI SAC IOVAs */
    DmaConfig.AddressWidthOverride = 32;
    return WdfDmaEnablerCreate (Device, &DmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->DmaEnabler);
}

NTSTATUS
DriverEntry (
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG Config;

    WDF_DRIVER_CONFIG_INIT (&Config, WcnEvtDeviceAdd);
    return WdfDriverCreate (DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
