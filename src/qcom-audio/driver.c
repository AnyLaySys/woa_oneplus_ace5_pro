#include "sia9177.h"

VOID
SiaSleep (
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER Interval;

    Interval.QuadPart = -10000LL * Milliseconds;
    KeDelayExecutionThread (KernelMode, FALSE, &Interval);
}

NTSTATUS
SiaAmpsProbe (PSIA_DEVICE Dev)
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
    LARGE_INTEGER Wait;
    for (;;) {
        Wait.QuadPart = -200000LL;
        KeWaitForSingleObject (&Dev->WorkEvent, Executive, KernelMode, FALSE, &Wait);
        if (Dev->Stopping) break;
        ExSetTimerResolution (10000, TRUE);
        Status = STATUS_SUCCESS;
        if (!LinkReady) {
            Status = SiaLinkStart (Dev);
            LinkReady = NT_SUCCESS (Status);
            if (!LinkReady) SiaLinkStop (Dev);
        }
        if (NT_SUCCESS (Status)) { SiaLinkPoll (Dev); if (Dev->Running) SiaAudioRun (Dev); }
        if (!NT_SUCCESS(Status)) {
            InterlockedExchange (&Dev->Running, 0);
            for (ULONG Index = 0; Index < 2; Index++) {
                Dev->Stream[Index].Status = Status;
                KeSetEvent (&Dev->Stream[Index].ReadyEvent, IO_NO_INCREMENT, FALSE);
        KeSetEvent (&Dev->Stream[Index].IdleEvent, IO_NO_INCREMENT, FALSE);
            }
        }
        ExSetTimerResolution (0, FALSE);
    }
    SiaLinkStop (Dev);
    for (ULONG Index = 0; Index < 2; Index++) {
        KeSetEvent (&Dev->Stream[Index].ReadyEvent, IO_NO_INCREMENT, FALSE);
        KeSetEvent (&Dev->Stream[Index].IdleEvent, IO_NO_INCREMENT, FALSE);
    }
    PsTerminateSystemThread (STATUS_SUCCESS);
}

NTSTATUS
SiaHardwareStart (PSIA_DEVICE Dev, PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource)
{
    PHYSICAL_ADDRESS Physical;
    OBJECT_ATTRIBUTES Attributes;
    RtlZeroMemory (Dev, sizeof (*Dev));
    KeInitializeEvent (&Dev->WorkEvent, SynchronizationEvent, FALSE);
    for (ULONG Index = 0; Index < 2; Index++) {
        KeInitializeEvent (&Dev->Stream[Index].IdleEvent, NotificationEvent, TRUE);
        KeInitializeEvent (&Dev->Stream[Index].ReadyEvent, NotificationEvent, FALSE);
        KeInitializeSpinLock (&Dev->Stream[Index].NotifyLock);
    }
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
SiaStreamStop (PSIA_DEVICE Dev, BOOLEAN Capture)
{
    PSIA_PCM Pcm = &Dev->Stream[Capture != FALSE];
    InterlockedAnd (&Dev->Running, ~(1L << (Capture != FALSE)));
    KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
    if (Dev->Thread) KeWaitForSingleObject (&Pcm->IdleEvent, Executive, KernelMode, FALSE, NULL);
    if (Pcm->PcmSize) Pcm->ReadOffset = (ULONG)((ULONGLONG)Pcm->PlayedBytes % Pcm->PcmSize);
}

NTSTATUS
SiaStreamStart (PSIA_DEVICE Dev, BOOLEAN Capture)
{
    PSIA_PCM Pcm = &Dev->Stream[Capture != FALSE];
    LONG Bit = 1L << (Capture != FALSE);
    if (Dev->Stopping || !Dev->Thread || !Pcm->Pcm || !Pcm->PcmSize || Dev->RetainedDma) return STATUS_DEVICE_NOT_READY;
    if (InterlockedCompareExchange (&Dev->Running, 0, 0) & Bit) return STATUS_SUCCESS;
    Pcm->Status = STATUS_PENDING;
    KeClearEvent (&Pcm->IdleEvent);
    KeClearEvent (&Pcm->ReadyEvent);
    InterlockedOr (&Dev->Running, Bit);
    KeSetEvent (&Dev->WorkEvent, IO_NO_INCREMENT, FALSE);
    if(Capture) {
        LARGE_INTEGER Timeout;Timeout.QuadPart=-100000000LL;
        NTSTATUS Status=KeWaitForSingleObject(&Pcm->ReadyEvent,Executive,KernelMode,FALSE,&Timeout);
        if(Status!=STATUS_SUCCESS){SiaStreamStop(Dev,Capture);return STATUS_IO_TIMEOUT;}
        return Pcm->Status;
    }
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
