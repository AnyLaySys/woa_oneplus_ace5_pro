#include <ntifs.h>
#include "battery.h"

#define BAT_TLMM_FUNC_GPIO      0u
#define BAT_TLMM_OE             (1u << 9)
#define BAT_TLMM_IN             (1u << 0)
#define BAT_I2C_DELAY_US        10u
#define BAT_STRETCH_TRIES       500u

static VOID BatBusRecovery (_In_ PBAT_DEVICE Dev);

static __inline ULONG BatRd (PUCHAR Base, ULONG Offset)
{
    return READ_REGISTER_ULONG ((volatile ULONG *)(Base + Offset));
}

static __inline VOID BatWr (PUCHAR Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Base + Offset), Value);
    KeMemoryBarrier ();
}

static __inline VOID BatDelay (VOID)
{
    KeStallExecutionProcessor (BAT_I2C_DELAY_US);
}

static __inline VOID BatPinIdle (_In_ PBAT_DEVICE Dev, _In_ ULONG Pin)
{
    BatWr (Dev->Tlmm, Pin * BAT_TLMM_STRIDE, BAT_TLMM_FUNC_GPIO);
}

static __inline VOID BatPinLow (_In_ PBAT_DEVICE Dev, _In_ ULONG Pin)
{
    BatWr (Dev->Tlmm, Pin * BAT_TLMM_STRIDE + 4, 0);
    BatWr (Dev->Tlmm, Pin * BAT_TLMM_STRIDE, BAT_TLMM_FUNC_GPIO | BAT_TLMM_OE);
}

static __inline ULONG BatPinGet (_In_ PBAT_DEVICE Dev, _In_ ULONG Pin)
{
    return BatRd (Dev->Tlmm, Pin * BAT_TLMM_STRIDE + 4) & BAT_TLMM_IN;
}

static __inline VOID BatSclRelease (_In_ PBAT_DEVICE Dev)
{
    ULONG Loop;

    BatPinIdle (Dev, BAT_GPIO_SCL);
    for (Loop = 0; Loop < BAT_STRETCH_TRIES && BatPinGet (Dev, BAT_GPIO_SCL) == 0; Loop++) {
        KeStallExecutionProcessor (10);
    }
}

