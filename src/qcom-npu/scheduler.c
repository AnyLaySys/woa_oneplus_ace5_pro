#include <ntifs.h>
#include "npu.h"
#define NPU_SCHEDULER_EXPORTS
#include "scheduler-abi.h"

static KSPIN_LOCK DeviceLock;
static WDFDEVICE Device;
VOID NpuWakeTimer(PEX_TIMER timer, PVOID context) {
    UNREFERENCED_PARAMETER(timer);
    KeSetEvent((PKEVENT)context, IO_NO_INCREMENT, FALSE);
}
VOID NpuSchedulerInitialize(VOID) { KeInitializeSpinLock(&DeviceLock); }
VOID NpuSchedulerBind(WDFDEVICE device) {
    KIRQL old; KeAcquireSpinLock(&DeviceLock, &old); Device = device; KeReleaseSpinLock(&DeviceLock, old);
}
VOID NpuSchedulerUnbind(WDFDEVICE device) {
    KIRQL old; KeAcquireSpinLock(&DeviceLock, &old); if (Device == device) Device = NULL; KeReleaseSpinLock(&DeviceLock, old);
}
static WDFDEVICE ReferenceDevice(VOID) {
    WDFDEVICE device; KIRQL old;
    KeAcquireSpinLock(&DeviceLock, &old); device = Device;
    if (device) WdfObjectReference(device);
    KeReleaseSpinLock(&DeviceLock, old); return device;
}
__declspec(dllexport) BOOLEAN NpuSchedulerReady(VOID) {
    WDFDEVICE device = ReferenceDevice(); if (!device) return FALSE;
    WdfObjectAcquireLock(device); NPU *n = NpuContext(device);
    if (n->Firmware && n->Smem && !n->Closed) NpuLinkPoll(n);
    BOOLEAN ready = n->Firmware && n->Online && !n->Closed && !n->Poisoned && n->Smem;
    WdfObjectReleaseLock(device); WdfObjectDereference(device); return ready;
}
__declspec(dllexport) BOOLEAN NpuSchedulerMemoryReference(const NPU_SCHEDULER_MEMORY *memory, BOOLEAN acquire) {
    if (memory->Magic != NPU_SCHEDULER_MAGIC || !memory->Id || !memory->Bytes || !memory->Address) return FALSE;
    WDFDEVICE device = ReferenceDevice(); if (!device) return FALSE;
    WdfObjectAcquireLock(device);
    BOOLEAN valid = NpuMemoryReference(NpuContext(device), memory->Id, memory->Owner, memory->Bytes, memory->Address, memory->Cookie, acquire);
    WdfObjectReleaseLock(device); WdfObjectDereference(device); return valid;
}
__declspec(dllexport) VOID NpuSchedulerMemoryPurge(ULONG owner, ULONGLONG cookie) {
    WDFDEVICE device = ReferenceDevice(); if (!device) return;
    WdfObjectAcquireLock(device); NpuMemoryPurge(NpuContext(device), owner, cookie);
    WdfObjectReleaseLock(device); WdfObjectDereference(device);
}
__declspec(dllexport) ULONGLONG NpuSchedulerMemoryBudget(VOID) {
    WDFDEVICE device = ReferenceDevice(); if (!device) return 0;
    WdfObjectAcquireLock(device); ULONGLONG budget = NpuContext(device)->MemoryBudget;
    WdfObjectReleaseLock(device); WdfObjectDereference(device); return budget;
}
NTSTATUS NpuQueueExecution(NPU_CLIENT *s, const QCOM_NPU_PACKET *q, ULONG input, WDFREQUEST request, QCOM_NPU_TICKET *ticket) {
    NPU *n = s->Device;
    if (input != 236 || q->Abi != 1 || q->Scalar != 0x07020100 || q->Count != 3 || q->Handle < 256 || q->Result ||
        q->Arg[0].Offset != 148 || q->Arg[0].Length != 8 || q->Arg[1].Offset != 156 || q->Arg[1].Length != 72 ||
        q->Arg[2].Offset != 228 || q->Arg[2].Length != 8) return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1 || !s->Pid || s->Pid != IoGetRequestorProcessId(WdfRequestWdmGetIrp(request)))
        return STATUS_ACCESS_DENIED;
    ULONGLONG flags; RtlCopyMemory(&flags, (PUCHAR)q + 148, 8);
    if (flags != 9 || n->Poisoned || !n->Online || n->Closed) return STATUS_INVALID_DEVICE_STATE;
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i]; if (call->State) continue;
        call->Packet = ExAllocatePool2(POOL_FLAG_NON_PAGED, input, NPU_TAG);
        if (!call->Packet) return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(call->Packet, q, input); call->Bytes = input; call->Owner = s->Pid;
        call->Thread = NpuRequestThreadId(request);
        call->Token = (++n->Sequence << 16) | 0xe0 | i; call->State = 1;
        *ticket = (QCOM_NPU_TICKET){1, 0, call->Token}; return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_BUSY;
}
typedef struct { ULONG Offset, Length, ReadOffset, WriteOffset; } NPU_QUEUE_RANGE;
typedef struct { ULONG Version, Error, Flags; NPU_QUEUE_RANGE Request, Response; ULONG Count; } NPU_QUEUE_HEADER;
static NPU_ALLOCATION *SharedAllocation(NPU_CLIENT *s, ULONG id) {
    NPU_ALLOCATION *a = NpuAllocation(s, id);
    return a && a->Flags == QCOM_NPU_SHARED && a->Owner && HandleToUlong(PsGetProcessId(a->Owner)) == s->Pid ? a : NULL;
}
static BOOLEAN QueueRange(ULONG offset, ULONG bytes, ULONG capacity) {
    return offset >= 64 && offset <= capacity && bytes <= capacity - offset;
}
typedef struct { ULONG Fd, Size, Offset, Flags; ULONGLONG Address; } NPU_QUEUE_BUFFER;
typedef struct { ULONGLONG Base, Size; ULONG Flags, Fd; } NPU_MODEL_BUFFER;
C_ASSERT(sizeof(NPU_QUEUE_BUFFER) == 24);
C_ASSERT(sizeof(NPU_MODEL_BUFFER) == 24);
static NTSTATUS AddBuffer(NPU_CLIENT *s, NPU_SCHEDULED_CALL *call, ULONG id) {
    if (!SharedAllocation(s, id)) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < call->BufferCount; i++) if (call->Buffers[i] == id) return STATUS_SUCCESS;
    if (call->BufferCount == RTL_NUMBER_OF(call->Buffers)) return STATUS_INVALID_PARAMETER;
    call->Buffers[call->BufferCount++] = id; return STATUS_SUCCESS;
}
static NTSTATUS CaptureBuffers(NPU_CLIENT *s, NPU_SCHEDULED_CALL *call, const QCOM_NPU_QUEUE_DISPATCH *q,
                              NPU_ALLOCATION *queue, const NPU_QUEUE_HEADER *header) {
    if (q->Abi != 2) return STATUS_SUCCESS;
    ULONG position = *(volatile ULONG *)(queue->Buffer + header->Request.WriteOffset);
    if (header->Request.Length < 56 || (header->Request.Length & 7) || (position & 7) || position >= header->Request.Length) return STATUS_INVALID_PARAMETER;
    ULONGLONG packet; PUCHAR data = queue->Buffer + header->Request.Offset;
    RtlCopyMemory(&packet, data + position, sizeof(packet));
    if ((ULONG)packet != 56 || ((packet >> 48) & 255) != 1 || ((packet >> 32) & 65535) != 0x13)
        return STATUS_INVALID_PARAMETER;
    if (header->Request.Length - position < 56) {
        position = 0; ULONGLONG repeated; RtlCopyMemory(&repeated, data, sizeof(repeated));
        if (repeated != packet) return STATUS_INVALID_PARAMETER;
    }
    if ((position + 56 == header->Request.Length ? 0 : position + 56) != q->WritePosition) return STATUS_INVALID_PARAMETER;
    NPU_QUEUE_BUFFER descriptor; RtlCopyMemory(&descriptor, data + position + 8, sizeof(descriptor));
    NPU_ALLOCATION *payload = SharedAllocation(s, descriptor.Fd);
    if (!payload || !payload->Pinned || descriptor.Offset > payload->Bytes || descriptor.Size > payload->Bytes - descriptor.Offset ||
        descriptor.Address != (ULONGLONG)payload->UserAddress + descriptor.Offset || (descriptor.Flags & 0xffffff00u))
        return STATUS_INVALID_PARAMETER;
    ULONG count; RtlCopyMemory(&count, data + position + 8 + sizeof(descriptor) + 8, sizeof(count));
    if (count > QCOM_NPU_MODEL_BUFFERS || count > descriptor.Size / sizeof(NPU_MODEL_BUFFER)) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = AddBuffer(s, call, q->QueueId);
    if (NT_SUCCESS(status)) status = AddBuffer(s, call, q->StateId);
    if (NT_SUCCESS(status)) status = AddBuffer(s, call, descriptor.Fd);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG i = 0; i < count; i++) {
        NPU_MODEL_BUFFER model; RtlCopyMemory(&model, payload->Buffer + descriptor.Offset + i * sizeof(model), sizeof(model));
        NPU_ALLOCATION *a = SharedAllocation(s, model.Fd);
        if (!a || model.Flags || model.Base != (ULONGLONG)a->UserAddress || !model.Size || model.Size > a->Bytes)
            return STATUS_INVALID_PARAMETER;
        status = AddBuffer(s, call, model.Fd); if (!NT_SUCCESS(status)) return status;
    }
    s->ComputeHandle = q->ComputeHandle; return STATUS_SUCCESS;
}
static BOOLEAN SensitivePending(NPU *n) {
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) if (s->Calls[i].Pending && s->Calls[i].Handle != 3) return TRUE;
    }
    return FALSE;
}
NTSTATUS NpuQueueDispatch(NPU_CLIENT *s, const QCOM_NPU_QUEUE_DISPATCH *q, WDFREQUEST request, QCOM_NPU_TICKET *ticket) {
    NPU *n = s->Device;
    if ((q->Abi != 1 && q->Abi != 2) || (q->Abi == 1 && q->ComputeHandle) || (q->Abi == 2 && q->ComputeHandle < 256) || q->Handle < 256 || q->QueueId == q->StateId || q->QueueCount != 1 || (q->WritePosition & 7))
        return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1 || !s->Pid || s->Pid != IoGetRequestorProcessId(WdfRequestWdmGetIrp(request))) return STATUS_ACCESS_DENIED;
    if (n->Poisoned || !n->Online || n->Closed) return STATUS_INVALID_DEVICE_STATE;
    NPU_ALLOCATION *queue = SharedAllocation(s, q->QueueId), *state = SharedAllocation(s, q->StateId);
    if (!queue || !state || queue->Bytes < sizeof(NPU_QUEUE_HEADER) || state->Bytes < 1024) return STATUS_INVALID_PARAMETER;
    NPU_QUEUE_HEADER header; RtlCopyMemory(&header, queue->Buffer, sizeof(header));
    if (header.Version != 2 || header.Error || header.Flags != 1 || header.Count != q->QueueCount ||
        !header.Request.Length || !header.Response.Length || q->WritePosition >= header.Request.Length ||
        (header.Request.Offset & 127) || (header.Response.Offset & 127) ||
        (header.Request.WriteOffset & 127) || (header.Response.WriteOffset & 127) ||
        !QueueRange(header.Request.Offset, header.Request.Length, queue->Bytes) ||
        !QueueRange(header.Response.Offset, header.Response.Length, queue->Bytes) ||
        !QueueRange(header.Request.WriteOffset, 12, queue->Bytes) ||
        !QueueRange(header.Response.WriteOffset, 12, queue->Bytes)) return STATUS_INVALID_PARAMETER;
    KeMemoryBarrier();
    if (*(volatile ULONG *)(queue->Buffer + header.Request.WriteOffset + 4) + 1 != q->PacketCount ||
        *(volatile ULONG *)state->Buffer + 1 != q->PacketCount) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i];
        if (call->Kind == 2 && (call->State == 1 || call->State == 2)) return STATUS_DEVICE_BUSY;
    }
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i]; if (call->State) continue;
        call->Packet = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*q), NPU_TAG);
        if (!call->Packet) return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS status = CaptureBuffers(s, call, q, queue, &header);
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(call->Packet, NPU_TAG); RtlZeroMemory(call, sizeof(*call)); return status; }
        RtlCopyMemory(call->Packet, q, sizeof(*q)); call->Bytes = sizeof(*q); call->Owner = s->Pid;
        call->Thread = NpuRequestThreadId(request);
        call->Kind = 2; call->WriteOffset = header.Request.WriteOffset; call->ResponseOffset = header.Response.WriteOffset + 4;
        call->Token = (++n->Sequence << 16) | 0xe0 | i; call->State = 1;
        *ticket = (QCOM_NPU_TICKET){1, 0, call->Token}; return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_BUSY;
}
NTSTATUS NpuDispatchResult(NPU_CLIENT *s, const QCOM_NPU_TICKET *ticket) {
    if (ticket->Abi != 1 || ticket->Reserved) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i];
        if (!call->State || call->Kind != 2 || call->Token != ticket->Token) continue;
        if (call->Owner != s->Pid) return STATUS_ACCESS_DENIED;
        if (call->State != 3) return STATUS_DEVICE_BUSY;
        NTSTATUS status = call->Status;
        ExFreePoolWithTag(call->Packet, NPU_TAG); RtlZeroMemory(call, sizeof(*call)); return status;
    }
    return STATUS_INVALID_PARAMETER;
}
VOID NpuCancelScheduled(NPU_CLIENT *s) {
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i];
        if (call->State && call->State != 2) {
            if (call->Packet) ExFreePoolWithTag(call->Packet, NPU_TAG);
            RtlZeroMemory(call, sizeof(*call));
        }
    }
}
NTSTATUS NpuExecutionResult(NPU_CLIENT *s, const QCOM_NPU_TICKET *ticket, QCOM_NPU_PACKET *q, ULONG output, PULONG bytes) {
    if (ticket->Abi != 1 || ticket->Reserved || output != 236) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i];
        if (!call->State || call->Kind == 2 || call->Token != ticket->Token) continue;
        if (call->Owner != s->Pid) return STATUS_ACCESS_DENIED;
        if (call->State != 3) return STATUS_DEVICE_BUSY;
        NTSTATUS status = call->Status;
        if (NT_SUCCESS(status)) { RtlCopyMemory(q, call->Packet, call->Output); *bytes = call->Output; }
        ExFreePoolWithTag(call->Packet, NPU_TAG); RtlZeroMemory(call, sizeof(*call)); return status;
    }
    return STATUS_INVALID_PARAMETER;
}
__declspec(dllexport) NTSTATUS NpuSchedulerRun(ULONGLONG token, ULONG owner, const volatile LONG *preempt) {
    WDFDEVICE device = ReferenceDevice(); if (!device) return STATUS_DEVICE_NOT_CONNECTED;
    NPU *n = NpuContext(device); NPU_CLIENT *s = NULL; NPU_SCHEDULED_CALL *call = NULL; QCOM_NPU_TICKET ticket = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER; ULONGLONG deadline = KeQueryInterruptTime() + 200000000ULL;
    WdfObjectAcquireLock(device);
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients && !call; p = p->Flink) {
        NPU_CLIENT *client = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        if (client->Pid != owner || client->Closing || !client->Attached) continue;
        for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) if (client->Scheduled[i].State == 1 && client->Scheduled[i].Token == token) {
            s = client; call = &client->Scheduled[i]; break;
        }
    }
    if (!call || n->Running) { WdfObjectReleaseLock(device); WdfObjectDereference(device); return STATUS_ACCESS_DENIED; }
    call->State = 2; s->RunRefs++; WdfObjectReleaseLock(device);
    KEVENT wake; KeInitializeEvent(&wake, SynchronizationEvent, FALSE);
    PEX_TIMER timer = ExAllocateTimer(NpuWakeTimer, &wake, EX_TIMER_HIGH_RESOLUTION);
    BOOLEAN prepared = FALSE, published = FALSE, signaled = FALSE, preempted = FALSE;
    for (;;) {
        WdfObjectAcquireLock(device);
        if (!s->Attached || s->Closing) status = STATUS_CANCELLED;
        else if (!n->Smem || !n->Online || n->Closed) status = STATUS_DEVICE_NOT_CONNECTED;
        else if (preempt && *preempt && !published && !ticket.Token) { status = STATUS_DEVICE_BUSY; preempted = TRUE; }
        else if (!prepared) {
            // A module load may need this process's file listener. Do not hold
            // execution ownership while waiting for another client's RPC.
            if (n->Pending || SensitivePending(n)) status = STATUS_DEVICE_BUSY;
            else {
                n->Running = s;
                n->Preparing = TRUE;
                status = n->PagedMemory ? NpuPrepareBuffers(s, call->Buffers, call->BufferCount) : STATUS_SUCCESS;
                n->Preparing = FALSE;
                if (NT_SUCCESS(status)) prepared = TRUE;
                else n->Running = NULL;
            }
        } else if (call->Kind == 2) {
            const QCOM_NPU_QUEUE_DISPATCH *dispatch = (const QCOM_NPU_QUEUE_DISPATCH *)call->Packet;
            NPU_ALLOCATION *queue = SharedAllocation(s, dispatch->QueueId), *state = SharedAllocation(s, dispatch->StateId);
            status = STATUS_INVALID_DEVICE_STATE;
            if (queue && state) {
                if (!published) {
                    KeMemoryBarrier();
                    *(volatile ULONG *)(queue->Buffer + call->WriteOffset) = dispatch->WritePosition;
                    *(volatile ULONG *)(queue->Buffer + call->WriteOffset + 4) = dispatch->PacketCount;
                    *(volatile ULONG *)state->Buffer = dispatch->PacketCount;
                    KeMemoryBarrier(); published = TRUE;
                }
                QCOM_NPU_PACKET signal = {0}; ULONG collected = 0;
                signal.Abi = 1; signal.Handle = dispatch->Handle; signal.Scalar = 0x08000000;
                if (!ticket.Token) status = NpuSubmitThread(s, &signal, FIELD_OFFSET(QCOM_NPU_PACKET, Data), call->Thread, &ticket);
                else if (!signaled) {
                    status = NpuCollect(s, &ticket, &signal, FIELD_OFFSET(QCOM_NPU_PACKET, Data), &collected);
                    if (NT_SUCCESS(status)) { signaled = TRUE; if (signal.Result) status = STATUS_IO_DEVICE_ERROR; }
                } else status = STATUS_SUCCESS;
                if (signaled && NT_SUCCESS(status)) {
                    KeMemoryBarrier();
                    if (*(volatile ULONG *)(queue->Buffer + call->ResponseOffset) == dispatch->PacketCount) call->Output = sizeof(*dispatch);
                    else status = STATUS_DEVICE_BUSY;
                }
            }
        } else if (!ticket.Token) status = NpuSubmitThread(s, call->Packet, call->Bytes, call->Thread, &ticket);
        else status = NpuCollect(s, &ticket, call->Packet, call->Bytes, &call->Output);
        WdfObjectReleaseLock(device);
        if (preempted) break;
        if (ticket.Token && NT_SUCCESS(status) && call->Output) break;
        if (status != STATUS_DEVICE_BUSY && !NT_SUCCESS(status)) break;
        if (KeQueryInterruptTime() >= deadline) { status = STATUS_IO_TIMEOUT; break; }
        if (timer) { ExSetTimer(timer, -1000, 0, NULL); KeWaitForSingleObject(&wake, Executive, KernelMode, FALSE, NULL); }
        else NpuSleep();
    }
    if (timer) ExDeleteTimer(timer, TRUE, TRUE, NULL);
    WdfObjectAcquireLock(device);
    call->Status = status; call->State = preempted ? 1 : 3; s->RunRefs--; n->Running = NULL;
    if (published && !NT_SUCCESS(status) && s->Attached) n->Poisoned = TRUE;
    if (s->Closing) NpuClientClose(s);
    NpuClientSweep(n);
    WdfObjectReleaseLock(device); WdfObjectDereference(device); return status;
}
__declspec(dllexport) VOID NpuSchedulerCancel(ULONGLONG token, ULONG owner) {
    WDFDEVICE device = ReferenceDevice(); if (!device) return;
    WdfObjectAcquireLock(device); NPU *n = NpuContext(device);
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        if (s->Pid != owner) continue;
        for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
            NPU_SCHEDULED_CALL *call = &s->Scheduled[i];
            if (call->State == 1 && call->Token == token) { call->Status = STATUS_CANCELLED; call->State = 3; break; }
        }
    }
    WdfObjectReleaseLock(device); WdfObjectDereference(device);
}
__declspec(dllexport) NTSTATUS NpuSchedulerReset(VOID) {
    WDFDEVICE device = ReferenceDevice(); if (!device) return STATUS_DEVICE_NOT_CONNECTED;
    WdfObjectAcquireLock(device); NPU *n = NpuContext(device);
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) NpuDetach(CONTAINING_RECORD(p, NPU_CLIENT, Entry));
    NTSTATUS status = NpuBusy(n) ? STATUS_DEVICE_BUSY : STATUS_SUCCESS;
    WdfObjectReleaseLock(device); WdfObjectDereference(device); return status;
}
