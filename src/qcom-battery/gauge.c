#include <ntifs.h>
#include "battery.h"

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

    BatPinIdle (Dev, BAT_GPIO_SDA);
    BatPinIdle (Dev, BAT_GPIO_SCL);
    BatDelay ();

    if (SdaIdle == 0 || SclIdle == 0) {
        return STATUS_IO_DEVICE_ERROR;
    }

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
    BatI2cSendBit (Dev, !Ack);
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

// Read-only DAStatus1 query, exactly native0x3e/0x0071. No unseal/dataflash/FET command.
static BOOLEAN BatGaugeCells(PBAT_DEVICE Dev,BAT_SNAPSHOT *S)
{
    UCHAR B[36];ULONG Sum=0;LARGE_INTEGER Wait;
    BatBusRecovery(Dev);BatI2cStart(Dev);
    BOOLEAN Ok=!BatI2cSendByte(Dev,BAT_GAUGE_ADDRESS<<1) && !BatI2cSendByte(Dev,0x3e) &&
        !BatI2cSendByte(Dev,0x71) && !BatI2cSendByte(Dev,0);
    BatI2cStop(Dev);if(!Ok)return FALSE;
    Wait.QuadPart=-10000;KeDelayExecutionThread(KernelMode,FALSE,&Wait);
    BatI2cStart(Dev);
    Ok=!BatI2cSendByte(Dev,BAT_GAUGE_ADDRESS<<1) && !BatI2cSendByte(Dev,0x3e);
    if(Ok){BatI2cStart(Dev);Ok=!BatI2cSendByte(Dev,(BAT_GAUGE_ADDRESS<<1)|1);}
    if(Ok)for(ULONG I=0;I<sizeof(B);I++)B[I]=BatI2cRecvByte(Dev,I+1<sizeof(B));
    BatI2cStop(Dev);if(!Ok || B[0]!=0x71 || B[1] || B[35]<8 || B[35]>36)return FALSE;
    for(ULONG I=0;I<(ULONG)B[35]-2;I++)Sum+=B[I];
    if(((Sum+B[34])&255)!=255)return FALSE;
    S->Cell1Mv=B[2]|((ULONG)B[3]<<8);S->Cell2Mv=B[4]|((ULONG)B[5]<<8);
    if(S->Cell1Mv<2500 || S->Cell1Mv>5000 || S->Cell2Mv<2500 || S->Cell2Mv>5000 ||
       S->Cell1Mv+S->Cell2Mv+100<S->VoltageMv || S->Cell1Mv+S->Cell2Mv>S->VoltageMv+100)return FALSE;
    return BatGaugeRead(Dev,0x0a,&S->BatteryStatus) && BatGaugeRead(Dev,0x30,&S->ChargingVoltageMv) &&
        BatGaugeRead(Dev,0x32,&S->ChargingCurrentMa);
}

BOOLEAN
BatGaugeSample (
    _In_ PBAT_DEVICE Dev
    )
{
    BAT_SNAPSHOT S;
    NTSTATUS Status;
    KIRQL OldIrql;
    BOOLEAN Changed;
    BOOLEAN Complete = TRUE;

    if (!Dev->I2cReady) {
        return FALSE;
    }

    RtlZeroMemory (&S, sizeof (S));
    if (!BatGaugeRead (Dev, BQ_REG_VOLT, &S.VoltageMv)) {

        goto Store;
    }

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

    S.CellValid = BatGaugeCells(Dev,&S);

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

    Timeout.QuadPart = -10000LL * BAT_POLL_MS;
    do {
        if (BatGaugeSample (Dev)) {
            if (Dev->ClassData != NULL) {
                (VOID)BatteryClassStatusNotify (Dev->ClassData);
            }
        }
    } while (KeWaitForSingleObject (&Dev->StopEvent, Executive, KernelMode, FALSE, &Timeout) == STATUS_TIMEOUT);
    PsTerminateSystemThread (STATUS_SUCCESS);
}

