#include <ntifs.h>
#include "npu.h"

NTSYSAPI NTSTATUS NTAPI PsAcquireProcessExitSynchronization(PEPROCESS Process);
NTSYSAPI VOID NTAPI PsReleaseProcessExitSynchronization(PEPROCESS Process);

static NTSTATUS MapFile(NPU_CLIENT *s, NPU_ALLOCATION *a, WDFREQUEST request) {
    struct { QCOM_NPU_PACKET Packet; UCHAR Extra[64]; } storage = { 0 };
    QCOM_NPU_PACKET *q = &storage.Packet;
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
    struct { ULONG Pid, Fd, Offset, Flags; ULONGLONG Virtual; ULONG PageBytes, DataBytes; } parameters = {
        s->Pid, a->Id, 0, a->Flags == QCOM_NPU_SHARED ? 2 : a->Flags, 0, sizeof(NPU_PAGE), 0 };
    NPU_PAGE page = { (a->Paged ? a->Iova : (ULONGLONG)MmGetPhysicalAddress(a->Buffer).QuadPart) | 0x100000000ULL, a->Bytes };
    RtlCopyMemory((PUCHAR)q + base + sizeof(parameters), &page, sizeof(page));
    q->Abi = QCOM_NPU_ABI; q->Handle = 1; q->Scalar = 0x0a030100; q->Count = 4;
    q->Arg[0] = (QCOM_NPU_ARGUMENT){ base, sizeof(parameters) };
    q->Arg[1] = (QCOM_NPU_ARGUMENT){ base + sizeof(parameters), parameters.PageBytes };
    q->Arg[2] = (QCOM_NPU_ARGUMENT){ base, 0 };
    q->Arg[3] = (QCOM_NPU_ARGUMENT){ base + sizeof(parameters) + parameters.PageBytes, sizeof(a->Remote) };
    RtlCopyMemory((PUCHAR)q + q->Arg[0].Offset, &parameters, sizeof(parameters));
    ULONG capacity = q->Arg[3].Offset + q->Arg[3].Length;
    NTSTATUS status = NpuInvoke(s, q, capacity, capacity, request, &bytes, TRUE);
    if (NT_SUCCESS(status) && q->Result) status = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(status)) RtlCopyMemory(&a->Remote, (PUCHAR)q + q->Arg[3].Offset, sizeof(a->Remote));
    return status;
}

static VOID ReleaseAllocation(NPU_ALLOCATION *a) {
    if (a->VideoRefs || a->Registered) { a->Detached = TRUE; return; }
    if (a->UserAddress && a->Owner) {
        NTSTATUS status = PsAcquireProcessExitSynchronization(a->Owner);
        if (NT_SUCCESS(status)) {
            KAPC_STATE state;
            KeStackAttachProcess(a->Owner, &state);
            MmUnmapLockedPages(a->UserAddress, a->Mdl);
            KeUnstackDetachProcess(&state);
            PsReleaseProcessExitSynchronization(a->Owner);
        } else {
            // Exit has begun, but its user mappings may still reference these
            // pages. Keep them until the process dispatcher object is signaled.
            LARGE_INTEGER poll = {0};
            if (KeWaitForSingleObject(a->Owner, Executive, KernelMode, FALSE, &poll) != STATUS_SUCCESS) {
                a->Detached = TRUE; return;
            }
        }
    }
    if (a->Owner) ObDereferenceObject(a->Owner);
    if (a->Paged) {
        if (a->Buffer) MmUnmapLockedPages(a->Buffer, a->Mdl);
        if (a->Mdl) { MmFreePagesFromMdl(a->Mdl); ExFreePool(a->Mdl); }
    } else {
        if (a->Mdl) IoFreeMdl(a->Mdl);
        if (a->Buffer) MmFreeContiguousMemorySpecifyCache(a->Buffer, a->Bytes, MmNonCached);
    }
    RtlZeroMemory(a, sizeof(*a));
}

