#pragma once
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t UCHAR;
typedef uint16_t USHORT;
typedef UCHAR *PUCHAR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef int32_t NTSTATUS;
typedef void VOID;
typedef void *PVOID;
typedef int BOOLEAN;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
typedef LARGE_INTEGER PHYSICAL_ADDRESS;

#define TRUE 1
#define FALSE 0
#define KernelMode 0
#define MAXULONG UINT32_MAX
#define MAXLONGLONG INT64_MAX
#define MmNonCached 0
#define KeMemoryBarrier() __sync_synchronize()
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xc000009c)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xc00000b5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_DEVICE_PROTOCOL_ERROR ((NTSTATUS)0xc0000186)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bb)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_DATATYPE_MISALIGNMENT ((NTSTATUS)0x80000002)
#define NT_SUCCESS(x) ((NTSTATUS)(x) >= 0)
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define RtlZeroMemory(p,n) memset((p), 0, (n))
#define RtlCopyMemory(d,s,n) memcpy((d), (s), (n))

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
    JFP_DIAGNOSTICS Info;
    JFP_TEE Tee;
    ULONG SavedPower[3];
    BOOLEAN RegistersOwned;
    BOOLEAN Faulted;
} JFP_DEVICE;

ULONG JfpRegisterRead(JFP_DEVICE *device, ULONG offset);
VOID JfpRegisterWrite(JFP_DEVICE *device, ULONG offset, ULONG value);
ULONGLONG KeQueryInterruptTime(void);
VOID KeStallExecutionProcessor(ULONG microseconds);
NTSTATUS KeDelayExecutionThread(int mode, BOOLEAN alertable, LARGE_INTEGER *delay);
NTSTATUS JfpTransfer(JFP_DEVICE *device, const UCHAR *tx, UCHAR *rx, ULONG size);
NTSTATUS JfpReadChipId(JFP_DEVICE *device);
PUCHAR MmAllocateContiguousMemorySpecifyCache(ULONG size, PHYSICAL_ADDRESS low,
    PHYSICAL_ADDRESS high, PHYSICAL_ADDRESS boundary, int cache);
VOID MmFreeContiguousMemory(PUCHAR memory);
PHYSICAL_ADDRESS MmGetPhysicalAddress(PUCHAR memory);
VOID JfpScmCall(ULONGLONG *registers);
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