static NTSTATUS
BatQueryTag (
    _In_ PVOID Context,
    _Out_ PULONG BatteryTag
    )
{
    BAT_SNAPSHOT snapshot;
    BatSnapshotGet((PBAT_DEVICE)Context,&snapshot);
    *BatteryTag=snapshot.Valid?BAT_BATTERY_TAG:0;
    return snapshot.Valid?STATUS_SUCCESS:STATUS_NO_SUCH_DEVICE;
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

    Status->Capacity = (ULONG)((ULONGLONG)S.RemainingMah * BAT_NOMINAL_MV / 1000);
    Status->Voltage = S.VoltageMv;
    Status->Rate = (LONG)((LONGLONG)S.CurrentMa * S.VoltageMv / 1000);

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
    static const WCHAR Manufacturer[] = L"OnePlus";
    static const WCHAR UniqueId[] = L"ACPI\\HMRB0001\\0";
    static const WCHAR Empty[] = L"";
    PBAT_DEVICE Dev = (PBAT_DEVICE)Context;
    BAT_SNAPSHOT S;
    PBATTERY_INFORMATION Info;
    PBATTERY_REPORTING_SCALE Scale;

    UNREFERENCED_PARAMETER (BatteryTag);
    UNREFERENCED_PARAMETER (AtRate);

    *ReturnedLength = 0;
    BatSnapshotGet (Dev, &S);
    if(!S.Valid)return STATUS_NO_SUCH_DEVICE;

    switch (Level) {
    case BatteryInformation:
        if (BufferLength < sizeof (BATTERY_INFORMATION)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        Info = (PBATTERY_INFORMATION)Buffer;
        RtlZeroMemory (Info, sizeof (*Info));
        Info->Capabilities = BATTERY_SYSTEM_BATTERY;
        Info->Technology = 1;
        Info->Chemistry[0] = 'L';
        Info->Chemistry[1] = 'I';
        Info->Chemistry[2] = 'O';
        Info->Chemistry[3] = 'N';
        Info->DesignedCapacity = S.Valid ? (ULONG)((ULONGLONG)S.DesignMah * BAT_NOMINAL_MV / 1000) : BATTERY_UNKNOWN_CAPACITY;
        Info->FullChargedCapacity = S.Valid ? (ULONG)((ULONGLONG)S.FullMah * BAT_NOMINAL_MV / 1000) : BATTERY_UNKNOWN_CAPACITY;
        Info->DefaultAlert1 = Info->DesignedCapacity / 10;
        Info->DefaultAlert2 = Info->DesignedCapacity / 20;
        Info->CycleCount = S.CycleCount;
        *ReturnedLength = sizeof (*Info);

        return STATUS_SUCCESS;

    case BatteryGranularityInformation:
        if (BufferLength < sizeof (BATTERY_REPORTING_SCALE)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        Scale = (PBATTERY_REPORTING_SCALE)Buffer;
        Scale->Granularity = 1;
        Scale->Capacity = S.Valid ? (ULONG)((ULONGLONG)S.FullMah * BAT_NOMINAL_MV / 1000) : BATTERY_UNKNOWN_CAPACITY;
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
    case BatteryManufactureName:
    case BatteryUniqueID:
    case BatterySerialNumber: {
        const WCHAR *value=Level==BatteryDeviceName?DeviceName:Level==BatteryManufactureName?Manufacturer:
            Level==BatteryUniqueID?UniqueId:Empty;
        ULONG bytes=(ULONG)((wcslen(value)+1)*sizeof(WCHAR));
        if(BufferLength<bytes)return STATUS_BUFFER_TOO_SMALL;
        RtlCopyMemory(Buffer,value,bytes);*ReturnedLength=bytes;
        return STATUS_SUCCESS;
    }

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

    return STATUS_SUCCESS;
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

    return BatteryClassInitializeDevice ((PBATTERY_MINIPORT_INFO)&Dev->Info, &Dev->ClassData);
}

VOID
BatMiniportStop (
    _In_ PBAT_DEVICE Dev
    )
{
    if (Dev->ClassData != NULL) {
        if(Dev->WmiRegistered){IoWMIRegistrationControl(Dev->Self,WMIREG_ACTION_DEREGISTER);Dev->WmiRegistered=FALSE;}
        (VOID)BatteryClassUnload (Dev->ClassData);
        Dev->ClassData = NULL;
    }
}