VOID NpuFreeAllocations(NPU_CLIENT *s) {
    NPU *n = s->Device;
    // Called only after confirmed DSP process release, which fences every map.
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
        NPU_ALLOCATION *a = &s->Allocations[i];
        if (a->Iova && !NT_SUCCESS(NpuIommuUnmap(n, a))) continue;
        ReleaseAllocation(a);
    }
}
VOID NpuMemoryInitialize(NPU *n) {
    ULONGLONG visible = 0, limit = n->PagedMemory ? MAXULONGLONG : 0x100000000ULL;
    ULONGLONG reserve = n->PagedMemory ? 0x40000000ULL : 0x10000000ULL;
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    n->MemoryBudget = 768ULL * 1024 * 1024;
    if (!ranges) return;
    for (ULONG i=0; ranges[i].NumberOfBytes.QuadPart; i++) {
        ULONGLONG base = ranges[i].BaseAddress.QuadPart, bytes = ranges[i].NumberOfBytes.QuadPart;
        if (n->PagedMemory) {
            ULONGLONG first = max(base, 0x100000000ULL);
            if (base + bytes > first) visible += base + bytes - first;
        } else if (base < limit) visible += min(bytes, limit - base);
    }
    ExFreePool(ranges);
    n->MemoryBudget = visible > reserve ? (n->PagedMemory ? visible - reserve : min(visible - reserve, 0xc0000000ULL)) : 0;
}
VOID NpuMemoryStatus(NPU *n, QCOM_NPU_MEMORY_STATUS *status) {
    *status = (QCOM_NPU_MEMORY_STATUS){QCOM_NPU_ABI, 0, 0, n->PagedMemory ? 0x70000000u : 0,
        n->MemoryBudget, n->RpcBytes, 0, 0};
    ULONG attached = 0;
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        if (s->Attached) { attached++; status->Owner = s->Pid; }
        status->Allocated += s->HeapBytes;
        for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) status->Allocated += s->Calls[i].Bytes;
        for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
            NPU_ALLOCATION *a = &s->Allocations[i];
            if (!a->Bytes) continue;
            status->Allocations++; status->Allocated += a->Bytes;
            if (a->UserAddress) status->Shared += a->Bytes;
        }
    }
    if (attached != 1) status->Owner = 0;
    status->Available = status->Allocated < status->Budget ? status->Budget - status->Allocated : 0;
}
BOOLEAN NpuMemoryReference(NPU *n, ULONG id, ULONG owner, ULONG bytes, ULONGLONG address, ULONGLONG cookie, BOOLEAN acquire) {
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        if (s->Pid != owner) continue;
        for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
            NPU_ALLOCATION *a = &s->Allocations[i];
            if (!a->Buffer || a->Id != id || a->Bytes != bytes || !a->Owner ||
                HandleToUlong(PsGetProcessId(a->Owner)) != owner || (ULONGLONG)a->UserAddress != address) continue;
            if (acquire) {
                if (!cookie || a->Detached || !s->Attached || s->Closing || a->VideoRefs == MAXULONG ||
                    (a->Registered && a->VideoCookie != cookie)) return FALSE;
                a->Registered = TRUE; a->VideoCookie = cookie; a->VideoRefs++;
            } else {
                if (!a->VideoRefs || a->VideoCookie != cookie) return FALSE;
                a->VideoRefs--;
            }
            return TRUE;
        }
    }
    return FALSE;
}
VOID NpuMemoryPurge(NPU *n, ULONG owner, ULONGLONG cookie) {
    if (!cookie) return;
    PLIST_ENTRY p = n->Clients.Flink;
    while (p != &n->Clients) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry); p = p->Flink;
        if (s->Pid != owner) continue;
        for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
            NPU_ALLOCATION *a = &s->Allocations[i];
            if (a->VideoCookie != cookie || a->VideoRefs || !a->Owner) continue;
            a->Registered = FALSE;
            if (a->Detached) ReleaseAllocation(a);
        }
        NpuClientRetire(s);
    }
}
NTSTATUS NpuAllocate(NPU_CLIENT *s, QCOM_NPU_ALLOCATION *q, WDFREQUEST request) {
    NPU *n = s->Device;
    if (q->Abi != QCOM_NPU_ABI || q->Id || q->DspAddress || (q->Flags != 0x1000 && q->Flags != 2 && q->Flags != 3 && q->Flags != QCOM_NPU_SHARED) ||
        !q->Bytes || q->Bytes > (q->Flags == QCOM_NPU_SHARED ? QCOM_NPU_MAX_SHARED : QCOM_NPU_MAX_HEAP) || (q->Bytes & (PAGE_SIZE - 1)))
        return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1) return STATUS_ACCESS_DENIED;
    if (!n->PagedMemory && q->Flags == QCOM_NPU_SHARED && q->Bytes > 0x10000000u) return STATUS_NOT_SUPPORTED;
    if (n->Pending || n->Poisoned || NpuGpuBusy(s)) return STATUS_DEVICE_BUSY;
    QCOM_NPU_MEMORY_STATUS memory; NpuMemoryStatus(n, &memory);
    ULONG slot = NPU_ALLOCATIONS;
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) if (!s->Allocations[i].Buffer && slot == NPU_ALLOCATIONS) slot = i;
    if (slot == NPU_ALLOCATIONS || q->Bytes > memory.Available)
        return STATUS_INSUFFICIENT_RESOURCES;
    NPU_ALLOCATION *a = &s->Allocations[slot];
    PHYSICAL_ADDRESS low = { 0 }, high, boundary = { 0 };
    high.QuadPart = MAXULONG;
    if (q->Flags == QCOM_NPU_SHARED && n->PagedMemory) {
        low.QuadPart = 0x100000000ULL;
        high.QuadPart = MAXLONGLONG;
        a->Paged = TRUE; a->Bytes = q->Bytes;
        a->Mdl = MmAllocatePagesForMdlEx(low, high, boundary, q->Bytes, MmNonCached,
            MM_ALLOCATE_FULLY_REQUIRED | MM_ALLOCATE_NO_WAIT);
        if (!a->Mdl || MmGetMdlByteCount(a->Mdl) != q->Bytes) { ReleaseAllocation(a); return STATUS_INSUFFICIENT_RESOURCES; }
        a->Buffer = MmMapLockedPagesSpecifyCache(a->Mdl, KernelMode, MmNonCached, NULL, FALSE,
            NormalPagePriority | MdlMappingNoExecute);
    } else if (q->Flags == QCOM_NPU_SHARED) {
        // The existing firmware route supplies one contiguous DMA range per fd.
        a->Bytes = q->Bytes;
        a->Buffer = MmAllocateContiguousMemorySpecifyCache(q->Bytes, low, high, boundary, MmNonCached);
        if (a->Buffer) {
            a->Mdl = IoAllocateMdl(a->Buffer, q->Bytes, FALSE, FALSE, NULL);
            if (a->Mdl) MmBuildMdlForNonPagedPool(a->Mdl);
            else { ReleaseAllocation(a); return STATUS_INSUFFICIENT_RESOURCES; }
        }
    } else a->Buffer = MmAllocateContiguousMemorySpecifyCache(q->Bytes, low, high, boundary, MmNonCached);
    if (!a->Buffer) { ReleaseAllocation(a); return STATUS_INSUFFICIENT_RESOURCES; }
    a->Bytes = q->Bytes;
    a->Flags = q->Flags;
    a->Id = ++n->AllocationSequence;
    if (!a->Id) a->Id = ++n->AllocationSequence;
    RtlZeroMemory(a->Buffer, a->Bytes);
    if (a->Paged) { q->Id = a->Id; q->DspAddress = 0; return STATUS_SUCCESS; }
    if (q->Flags == 2 || q->Flags == 3 || q->Flags == QCOM_NPU_SHARED) {
        NTSTATUS status = MapFile(s, a, request);
        if (NT_SUCCESS(status)) { q->Id = a->Id; q->DspAddress = a->Remote; }
        else if (!n->Pending) {
            ReleaseAllocation(a);
        }
        return status;
    }
    struct { QCOM_NPU_PACKET Packet; UCHAR Extra[64]; } storage = { 0 };
    QCOM_NPU_PACKET *packet = &storage.Packet;
    struct { ULONG Pid, Flags; ULONGLONG Virtual; ULONG PageBytes, Reserved; } parameters = { s->Pid, q->Flags, 0, sizeof(NPU_PAGE), 0 };
    NPU_PAGE page = { (ULONGLONG)MmGetPhysicalAddress(a->Buffer).QuadPart | 0x100000000ULL, a->Bytes };
    ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
    packet->Abi = QCOM_NPU_ABI; packet->Handle = 1; packet->Scalar = 0x04020100; packet->Count = 3;
    packet->Arg[0] = (QCOM_NPU_ARGUMENT){ base, sizeof(parameters) };
    packet->Arg[1] = (QCOM_NPU_ARGUMENT){ base + sizeof(parameters), sizeof(page) };
    packet->Arg[2] = (QCOM_NPU_ARGUMENT){ base + sizeof(parameters) + sizeof(page), sizeof(a->Remote) };
    RtlCopyMemory((PUCHAR)packet + packet->Arg[0].Offset, &parameters, sizeof(parameters));
    RtlCopyMemory((PUCHAR)packet + packet->Arg[1].Offset, &page, sizeof(page));
    NTSTATUS status = NpuInvoke(s, packet, sizeof(storage), sizeof(storage), request, &bytes, TRUE);
    if (NT_SUCCESS(status) && packet->Result) status = STATUS_IO_DEVICE_ERROR;
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(&a->Remote, (PUCHAR)packet + packet->Arg[2].Offset, sizeof(a->Remote));
        if (!a->Remote) { n->Poisoned = TRUE; return STATUS_DEVICE_PROTOCOL_ERROR; }
        q->Id = a->Id; q->DspAddress = a->Remote;
    } else if (!n->Pending) {
        MmFreeContiguousMemorySpecifyCache(a->Buffer, a->Bytes, MmNonCached);
        RtlZeroMemory(a, sizeof(*a));
    }
    // An unconfirmed mapping retains the owned allocation until process release.
    return status;
}
NTSTATUS NpuAllocateShared(NPU_CLIENT *s, QCOM_NPU_SHARED_ALLOCATION *q, WDFREQUEST request) {
    if (q->UserAddress || (q->Allocation.Flags != QCOM_NPU_SHARED && q->Allocation.Flags != 3) ||
        !s->Pid || s->Pid != IoGetRequestorProcessId(WdfRequestWdmGetIrp(request))) return STATUS_ACCESS_DENIED;
    PEPROCESS process = NULL;
    NTSTATUS status = PsLookupProcessByProcessId(UlongToHandle(s->Pid), &process);
    if (!NT_SUCCESS(status)) return status;
    status = NpuAllocate(s, &q->Allocation, request);
    if (!NT_SUCCESS(status)) { ObDereferenceObject(process); return status; }
    NPU_ALLOCATION *a = NULL;
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) if (s->Allocations[i].Id == q->Allocation.Id) { a = &s->Allocations[i]; break; }
    if (!a) { ObDereferenceObject(process); return STATUS_INTERNAL_ERROR; }
    if (!a->Mdl) {
        a->Mdl = IoAllocateMdl(a->Buffer, a->Bytes, FALSE, FALSE, NULL);
        if (!a->Mdl) { ObDereferenceObject(process); return STATUS_INSUFFICIENT_RESOURCES; }
        MmBuildMdlForNonPagedPool(a->Mdl);
    }
    a->Owner = process;
    status = PsAcquireProcessExitSynchronization(process);
    if (NT_SUCCESS(status)) {
        KAPC_STATE state;
        KeStackAttachProcess(process, &state);
        __try {
            a->UserAddress = MmMapLockedPagesSpecifyCache(a->Mdl, UserMode, MmNonCached, NULL, FALSE, NormalPagePriority | MdlMappingNoExecute);
            if (!a->UserAddress) status = STATUS_INSUFFICIENT_RESOURCES;
        } __except (EXCEPTION_EXECUTE_HANDLER) { status = GetExceptionCode(); }
        KeUnstackDetachProcess(&state);
        PsReleaseProcessExitSynchronization(process);
    }
    if (NT_SUCCESS(status)) q->UserAddress = (ULONGLONG)(ULONG_PTR)a->UserAddress;
    // A failed host mapping keeps its DSP allocation until process release.
    return status;
}
NTSTATUS NpuFree(NPU_CLIENT *s, const QCOM_NPU_ALLOCATION *q, WDFREQUEST request) {
    NPU *n = s->Device;
    if (q->Abi != QCOM_NPU_ABI || (q->Flags != 0x1000 && q->Flags != 2 && q->Flags != 3 && q->Flags != QCOM_NPU_SHARED) || !q->Id || !q->Bytes ||
        (q->Flags == 0x1000 && !q->DspAddress))
        return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1) return STATUS_ACCESS_DENIED;
    if (n->Pending || n->Poisoned || NpuGpuBusy(s)) return STATUS_DEVICE_BUSY;
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) {
        NPU_SCHEDULED_CALL *call = &s->Scheduled[i];
        if (call->Kind == 2 && (call->State == 1 || call->State == 2)) {
            const QCOM_NPU_QUEUE_DISPATCH *dispatch = (const QCOM_NPU_QUEUE_DISPATCH *)call->Packet;
            if (dispatch->QueueId == q->Id || dispatch->StateId == q->Id) return STATUS_DEVICE_BUSY;
            for (ULONG j = 0; j < call->BufferCount; j++) if (call->Buffers[j] == q->Id) return STATUS_DEVICE_BUSY;
        }
    }
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
        NPU_ALLOCATION *a = &s->Allocations[i];
        if (!a->Buffer || a->Id != q->Id || (!a->Paged && a->Remote != q->DspAddress) || a->Bytes != q->Bytes || a->Flags != q->Flags) continue;
        if (a->Detached) {
            if (!a->VideoRefs) { a->Registered = FALSE; ReleaseAllocation(a); }
            return STATUS_SUCCESS;
        }
        if (a->Paged) {
            QCOM_NPU_SHARED_MAPPING mapping = {QCOM_NPU_ABI, a->Id, 0, 0};
            NTSTATUS status = NpuMapShared(s, &mapping, request);
            if (NT_SUCCESS(status)) ReleaseAllocation(a);
            return status;
        }
        struct { QCOM_NPU_PACKET Packet; UCHAR Extra[32]; } storage = { 0 };
        struct { ULONG Pid, Reserved; ULONGLONG Bytes, Remote; } parameters = { s->Pid, 0, a->Bytes, a->Remote };
        if (a->Flags == 2 || a->Flags == 3 || a->Flags == QCOM_NPU_SHARED) {
            parameters.Reserved = a->Id; parameters.Bytes = a->Remote; parameters.Remote = a->Bytes;
        }
        QCOM_NPU_PACKET *packet = &storage.Packet;
        ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
        packet->Abi = QCOM_NPU_ABI; packet->Handle = 1; packet->Scalar = 0x05010000; packet->Count = 1;
        if (a->Flags == 2 || a->Flags == 3 || a->Flags == QCOM_NPU_SHARED) packet->Scalar = 0x0b010000;
        packet->Arg[0] = (QCOM_NPU_ARGUMENT){ base, sizeof(parameters) };
        RtlCopyMemory((PUCHAR)packet + base, &parameters, sizeof(parameters));
        NTSTATUS status = NpuInvoke(s, packet, sizeof(storage), sizeof(storage), request, &bytes, TRUE);
        if (NT_SUCCESS(status) && packet->Result) status = STATUS_IO_DEVICE_ERROR;
        if (NT_SUCCESS(status)) {
            ReleaseAllocation(a);
        }
        return status;
    }
    return STATUS_INVALID_PARAMETER;
}
NTSTATUS NpuUnmapAllocation(NPU_CLIENT *s, NPU_ALLOCATION *a, WDFREQUEST request) {
    NPU *n = s->Device;
    if (a->DspMapped) {
        if (!a->Pinned && s->ComputeHandle) {
            struct { QCOM_NPU_PACKET Packet; UCHAR Extra[8]; } storage = {0};
            QCOM_NPU_PACKET *q = &storage.Packet; ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
            q->Abi = 1; q->Handle = s->ComputeHandle; q->Scalar = 0x05010000; q->Count = 1;
            q->Arg[0] = (QCOM_NPU_ARGUMENT){base, sizeof(a->Id)}; RtlCopyMemory((PUCHAR)q + base, &a->Id, sizeof(a->Id));
            NTSTATUS status = NpuInvoke(s, q, sizeof(storage), sizeof(storage), NULL, &bytes, FALSE);
            if (!NT_SUCCESS(status) || q->Result) return NT_SUCCESS(status) ? STATUS_IO_DEVICE_ERROR : status;
        }
        struct { QCOM_NPU_PACKET Packet; UCHAR Extra[32]; } storage = {0};
        struct { ULONG Pid, Fd; ULONGLONG Remote, Bytes; } parameters = {s->Pid, a->Id, a->Remote, a->Bytes};
        QCOM_NPU_PACKET *q = &storage.Packet; ULONG base = FIELD_OFFSET(QCOM_NPU_PACKET, Data), bytes = 0;
        q->Abi = 1; q->Handle = 1; q->Scalar = 0x0b010000; q->Count = 1;
        q->Arg[0] = (QCOM_NPU_ARGUMENT){base, sizeof(parameters)}; RtlCopyMemory((PUCHAR)q + base, &parameters, sizeof(parameters));
        NTSTATUS status = NpuInvoke(s, q, sizeof(storage), sizeof(storage), request, &bytes, TRUE);
        if (!NT_SUCCESS(status)) return status;
        if (q->Result) return STATUS_IO_DEVICE_ERROR;
        a->DspMapped = FALSE; a->Remote = 0;
    }
    return NpuIommuUnmap(n, a);
}
NTSTATUS NpuMapAllocation(NPU_CLIENT *s, NPU_ALLOCATION *a, WDFREQUEST request) {
    NPU *n = s->Device;
    if (a->DspMapped) return STATUS_SUCCESS;
    NTSTATUS status = NpuIommuMap(n, a);
    if (NT_SUCCESS(status)) status = MapFile(s, a, request);
    if (NT_SUCCESS(status)) a->DspMapped = TRUE;
    else if (!n->Pending && !n->Poisoned) NpuIommuUnmap(n, a);
    return status;
}
static BOOLEAN Required(const ULONG *ids, ULONG count, ULONG id) {
    for (ULONG i = 0; i < count; i++) if (ids[i] == id) return TRUE;
    return FALSE;
}
NTSTATUS NpuPrepareBuffers(NPU_CLIENT *s, const ULONG *ids, ULONG count) {
    NPU *n = s->Device;
    if (NpuGpuBusy(s) || n->Pending || n->Poisoned) return STATUS_DEVICE_BUSY;
    ULONG order[QCOM_NPU_MODEL_BUFFERS + 3];
    if (count > RTL_NUMBER_OF(order)) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < count; i++) {
        if (!NpuAllocation(s, ids[i])) return STATUS_INVALID_PARAMETER;
        order[i] = ids[i];
        for (ULONG j = i; j && NpuAllocation(s, order[j])->Bytes > NpuAllocation(s, order[j - 1])->Bytes; j--) {
            ULONG swap = order[j]; order[j] = order[j - 1]; order[j - 1] = swap;
        }
    }
    BOOLEAN compacted = FALSE;
