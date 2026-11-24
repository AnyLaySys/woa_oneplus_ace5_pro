#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "../syna-touch/drv/hummer_rpmh.h"
#include "uapi.h"

typedef struct {
    PUCHAR Memory;
    ULONGLONG Bridge;
    ULONG Client;
    ULONG Service;
    ULONG App;
    ULONG Region;
    ULONG StorageService;
    ULONG StorageObject;
    ULONG StorageMemory;
    ULONG StorageOffset;
    ULONG StorageSize;
    ULONG FileObject;
    ULONG FileMemory;
    ULONG FileOffset;
    ULONG FileSize;
    ULONG FileError;
    PVOID Files[16];
    ULONG ImageOffset;
    ULONG ImageCapacity;
    ULONG MessageSize;
    ULONG DataOffset;
    ULONG DataSize;
    ULONG NextObject;
    ULONG Objects[32];
    ULONG ObjectKinds[32];
    ULONGLONG ResumeArgs[8];
    ULONGLONG ResumeReturn[8];
    ULONG AuditSequence;
    BOOLEAN AuditEnabled;
    BOOLEAN Shared;
    BOOLEAN Pending;
    BOOLEAN VerifyStarted;
    JFP_TEE_DIAGNOSTICS Info;
} JFP_TEE;

typedef struct {
    WDFDEVICE Device;
    WDFFILEOBJECT SessionOwner;
    PUCHAR Se;
    PUCHAR Gcc;
    PUCHAR Pins;
    PUCHAR Rsc;
    PUCHAR Db;
    HRPMH Power;
    JFP_TEE Tee;
    JFP_DIAGNOSTICS Info;
    ULONG SavedPower[3];
    BOOLEAN PowerOwned;
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
NTSTATUS JfpPowerOff(JFP_DEVICE *device);
NTSTATUS JfpSensorOn(JFP_DEVICE *device);
NTSTATUS JfpSensorClockOn(JFP_DEVICE *device);
NTSTATUS JfpSensorOff(JFP_DEVICE *device);
NTSTATUS JfpQteeTrace(JFP_TEE *tee, ULONG phase, const ULONGLONG *registers, NTSTATUS result);
NTSTATUS JfpQteeTaLog(JFP_TEE *tee, const UCHAR *data, ULONG size);
NTSTATUS JfpQteeStorage(JFP_TEE *tee, BOOLEAN normal);
VOID JfpQteeStorageClose(JFP_TEE *tee);
NTSTATUS JfpQteeStoragePrepare(VOID);
NTSTATUS JfpQteeProbe(JFP_DEVICE *device, ULONG uid);
NTSTATUS JfpQteeSpiServiceProbe(JFP_DEVICE *device);
NTSTATUS JfpQteeLoadProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size);
NTSTATUS JfpQteeKeymasterProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size, ULONG operation);
NTSTATUS JfpQteeSpiProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size, ULONG operation);
NTSTATUS JfpQteeRelease(JFP_DEVICE *device);
NTSTATUS JfpQteeContinue(JFP_DEVICE *device);
NTSTATUS JfpQteeJvProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size);
NTSTATUS JfpQteeConfigProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size);
NTSTATUS JfpQteeCaptureProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize);
NTSTATUS JfpQteeFeatureProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize);
NTSTATUS JfpQteeInitProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize);
NTSTATUS JfpQteeSessionBegin(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize);
NTSTATUS JfpQteeSessionSample(JFP_DEVICE *device);
NTSTATUS JfpQteeSessionEnd(JFP_DEVICE *device);
NTSTATUS JfpQteeEnrollPreflight(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize);
VOID JfpScmCall(ULONGLONG *registers);
NTSTATUS JfpReadChipId(JFP_DEVICE *device);
NTSTATUS JfpTransfer(JFP_DEVICE *device, const UCHAR *tx, UCHAR *rx, ULONG size);
ULONG JfpRegisterRead(JFP_DEVICE *device, ULONG offset);
VOID JfpRegisterWrite(JFP_DEVICE *device, ULONG offset, ULONG value);
