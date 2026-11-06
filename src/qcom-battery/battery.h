#pragma once

#include <ntddk.h>
#include <batclass.h>
#include "power.h"

#define BAT_GAUGE_ADDRESS       0x55u
#define BAT_GPIO_SDA            66u
#define BAT_GPIO_SCL            67u

#define BAT_TLMM_PHYS           0x0F100000ull
#define BAT_TLMM_SIZE           0x000A6000u

#define BAT_TLMM_STRIDE         0x1000u
#define BAT_TLMM_FUNC_GPIO      0u

#define BQ_REG_TEMP             0x06u
#define BQ_REG_VOLT             0x08u
#define BQ_REG_REMAIN           0x10u
#define BQ_REG_FCC              0x12u
#define BQ_REG_CURRENT          0x14u
#define BQ_REG_CYCLES           0x2Au
#define BQ_REG_SOC              0x2Cu
#define BQ_REG_DESIGN           0x3Cu

#define BAT_POLL_MS             5000u
#define BAT_SLICE_MS            250u
#define BAT_PENDING_MAX          32u
#define BAT_BATTERY_TAG         1u
#define BAT_DEVICE_NAME         L"OnePlus Ace 5 Pro Battery"

typedef struct _BAT_SNAPSHOT
{
    BOOLEAN Valid;
    BOOLEAN Online;
    ULONGLONG SampleTimeMs;
    ULONG   Soc;
    ULONG   VoltageMv;
    ULONG   RemainingMah;
    ULONG   FullMah;
    ULONG   DesignMah;
    ULONG   CycleCount;
    ULONG   TempDeciK;
    LONG    CurrentMa;
} BAT_SNAPSHOT;

typedef struct _BAT_DEVICE
{
    PDEVICE_OBJECT             Self;
    PDEVICE_OBJECT             Lower;
    PDEVICE_OBJECT             Pdo;
    PVOID                      ClassData;
    BATTERY_MINIPORT_INFO_V1_1 Info;
    UNICODE_STRING             DeviceName;
    PUCHAR                     Tlmm;
    HANDLE                     Thread;
    KEVENT                     StopEvent;
    volatile LONG              Stopping;
    BOOLEAN                    I2cReady;
    KSPIN_LOCK                 Lock;
    KSPIN_LOCK                 PendingLock;
    PIRP                       Pending[BAT_PENDING_MAX];
    BAT_SNAPSHOT               Now;
    QCB_SOURCE                 Source;
} BAT_DEVICE, *PBAT_DEVICE;

VOID
BatSnapshotGet (
    _In_ PBAT_DEVICE Dev,
    _Out_ BAT_SNAPSHOT *Snapshot
    );

BOOLEAN
BatGaugeSample (
    _In_ PBAT_DEVICE Dev
    );

NTSTATUS
BatI2cInit (
    _In_ PBAT_DEVICE Dev
    );

BOOLEAN
BatPendingAdd (
    _In_ PBAT_DEVICE Dev,
    _In_ PIRP Irp
    );

VOID
BatPendingDrain (
    _In_ PBAT_DEVICE Dev,
    _In_ NTSTATUS Status
    );

NTSTATUS
BatI2cReadWord (
    _In_ PBAT_DEVICE Dev,
    _In_ UCHAR Address,
    _In_ UCHAR Reg,
    _Out_ PUSHORT Value
    );

NTSTATUS
BatMiniportInit (
    _In_ PBAT_DEVICE Dev
    );

VOID
BatMiniportStop (
    _In_ PBAT_DEVICE Dev
    );

VOID
BatPollThread (
    _In_ PVOID Context
    );

NTSTATUS
BatHardwareInit (
    _In_ PBAT_DEVICE Dev
    );

NTSTATUS
BatHardwareStart (
    _In_ PBAT_DEVICE Dev
    );

VOID
BatHardwareStop (
    _In_ PBAT_DEVICE Dev
    );