restart:
    for (ULONG i = 0; i < count; i++) {
        NPU_ALLOCATION *a = NpuAllocation(s, order[i]);
        a->LastUse = ++n->MapSequence;
        while (!a->DspMapped) {
            ULONG cached = 0;
            for (ULONG j = 0; j < NPU_ALLOCATIONS; j++) if (s->Allocations[j].DspMapped && !s->Allocations[j].Pinned) cached++;
            NTSTATUS status = cached >= QCOM_NPU_MODEL_BUFFERS && !a->Pinned ? STATUS_INSUFFICIENT_RESOURCES : NpuMapAllocation(s, a, NULL);
            if (NT_SUCCESS(status)) break;
            if (status != STATUS_INSUFFICIENT_RESOURCES) return status;
            NPU_CLIENT *owner = NULL; NPU_ALLOCATION *victim = NULL;
            for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
                NPU_CLIENT *other = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
                if (!other->Attached || (other == n->Running && !n->Preparing)) continue;
                for (ULONG j = 0; j < NPU_ALLOCATIONS; j++) {
                    NPU_ALLOCATION *item = &other->Allocations[j];
                    if (!item->DspMapped || item->Pinned || (other == s && Required(ids, count, item->Id))) continue;
                    if (cached >= QCOM_NPU_MODEL_BUFFERS && other != s) continue;
                    if (!victim || item->LastUse < victim->LastUse) { owner = other; victim = item; }
                }
            }
            if (victim) {
                status = NpuUnmapAllocation(owner, victim, NULL);
                if (!NT_SUCCESS(status)) return status;
                continue;
            }
            if (compacted || (n->Running && !n->Preparing)) return STATUS_INSUFFICIENT_RESOURCES;
            for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
                NPU_CLIENT *other = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
                if (!other->Attached || (other == n->Running && !n->Preparing)) continue;
                for (ULONG j = 0; j < NPU_ALLOCATIONS; j++) {
                    NPU_ALLOCATION *item = &other->Allocations[j];
                    if (item->DspMapped && !item->Pinned) {
                        status = NpuUnmapAllocation(other, item, NULL);
                        if (!NT_SUCCESS(status)) return status;
                    }
                }
            }
            compacted = TRUE; goto restart;
        }
    }
    return STATUS_SUCCESS;
}
NTSTATUS NpuMapShared(NPU_CLIENT *s, const QCOM_NPU_SHARED_MAPPING *q, WDFREQUEST request) {
    NPU *n = s->Device;
    if (q->Abi != 1 || !q->Id || q->Map > 1 || q->Reserved > 1) return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1 || !n->PagedMemory) return STATUS_INVALID_DEVICE_STATE;
    if (n->Pending || n->Poisoned || NpuGpuBusy(s)) return STATUS_DEVICE_BUSY;
    NPU_ALLOCATION *a = NpuAllocation(s, q->Id);
    if (!a || !a->Paged) return STATUS_INVALID_PARAMETER;
    if (q->Map) {
        if (q->Reserved) a->Pinned = TRUE;
        ULONG id = q->Id; return NpuPrepareBuffers(s, &id, 1);
    }
    return NpuUnmapAllocation(s, a, request);
}
static BOOLEAN MemoryBusy(NPU_CLIENT *s, BOOLEAN fileWrite) {
    NPU *n = s->Device;
    if (n->Pending || n->Poisoned) return TRUE;
    // The reverse listener only waits for host service requests. Every other
    // outstanding RPC may access tensor memory and must fence before host I/O.
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++)
        if (s->Calls[i].Pending && !(s->Calls[i].Handle == 3 &&
            (s->Calls[i].Scalar == 0x04020200 || s->Calls[i].Scalar == 0x05010200)) &&
            !(fileWrite && ((s->Calls[i].Handle == 8 && s->Calls[i].Scalar == 0x02020200) ||
                           (s->Calls[i].Handle == 0 && s->Calls[i].Scalar == 0x20200)))) return TRUE;
    return FALSE;
}
NTSTATUS NpuWriteMemory(NPU_CLIENT *s, const QCOM_NPU_MEMORY_WRITE *q, ULONG input) {
    ULONG base = FIELD_OFFSET(QCOM_NPU_MEMORY_WRITE, Data);
    if (input < base || q->Abi != QCOM_NPU_ABI || !q->Id || q->Bytes != input - base || q->Bytes > 0x10000)
        return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1) return STATUS_ACCESS_DENIED;
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
        NPU_ALLOCATION *a = &s->Allocations[i];
        if (!a->Buffer || a->Detached || a->Id != q->Id || (a->Flags != 2 && a->Flags != 3 && a->Flags != QCOM_NPU_SHARED)) continue;
        // During remote open, the loader waits for apps_std.fopen_fd to finish
        // filling its new file map before that map can be consumed on DSP.
        if (MemoryBusy(s, a->Flags == 2)) return STATUS_DEVICE_BUSY;
        if (q->Offset > a->Bytes || q->Bytes > a->Bytes - q->Offset) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(a->Buffer + q->Offset, (PUCHAR)q + base, q->Bytes);
        KeMemoryBarrier(); return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}
NTSTATUS NpuReadMemory(NPU_CLIENT *s, const QCOM_NPU_MEMORY_READ *q, PUCHAR output, ULONG capacity) {
    if (q->Abi != QCOM_NPU_ABI || !q->Id || !q->Bytes || q->Bytes > 0x10000 || capacity != q->Bytes)
        return STATUS_INVALID_PARAMETER;
    if (!s->Attached || s->Pd != 1) return STATUS_ACCESS_DENIED;
    if (MemoryBusy(s, FALSE)) return STATUS_DEVICE_BUSY;
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
        NPU_ALLOCATION *a = &s->Allocations[i];
        if (!a->Buffer || a->Detached || a->Id != q->Id || (a->Flags != 2 && a->Flags != 3 && a->Flags != QCOM_NPU_SHARED)) continue;
        if (q->Offset > a->Bytes || q->Bytes > a->Bytes - q->Offset) return STATUS_INVALID_PARAMETER;
        KeMemoryBarrier(); RtlCopyMemory(output, a->Buffer + q->Offset, q->Bytes);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_PARAMETER;
}