NTSTATUS
BatI2cInit (
    _In_ PBAT_DEVICE Dev
    )
{
    ULONG SdaIdle;
    ULONG SclIdle;
    ULONG SdaLow;
    ULONG SclLow;

    if (Dev->Tlmm == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatPinIdle (Dev, BAT_GPIO_SCL);
    KeStallExecutionProcessor (2000);

    BatBusRecovery (Dev);

    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatPinIdle (Dev, BAT_GPIO_SCL);
    BatDelay ();
    SdaIdle = BatPinGet (Dev, BAT_GPIO_SDA) ? 1u : 0u;
    SclIdle = BatPinGet (Dev, BAT_GPIO_SCL) ? 1u : 0u;

    BatPinLow (Dev, BAT_GPIO_SDA);
    BatPinLow (Dev, BAT_GPIO_SCL);
    BatDelay ();
    SdaLow = BatPinGet (Dev, BAT_GPIO_SDA) ? 1u : 0u;
    SclLow = BatPinGet (Dev, BAT_GPIO_SCL) ? 1u : 0u;

    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatPinIdle (Dev, BAT_GPIO_SCL);
    BatDelay ();

    if (SdaIdle == 0 || SclIdle == 0) {
        return STATUS_IO_DEVICE_ERROR;
    }

    Dev->I2cReady = TRUE;
    return STATUS_SUCCESS;
}

static VOID
BatI2cStart (
    _In_ PBAT_DEVICE Dev
    )
{
    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatSclRelease (Dev);
    BatDelay ();
    BatPinLow (Dev, BAT_GPIO_SDA);
    BatDelay ();
    BatPinLow (Dev, BAT_GPIO_SCL);
    BatDelay ();
}

static VOID
BatI2cStop (
    _In_ PBAT_DEVICE Dev
    )
{
    BatPinLow (Dev, BAT_GPIO_SDA);
    BatDelay ();
    BatSclRelease (Dev);
    BatDelay ();
    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatDelay ();
}

static VOID
BatBusRecovery (
    _In_ PBAT_DEVICE Dev
    )
{
    ULONG Clocks;

    BatPinIdle (Dev, BAT_GPIO_SDA);
    for (Clocks = 0; Clocks < 9 && BatPinGet (Dev, BAT_GPIO_SDA) == 0; Clocks++) {
        BatPinLow (Dev, BAT_GPIO_SCL);
        BatDelay ();
        BatPinIdle (Dev, BAT_GPIO_SCL);
        BatDelay ();
    }
    if (Clocks != 0) {
        BatI2cStop (Dev);
        
    }
}

static VOID
BatI2cSendBit (
    _In_ PBAT_DEVICE Dev,
    _In_ ULONG High
    )
{
    if (High != 0) {
        BatPinIdle (Dev, BAT_GPIO_SDA);
    } else {
        BatPinLow (Dev, BAT_GPIO_SDA);
    }
    BatDelay ();
    BatSclRelease (Dev);
    BatDelay ();
    BatPinLow (Dev, BAT_GPIO_SCL);
    BatDelay ();
}

static BOOLEAN
BatI2cSendByte (
    _In_ PBAT_DEVICE Dev,
    _In_ UCHAR Byte
    )
{
    ULONG Index;
    BOOLEAN Nak;

    for (Index = 0; Index < 8; Index++) {
        BatI2cSendBit (Dev, (Byte >> (7 - Index)) & 1u);
    }
    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatDelay ();
    BatSclRelease (Dev);
    BatDelay ();
    Nak = BatPinGet (Dev, BAT_GPIO_SDA) != 0;
    BatPinLow (Dev, BAT_GPIO_SCL);
    BatDelay ();
    return Nak;
}

static UCHAR
BatI2cRecvByte (
    _In_ PBAT_DEVICE Dev,
    _In_ BOOLEAN Ack
    )
{
    ULONG Index;
    UCHAR Value = 0;

    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatDelay ();
    for (Index = 0; Index < 8; Index++) {
        BatSclRelease (Dev);
        BatDelay ();
        Value = (UCHAR)((Value << 1) | (BatPinGet (Dev, BAT_GPIO_SDA) != 0 ? 1u : 0u));
        BatPinLow (Dev, BAT_GPIO_SCL);
        BatDelay ();
    }
    if (Ack) {
        BatPinLow (Dev, BAT_GPIO_SDA);
    } else {
        BatPinIdle (Dev, BAT_GPIO_SDA);
    }
    BatDelay ();
    BatSclRelease (Dev);
    BatDelay ();
    BatPinLow (Dev, BAT_GPIO_SCL);
    BatDelay ();
    BatPinIdle (Dev, BAT_GPIO_SDA);
    return Value;
}

NTSTATUS
BatI2cReadWord (
    _In_ PBAT_DEVICE Dev,
    _In_ UCHAR Address,
    _In_ UCHAR Reg,
    _Out_ PUSHORT Value
    )
{
    UCHAR Low;
    UCHAR High;

    *Value = 0;
    if (!Dev->I2cReady) {
        return STATUS_DEVICE_NOT_READY;
    }

    BatBusRecovery (Dev);

    BatI2cStart (Dev);
    if (BatI2cSendByte (Dev, (UCHAR)(Address << 1)) || BatI2cSendByte (Dev, Reg)) {
        BatI2cStop (Dev);
        return STATUS_IO_DEVICE_ERROR;
    }
    BatI2cStart (Dev);
    if (BatI2cSendByte (Dev, (UCHAR)((Address << 1) | 1u))) {
        BatI2cStop (Dev);
        return STATUS_IO_DEVICE_ERROR;
    }
    Low = BatI2cRecvByte (Dev, TRUE);
    High = BatI2cRecvByte (Dev, FALSE);
    BatI2cStop (Dev);

    *Value = (USHORT)(Low | (High << 8));
    return STATUS_SUCCESS;
}

static BOOLEAN
BatGaugeRead (
    _In_ PBAT_DEVICE Dev,
    _In_ UCHAR Reg,
    _Out_ PULONG Value
    )
{
    USHORT Raw = 0;

    NTSTATUS Status = BatI2cReadWord (Dev, BAT_GAUGE_ADDRESS, Reg, &Raw);
    *Value = NT_SUCCESS (Status) ? Raw : 0;
    return NT_SUCCESS (Status);
}

BOOLEAN
BatGaugeSample (
    _In_ PBAT_DEVICE Dev
    )
{
    BAT_SNAPSHOT S;
    USHORT Volt = 0;
    NTSTATUS Status;
    KIRQL OldIrql;
    BOOLEAN Changed;
    BOOLEAN Complete = TRUE;

    if (!Dev->I2cReady) {
        return FALSE;
    }

    RtlZeroMemory (&S, sizeof (S));
    Status = BatI2cReadWord (Dev, BAT_GAUGE_ADDRESS, BQ_REG_VOLT, &Volt);
    if (!NT_SUCCESS (Status)) {
        
        goto Store;
    }

    S.VoltageMv = Volt;
    Complete &= BatGaugeRead (Dev, BQ_REG_TEMP, &S.TempDeciK);
    Complete &= BatGaugeRead (Dev, BQ_REG_SOC, &S.Soc);
    Complete &= BatGaugeRead (Dev, BQ_REG_REMAIN, &S.RemainingMah);
    Complete &= BatGaugeRead (Dev, BQ_REG_FCC, &S.FullMah);
    Complete &= BatGaugeRead (Dev, BQ_REG_CYCLES, &S.CycleCount);
    Complete &= BatGaugeRead (Dev, BQ_REG_DESIGN, &S.DesignMah);
    {
        USHORT Current = 0;
        Status = BatI2cReadWord (Dev, BAT_GAUGE_ADDRESS, BQ_REG_CURRENT, &Current);
        S.CurrentMa = (LONG)(SHORT)Current;
    }

    S.Valid = Complete && NT_SUCCESS (Status) && S.TempDeciK >= 2332 && S.TempDeciK <= 3532 &&
              S.VoltageMv >= 5000 && S.VoltageMv <= 10000 && S.Soc <= 100 && S.FullMah != 0;
    if (S.Valid) S.SampleTimeMs = KeQueryInterruptTime () / 10000;

Store:
    KeAcquireSpinLock (&Dev->Lock, &OldIrql);
    ULONGLONG Now = KeQueryInterruptTime () / 10000;
    S.Online = Dev->Source.Valid && Dev->Source.Online && Dev->Source.TimeMs <= Now &&
               Now - Dev->Source.TimeMs <= QCB_POWER_MAX_AGE_MS;
    Changed = (S.Online != Dev->Now.Online) || (S.Valid != Dev->Now.Valid) || (S.Soc != Dev->Now.Soc) ||
              (S.VoltageMv != Dev->Now.VoltageMv) || (S.RemainingMah != Dev->Now.RemainingMah) ||
              (S.FullMah != Dev->Now.FullMah) ||
              (S.CurrentMa != Dev->Now.CurrentMa);
    Dev->Now = S;
    KeReleaseSpinLock (&Dev->Lock, OldIrql);

    return Changed;
}

VOID
BatPollThread (
    _In_ PVOID Context
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)Context;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;
    ULONG Slice;

    for (;;) {
        if (BatGaugeSample (Dev)) {
            if (Dev->ClassData == NULL && Dev->Now.Valid) {
                Status = BatMiniportInit (Dev);
                
            }
            if (Dev->ClassData != NULL) {
                (VOID)BatteryClassStatusNotify (Dev->ClassData);
            }
        }
        
        for (Slice = 0; Slice < BAT_POLL_MS / BAT_SLICE_MS; Slice++) {
            BatPendingDrain (Dev, STATUS_INVALID_DEVICE_REQUEST);
            Timeout.QuadPart = -10000LL * BAT_SLICE_MS;
            KeWaitForSingleObject (&Dev->StopEvent, Executive, KernelMode, FALSE, &Timeout);
            if (InterlockedCompareExchange (&Dev->Stopping, 0, 0) != 0) {
                break;
            }
        }
        if (InterlockedCompareExchange (&Dev->Stopping, 0, 0) != 0) {
            break;
        }
    }
    BatPendingDrain (Dev, STATUS_CANCELLED);
    
    PsTerminateSystemThread (STATUS_SUCCESS);
}

