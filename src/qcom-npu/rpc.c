#include <ntifs.h>
#include "npu.h"

C_ASSERT(sizeof(NPU_MESSAGE) == 40);
C_ASSERT(sizeof(NPU_ARGUMENT) == 16);
C_ASSERT(sizeof(NPU_BUFFER) == 8);
C_ASSERT(sizeof(NPU_PAGE) == 16);

static ULONG RequestThreadId(WDFREQUEST request) {
    if (!request) return 0;
    PIRP irp = WdfRequestWdmGetIrp(request);
    // KMDF may run this callback on its passive worker. FastRPC must identify
    // the original caller so the listener and its blocked client stay distinct.
    return irp->Tail.Overlay.Thread ? HandleToUlong(PsGetThreadId(irp->Tail.Overlay.Thread)) : 0;
}

VOID NpuRpcReply(NPU *n, ULONGLONG context, ULONG result) {
if (n->Pending && context == n->Context) {
    n->LastResult = result;
    n->Reply = TRUE;
    n->Pending = FALSE;
    n->Responses++;
    KeMemoryBarrier();
} else {
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
            NPU_ASYNC_CALL *call = &s->Calls[i];
            if (!call->Pending || context != call->Context) continue;
            call->Result = result; call->Pending = FALSE; call->Complete = TRUE;
            n->Responses++; KeMemoryBarrier(); break;
        }
    }
}
}
NTSTATUS NpuInvoke(NPU_CLIENT *s, QCOM_NPU_PACKET *q, ULONG input, ULONG output,
                   WDFREQUEST request, PULONG bytes, BOOLEAN kernel) {
    NPU *n = s->Device;
    if (NpuGpuBusy(s)) return STATUS_DEVICE_BUSY;
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data);
    if (input < base || output < base || q->Abi != QCOM_NPU_ABI ||
        q->Count > QCOM_NPU_MAX_ARGUMENTS || (q->Scalar & 255u) ||
        q->Count != ((q->Scalar >> 16) & 255u) + ((q->Scalar >> 8) & 255u))
        return STATUS_INVALID_PARAMETER;
    if (!kernel && (q->Handle == 1 ||
        (q->Handle == 0 && (!s->Attached || (q->Scalar != 0x20200u && q->Scalar != 0x1010200u))) ||
        (!s->Attached && (q->Handle != 2 || q->Scalar != 0x10100u))))
        return STATUS_ACCESS_DENIED;
    ULONG inputs = (q->Scalar >> 16) & 255u;
    // The wire trailer includes fd[16], CRC[64], and performance data[9].
    ULONG used = (40 * q->Count + 388 + 127) & ~127u;
    for (ULONG i = 0; i < q->Count; i++) {
        ULONG limit = i < inputs ? input : output;
        if (q->Arg[i].Offset < base || q->Arg[i].Offset > limit || q->Arg[i].Length > limit - q->Arg[i].Offset)
            return STATUS_INVALID_PARAMETER;
        used = (used + 127) & ~127u;
        if (used > QCOM_NPU_MAX_DATA || q->Arg[i].Length > QCOM_NPU_MAX_DATA - used) return STATUS_BUFFER_TOO_SMALL;
        used += q->Arg[i].Length;
    }
    if (!n->Online || n->Closed) return STATUS_DEVICE_NOT_CONNECTED;
    if (n->Pending || n->Poisoned) return STATUS_DEVICE_BUSY;
    NTSTATUS status = NpuMappingAcquire(n);
    if (!NT_SUCCESS(status)) return status;
    ULONG required = max(0x200000u, (used + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u)) + PAGE_SIZE;
    if (!n->Rpc || n->RpcBytes < required) {
        PHYSICAL_ADDRESS low = { 0 }, high, boundary = { 0 };
        high.QuadPart = MAXULONG;
        PUCHAR rpc = MmAllocateContiguousMemorySpecifyCache(required, low, high, boundary, MmNonCached);
        if (!rpc) return STATUS_INSUFFICIENT_RESOURCES;
        // A matching RPC response has already fenced this temporary argument
        // buffer; persistent process heaps have their own allocation lifetime.
        if (n->Rpc) MmFreeContiguousMemorySpecifyCache(n->Rpc, n->RpcBytes, MmNonCached);
        n->Rpc = rpc; n->RpcBytes = required;
        n->Physical = MmGetPhysicalAddress(n->Rpc);
    }
    RtlZeroMemory(n->Rpc, used);
    NPU_ARGUMENT *args = (NPU_ARGUMENT *)n->Rpc;
    NPU_BUFFER *bufs = (NPU_BUFFER *)(args + q->Count);
    NPU_PAGE *pages = (NPU_PAGE *)(bufs + q->Count);
    ULONG at = (40 * q->Count + 388 + 127) & ~127u;
    for (ULONG i = 0; i < q->Count; i++) {
        at = (at + 127) & ~127u;
        n->Offset[i] = at;
        args[i].Pointer = (ULONGLONG)(ULONG_PTR)(n->Rpc + at);
        args[i].Length = q->Arg[i].Length;
        bufs[i].Count = q->Arg[i].Length ? 1 : 0;
        bufs[i].Index = i;
        pages[i].Address = ((ULONGLONG)n->Physical.QuadPart + (at & ~(PAGE_SIZE - 1u))) | 0x100000000ULL;
        pages[i].Size = (q->Arg[i].Length + (at & (PAGE_SIZE - 1u)) + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
        if (!q->Arg[i].Length) { args[i].Pointer = 0; pages[i] = (NPU_PAGE){ 0 }; }
        if (i < inputs) RtlCopyMemory(n->Rpc + at, (PUCHAR)q + q->Arg[i].Offset, q->Arg[i].Length);
        at += q->Arg[i].Length;
    }
    n->Context = (++n->Sequence << 16) | 0x10 | s->Pd;
    NPU_MESSAGE message = { kernel || !s->Attached ? 0 : s->Pid,
        (request ? RequestThreadId(request) : (kernel ? 0 : s->Thread)), n->Context,
        q->Handle, q->Scalar, (ULONGLONG)n->Physical.QuadPart | 0x100000000ULL,
        (used + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u) };
    n->Reply = FALSE;
    BOOLEAN sent = FALSE;
    KEVENT wake; KeInitializeEvent(&wake, SynchronizationEvent, FALSE);
    PEX_TIMER timer = ExAllocateTimer(NpuWakeTimer, &wake, EX_TIMER_HIGH_RESOLUTION);
    ULONGLONG deadline = KeQueryInterruptTime() + 100000000ULL;
    while (KeQueryInterruptTime() < deadline) {
        NpuLinkPoll(n);
        if (n->Reply) {
            KeMemoryBarrier();
            q->Result = n->LastResult;
            *bytes = base;
            for (ULONG i = inputs; i < q->Count; i++) {
                // Trust our original offsets and lengths, never DSP-written descriptors.
                RtlCopyMemory((PUCHAR)q + q->Arg[i].Offset, n->Rpc + n->Offset[i], q->Arg[i].Length);
                *bytes = max(*bytes, q->Arg[i].Offset + q->Arg[i].Length);
            }
            status = STATUS_SUCCESS; goto done;
        }
        if (!n->Online || n->Closed) { status = STATUS_DEVICE_NOT_CONNECTED; goto fail; }
        if (request && WdfRequestIsCanceled(request)) { status = STATUS_CANCELLED; goto fail; }
        if (!sent) {
            KeMemoryBarrier();
            sent = NpuLinkSend(n, &message);
            if (sent) { n->Pending = TRUE; n->Requests++; }
        }
        if (timer) {
            ExSetTimer(timer, -1000, 0, NULL);
            KeWaitForSingleObject(&wake, Executive, KernelMode, FALSE, NULL);
        } else NpuSleep();
    }
    status = STATUS_IO_TIMEOUT;
fail:
    // A timeout is not DMA completion: keep the allocation and its SMMU mapping
    // until a matching response or confirmed process release has arrived.
    if (sent) n->Poisoned = TRUE;
done:
    if (timer) ExDeleteTimer(timer, TRUE, TRUE, NULL);
    return status;
}
NTSTATUS NpuAttach(NPU_CLIENT *s, WDFREQUEST request) {
    if (s->Attached) return STATUS_SUCCESS;
    struct { QCOM_NPU_PACKET Packet; UCHAR Padding[16]; } data = { 0 };
    QCOM_NPU_PACKET *q = &data.Packet;
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
    ULONG pid = IoGetRequestorProcessId(WdfRequestWdmGetIrp(request));
    q->Abi = QCOM_NPU_ABI;
    q->Handle = 1;
    q->Scalar = 0x10000;
    q->Count = 1;
    q->Arg[0] = (QCOM_NPU_ARGUMENT){ base, sizeof(pid) };
    RtlCopyMemory(q->Data, &pid, sizeof(pid));
    NTSTATUS status = NpuInvoke(s, q, sizeof(data), sizeof(data), request, &bytes, TRUE);
    if (NT_SUCCESS(status) && q->Result) status = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(status)) { s->Attached = TRUE; s->Pid = pid; }
    return status;
}
NTSTATUS NpuCreateProcess(NPU_CLIENT *s, const QCOM_NPU_PROCESS *p, ULONG input, WDFREQUEST request) {
    NPU *n = s->Device;
    ULONG header = FIELD_OFFSET(QCOM_NPU_PROCESS, Shell);
    if (input < header || p->Abi != QCOM_NPU_ABI || p->Attributes != 8 ||
        p->ShellBytes < 52 || p->ShellBytes > QCOM_NPU_MAX_SHELL - 1024 ||
        p->ShellBytes != input - header || p->HeapBytes < 0x200000 ||
        p->HeapBytes > QCOM_NPU_MAX_HEAP || (p->HeapBytes & (0x100000 - 1)) ||
        p->HeapBytes < p->ShellBytes * 4)
        return STATUS_INVALID_PARAMETER;
    // ELF32, little endian, Hexagon (EM_HEXAGON=164), executable or shared object.
    if (RtlCompareMemory(p->Shell, "\177ELF\1\1\1", 7) != 7 ||
        (p->Shell[16] != 2 && p->Shell[16] != 3) || p->Shell[17] ||
        p->Shell[18] != 164 || p->Shell[19])
        return STATUS_INVALID_IMAGE_FORMAT;
    if (s->Attached || n->Pending || n->Poisoned || s->Heap) return STATUS_DEVICE_BUSY;
    NTSTATUS status = NpuMappingAcquire(n);
    if (!NT_SUCCESS(status)) return status;
    PHYSICAL_ADDRESS low = { 0 }, high, boundary = { 0 };
    high.QuadPart = MAXULONG;
    s->Heap = MmAllocateContiguousMemorySpecifyCache(p->HeapBytes, low, high, boundary, MmNonCached);
    if (!s->Heap) return STATUS_INSUFFICIENT_RESOURCES;
    s->HeapBytes = p->HeapBytes;
    s->HeapPhysical = MmGetPhysicalAddress(s->Heap);
    RtlZeroMemory(s->Heap, s->HeapBytes);
    const CHAR name[] = "qcom-npu";
    struct { ULONG Pid, NameBytes, FileBytes, Pages, Attributes, SignatureBytes; } parameters = {
        IoGetRequestorProcessId(WdfRequestWdmGetIrp(request)), sizeof(name), p->ShellBytes, 1, 8, 0 };
    NPU_PAGE page = { (ULONGLONG)s->HeapPhysical.QuadPart | 0x100000000ULL, s->HeapBytes };
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data);
    ULONG size = base + sizeof(parameters) + sizeof(name) + p->ShellBytes + sizeof(page);
    QCOM_NPU_PACKET *q = ExAllocatePool2(POOL_FLAG_NON_PAGED, size, NPU_TAG);
    if (!q) { status = STATUS_INSUFFICIENT_RESOURCES; goto free_heap; }
    q->Abi = QCOM_NPU_ABI; q->Handle = 1; q->Scalar = 0x07040000; q->Count = 4;
    ULONG at = base;
    const VOID *values[] = { &parameters, name, p->Shell, &page };
    const ULONG lengths[] = { sizeof(parameters), sizeof(name), p->ShellBytes, sizeof(page) };
    for (ULONG i = 0; i < 4; i++) {
        q->Arg[i] = (QCOM_NPU_ARGUMENT){ at, lengths[i] };
        RtlCopyMemory((PUCHAR)q + at, values[i], lengths[i]); at += lengths[i];
    }
    ULONG bytes = 0;
    s->Pd = 1;
    status = NpuInvoke(s, q, size, size, request, &bytes, TRUE);
    if (NT_SUCCESS(status) && q->Result) status = STATUS_IO_DEVICE_ERROR;
    ExFreePoolWithTag(q, NPU_TAG);
    if (NT_SUCCESS(status)) { s->Attached = TRUE; s->Pid = parameters.Pid; return status; }
    // A transmitted request may still be using the initial process heap.
    if (n->Pending) { s->Attached = TRUE; s->Pid = parameters.Pid; return status; }
    s->Pd = 0;
