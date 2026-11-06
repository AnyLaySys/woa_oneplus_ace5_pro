#pragma once

#define QCB_POWER_EXCHANGE CTL_CODE(FILE_DEVICE_BATTERY, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define QCB_POWER_MAX_AGE_MS 10000ull

typedef struct _QCB_SOURCE {
    ULONGLONG TimeMs;
    ULONG Valid;
    ULONG Online;
} QCB_SOURCE;

typedef struct _QCB_POWER {
    ULONGLONG SampleTimeMs;
    ULONG Valid;
    ULONG Soc;
    ULONG TemperatureDeciK;
    ULONG VoltageMv;
    LONG CurrentMa;
    ULONG RemainingMah;
    ULONG FullMah;
    ULONG DesignMah;
    ULONG CycleCount;
    ULONG Online;
} QCB_POWER;

C_ASSERT(sizeof(QCB_SOURCE) == 16);
C_ASSERT(sizeof(QCB_POWER) == 48);
