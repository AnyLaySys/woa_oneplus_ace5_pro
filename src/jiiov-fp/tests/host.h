#pragma once
#include <assert.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t UCHAR;
typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef int32_t NTSTATUS;
typedef void VOID;
typedef int BOOLEAN;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;

#define TRUE 1
#define FALSE 0
#define KernelMode 0
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000d)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xc000009c)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xc00000b5)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xc0000185)
#define STATUS_DEVICE_PROTOCOL_ERROR ((NTSTATUS)0xc0000186)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022)
#define NT_SUCCESS(x) ((NTSTATUS)(x) >= 0)
#define RTL_NUMBER_OF(x) (sizeof(x) / sizeof((x)[0]))
#define min(a,b) ((a) < (b) ? (a) : (b))
#define RtlZeroMemory(p,n) memset((p), 0, (n))
#define RtlCopyMemory(d,s,n) memcpy((d), (s), (n))

#include "uapi.h"

typedef struct {
    JFP_DIAGNOSTICS Info;
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