static NTSTATUS
BatQueryTag (
    _In_ PVOID Context,
    _Out_ PULONG BatteryTag
    )
{
    UNREFERENCED_PARAMETER (Context);

    *BatteryTag = BAT_BATTERY_TAG;
    return STATUS_SUCCESS;
}

static NTSTATUS
BatQueryStatus (
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _Out_ PBATTERY_STATUS Status
    )
{
    PBAT_DEVICE Dev = (PBAT_DEVICE)Context;
    BAT_SNAPSHOT S;

    UNREFERENCED_PARAMETER (BatteryTag);

    RtlZeroMemory (Status, sizeof (*Status));
    BatSnapshotGet (Dev, &S);
    Status->PowerState = S.Online ? BATTERY_POWER_ON_LINE : 0;
    if (!S.Valid) {
        Status->Capacity = BATTERY_UNKNOWN_CAPACITY;
        Status->Rate = BATTERY_UNKNOWN_CURRENT;
        return STATUS_SUCCESS;
    }

    if (S.CurrentMa < 0) Status->PowerState |= BATTERY_DISCHARGING;
    if (S.CurrentMa > 0) Status->PowerState |= BATTERY_CHARGING;
    if (S.Soc <= 5) {
        Status->PowerState |= BATTERY_CRITICAL;
    }

    Status->Capacity = S.RemainingMah;
    Status->Voltage = S.VoltageMv;
    Status->Rate = S.CurrentMa;
    
    return STATUS_SUCCESS;
}