free_heap:
    MmFreeContiguousMemorySpecifyCache(s->Heap, s->HeapBytes, MmNonCached);
    s->Heap = NULL; s->HeapBytes = 0;
    return status;
}
VOID NpuDetach(NPU_CLIENT *s) {
    NPU *n = s->Device;
    if (!s->Attached || n->Pending || NpuGpuBusy(s)) return;
    struct { QCOM_NPU_PACKET Packet; UCHAR Padding[16]; } data = { 0 };
    QCOM_NPU_PACKET *q = &data.Packet;
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
    q->Abi = QCOM_NPU_ABI;
    q->Handle = 1;
    q->Scalar = 0x1010000;
    q->Count = 1;
    q->Arg[0] = (QCOM_NPU_ARGUMENT){ base, sizeof(s->Pid) };
    RtlCopyMemory(q->Data, &s->Pid, sizeof(s->Pid));
    BOOLEAN poisoned = n->Poisoned;
    n->Poisoned = FALSE;
    NTSTATUS status = NpuInvoke(s, q, sizeof(data), sizeof(data), NULL, &bytes, TRUE);
    if (NT_SUCCESS(status) && !q->Result) {
        s->Attached = FALSE;
        s->Pd = 0;
        n->Poisoned = FALSE;
        NpuFreeAllocations(s);
        // Confirmed process release fences its outstanding listener and graph
        // requests. Closing a file never frees DMA before that acknowledgement.
        for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
            NPU_ASYNC_CALL *call = &s->Calls[i];
            if (call->Rpc) MmFreeContiguousMemorySpecifyCache(call->Rpc, call->Bytes, MmNonCached);
            RtlZeroMemory(call, sizeof(*call));
        }
        if (s->Heap) {
            MmFreeContiguousMemorySpecifyCache(s->Heap, s->HeapBytes, MmNonCached);
            s->Heap = NULL; s->HeapBytes = 0;
        }
    } else n->Poisoned = poisoned || n->Pending;
}

