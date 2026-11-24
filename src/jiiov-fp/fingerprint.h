#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "../syna-touch/drv/hummer_rpmh.h"
#include "uapi.h"

typedef struct {
    WDFDEVICE Device;
    PUCHAR Se;
    PUCHAR Gcc;
    PUCHAR Pins;
    PUCHAR Rsc;
    PUCHAR Db;
    HRPMH Power;
    JFP_DIAGNOSTICS Info;
    ULONG SavedClockVote;
    ULONG AddedClockVotes;
    ULONG SavedPins[4];
    ULONG SavedReset[2];
    ULONG SavedRegisters[20];
    BOOLEAN ClockOwned;
    BOOLEAN PinsOwned;
    BOOLEAN RegistersOwned;
    BOOLEAN Powered;
    BOOLEAN Faulted;
} JFP_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(JFP_DEVICE, JfpContext);

NTSTATUS JfpPrepare(JFP_DEVICE *device);
VOID JfpRelease(JFP_DEVICE *device);
NTSTATUS JfpInspect(JFP_DEVICE *device);
NTSTATUS JfpInitialize(JFP_DEVICE *device);
NTSTATUS JfpPowerOn(JFP_DEVICE *device);
NTSTATUS JfpReadChipId(JFP_DEVICE *device);
NTSTATUS JfpTransfer(JFP_DEVICE *device, const UCHAR *tx, UCHAR *rx, ULONG size);
ULONG JfpRegisterRead(JFP_DEVICE *device, ULONG offset);
VOID JfpRegisterWrite(JFP_DEVICE *device, ULONG offset, ULONG value);
