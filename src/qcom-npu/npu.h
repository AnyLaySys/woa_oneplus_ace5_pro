#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "uapi.h"

#define NPU_SMEM_SIZE 0x200000u
#define NPU_INTENTS 128u
#define NPU_LOCAL_CID 1u
#define NPU_RESPONSE_SIZE 64u
#define NPU_TAG 'upNQ'
#define NPU_ASYNC_SLOTS 8u
#define NPU_ALLOCATIONS 256u
#define NPU_CLIENTS 8u
typedef struct NPU NPU;
typedef struct NPU_CLIENT NPU_CLIENT;
typedef struct {
    ULONGLONG Token;
    QCOM_NPU_PACKET *Packet;
    ULONG Bytes, Output, Owner, Thread, State, Kind, WriteOffset, ResponseOffset;
    NTSTATUS Status;
    ULONG BufferCount, Buffers[QCOM_NPU_MODEL_BUFFERS + 3];
} NPU_SCHEDULED_CALL;

typedef struct {
    PUCHAR Buffer;
    ULONG Bytes, Id, Flags;
    ULONGLONG Remote;
    ULONGLONG Iova;
    PMDL Mdl;
    PVOID UserAddress;
    PEPROCESS Owner;
    ULONG VideoRefs;
    ULONGLONG VideoCookie;
    BOOLEAN Detached, Registered, Paged, DspMapped, Pinned;
    ULONGLONG LastUse;
} NPU_ALLOCATION;

typedef struct {
    PUCHAR Rpc;
    ULONG Bytes, Handle, Scalar, Count, Inputs, Result;
    ULONG Offset[QCOM_NPU_MAX_ARGUMENTS];
    QCOM_NPU_ARGUMENT Arg[QCOM_NPU_MAX_ARGUMENTS];
    ULONGLONG Context;
    BOOLEAN Pending, Complete;
} NPU_ASYNC_CALL;

typedef struct {
    ULONG Pid, Tid;
    ULONGLONG Context;
    ULONG Handle, Scalar;
    ULONGLONG Address, Size;
} NPU_MESSAGE;
typedef struct { ULONGLONG Pointer, Length; } NPU_ARGUMENT;
typedef struct { ULONG Count, Index; } NPU_BUFFER;
typedef struct { ULONGLONG Address, Size; } NPU_PAGE;
struct NPU_CLIENT {
    LIST_ENTRY Entry;
    NPU *Device;
    WDFFILEOBJECT File;
    ULONG Pid, Pd, Thread, HeapBytes, RunRefs, ComputeHandle;
    BOOLEAN Attached, Closing;
    PUCHAR Heap;
    PHYSICAL_ADDRESS HeapPhysical;
    NPU_ASYNC_CALL Calls[NPU_ASYNC_SLOTS];
    NPU_ALLOCATION Allocations[NPU_ALLOCATIONS];
    NPU_SCHEDULED_CALL Scheduled[NPU_ASYNC_SLOTS];
};
typedef struct { NPU_CLIENT *Client; BOOLEAN Closing; } NPU_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NPU_FILE, NpuFileContext);
struct NPU {
    PUCHAR Smem, Mutex, Ipcc, Smmu;
    ULONG SmmuBytes, IommuBank;
    PULONGLONG PageTables;
    PHYSICAL_ADDRESS PageTableAddress;
    BOOLEAN PagedMemory;
    ULONG Partition, PartitionSize, Cacheline;
    PUCHAR In, Out, Desc, Tx, Rx;
    ULONG InSize, TxSize, RxSize;
    ULONG Rcid, IntentCount, Iid[NPU_INTENTS], IntentSize[NPU_INTENTS];
    BOOLEAN IntentFree[NPU_INTENTS];
    BOOLEAN VersionReady, Opening, Acked, Advertised, IntentRequested, Online;
    ULONG LocalIid, PendingCommand, PendingIid;
    ULONG State, LastResult, Requests, Responses;
    NTSTATUS LastStatus;
    BOOLEAN Firmware, MappingReady, Pending, Reply;
    BOOLEAN Poisoned, Closed;
    PUCHAR Rpc;
    ULONG RpcBytes;
    PHYSICAL_ADDRESS Physical;
    ULONGLONG Sequence, Context;
    ULONG Offset[QCOM_NPU_MAX_ARGUMENTS];
    LIST_ENTRY Clients;
    ULONG ClientCount;
    NPU_CLIENT *Running;
    BOOLEAN Preparing;
    ULONGLONG MapSequence;
    ULONGLONG MemoryBudget;
    ULONG AllocationSequence;
    ULONG Slot[3], SavedSmr[3], SavedS2cr[3], ActiveS2cr;
    BOOLEAN OwnSlot[3];
    WDFTIMER Timer;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NPU, NpuContext);

static __inline ULONG NpuRead(PUCHAR base, ULONG offset) {
    return READ_REGISTER_ULONG((PULONG)(base + offset));
}
static __inline VOID NpuWrite(PUCHAR base, ULONG offset, ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(base + offset), value);
}
static __inline VOID NpuSleep(VOID) {
    LARGE_INTEGER delay;
    delay.QuadPart = -10000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}
VOID NpuWakeTimer(PEX_TIMER timer, PVOID context);
VOID NpuRpcReply(NPU *n, ULONGLONG context, ULONG result);

