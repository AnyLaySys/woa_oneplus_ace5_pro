#pragma once
#include <ntifs.h>
#include <wdmsec.h>
#define PEP_KERNEL_INFORMATION_VERSION 3
#include <pepfx.h>
#include "policy.h"

#define SM_MAGIC 0x50434d53u
#define SM_READ CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_BUFFERED,FILE_READ_ACCESS)
#define SM_CPU_SUSPEND 0xC4000001u

typedef struct {
    ULONG Magic,Version,Size,Flags;
    ULONG RegisteredMask,MinIndex[2],MaxIndex[2],DesiredIndex[2],AppliedIndex[2];
    LONG ScmiStatus[2];
    ULONG Retention,Busy[2],Khz[2];
} SM_REPORT;
C_ASSERT(sizeof(SM_REPORT)==80);

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
    SM_CORE Core[8];
    volatile LONG RegisteredMask,Fault,Stopping,Retention;
    ULONG NominalKhz[2];
    volatile LONG Desired[2],Applied[2],LastStatus[2];
    volatile LONG Ceiling[2];
    ULONG Busy[2];
    SM_CPU_TIME Previous[8];
    ULONGLONG LastSample;
    KEVENT Wake;
    PETHREAD Thread;
    BOOLEAN LinkCreated,ShutdownRegistered;
} SM_CONTEXT;
extern SM_CONTEXT Sm;

NTSTATUS SmScmiInitialize(VOID);
VOID SmScmiClose(VOID);
NTSTATUS SmScmiRequest(ULONG command,ULONG domain,ULONG index,SM_REPLY *reply);
NTSTATUS SmScmiSet(ULONG domain,ULONG index);
LONG SmPsci(ULONG function,ULONG state);
VOID SmRequestLevel(ULONG domain,ULONG ceiling);
VOID SmWake(VOID);
PEPCALLBACKNOTIFYDPM SmDeviceNotification;
PEPCALLBACKNOTIFYPPM SmProcessorNotification;
