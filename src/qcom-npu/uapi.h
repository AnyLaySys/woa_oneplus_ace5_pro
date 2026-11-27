#pragma once
// Fixed-width ABI without importing user CRT headers into the kernel.
typedef unsigned int QCOM_NPU_U32;
typedef unsigned long long QCOM_NPU_U64;

#define QCOM_NPU_ABI 1u
#define QCOM_NPU_VERSION 0x26112771u
#define QCOM_NPU_QUERY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x830, METHOD_BUFFERED, FILE_READ_DATA)
#define QCOM_NPU_ATTACH CTL_CODE(FILE_DEVICE_UNKNOWN, 0x831, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_INVOKE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x832, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_CREATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x834, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_SUBMIT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x835, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_COLLECT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x836, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_ALLOCATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x837, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_FREE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x838, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_WRITE_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83a, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_READ_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83b, METHOD_BUFFERED, FILE_READ_DATA)
#define QCOM_NPU_QUEUE_EXECUTION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83c, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_EXECUTION_RESULT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83d, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_SHARED_ALLOCATE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83e, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_DISPATCH_QUEUE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x83f, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_DISPATCH_RESULT CTL_CODE(FILE_DEVICE_UNKNOWN, 0x840, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_QUERY_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x841, METHOD_BUFFERED, FILE_READ_DATA)
#define QCOM_NPU_MAP_SHARED CTL_CODE(FILE_DEVICE_UNKNOWN, 0x842, METHOD_BUFFERED, FILE_READ_DATA | FILE_WRITE_DATA)
#define QCOM_NPU_MAX_ARGUMENTS 16u
#define QCOM_NPU_MAX_DATA 0x1000000u
#define QCOM_NPU_MAX_SHELL 0x200000u
#define QCOM_NPU_MAX_HEAP 0x2000000u
#define QCOM_NPU_MAX_SHARED 0x20000000u
#define QCOM_NPU_MODEL_BUFFERS 30u
#define QCOM_NPU_SHARED 4u
#define QCOM_NPU_FIRMWARE 1u
#define QCOM_NPU_DSP_READY 2u
#define QCOM_NPU_LINK_READY 4u
#define QCOM_NPU_DMA_READY 8u
#define QCOM_NPU_ATTACHED 16u
#define QCOM_NPU_DMA_PENDING 32u
#define QCOM_NPU_USER_PROCESS 64u
#define QCOM_NPU_PAGED_MEMORY 128u

// Creates an unsigned CDSP user process from a Hexagon ELF shell. The driver
// chooses the process ID and DMA addresses; HeapBytes is bounded and page aligned.
typedef struct {
    QCOM_NPU_U32 Abi, Attributes, ShellBytes, HeapBytes;
    unsigned char Shell[1];
} QCOM_NPU_PROCESS;

// All offsets are relative to the start of this buffered IOCTL. No user pointers
// or physical addresses cross the interface. Scalar uses the FastRPC wire ABI;
// DMA handles and persistent mappings are not implemented by this buffer API.
typedef struct { QCOM_NPU_U32 Offset, Length; } QCOM_NPU_ARGUMENT;
typedef struct {
    QCOM_NPU_U32 Abi, Handle, Scalar, Count, Result;
    QCOM_NPU_ARGUMENT Arg[QCOM_NPU_MAX_ARGUMENTS];
    unsigned char Data[1];
} QCOM_NPU_PACKET;
// SUBMIT accepts a PACKET and returns a ticket. COLLECT accepts that ticket and
// returns the completed PACKET, or ERROR_BUSY while the DSP still owns its DMA.
typedef struct {
    QCOM_NPU_U32 Abi, Reserved;
    QCOM_NPU_U64 Token;
} QCOM_NPU_TICKET;
typedef struct {
    QCOM_NPU_U32 Abi, Flags, Bytes, Id;
    QCOM_NPU_U64 DspAddress;
} QCOM_NPU_ALLOCATION;
// A driver-owned DMA allocation mapped into the requesting process and
// CDSP. The address is a process virtual address; no physical address is exposed.
typedef struct {
    QCOM_NPU_ALLOCATION Allocation;
    QCOM_NPU_U64 UserAddress;
} QCOM_NPU_SHARED_ALLOCATION;
typedef struct { QCOM_NPU_U32 Abi, Id, Map, Reserved; } QCOM_NPU_SHARED_MAPPING;
// Publishes a prepared GGML packet only when Windows dispatches its work.
// The matching DSP response count is the hardware completion fence.
typedef struct {
    QCOM_NPU_U32 Abi, QueueId, StateId, Handle;
    QCOM_NPU_U32 WritePosition, PacketCount, QueueCount, ComputeHandle;
} QCOM_NPU_QUEUE_DISPATCH;
typedef struct {
    QCOM_NPU_U32 Abi, Id, Offset, Bytes;
    unsigned char Data[1];
} QCOM_NPU_MEMORY_WRITE;
typedef struct { QCOM_NPU_U32 Abi, Id, Offset, Bytes; } QCOM_NPU_MEMORY_READ;
typedef struct {
    QCOM_NPU_U32 Abi, Version, Flags, State, LastStatus, LastResult, Requests, Responses, SocId;
} QCOM_NPU_STATUS;
typedef struct {
    QCOM_NPU_U32 Abi, Owner, Allocations, WindowBytes;
    QCOM_NPU_U64 Budget, Allocated, Shared, Available;
} QCOM_NPU_MEMORY_STATUS;