NTSTATUS NpuLinkStart(NPU *n);
VOID NpuLinkPoll(NPU *n);
BOOLEAN NpuLinkSend(NPU *n, const NPU_MESSAGE *message);
VOID NpuLinkClose(NPU *n);
ULONG NpuSocId(NPU *n);
NTSTATUS NpuMappingAcquire(NPU *n);
VOID NpuMappingRelease(NPU *n);
NTSTATUS NpuInvoke(NPU_CLIENT *s, QCOM_NPU_PACKET *packet, ULONG input, ULONG output,
                   WDFREQUEST request, PULONG bytes, BOOLEAN kernel);
NTSTATUS NpuAttach(NPU_CLIENT *s, WDFREQUEST request);
NTSTATUS NpuCreateProcess(NPU_CLIENT *s, const QCOM_NPU_PROCESS *process, ULONG input, WDFREQUEST request);
NTSTATUS NpuSubmit(NPU_CLIENT *s, QCOM_NPU_PACKET *packet, ULONG input, WDFREQUEST request, QCOM_NPU_TICKET *ticket);
NTSTATUS NpuSubmitThread(NPU_CLIENT *s, QCOM_NPU_PACKET *packet, ULONG input, ULONG thread, QCOM_NPU_TICKET *ticket);
NTSTATUS NpuCollect(NPU_CLIENT *s, const QCOM_NPU_TICKET *ticket, QCOM_NPU_PACKET *packet, ULONG output, PULONG bytes);
NTSTATUS NpuAllocate(NPU_CLIENT *s, QCOM_NPU_ALLOCATION *allocation, WDFREQUEST request);
NTSTATUS NpuAllocateShared(NPU_CLIENT *s, QCOM_NPU_SHARED_ALLOCATION *allocation, WDFREQUEST request);
NTSTATUS NpuMapShared(NPU_CLIENT *s, const QCOM_NPU_SHARED_MAPPING *mapping, WDFREQUEST request);
NTSTATUS NpuIommuAcquire(NPU *n);
VOID NpuIommuRelease(NPU *n);
NTSTATUS NpuIommuMap(NPU *n, NPU_ALLOCATION *allocation);
NTSTATUS NpuIommuUnmap(NPU *n, NPU_ALLOCATION *allocation);
NTSTATUS NpuFree(NPU_CLIENT *s, const QCOM_NPU_ALLOCATION *allocation, WDFREQUEST request);
NTSTATUS NpuWriteMemory(NPU_CLIENT *s, const QCOM_NPU_MEMORY_WRITE *write, ULONG input);
NTSTATUS NpuReadMemory(NPU_CLIENT *s, const QCOM_NPU_MEMORY_READ *read, PUCHAR output, ULONG capacity);
VOID NpuFreeAllocations(NPU_CLIENT *s);
VOID NpuMemoryStatus(NPU *n, QCOM_NPU_MEMORY_STATUS *status);
VOID NpuMemoryInitialize(NPU *n);
BOOLEAN NpuMemoryReference(NPU *n, ULONG id, ULONG owner, ULONG bytes, ULONGLONG address, ULONGLONG cookie, BOOLEAN acquire);
VOID NpuMemoryPurge(NPU *n, ULONG owner, ULONGLONG cookie);
static __inline BOOLEAN NpuAsyncPending(NPU_CLIENT *s) {
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) if (s->Calls[i].Pending) return TRUE;
    return FALSE;
}
VOID NpuDetach(NPU_CLIENT *s);
VOID NpuSchedulerInitialize(VOID);
VOID NpuSchedulerBind(WDFDEVICE device);
VOID NpuSchedulerUnbind(WDFDEVICE device);
NTSTATUS NpuQueueExecution(NPU_CLIENT *s, const QCOM_NPU_PACKET *packet, ULONG input, WDFREQUEST request, QCOM_NPU_TICKET *ticket);
NTSTATUS NpuQueueDispatch(NPU_CLIENT *s, const QCOM_NPU_QUEUE_DISPATCH *dispatch, WDFREQUEST request, QCOM_NPU_TICKET *ticket);
NTSTATUS NpuDispatchResult(NPU_CLIENT *s, const QCOM_NPU_TICKET *ticket);
NTSTATUS NpuExecutionResult(NPU_CLIENT *s, const QCOM_NPU_TICKET *ticket, QCOM_NPU_PACKET *packet, ULONG output, PULONG bytes);
VOID NpuCancelScheduled(NPU_CLIENT *s);

NPU_CLIENT *NpuClientGet(NPU *n, WDFREQUEST request, PNTSTATUS status);
ULONG NpuRequestThreadId(WDFREQUEST request);
VOID NpuClientClose(NPU_CLIENT *s);
VOID NpuClientRetire(NPU_CLIENT *s);
VOID NpuClientSweep(NPU *n);
BOOLEAN NpuBusy(NPU *n);
BOOLEAN NpuGpuBusy(NPU_CLIENT *s);
NPU_ALLOCATION *NpuAllocation(NPU_CLIENT *s, ULONG id);
NTSTATUS NpuUnmapAllocation(NPU_CLIENT *s, NPU_ALLOCATION *a, WDFREQUEST request);
NTSTATUS NpuMapAllocation(NPU_CLIENT *s, NPU_ALLOCATION *a, WDFREQUEST request);
NTSTATUS NpuPrepareBuffers(NPU_CLIENT *s, const ULONG *ids, ULONG count);