NTSTATUS NpuSubmit(NPU_CLIENT *s, QCOM_NPU_PACKET *q, ULONG input, WDFREQUEST request, QCOM_NPU_TICKET *ticket) {
    return NpuSubmitThread(s, q, input, RequestThreadId(request), ticket);
}
NTSTATUS NpuSubmitThread(NPU_CLIENT *s, QCOM_NPU_PACKET *q, ULONG input, ULONG thread, QCOM_NPU_TICKET *ticket) {
    NPU *n = s->Device;
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data);
    if (input < base || q->Abi != QCOM_NPU_ABI || q->Count > QCOM_NPU_MAX_ARGUMENTS ||
        (q->Scalar & 255u) || q->Count != ((q->Scalar >> 16) & 255u) + ((q->Scalar >> 8) & 255u))
        return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1 || q->Handle == 1 ||
        (q->Handle == 0 && q->Scalar != 0x20200u && q->Scalar != 0x1010200u))
        return STATUS_ACCESS_DENIED;
    ULONG inputs = (q->Scalar >> 16) & 255u;
    ULONG used = (40 * q->Count + 388 + 127) & ~127u;
    for (ULONG i = 0; i < q->Count; i++) {
        ULONG limit = i < inputs ? input : QCOM_NPU_MAX_DATA + base;
        if (q->Arg[i].Offset < base || q->Arg[i].Offset > limit || q->Arg[i].Length > limit - q->Arg[i].Offset)
            return STATUS_INVALID_PARAMETER;
        used = (used + 127) & ~127u;
        if (used > QCOM_NPU_MAX_DATA || q->Arg[i].Length > QCOM_NPU_MAX_DATA - used) return STATUS_BUFFER_TOO_SMALL;
        used += q->Arg[i].Length;
    }
    if (!n->Online || n->Closed) return STATUS_DEVICE_NOT_CONNECTED;
    if (n->Poisoned || (NpuGpuBusy(s) && q->Handle != 3)) return STATUS_DEVICE_BUSY;
    ULONG slot;
    for (slot = 0; slot < NPU_ASYNC_SLOTS; slot++) if (!s->Calls[slot].Rpc) break;
    if (slot == NPU_ASYNC_SLOTS) return STATUS_DEVICE_BUSY;
    NPU_ASYNC_CALL *call = &s->Calls[slot];
    PHYSICAL_ADDRESS low = { 0 }, high, boundary = { 0 };
    high.QuadPart = MAXULONG;
    call->Bytes = (used + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1u);
    call->Rpc = MmAllocateContiguousMemorySpecifyCache(call->Bytes, low, high, boundary, MmNonCached);
    if (!call->Rpc) { call->Bytes = 0; return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(call->Rpc, call->Bytes);
    PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(call->Rpc);
    call->Handle = q->Handle; call->Scalar = q->Scalar; call->Count = q->Count; call->Inputs = inputs;
    NPU_ARGUMENT *args = (NPU_ARGUMENT *)call->Rpc;
    NPU_BUFFER *bufs = (NPU_BUFFER *)(args + q->Count);
    NPU_PAGE *pages = (NPU_PAGE *)(bufs + q->Count);
    ULONG at = (40 * q->Count + 388 + 127) & ~127u;
    for (ULONG i = 0; i < q->Count; i++) {
        at = (at + 127) & ~127u;
        call->Offset[i] = at; call->Arg[i] = q->Arg[i];
        args[i] = (NPU_ARGUMENT){ (ULONGLONG)(ULONG_PTR)(call->Rpc + at), q->Arg[i].Length };
        bufs[i] = (NPU_BUFFER){ q->Arg[i].Length ? 1 : 0, i };
        pages[i] = (NPU_PAGE){ ((ULONGLONG)physical.QuadPart + (at & ~(PAGE_SIZE - 1u))) | 0x100000000ULL,
            (q->Arg[i].Length + (at & (PAGE_SIZE - 1u)) + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u) };
        if (!q->Arg[i].Length) { args[i].Pointer = 0; pages[i] = (NPU_PAGE){ 0 }; }
        if (i < inputs) RtlCopyMemory(call->Rpc + at, (PUCHAR)q + q->Arg[i].Offset, q->Arg[i].Length);
        at += q->Arg[i].Length;
    }
    call->Context = (++n->Sequence << 16) | ((ULONGLONG)(slot + 2) << 4) | s->Pd;
    NPU_MESSAGE message = { s->Pid, thread, call->Context,
        q->Handle, q->Scalar, (ULONGLONG)physical.QuadPart | 0x100000000ULL, call->Bytes };
    NpuLinkPoll(n);
    KeMemoryBarrier();
    if (!NpuLinkSend(n, &message)) {
        MmFreeContiguousMemorySpecifyCache(call->Rpc, call->Bytes, MmNonCached);
        RtlZeroMemory(call, sizeof(*call));
        return STATUS_DEVICE_BUSY;
    }
    call->Pending = TRUE; n->Requests++;
    *ticket = (QCOM_NPU_TICKET){ QCOM_NPU_ABI, 0, call->Context };
    return STATUS_SUCCESS;
}
NTSTATUS NpuCollect(NPU_CLIENT *s, const QCOM_NPU_TICKET *ticket, QCOM_NPU_PACKET *q, ULONG output, PULONG bytes) {
    NPU *n = s->Device;
    if (ticket->Abi != QCOM_NPU_ABI || ticket->Reserved) return STATUS_INVALID_PARAMETER;
    NpuLinkPoll(n);
    for (ULONG slot = 0; slot < NPU_ASYNC_SLOTS; slot++) {
        NPU_ASYNC_CALL *call = &s->Calls[slot];
        if (!call->Rpc || call->Context != ticket->Token) continue;
        if (!call->Complete) return STATUS_DEVICE_BUSY;
        ULONG size = FIELD_OFFSET(QCOM_NPU_PACKET, Data);
        for (ULONG i = call->Inputs; i < call->Count; i++) size = max(size, call->Arg[i].Offset + call->Arg[i].Length);
        if (output < size) return STATUS_BUFFER_TOO_SMALL;
        KeMemoryBarrier();
        RtlZeroMemory(q, size);
        q->Abi = QCOM_NPU_ABI; q->Handle = call->Handle; q->Scalar = call->Scalar;
        q->Count = call->Count; q->Result = call->Result;
        RtlCopyMemory(q->Arg, call->Arg, sizeof(q->Arg));
        for (ULONG i = call->Inputs; i < call->Count; i++)
            RtlCopyMemory((PUCHAR)q + call->Arg[i].Offset, call->Rpc + call->Offset[i], call->Arg[i].Length);
        MmFreeContiguousMemorySpecifyCache(call->Rpc, call->Bytes, MmNonCached);
        RtlZeroMemory(call, sizeof(*call));
        *bytes = size;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}
