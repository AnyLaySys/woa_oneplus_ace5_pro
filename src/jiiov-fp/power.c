#include "fingerprint.h"
#include "../syna-touch/drv/hummer_rpmh.c"

static NTSTATUS Commands(JFP_DEVICE *device, ULONG *values, BOOLEAN write) {
    HRPMH *power = &device->Power;
    if (!power->Initialized || power->Faulted) return STATUS_INVALID_DEVICE_STATE;
    const HR_LAYOUT *layout = power->Major >= 3 ? &HrV3 : &HrV2;
    PUCHAR rsc = device->Rsc;
    ULONG address = device->Info.PowerAddress;
    ULONG irq = HrRd(rsc, HR_TCS_OFF), slot = MAXULONG;
    ULONG commands = (HrRd(rsc, 0xc) >> 27) & 31;
    if (irq == MAXULONG || commands < 3) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (ULONG i = 0; i < HR_ACTIVE_N; i++) {
        ULONG base = HR_TCS_OFF + i * HR_TCS_STRIDE;
        ULONG enabled = HrRd(rsc, base + layout->Enable);
        if (enabled == MAXULONG) return STATUS_IO_DEVICE_ERROR;
        for (ULONG j = 0; j < commands; j++) {
            if ((enabled & (1u << j)) &&
                ((HrRd(rsc, base + layout->Addr + j * layout->CmdStride) ^ address) & ~15u) == 0)
                return STATUS_DEVICE_BUSY;
        }
        ULONG state = HrRd(rsc, base + layout->Status);
        if (slot == MAXULONG && state != MAXULONG && (state & 1u) &&
            !HrRd(rsc, base + layout->Control) && !enabled &&
            !HrRd(rsc, base + layout->Wait) && !(HrRd(rsc, HR_TCS_OFF + 4) & (1u << i))) slot = i;
    }
    if (slot == MAXULONG) return STATUS_DEVICE_BUSY;
    ULONG base = HR_TCS_OFF + slot * HR_TCS_STRIDE;
    ULONG bit = 1u << slot;
    NTSTATUS status = HrSync(power, HR_TCS_OFF, irq & ~bit);
    BOOLEAN triggered = FALSE, done = FALSE;
    static const ULONG offsets[] = {0, 8, 4};
    for (ULONG i = 0; NT_SUCCESS(status) && i < 3; i++) {
        ULONG offset = base + i * layout->CmdStride;
        status = HrSync(power, offset + layout->MsgId, 0x108u | (write ? 0x10000u : 0));
        if (NT_SUCCESS(status)) status = HrSync(power, offset + layout->Addr, address + offsets[i]);
        if (NT_SUCCESS(status)) status = HrSync(power, offset + layout->Data, write ? values[i] : 0);
    }
    if (NT_SUCCESS(status)) status = HrSync(power, base + layout->Wait, 7);
    if (NT_SUCCESS(status)) status = HrSync(power, base + layout->Enable, 7);
    if (NT_SUCCESS(status)) status = HrSync(power, base + layout->Control, 0x10000);
    for (ULONG i = 0; NT_SUCCESS(status) && i < 3; i++) {
        ULONG observed = HrRd(rsc, base + layout->CmdStatus + i * layout->CmdStride);
        if (observed == MAXULONG || (observed & 0x10100u)) status = STATUS_IO_DEVICE_ERROR;
    }
    if (NT_SUCCESS(status)) {
        triggered = TRUE;
        HrWr(rsc, base + layout->Control, 0x1010000);
        ULONGLONG deadline = KeQueryInterruptTime() + 1000000;
        do {
            done = TRUE;
            for (ULONG i = 0; i < 3; i++)
                if ((HrRd(rsc, base + layout->CmdStatus + i * layout->CmdStride) & 0x10100u) != 0x10100u)
                    done = FALSE;
            ULONG state = HrRd(rsc, base + layout->Status);
            if (done && state && state != MAXULONG) break;
            done = FALSE;
            HrWaitUs(100);
        } while (KeQueryInterruptTime() < deadline);
        status = done ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
        if (done && !write)
            for (ULONG i = 0; i < 3; i++) values[i] = HrRd(rsc, base + layout->CmdStatus + 4 + i * layout->CmdStride);
    }
    if (!triggered || done) {
        NTSTATUS cleanup = HrSync(power, base + layout->Control, 0x10000);
        if (NT_SUCCESS(cleanup)) cleanup = HrSync(power, base + layout->Control, 0);
        if (NT_SUCCESS(cleanup)) cleanup = HrSync(power, base + layout->Enable, 0);
        if (NT_SUCCESS(cleanup)) cleanup = HrSync(power, base + layout->Wait, 0);
        HrWr(rsc, HR_TCS_OFF + 8, bit);
        ULONG tries;
        for (tries = 0; tries < 100; tries++) {
            ULONG pending = HrRd(rsc, HR_TCS_OFF + 4);
            if (pending != MAXULONG && !(pending & bit)) break;
            HrWaitUs(100);
        }
        if (tries == 100) cleanup = STATUS_IO_TIMEOUT;
        if (NT_SUCCESS(cleanup)) cleanup = HrSync(power, HR_TCS_OFF, irq);
        if (!NT_SUCCESS(cleanup)) { power->Faulted = TRUE; status = cleanup; }
    } else power->Faulted = TRUE;
    return status;
}

