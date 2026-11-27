#include "npu.h"
#include "../platform/lock.h"
#include "memory-route.h"

// SM8750 CDSP compute context bank 1, from the platform's FastRPC device tree.
static const ULONG Streams[] = { 0x800019c1u, 0x80000c21u, 0x80400c01u };
static BOOLEAN Overlap(ULONG a, ULONG b) {
    return (a & b & 0x80000000u) && !((a ^ b) & ~((a | b) >> 16) & 0x7fffu);
}
NTSTATUS NpuMappingAcquire(NPU *n) {
    if (n->MappingReady) return STATUS_SUCCESS;
    PLATFORM_LOCK_GUARD guard = { 0 };
    NTSTATUS status = PlatformLockAcquire(&guard, TRUE);
    if (!NT_SUCCESS(status)) return status;
    ULONG count = NpuRead(n->Smmu, 0x20) & 255u, ufs = MAXULONG;
    status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!count) goto done;
    for (ULONG i = 0; i < 3; i++) n->Slot[i] = MAXULONG;
    for (ULONG i = 0; i < count; i++) {
        ULONG smr = NpuRead(n->Smmu, 0x800 + i * 4);
        if (smr == MAXULONG) goto done;
        if (!(smr & 0x80000000u)) continue;
        if (Overlap(smr, 0x80000060u)) {
            ULONG s2cr = NpuRead(n->Smmu, 0xc00 + i * 4);
            if (ufs != MAXULONG && ufs != s2cr) goto done;
            ufs = s2cr;
        }
        for (ULONG j = 0; j < 3; j++) {
            if (!Overlap(smr, Streams[j])) continue;
            if (smr != Streams[j] || n->Slot[j] != MAXULONG) goto done;
            n->Slot[j] = i;
        }
    }
    if (ufs == MAXULONG || ((ufs >> 16) & 3u)) goto done;
    if (n->SmmuBytes >= 0x100000) {
        for (ULONG j = 0; j < 3; j++) if (n->Slot[j] != MAXULONG) goto done;
        status = NpuIommuAcquire(n);
        if (!NT_SUCCESS(status)) goto done;
        status = STATUS_DEVICE_CONFIGURATION_ERROR;
        ufs = n->IommuBank;
    }
    n->ActiveS2cr = ufs;
    // Validate all existing mappings before allocating any new stream slots.
    for (ULONG j = 0; j < 3; j++) {
        if (n->Slot[j] != MAXULONG && NpuRead(n->Smmu, 0xc00 + n->Slot[j] * 4) != ufs) goto done;
    }
    for (ULONG j = 0; j < 3; j++) {
        if (n->Slot[j] != MAXULONG) continue;
        for (ULONG i = min(count, 128u); i-- > 0;) {
            ULONG smr = NpuRead(n->Smmu, 0x800 + i * 4);
            if (smr & 0x80000000u) continue;
            n->Slot[j] = i;
            n->SavedSmr[j] = smr;
            n->SavedS2cr[j] = NpuRead(n->Smmu, 0xc00 + i * 4);
            if (n->SavedS2cr[j] == MAXULONG) goto undo;
            n->OwnSlot[j] = TRUE;
            NpuWrite(n->Smmu, 0xc00 + i * 4, ufs);
            KeMemoryBarrier();
            NpuWrite(n->Smmu, 0x800 + i * 4, Streams[j]);
            if (NpuRead(n->Smmu, 0x800 + i * 4) != Streams[j] || NpuRead(n->Smmu, 0xc00 + i * 4) != ufs) goto undo;
            break;
        }
        if (n->Slot[j] == MAXULONG) { status = STATUS_INSUFFICIENT_RESOURCES; goto undo; }
    }
    n->MappingReady = TRUE;
    NpuMemoryInitialize(n);
    status = STATUS_SUCCESS;
    goto done;
undo:
    for (ULONG j = 3; j-- > 0;) {
        if (!n->OwnSlot[j]) continue;
        ULONG i = n->Slot[j];
        NpuRouteInvalidate(n->Smmu, i, Streams[j], n->ActiveS2cr, n->SavedSmr[j]);
        n->OwnSlot[j] = FALSE;
    }
done:
    if (!NT_SUCCESS(status) && n->PageTables && !n->MappingReady) NpuIommuRelease(n);
    PlatformLockRelease(&guard);
    return status;
}
VOID NpuMappingRelease(NPU *n) {
    if (!n->Smmu || !n->MappingReady || NpuBusy(n)) return;
    PLATFORM_LOCK_GUARD guard = { 0 };
    if (!NT_SUCCESS(PlatformLockAcquire(&guard, TRUE))) return;
    for (ULONG j = 3; j-- > 0;) {
        if (!n->OwnSlot[j]) continue;
        ULONG i = n->Slot[j];
        NpuRouteInvalidate(n->Smmu, i, Streams[j], n->ActiveS2cr, n->SavedSmr[j]);
        n->OwnSlot[j] = FALSE;
    }
    n->MappingReady = FALSE;
    NpuIommuRelease(n);
    NpuMemoryInitialize(n);
    PlatformLockRelease(&guard);
}
