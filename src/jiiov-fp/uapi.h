#pragma once

#define JFP_VERSION 1u
#define JFP_QUERY CTL_CODE(0x44, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define JFP_INITIALIZE CTL_CODE(0x44, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_FLAG_CLOCK 1u
#define JFP_FLAG_SPI 2u
#define JFP_FLAG_FIFO 4u
#define JFP_FLAG_POWER 8u
#define JFP_FLAG_CHIP 16u
#define JFP_FLAG_REGISTER_IO 32u

typedef struct {
    ULONG Size;
    ULONG Version;
    LONG Status;
    ULONG Flags;
    ULONG SpiAddress;
    ULONG ClockVote;
    ULONG ClockState;
    ULONG Firmware;
    ULONG InterfaceDisable;
    ULONG HardwareParameters;
    ULONG SequencerStatus;
    ULONG IrqStatus;
    ULONG ResetConfig;
    ULONG ResetValue;
    ULONG Pins[4];
    ULONG PowerAddress;
    ULONG ChipId;
    ULONG TransferCount;
    ULONG RegisterAddress;
    LONG RegisterStatus;
    ULONGLONG ScmReturn[4];
    UCHAR LastTx[4];
    UCHAR LastRx[4];
} JFP_DIAGNOSTICS;
