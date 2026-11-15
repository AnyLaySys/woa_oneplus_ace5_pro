#ifndef HUMMER_RPMH_H
#define HUMMER_RPMH_H

#include <ntddk.h>


typedef struct _HRPMH
{
    PUCHAR Rsc;
    PUCHAR Db;
    ULONG  Major;

    ULONG  SBase;
    ULONG  HBase;

    BOOLEAN Initialized;
    BOOLEAN Faulted;
} HRPMH, *PHRPMH;

NTSTATUS
HrpmhInit (
    _Out_ PHRPMH Ctx,
    _In_  PUCHAR Rsc,
    _In_  PUCHAR CmdDb
    );

NTSTATUS
HrpmhVoteLdo (
    _In_ PHRPMH Ctx,
    _In_ ULONG  Id,
    _In_ ULONG  MilliVolts
    );

#endif