static NTSTATUS
BatQueryInformation (
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_QUERY_INFORMATION_LEVEL Level,
    _In_ LONG AtRate,
    _Out_writes_bytes_to_ (BufferLength, *ReturnedLength) PVOID Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG ReturnedLength
    )
{
    static const WCHAR DeviceName[] = BAT_DEVICE_NAME;
    PBAT_DEVICE Dev = (PBAT_DEVICE)Context;
    BAT_SNAPSHOT S;
    PBATTERY_INFORMATION Info;
    PBATTERY_REPORTING_SCALE Scale;

    UNREFERENCED_PARAMETER (BatteryTag);
    UNREFERENCED_PARAMETER (AtRate);

    *ReturnedLength = 0;
    BatSnapshotGet (Dev, &S);

    switch (Level) {
    case BatteryInformation:
        if (BufferLength < sizeof (BATTERY_INFORMATION)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        Info = (PBATTERY_INFORMATION)Buffer;
        RtlZeroMemory (Info, sizeof (*Info));
        Info->Capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
        Info->Technology = 1;
        Info->Chemistry[0] = 'L';
        Info->Chemistry[1] = 'I';
        Info->Chemistry[2] = 'O';
        Info->Chemistry[3] = 'N';
        Info->DesignedCapacity = S.Valid ? S.DesignMah : BATTERY_UNKNOWN_CAPACITY;
        Info->FullChargedCapacity = S.Valid ? S.FullMah : BATTERY_UNKNOWN_CAPACITY;
        Info->DefaultAlert1 = S.DesignMah / 10;
        Info->DefaultAlert2 = S.DesignMah / 20;
        Info->CycleCount = S.CycleCount;
        *ReturnedLength = sizeof (*Info);
        
        return STATUS_SUCCESS;

    case BatteryGranularityInformation:
        if (BufferLength < sizeof (BATTERY_REPORTING_SCALE)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        Scale = (PBATTERY_REPORTING_SCALE)Buffer;
        Scale->Granularity = 1;
        Scale->Capacity = S.Valid ? S.FullMah : BATTERY_UNKNOWN_CAPACITY;
        *ReturnedLength = sizeof (*Scale);
        return STATUS_SUCCESS;

    case BatteryTemperature:
        if (BufferLength < sizeof (ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)Buffer = S.TempDeciK;
        *ReturnedLength = sizeof (ULONG);
        return STATUS_SUCCESS;

    case BatteryEstimatedTime:
        if (BufferLength < sizeof (ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)Buffer = BATTERY_UNKNOWN_TIME;
        *ReturnedLength = sizeof (ULONG);
        return STATUS_SUCCESS;

    case BatteryDeviceName:
        if (BufferLength < sizeof (DeviceName)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlCopyMemory (Buffer, DeviceName, sizeof (DeviceName));
        *ReturnedLength = sizeof (DeviceName);
        return STATUS_SUCCESS;

    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS
BatSetInformation (
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ BATTERY_SET_INFORMATION_LEVEL Level,
    _In_opt_ PVOID Buffer
    )
{
    UNREFERENCED_PARAMETER (Context);
    UNREFERENCED_PARAMETER (BatteryTag);
    UNREFERENCED_PARAMETER (Buffer);

    return Level == BatteryCriticalBias ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static NTSTATUS
BatSetStatusNotify (
    _In_ PVOID Context,
    _In_ ULONG BatteryTag,
    _In_ PBATTERY_NOTIFY Notify
    )
{
    UNREFERENCED_PARAMETER (Context);
    UNREFERENCED_PARAMETER (BatteryTag);
    UNREFERENCED_PARAMETER (Notify);

    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
BatDisableStatusNotify (
    _In_ PVOID Context
    )
{
    UNREFERENCED_PARAMETER (Context);

    return STATUS_SUCCESS;
}

NTSTATUS
BatMiniportInit (
    _In_ PBAT_DEVICE Dev
    )
{
    RtlZeroMemory (&Dev->Info, sizeof (Dev->Info));
    Dev->Info.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    Dev->Info.MinorVersion = BATTERY_CLASS_MINOR_VERSION_1;
    Dev->Info.Context = Dev;
    Dev->Info.QueryTag = BatQueryTag;
    Dev->Info.QueryInformation = BatQueryInformation;
    Dev->Info.SetInformation = BatSetInformation;
    Dev->Info.QueryStatus = BatQueryStatus;
    Dev->Info.SetStatusNotify = BatSetStatusNotify;
    Dev->Info.DisableStatusNotify = BatDisableStatusNotify;
    Dev->Info.Pdo = Dev->Pdo;
    Dev->Info.Fdo = Dev->Self;
    RtlInitUnicodeString (&Dev->DeviceName, BAT_DEVICE_NAME);
    Dev->Info.DeviceName = &Dev->DeviceName;

    return BatteryClassInitializeDevice ((PBATTERY_MINIPORT_INFO)&Dev->Info, &Dev->ClassData);
}

VOID
BatMiniportStop (
    _In_ PBAT_DEVICE Dev
    )
{
    if (Dev->ClassData != NULL) {
        (VOID)BatteryClassUnload (Dev->ClassData);
        Dev->ClassData = NULL;
    }
}
