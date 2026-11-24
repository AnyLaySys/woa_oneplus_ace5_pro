#include "fingerprint.h"
#include "../syna-touch/drv/hummer_rpmh.c"

NTSTATUS JfpPowerOn(JFP_DEVICE *device) {
    static const CHAR *const names[] = {"L14B_E0", "L14B_E1", "L14B", "ldob14"};
    if (device->Powered) return STATUS_SUCCESS;
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
            status = HrVote(&device->Power, names[i], 3008, address);
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
