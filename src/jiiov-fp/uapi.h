#pragma once

#define JFP_VERSION 8u
#define JFP_CONFIG_COUNT 11u
#define JFP_QUERY CTL_CODE(0x44, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS)
#define JFP_INITIALIZE CTL_CODE(0x44, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_TEE_PROBE CTL_CODE(0x44, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_TA_PROBE CTL_CODE(0x44, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_CAPTURE_PROBE CTL_CODE(0x44, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_INIT_PROBE CTL_CODE(0x44, 0x807, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_SPI_SERVICE_PROBE CTL_CODE(0x44, 0x808, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_FEATURE_PROBE CTL_CODE(0x44, 0x809, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_SESSION_BEGIN CTL_CODE(0x44, 0x80a, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_SESSION_SAMPLE CTL_CODE(0x44, 0x80b, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_SESSION_END CTL_CODE(0x44, 0x80c, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_SESSION_STATUS CTL_CODE(0x44, 0x80d, METHOD_BUFFERED, FILE_READ_ACCESS)
#define JFP_ENROLL_PREFLIGHT CTL_CODE(0x44, 0x80e, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_TEE_CONTINUE CTL_CODE(0x44, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define JFP_TA_MAX_IMAGE 262144u
#define JFP_KM_MAX_IMAGE 1048576u
#define JFP_JV_MAX_IMAGE 16777216u
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

typedef struct {
    ULONG Id;
    ULONG Size;
    LONG Status;
    LONG Result;
    ULONG Data[32];
} JFP_CONFIG_DIAGNOSTICS;

typedef struct {
    ULONG Size;
    ULONG Version;
    LONG Status;
    ULONG Stage;
    ULONG Calls;
    ULONG CallId;
    LONG ScmStatus;
    LONG ObjectStatus;
    ULONG ResponseType;
    ULONG Available;
    ULONG Client;
    ULONG Service;
    ULONG Callbacks;
    ULONG Pinned;
    LONG CleanupStatus;
    ULONG Uid;
    ULONGLONG Return[4];
    ULONG App;
    ULONG ImageSize;
    ULONG Maps;
    ULONG Releases;
    ULONG LiveObjects;
    ULONG CallbackContext;
    ULONG CallbackOperation;
    ULONG CallbackCounts;
    ULONG Command;
    LONG CommandStatus;
    ULONG ResponseSize;
    ULONG Response[8];
    ULONG Data[128];
    ULONG Region;
    ULONG PowerAddress;
    ULONG SavedPower[3];
    LONG PowerStatus;
    ULONGLONG ScmRegisters[8];
    LONG Flow[6];
    LONG AuditStatus;
    JFP_CONFIG_DIAGNOSTICS Config[JFP_CONFIG_COUNT];
    ULONG TaLogSize;
    LONG TaLogStatus;
    LONG StorageStatus;
    ULONG StorageRequests;
    ULONG StorageCommand;
    LONG StorageIoStatus;
    ULONG InitResult;
    ULONG WakeResult;
    ULONG CaptureResult;
    ULONG AuthStartResult;
    ULONG FeatureResult;
    ULONG CancelResult;
    ULONG FeatureCommandResult;
    ULONG FeatureValue;
    ULONG SessionActive;
    ULONG SampleCount;
    ULONG PreEnrollResult;
    ULONG ChallengePresent;
    ULONG MissingAuthResult;
    ULONG PostEnrollResult;
} JFP_TEE_DIAGNOSTICS;

typedef struct {
    ULONG Size;
    ULONG Version;
    ULONG ImageSize;
    ULONG Reserved;
    UCHAR Image[1];
} JFP_TA_INPUT;

#define JFP_TA_HEADER_SIZE 16u

typedef struct {
    ULONG Size;
    ULONG Version;
    ULONG SpiSize;
    ULONG JvSize;
    UCHAR Images[1];
} JFP_CAPTURE_INPUT;

#define JFP_CAPTURE_HEADER_SIZE 16u
