#pragma once
#ifdef _KERNEL_MODE
typedef ULONG GPU_U32;
typedef ULONGLONG GPU_U64;
#else
#include <stdint.h>
typedef uint32_t GPU_U32;
typedef uint64_t GPU_U64;
#endif

#define GPU_MAX_BUFFERS 1024
#define GPU_MAX_BUFFER_SIZE 0x20000000
#define GPU_CLIENT_VA_BASE 0x100000000ULL
#define GPU_CLIENT_VA_SIZE 0x200000000ULL
#define GPU_CLIENT_VA_END (GPU_CLIENT_VA_BASE+GPU_CLIENT_VA_SIZE)
#define GPU_VA_END 0x400000000ULL
#define GPU_IOCTL_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN,0x800,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_ALLOC CTL_CODE(FILE_DEVICE_UNKNOWN,0x801,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_FREE CTL_CODE(FILE_DEVICE_UNKNOWN,0x802,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_SUBMIT CTL_CODE(FILE_DEVICE_UNKNOWN,0x803,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_WAIT CTL_CODE(FILE_DEVICE_UNKNOWN,0x804,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_SUBMIT_BATCH CTL_CODE(FILE_DEVICE_UNKNOWN,0x805,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_CLIENT_LEASE CTL_CODE(FILE_DEVICE_UNKNOWN,0x806,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_CLIENT_STATS CTL_CODE(FILE_DEVICE_UNKNOWN,0x807,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_DISPLAY_CAPTURE CTL_CODE(FILE_DEVICE_UNKNOWN,0x808,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_ALLOC_CACHED CTL_CODE(FILE_DEVICE_UNKNOWN,0x809,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_CACHE CTL_CODE(FILE_DEVICE_UNKNOWN,0x80A,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_TELEMETRY CTL_CODE(FILE_DEVICE_UNKNOWN,0x80B,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_UMD_METRICS CTL_CODE(FILE_DEVICE_UNKNOWN,0x80C,METHOD_BUFFERED,FILE_WRITE_DATA)
#define GPU_IOCTL_POWER CTL_CODE(FILE_DEVICE_UNKNOWN,0x80D,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_POWER_SET CTL_CODE(FILE_DEVICE_UNKNOWN,0x80E,METHOD_BUFFERED,FILE_WRITE_DATA)
#define GPU_IOCTL_SHARED_RETAIN CTL_CODE(FILE_DEVICE_UNKNOWN,0x80F,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_SHARED_RELEASE CTL_CODE(FILE_DEVICE_UNKNOWN,0x810,METHOD_BUFFERED,FILE_WRITE_DATA)
#define GPU_IOCTL_CONTEXT_PROTOCOL CTL_CODE(FILE_DEVICE_UNKNOWN,0x817,METHOD_BUFFERED,FILE_READ_DATA)
typedef struct { GPU_U32 Version,Size,Enabled,CookieValid; GPU_U64 Switches,LastCookie; } GPU_CONTEXT_PROTOCOL;
#define GPU_IOCTL_CLIENT_DIAGNOSTICS CTL_CODE(FILE_DEVICE_UNKNOWN,0x811,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_PATCH_DIAGNOSTICS CTL_CODE(FILE_DEVICE_UNKNOWN,0x812,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_DISPLAY_CLOCKS CTL_CODE(FILE_DEVICE_UNKNOWN,0x813,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_DISPLAY_BUS_INFO CTL_CODE(FILE_DEVICE_UNKNOWN,0x814,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_DISPLAY_ACTIVATION CTL_CODE(FILE_DEVICE_UNKNOWN,0x815,METHOD_BUFFERED,FILE_READ_DATA|FILE_WRITE_DATA)
#define GPU_IOCTL_DPU_TRACE CTL_CODE(FILE_DEVICE_UNKNOWN,0x816,METHOD_BUFFERED,FILE_READ_DATA)
#define GPU_IOCTL_SCHEDULER_INFO CTL_CODE(FILE_DEVICE_UNKNOWN,0x818,METHOD_BUFFERED,FILE_READ_DATA)
typedef struct {
    GPU_U32 Version,Size,Generation,Flags,Failed,Submitted,Completed,IrqRead,IrqWrite;
    GPU_U32 Pending,Inflight,CpuBarriers,PreemptionFence,PendingOperation,ResetCount,Reserved;
    GPU_U64 HardwareSubmitted,HardwareCompleted,OldestHardwareFence,CompletionPolls,PolledCompletions;
} GPU_SCHEDULER_INFO;
#define GPU_DPU_TRACE_ENTRIES 1024
typedef struct {
    GPU_U64 Serial,Time100ns,FlipSequence,Physical,Address,OldPhysical;
    GPU_U32 Kind,Frame,WrLine,TearStatus,MdpStatus,Flush,QueueFrame,Reserved;
} GPU_DPU_TRACE_ENTRY;
typedef struct {
    GPU_U32 Version,Size;GPU_U64 LatestSerial,Dropped;
    GPU_DPU_TRACE_ENTRY Entries[GPU_DPU_TRACE_ENTRIES];
} GPU_DPU_TRACE;
#define GPU_DISPLAY_TE_SAMPLES 64
typedef struct { GPU_U32 Version,Phase,Step,Index,Flags,Reserved; } GPU_DISPLAY_ACTIVATION_REQUEST;
typedef struct { GPU_U64 Sequence,Time100ns; GPU_U32 Frame,Delta; } GPU_DISPLAY_TE_SAMPLE;
typedef struct {
    GPU_U32 Version,Size,Phase,Step,Index,ProofMask,LastStatus,BackendFlags;
    GPU_U64 LastPhysical;
    GPU_U32 Cn1Address,Cn1Vote,GccAhb,GccAxi,DispPower,DispBranches[3],HwVersion;
    GPU_U32 SmmuId0,SmmuId1,MatchCount,Slots[2],Smr[2],S2cr[2],Sctlr[2],Cbar[2];
    GPU_U64 TeEdges,TeObserved,TeSkipped,TeLast100ns,TePeriod100ns;
    GPU_U64 ActiveAddress,PendingAddress,Queued,Applied;
    GPU_DISPLAY_TE_SAMPLE TeSamples[GPU_DISPLAY_TE_SAMPLES];
    GPU_U64 VsyncObserved,LatchedCount,GpuCopyCount,GpuCopyBytes,GpuCopyPending;
    GPU_DISPLAY_TE_SAMPLE VsyncSamples[GPU_DISPLAY_TE_SAMPLES];
    GPU_U32 ProbeBranchOriginal[3],ProbeBranchAdded;
    GPU_U32 TopologyGroup,TopologyIndex,TopologyOffsets[5],TopologyValues[5],DpuFailStage;
} GPU_DISPLAY_ACTIVATION;
typedef struct {
    GPU_U32 Version,Size,Cn1Address,Cn1AuxLength,Cn1Unit,Cn1Width,Cn1Vcd;
    GPU_U32 Cn1VoteX,Cn1VoteY,Sh1Address,Cn0Address,RegBusPeakKBps;
} GPU_DISPLAY_BUS_INFO;
typedef struct { GPU_U32 Version,Phase,Flags,Reserved; } GPU_DISPLAY_CLOCK_REQUEST;
typedef struct {
    GPU_U32 Version,Phase,Flags,Reserved;
    GPU_U32 GccAhb,GccAxi,DispPower,Reserved2;
} GPU_DISPLAY_CLOCKS;
typedef struct { GPU_U64 Address,Size; } GPU_SHARED_IMPORT;
#define GPU_POWER_LEVELS 16
typedef struct { GPU_U32 FixedKhz,MaxKhz; } GPU_POWER_POLICY;
typedef struct {
    GPU_U32 Version,SpeedBin,Count,Index,BusyPercent,Status,FixedKhz,MaxKhz;
    GPU_U32 Khz[GPU_POWER_LEVELS],Bandwidth[GPU_POWER_LEVELS];
    GPU_U64 Residency100ns[GPU_POWER_LEVELS],Transitions;
} GPU_POWER_INFO;
#define GPU_TELEMETRY_VERSION 3
#define GPU_TIMING_BUCKETS 9
#define GPU_PRESENT_FLAG_BITS 32

