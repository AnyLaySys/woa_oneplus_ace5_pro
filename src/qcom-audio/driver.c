#include "sia9177.h"

VOID
SiaLog (
    _In_ PSIA_DEVICE Dev,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    )
{
    UNREFERENCED_PARAMETER (Dev);
    UNREFERENCED_PARAMETER (Format);
}

VOID
SiaSleep (
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread (KernelMode, FALSE, &Interval);
}

static NTSTATUS
SiaProbe (PSIA_DEVICE Dev)
{
    static const UCHAR Addresses[] = { 0x6C, 0x6D };
    static const ULONG Pins[] = { 2, 164 };
    ULONG Index;
    USHORT Value;
    NTSTATUS Status = SiaI2cInit (Dev);
    if (!NT_SUCCESS (Status)) return Status;
    for (Index = 0; Index < 2; Index++) {
        SiaGpioOutput (Dev, Pins[Index], TRUE);
        SiaSleep (6);
        SiaGpioOutput (Dev, Pins[Index], FALSE);
        SiaSleep (6);
        Status = SiaRegRead (Dev, Addresses[Index], 6, &Value);
        if (!NT_SUCCESS (Status)) return Status;
        if ((Value & 0xFFF0u) != 0x5890u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

static VOID
SiaWorker (PVOID Context)
{
    PSIA_DEVICE Dev = Context;
    BOOLEAN LinkReady = FALSE;
    NTSTATUS Status;
    SiaLog (Dev, "sia9177 v%s: Windows WaveRT, no automatic playback", SIA_DRIVER_VERSION);
    for (;;) {
        KeWaitForSingleObject (&Dev->WorkEvent, Executive, KernelMode, FALSE, NULL);
        if (Dev->Stopping) break;
        if (!Dev->Running) { KeSetEvent (&Dev->IdleEvent, IO_NO_INCREMENT, FALSE); continue; }
        ExSetTimerResolution (10000, TRUE);
        Status = SiaProbe (Dev);
        if (NT_SUCCESS (Status) && !LinkReady) {
            Status = SiaLinkStart (Dev);
            LinkReady = NT_SUCCESS (Status);
            if (!LinkReady) SiaLinkStop (Dev);
        }
        if (NT_SUCCESS (Status) && Dev->Running && !Dev->Stopping) SiaAudioPlay (Dev);
        InterlockedExchange (&Dev->Running, 0);
        ExSetTimerResolution (0, FALSE);
        KeSetEvent (&Dev->IdleEvent, IO_NO_INCREMENT, FALSE);
    }
    SiaLinkStop (Dev);
    KeSetEvent (&Dev->IdleEvent, IO_NO_INCREMENT, FALSE);
    PsTerminateSystemThread (STATUS_SUCCESS);
}

NTSTATUS
SiaHardwareStart (PSIA_DEVICE Dev, PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource)
{
    PHYSICAL_ADDRESS Physical;
    OBJECT_ATTRIBUTES Attributes;
    RtlZeroMemory (Dev, sizeof (*Dev));
    KeInitializeEvent (&Dev->WorkEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent (&Dev->IdleEvent, NotificationEvent, TRUE);
    KeInitializeSpinLock (&Dev->NotifyLock);
    Dev->SeSize = Resource->u.Memory.Length;
    Dev->Se = MmMapIoSpaceEx (Resource->u.Memory.Start, Dev->SeSize, PAGE_READWRITE | PAGE_NOCACHE);
    Physical.QuadPart = SIA_GCC_PHYS;
    Dev->Gcc = MmMapIoSpaceEx (Physical, SIA_GCC_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Physical.QuadPart = SIA_TLMM_PHYS;
    Dev->Tlmm = MmMapIoSpaceEx (Physical, SIA_TLMM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Dev->Se || !Dev->Gcc || !Dev->Tlmm) return STATUS_INSUFFICIENT_RESOURCES;
    InitializeObjectAttributes (&Attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    return PsCreateSystemThread (&Dev->Thread, THREAD_ALL_ACCESS, &Attributes, NULL, NULL, SiaWorker, Dev);
}

VOID
SiaStreamStop (PSIA_DEVICE Dev)
{
    InterlockedExchange (&Dev->Running, 0);
    if (Dev->Thread) KeWaitForSingleObject (&Dev->IdleEvent, Executive, KernelMode, FALSE, NULL);
    if (Dev->PcmSize) Dev->ReadOffset = (ULONG)((ULONGLONG)Dev->PlayedBytes % Dev->PcmSize);
}

NTSTATUS
SiaStreamStart (PSIA_DEVICE Dev)
{
    if (Dev->Stopping || !Dev->Thread || !Dev->Pcm || !Dev->PcmSize || Dev->RetainedDma) return STATUS_DEVICE_NOT_READY;
    if (InterlockedCompareExchange (&Dev->Running, 1, 0) != 0) return STATUS_SUCCESS;
    KeClearEvent (&Dev->IdleEvent);
    KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_SUCCESS;
}

VOID
SiaHardwareStop (PSIA_DEVICE Dev)
{
    PVOID ThreadObject;
    InterlockedExchange (&Dev->Stopping, 1);
    InterlockedExchange (&Dev->Running, 0);
    if (Dev->Thread) {
        KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
        if (NT_SUCCESS (ObReferenceObjectByHandle (Dev->Thread, SYNCHRONIZE, *PsThreadType, KernelMode, &ThreadObject, NULL))) {
            KeWaitForSingleObject (ThreadObject, Executive, KernelMode, FALSE, NULL);
            ObDereferenceObject (ThreadObject);
        }
        ZwClose (Dev->Thread);
        Dev->Thread = NULL;
    }
    if (Dev->Se) MmUnmapIoSpace (Dev->Se, Dev->SeSize);
    if (Dev->Gcc) MmUnmapIoSpace (Dev->Gcc, SIA_GCC_SIZE);
    if (Dev->Tlmm) MmUnmapIoSpace (Dev->Tlmm, SIA_TLMM_SIZE);
    Dev->Se = Dev->Gcc = Dev->Tlmm = NULL;
}