NTSTATUS JfpPowerOn(JFP_DEVICE *device) {
    static const CHAR *const names[] = {"L14B_E0", "L14B_E1", "L14B", "ldob14"};
    if (device->Powered) return STATUS_SUCCESS;
    if (device->PowerOwned || device->Power.Faulted) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = HrpmhInit(&device->Power, device->Rsc, device->Db);
    if (!NT_SUCCESS(status)) return status;
    PLATFORM_LOCK_GUARD guard = {0};
    status = PlatformLockAcquire(&guard, FALSE);
    if (!NT_SUCCESS(status)) return status;
    __try {
        status = STATUS_NOT_FOUND;
        for (ULONG i = 0; i < RTL_NUMBER_OF(names); i++) {
            ULONG address = 0;
            status = HrFind(&device->Power, names[i], &address);
            if (status == STATUS_NOT_FOUND) continue;
            if (!NT_SUCCESS(status)) break;
            if ((address & 15u) || address < 0x40000u || address >= 0x50000u) {
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                break;
            }
            device->Info.PowerAddress = address;
            status = Commands(device, device->SavedPower, FALSE);
            if (!NT_SUCCESS(status)) break;
            ULONG values[] = {max(3008u, device->SavedPower[0]), 7, 1};
            device->PowerOwned = TRUE;
            status = Commands(device, values, TRUE);
            if (NT_SUCCESS(status)) {
                device->Powered = TRUE;
                device->Info.Flags |= JFP_FLAG_POWER;
            }
            break;
        }
    } __finally {
        NTSTATUS released = PlatformLockRelease(&guard);
        if (NT_SUCCESS(status) && !NT_SUCCESS(released)) status = released;
    }
    return status;
}

NTSTATUS JfpPowerOff(JFP_DEVICE *device) {
    if (!device->PowerOwned) return STATUS_SUCCESS;
    PLATFORM_LOCK_GUARD guard = {0};
    NTSTATUS status = PlatformLockAcquire(&guard, FALSE);
    if (!NT_SUCCESS(status)) return status;
    status = Commands(device, device->SavedPower, TRUE);
    NTSTATUS released = PlatformLockRelease(&guard);
    if (NT_SUCCESS(status)) {
        device->PowerOwned = FALSE;
        device->Powered = FALSE;
        device->Info.Flags &= ~JFP_FLAG_POWER;
    }
    return NT_SUCCESS(status) ? released : status;
}
