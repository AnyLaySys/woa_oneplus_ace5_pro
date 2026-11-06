/* KMDF resource and power lifecycle; passive-level radio worker. */
#include "version.h"
#include <ntifs.h> /* documented ZwWaitForSingleObject for thread-create rollback */
#include "wcn7861.h"

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

/* Failures remain visible to a kernel debugger; no persistent trace files. */
VOID WcnLog(PWCN_DEVICE Dev,PCSTR Format,...)
{
    va_list Args;UNREFERENCED_PARAMETER(Dev);va_start(Args,Format);
    vDbgPrintExWithPrefix("wcn7861: ",DPFLTR_IHVNETWORK_ID,DPFLTR_ERROR_LEVEL,Format,Args);va_end(Args);
}

LONGLONG
WcnNowMs (
    VOID
    )
{
    return (LONGLONG)(KeQueryInterruptTime () / 10000);
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

VOID
WcnThread (
    _In_ PVOID Context
    )
{
    PWCN_DEVICE Dev = Context;
    NTSTATUS Status;

    Status = WcnPcieSetup (Dev);
    if (NT_SUCCESS (Status)) {
        Status = WcnPcieConfigSid (Dev);
    }
    if (NT_SUCCESS (Status)) {
        Status = WcnSmmuSetup ();

    }
    if (NT_SUCCESS (Status)) {
        Status = WcnReadFile (WCN_FIRMWARE_PATH, &Dev->Firmware, &Dev->FirmwareSize);

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

    if (Dev->Firmware != NULL) {
        ExFreePoolWithTag (Dev->Firmware, WCN_TAG);
        Dev->Firmware = NULL;
    }

    while (!Dev->Stop) {
        WcnWait (Dev, 100);
        if (Dev->Stop) {
            break;
        }
        WcnMhiService (Dev);
        WcnCeService (Dev);

        WcnStage6Service (Dev);
        WcnStage7DpPoll (Dev); /* Reclaim completions before submitting the next batch. */
        WcnStage8Service (Dev);
        WcnStage7Service (Dev);
        WcnTdlsService (Dev);

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
    WcnTdlsPowerDown (Dev);
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
    WcnTdlsPowerDown (Dev);
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
    UNREFERENCED_PARAMETER(TargetState);

    if (Dev->Thread != NULL) {
        Dev->Stop = TRUE;
        KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject (Dev->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject (Dev->Thread);
        Dev->Thread = NULL;
    }

    WcnStage8Stop (Dev);

    WcnMhiPowerDown (Dev);
    WcnTdlsPowerDown (Dev);

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
