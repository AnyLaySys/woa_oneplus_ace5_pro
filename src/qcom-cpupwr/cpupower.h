#pragma once
#include <ntifs.h>
#define PEP_KERNEL_INFORMATION_VERSION 3
#include <pepfx.h>
#include "policy.h"

#define SM_CPU_SUSPEND 0xC4000001u

typedef struct { ULONG Count,Words[120]; } SM_REPLY;
typedef struct { ULONG Number; } SM_CORE;
/* SystemProcessorPerformanceInformation: cumulative CPU times in 100 ns. */
typedef struct {
    ULONGLONG Idle,Kernel,User,Reserved[2];
    ULONG Interrupts;
} SM_CPU_TIME;
C_ASSERT(sizeof(SM_CPU_TIME)==48);
NTSYSAPI NTSTATUS NTAPI ZwQuerySystemInformation(ULONG information,PVOID buffer,ULONG length,PULONG returned);
typedef struct {
    PUCHAR Mailbox,Doorbell;
    ULONG Token;
    FAST_MUTEX ScmiLock;
    SM_CORE Core[8];
    volatile LONG RegisteredMask,Fault,Stopping,Retention;
    ULONG NominalKhz[2];
    volatile LONG Applied[2];
    volatile LONG Ceiling[2];
    ULONG Busy[2];
    SM_CPU_TIME Previous[8];
    ULONGLONG LastSample;
    volatile LONG64 LastRequest[2];
    KEVENT Wake;
    PETHREAD Thread;
    BOOLEAN ShutdownRegistered;
    PUCHAR Thermal[2];
    volatile LONG CoolingBand[2];
    volatile LONG SweetBand[2],TemperatureAverage[2];
    volatile LONG Temperature[2];
} SM_CONTEXT;
extern SM_CONTEXT Sm;

NTSTATUS SmScmiInitialize(VOID);
VOID SmScmiClose(VOID);
NTSTATUS SmScmiRequest(ULONG command,ULONG domain,ULONG index,SM_REPLY *reply);
NTSTATUS SmScmiSet(ULONG domain,ULONG index);
LONG SmPsci(ULONG function,ULONG state);
VOID SmRequestLevel(ULONG domain,ULONG ceiling);
VOID SmWake(VOID);
VOID SmThermalInitialize(VOID);
VOID SmThermalClose(VOID);
VOID SmThermalUpdate(VOID);
ULONG SmEffectiveCeiling(ULONG domain);
NTSTATUS SmWmiRegister(PDEVICE_OBJECT device,PDEVICE_OBJECT physical);
DRIVER_DISPATCH SmWmiControl;
DRIVER_DISPATCH SmForward;
PEPCALLBACKNOTIFYDPM SmDeviceNotification;
PEPCALLBACKNOTIFYPPM SmProcessorNotification;