typedef struct { GPU_U32 Handle,Size; GPU_U64 Address,Map; } GPU_ALLOCATION;
typedef struct { GPU_U32 Handle,Offset,Words,Reserved; } GPU_SUBMIT;
typedef struct { GPU_U32 Handle,Offset,Size,Readback; } GPU_CACHE;
typedef struct { GPU_U64 Fence,Timeout; } GPU_WAIT;
typedef struct { GPU_U32 Version,Chip,Stage,Status; GPU_U64 Submitted,Completed; GPU_U32 GmuVersion,Irqs,Step,Snapshot[20]; } GPU_INFO;
typedef struct { GPU_U32 Pid,Buffers; GPU_U64 Bytes,Submissions,Completions,Lease; } GPU_CLIENT_STATS;
typedef struct {
    GPU_U32 Version,Size,Pid,Flags,PendingRefs,Reserved;
    GPU_U64 Lease,Prepared,Issued,Retired,Cancelled;
} GPU_CLIENT_DIAGNOSTICS;
typedef struct {
    GPU_U32 Version,Size;
    GPU_U64 Calls,Failures;
    GPU_U32 Status,Flags,Pid,Start,End,PrivateStart,PrivateEnd,DmaSize,PrivateSize,Operation,OffsetOperation,Reserved;
    GPU_U64 Sequence,Lease;
} GPU_PATCH_DIAGNOSTICS;
/* DISPLAY_CAPTURE input: 0 firmware, 1 legacy primary, 2 confirmed DPU BO.
   Selector 2 reports the physical base sampled at capture start in Address. */
typedef struct { GPU_U32 Width,Height,Pitch,Frame; GPU_U64 Address; } GPU_DISPLAY_CAPTURE;
typedef struct { GPU_U64 SyncCalls,UploadBytes,ReadbackBytes,RotateCalls,BltCalls; } GPU_UMD_METRICS;
/* Timing buckets in milliseconds: <1, <2, <4, <8, <16, <20, <33, <67, >=67. */
typedef struct {
    GPU_U32 Version,Size;
    GPU_U64 PresentBlt,PresentFlip,PresentOther,AddressCalls,AddressDirql;
    GPU_U64 AddressFlags[GPU_PRESENT_FLAG_BITS];
    GPU_U64 CpuPresentBytes,CpuScanoutBytes,CpuPagingBytes,PagingCalls;
    GPU_U64 CreateCalls,OpenCalls,SubmitCalls,SubmitRejected,Completed,ExecutionFailures;
    GPU_U64 SubmitLatency[GPU_TIMING_BUCKETS],VsyncInterval[GPU_TIMING_BUCKETS];
    GPU_U64 LastVsyncTime,LastExecutionStatus,LastPagingOperation;
    GPU_UMD_METRICS Umd;
    GPU_U64 HardwareIssued,HardwareCompleted,MaxHardwareInflight;
    GPU_U64 PresentRotate,AddressRejected,RejectedWidth,RejectedHeight,RejectedPitch;
} GPU_TELEMETRY;
