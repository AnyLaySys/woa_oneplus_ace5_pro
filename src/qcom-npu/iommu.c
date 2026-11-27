#include "npu.h"

#define IOVA_START 0x10000000ULL
#define IOVA_END 0x80000000ULL
#define LEAF_COUNT ((IOVA_END - IOVA_START) >> 21)
#define TABLE_BYTES ((5 + LEAF_COUNT) * PAGE_SIZE)

static NTSTATUS Invalidate(NPU *n) {
    ULONG context = 0x80000 + n->IommuBank * PAGE_SIZE;
    KeMemoryBarrier();
    NpuWrite(n->Smmu, context + 0x610, 0);
    NpuWrite(n->Smmu, context + 0x7f0, 0);
    for (ULONG tick = 0; tick < 10000; tick++) {
        if (!(NpuRead(n->Smmu, context + 0x7f4) & 1)) return STATUS_SUCCESS;
        KeStallExecutionProcessor(1);
    }
    n->Poisoned = TRUE;
    return STATUS_IO_TIMEOUT;
}
static PULONGLONG Leaf(NPU *n, ULONGLONG iova) {
    return n->PageTables + (5 + ((iova - IOVA_START) >> 21)) * 512 + ((iova >> 12) & 511);
}
NTSTATUS NpuIommuAcquire(NPU *n) {
    if (n->PagedMemory) return STATUS_SUCCESS;
    if (n->SmmuBytes < 0x100000 || (NpuRead(n->Smmu, 0x24) & 0x80000000u) ||
        !(NpuRead(n->Smmu, 0x28) & 0x1000)) return STATUS_NOT_SUPPORTED;
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (!ranges) return STATUS_INSUFFICIENT_RESOURCES;
    BOOLEAN conflict = FALSE;
    for (ULONG i = 0; ranges[i].NumberOfBytes.QuadPart; i++) {
        ULONGLONG base = ranges[i].BaseAddress.QuadPart, size = ranges[i].NumberOfBytes.QuadPart;
        if (base < IOVA_END && base + size > IOVA_START) conflict = TRUE;
    }
    ExFreePool(ranges);
    if (conflict) return STATUS_DEVICE_CONFIGURATION_ERROR;
    BOOLEAN used[128] = { 0 };
    ULONG streams = min(NpuRead(n->Smmu, 0x20) & 255u, 128u);
    for (ULONG i = 0; i < streams; i++) {
        ULONG smr = NpuRead(n->Smmu, 0x800 + i * 4), control = NpuRead(n->Smmu, 0xc00 + i * 4);
        if ((smr & 0x80000000u) && !(control & 0x30000) && (control & 255) < 128) used[control & 255] = TRUE;
    }
    ULONG bank = MAXULONG, count = min(NpuRead(n->Smmu, 0x24) & 255u, 16u);
    for (ULONG i = 0; i < count; i++) {
        ULONG cbar = NpuRead(n->Smmu, 0x1000 + i * 4);
        if (!used[i] && (cbar & 0x30000) == 0x10000 && ((cbar & 255) == 0 || (cbar & 255) == 255) &&
            !(NpuRead(n->Smmu, 0x80000 + i * PAGE_SIZE) & 1) &&
            !(NpuRead(n->Smmu, 0x80058 + i * PAGE_SIZE) & 0xc00001ffu)) { bank = i; break; }
    }
    if (bank == MAXULONG) return STATUS_NOT_SUPPORTED;
    PHYSICAL_ADDRESS low = { 0 }, high, boundary = { 0 }; high.QuadPart = MAXULONG;
    n->PageTables = MmAllocateContiguousMemorySpecifyCache(TABLE_BYTES, low, high, boundary, MmNonCached);
    if (!n->PageTables) return STATUS_INSUFFICIENT_RESOURCES;
    n->PageTableAddress = MmGetPhysicalAddress(n->PageTables);
    RtlZeroMemory(n->PageTables, TABLE_BYTES);
    ULONGLONG physical = n->PageTableAddress.QuadPart;
    for (ULONG i = 0; i < 4; i++) n->PageTables[i] = (physical + (1ULL + i) * PAGE_SIZE) | 3;
    for (ULONG i = 0; i < 2048; i++) {
        ULONGLONG iova = (ULONGLONG)i << 21;
        n->PageTables[512 + i] = iova >= IOVA_START && iova < IOVA_END ?
            (physical + (5 + ((iova - IOVA_START) >> 21)) * PAGE_SIZE) | 3 : iova | 0xe41;
    }
    n->IommuBank = bank;
    ULONG context = 0x80000 + bank * PAGE_SIZE;
    const ULONG offsets[] = {0x1000 + bank * 4, 0x1800 + bank * 4, context + 4, context + 0x10, context + 0x30,
        context + 0x20, context + 0x24, context + 0x28, context + 0x2c, context + 0x34, context + 0x38, context + 0x3c};
    const ULONG values[] = {0x1f300, 1, 1, 0x38010 | ((NpuRead(n->Smmu, 0x28) >> 4) & 15), 0x803020,
        (ULONG)physical, (ULONG)(physical >> 32), 0, 0, 0, 0xff44, 0};
    for (ULONG i = 0; i < RTL_NUMBER_OF(offsets); i++) {
        NpuWrite(n->Smmu, offsets[i], values[i]);
        ULONG mask = offsets[i] == context + 0x10 ? 0x3801f : offsets[i] == context + 0x30 ? 0x00c0ff3f : MAXULONG;
        if ((NpuRead(n->Smmu, offsets[i]) ^ values[i]) & mask) {
            n->LastResult = offsets[i];
            NpuIommuRelease(n); return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    KeMemoryBarrier();
    // Poll faults; an unserviced context interrupt must not stall CDSP.
    NpuWrite(n->Smmu, context, 0x27);
    if ((NpuRead(n->Smmu, context) & 0x27) != 0x27) { NpuIommuRelease(n); return STATUS_DEVICE_CONFIGURATION_ERROR; }
    NTSTATUS status = Invalidate(n);
    if (!NT_SUCCESS(status)) return status;
    n->PagedMemory = TRUE;
    return STATUS_SUCCESS;
}
VOID NpuIommuRelease(NPU *n) {
    if (!n->PageTables || NpuBusy(n)) return;
    ULONG context = 0x80000 + n->IommuBank * PAGE_SIZE;
    NpuWrite(n->Smmu, context, 0);
    if (NpuRead(n->Smmu, context) & 1 || !NT_SUCCESS(Invalidate(n))) return;
    NpuWrite(n->Smmu, context + 0x20, 0);
    NpuWrite(n->Smmu, context + 0x24, 0);
    MmFreeContiguousMemorySpecifyCache(n->PageTables, TABLE_BYTES, MmNonCached);
    n->PageTables = NULL; n->PagedMemory = FALSE;
}
NTSTATUS NpuIommuMap(NPU *n, NPU_ALLOCATION *a) {
    if (!n->PagedMemory || !a->Paged || !a->Mdl) return STATUS_INVALID_DEVICE_STATE;
    if (a->Iova) return STATUS_SUCCESS;
    ULONGLONG next = IOVA_START;
    for (ULONG attempt = 0; attempt <= NPU_ALLOCATIONS * NPU_CLIENTS; attempt++) {
        if (a->Bytes > IOVA_END - next) return STATUS_INSUFFICIENT_RESOURCES;
        BOOLEAN collision = FALSE;
        for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients && !collision; p = p->Flink) {
            NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
            for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
                NPU_ALLOCATION *other = &s->Allocations[i];
                if (other->Iova && next < other->Iova + other->Bytes && next + a->Bytes > other->Iova) {
                    next = other->Iova + other->Bytes; collision = TRUE; break;
                }
            }
        }
        if (collision) continue;
        PPFN_NUMBER pages = MmGetMdlPfnArray(a->Mdl);
        ULONGLONG entries[128];
        for (ULONG i = 0; i < a->Bytes / PAGE_SIZE;) {
            ULONGLONG address = next + (ULONGLONG)i * PAGE_SIZE;
            ULONG count = min((ULONG)RTL_NUMBER_OF(entries), min(a->Bytes / PAGE_SIZE - i, 512u - ((ULONG)(address >> PAGE_SHIFT) & 511u)));
            for (ULONG j = 0; j < count; j++) entries[j] = ((ULONGLONG)pages[i + j] << PAGE_SHIFT) | 0xe43;
            // Generate entries in cached stack memory before writing the noncached table.
            RtlCopyMemory(Leaf(n, address), entries, count * sizeof(*entries));
            i += count;
        }
        a->Iova = next;
        return Invalidate(n);
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}
NTSTATUS NpuIommuUnmap(NPU *n, NPU_ALLOCATION *a) {
    if (!a->Iova) return STATUS_SUCCESS;
    for (ULONG i = 0; i < a->Bytes / PAGE_SIZE;) {
        ULONGLONG address = a->Iova + (ULONGLONG)i * PAGE_SIZE;
        ULONG count = min(a->Bytes / PAGE_SIZE - i, 512u - ((ULONG)(address >> PAGE_SHIFT) & 511u));
        RtlZeroMemory(Leaf(n, address), count * sizeof(ULONGLONG));
        i += count;
    }
    NTSTATUS status = Invalidate(n);
    if (NT_SUCCESS(status)) a->Iova = 0;
    return status;
}
