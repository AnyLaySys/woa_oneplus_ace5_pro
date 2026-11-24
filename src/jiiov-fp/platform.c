#include "fingerprint.h"
#include "../platform/lock.h"

static const ULONG JfpRegisterOffsets[] = {
    0x20, 0x24, 0x28, 0x48, 0x7c, 0x224, 0x230, 0x24c, 0x250, 0x254,
    0x258, 0x25c, 0x260, 0x264, 0x268, 0x284, 0x288, 0x614, 0xe18, 0xe1c
};

static ULONG Read(PUCHAR base, ULONG offset) {
    return READ_REGISTER_ULONG((PULONG)(base + offset));
}

static VOID Write(PUCHAR base, ULONG offset, ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(base + offset), value);
    KeMemoryBarrier();
}

static ULONG SeRead(JFP_DEVICE *device, ULONG offset) { return JfpRegisterRead(device, offset); }
static VOID SeWrite(JFP_DEVICE *device, ULONG offset, ULONG value) { JfpRegisterWrite(device, offset, value); }

static VOID Delay(ULONG milliseconds) {
    LARGE_INTEGER delay;
    delay.QuadPart = -(LONGLONG)milliseconds * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

NTSTATUS JfpPrepare(JFP_DEVICE *device) {
    static const ULONG addresses[] = {0x00a88000, 0x00100000, 0x0f100000, 0x16520000, 0x81c60000};
    static const ULONG sizes[] = {0x4000, 0x60000, 0x50000, 0x10000, 0x20000};
    PUCHAR *mapped[] = {&device->Se, &device->Gcc, &device->Pins, &device->Rsc, &device->Db};
    device->Info.Size = sizeof(device->Info);
    device->Info.Version = JFP_VERSION;
    device->Info.SpiAddress = addresses[0];
    device->Info.Status = STATUS_DEVICE_NOT_READY;
    for (ULONG i = 0; i < RTL_NUMBER_OF(mapped); i++) {
        PHYSICAL_ADDRESS address;
        address.QuadPart = addresses[i];
        *mapped[i] = MmMapIoSpaceEx(address, sizes[i], PAGE_READWRITE | PAGE_NOCACHE);
        if (!*mapped[i]) {
            JfpRelease(device);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS JfpInspect(JFP_DEVICE *device) {
    if (!device->Gcc || !device->Pins || !device->Se) return STATUS_DEVICE_NOT_READY;
    device->Info.ClockVote = Read(device->Gcc, 0x52008);
    device->Info.ClockState = Read(device->Gcc, 0x1827c);
    for (ULONG i = 0; i < 4; i++) device->Info.Pins[i] = Read(device->Pins, (40 + i) * 0x1000);
    device->Info.ResetConfig = Read(device->Pins, 78 * 0x1000);
    device->Info.ResetValue = Read(device->Pins, 78 * 0x1000 + 4);
    if (device->Info.ClockState == MAXULONG || (device->Info.ClockState & 0x80000000u)) {
        device->Info.Flags &= ~(JFP_FLAG_CLOCK | JFP_FLAG_SPI | JFP_FLAG_FIFO | JFP_FLAG_CHIP);
        device->Info.Status = STATUS_DEVICE_NOT_READY;
        return STATUS_SUCCESS;
    }
    device->Info.Flags |= JFP_FLAG_CLOCK;
    device->Info.Firmware = SeRead(device, 0x68);
    if (!(device->Info.Flags & JFP_FLAG_REGISTER_IO)) return device->Info.Status;
    device->Info.InterfaceDisable = SeRead(device, 0x64);
    device->Info.HardwareParameters = SeRead(device, 0xe24);
    device->Info.SequencerStatus = SeRead(device, 0x40);
    device->Info.IrqStatus = SeRead(device, 0x610);
    if (((device->Info.Firmware >> 8) & 255u) == 1u) device->Info.Flags |= JFP_FLAG_SPI;
    else device->Info.Flags &= ~JFP_FLAG_SPI;
    if (device->Info.InterfaceDisable != MAXULONG && !(device->Info.InterfaceDisable & 1u))
        device->Info.Flags |= JFP_FLAG_FIFO;
    else device->Info.Flags &= ~JFP_FLAG_FIFO;
    return STATUS_SUCCESS;
}

static NTSTATUS EnableClock(JFP_DEVICE *device) {
    PLATFORM_LOCK_GUARD guard = {0};
    NTSTATUS status = PlatformLockAcquire(&guard, FALSE);
    if (!NT_SUCCESS(status)) return status;
    ULONG vote = Read(device->Gcc, 0x52008);
    if (vote == MAXULONG) status = STATUS_IO_DEVICE_ERROR;
    else {
        device->SavedClockVote = vote;
        device->AddedClockVotes = 0x013c0000u & ~vote;
        device->ClockOwned = !(vote & 0x01000000u);
        Write(device->Gcc, 0x52008, vote | 0x013c0000u);
        status = STATUS_IO_TIMEOUT;
        for (ULONG i = 0; i < 100; i++) {
            ULONG halt = Read(device->Gcc, 0x1827c);
            if (halt != MAXULONG && !(halt & 0x80000000u)) {
                status = STATUS_SUCCESS;
                break;
            }
            Delay(1);
        }
    }
    NTSTATUS released = PlatformLockRelease(&guard);
    return NT_SUCCESS(status) && !NT_SUCCESS(released) ? released : status;
}

static NTSTATUS Initialize(JFP_DEVICE *device) {
    if (device->Faulted || device->Power.Faulted) return STATUS_INVALID_DEVICE_STATE;
    if (device->Info.Flags & JFP_FLAG_CHIP) return STATUS_SUCCESS;
    NTSTATUS status = JfpInspect(device);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG i = 0; i < 4; i++) {
        ULONG function = (device->Info.Pins[i] >> 2) & 15u;
        if (device->Info.Pins[i] == MAXULONG || function > 1u)
            return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    status = EnableClock(device);
    if (!NT_SUCCESS(status)) return status;
    status = JfpInspect(device);
    if (!NT_SUCCESS(status)) return status;
    if (!(device->Info.Flags & JFP_FLAG_SPI)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (!(device->Info.Flags & JFP_FLAG_FIFO)) return STATUS_NOT_SUPPORTED;
    if (device->Info.SequencerStatus & 0x1001u) return STATUS_DEVICE_BUSY;
    if (!device->RegistersOwned) {
        for (ULONG i = 0; i < RTL_NUMBER_OF(JfpRegisterOffsets); i++) {
            device->SavedRegisters[i] = SeRead(device, JfpRegisterOffsets[i]);
            if (!(device->Info.Flags & JFP_FLAG_REGISTER_IO)) return device->Info.Status;
        }
        device->RegistersOwned = TRUE;
    }
    status = JfpPowerOn(device);
    if (!NT_SUCCESS(status)) return status;
    if (!device->PinsOwned) {
        for (ULONG i = 0; i < 4; i++) device->SavedPins[i] = device->Info.Pins[i];
        device->SavedReset[0] = device->Info.ResetConfig;
        device->SavedReset[1] = device->Info.ResetValue;
        device->PinsOwned = TRUE;
    }
    for (ULONG i = 0; i < 4; i++) Write(device->Pins, (40 + i) * 0x1000, 0x84);
    Write(device->Pins, 78 * 0x1000 + 4, 0);
    Write(device->Pins, 78 * 0x1000, 0x200);
    Delay(3);
    Write(device->Pins, 78 * 0x1000 + 4, 2);
    Delay(10);
    SeWrite(device, 0xe18, 0);
    SeWrite(device, 0xe1c, 0);
    SeWrite(device, 0x614, 0);
    SeWrite(device, 0x258, 0);
    SeWrite(device, 0x28, SeRead(device, 0x28) | 0x7fu);
    SeWrite(device, 0x24, 0x7f);
    SeWrite(device, 0x20, 1);
    SeWrite(device, 0x48, 0x201);
    SeWrite(device, 0x224, 0);
    SeWrite(device, 0x230, 0);
    SeWrite(device, 0x24c, 0);
    SeWrite(device, 0x250, 0);
    SeWrite(device, 0x25c, 0);
    SeWrite(device, 0x268, 4);
    ULONG vectors[4];
    for (ULONG i = 0; i < 4; i++) vectors[i] = ((7 + i * 8) << 5) | 0x1eu | (i == 3 ? 1u : 0u);
    ULONG cfg0 = vectors[0] | (vectors[1] << 10), cfg1 = vectors[2] | (vectors[3] << 10);
    SeWrite(device, 0x260, cfg0);
    SeWrite(device, 0x264, cfg1);
    SeWrite(device, 0x284, cfg0);
    SeWrite(device, 0x288, cfg1);
    SeWrite(device, 0x254, 2);
    if (device->Faulted) return device->Info.Status;
    status = JfpReadChipId(device);
    if (NT_SUCCESS(status)) device->Info.Flags |= JFP_FLAG_CHIP;
    return status;
}

static VOID Restore(JFP_DEVICE *device) {
    if (device->RegistersOwned && device->Se && !device->Faulted) {
        if (!(SeRead(device, 0x40) & 1u)) {
            for (ULONG i = 0; i < RTL_NUMBER_OF(JfpRegisterOffsets); i++)
                SeWrite(device, JfpRegisterOffsets[i], device->SavedRegisters[i]);
        }
        device->RegistersOwned = FALSE;
    }
    if (device->PinsOwned && device->Pins) {
        for (ULONG i = 0; i < 4; i++) Write(device->Pins, (40 + i) * 0x1000, device->SavedPins[i]);
        Write(device->Pins, 78 * 0x1000 + 4, device->SavedReset[1]);
        Write(device->Pins, 78 * 0x1000, device->SavedReset[0]);
        device->PinsOwned = FALSE;
    }
    if (device->AddedClockVotes && device->Gcc) {
        PLATFORM_LOCK_GUARD guard = {0};
        if (NT_SUCCESS(PlatformLockAcquire(&guard, FALSE))) {
            ULONG vote = Read(device->Gcc, 0x52008);
            if (vote != MAXULONG) {
                ULONG remove = device->AddedClockVotes;
                ULONG remaining = vote & ~remove;
                ULONG other = Read(device->Gcc, 0x52010);
                if ((remaining & 0x1ec00000u) || other == MAXULONG || (other & 0x00010000u))
                    remove &= ~0x003c0000u;
                Write(device->Gcc, 0x52008, vote & ~remove);
                device->ClockOwned = FALSE;
                device->AddedClockVotes = 0;
            }
            PlatformLockRelease(&guard);
        }
    }
    device->Info.Flags &= ~(JFP_FLAG_CLOCK | JFP_FLAG_SPI | JFP_FLAG_FIFO | JFP_FLAG_CHIP);
}

NTSTATUS JfpSensorClockOn(JFP_DEVICE *device) {
    NTSTATUS status = JfpQteeTrace(&device->Tee, 24, NULL, STATUS_SUCCESS);
    if (!NT_SUCCESS(status)) return status;
    status = EnableClock(device);
    ULONGLONG observed[8] = {Read(device->Gcc, 0x52008), Read(device->Gcc, 0x1827c),
        device->AddedClockVotes, device->Info.PowerAddress};
    JfpQteeTrace(&device->Tee, 25, observed, status);
    return status;
}

NTSTATUS JfpSensorOn(JFP_DEVICE *device) {
    if (!device->Pins || !device->Rsc || !device->Db || device->Power.Faulted)
        return STATUS_INVALID_DEVICE_STATE;
    if (device->PinsOwned) return STATUS_INVALID_DEVICE_STATE;
    for (ULONG i = 0; i < 4; i++) {
        ULONG pin = Read(device->Pins, (40 + i) * 0x1000);
        if (pin == MAXULONG || ((pin >> 2) & 15u) > 1) return STATUS_DEVICE_CONFIGURATION_ERROR;
        device->SavedPins[i] = pin;
    }
    device->SavedReset[0] = Read(device->Pins, 78 * 0x1000);
    device->SavedReset[1] = Read(device->Pins, 78 * 0x1000 + 4);
    if (device->SavedReset[0] == MAXULONG || device->SavedReset[1] == MAXULONG)
        return STATUS_IO_DEVICE_ERROR;
    NTSTATUS status = JfpQteeTrace(&device->Tee, 20, NULL, STATUS_SUCCESS);
    if (!NT_SUCCESS(status)) return status;
    status = JfpPowerOn(device);
    JfpQteeTrace(&device->Tee, 21, NULL, status);
    if (!NT_SUCCESS(status)) return status;
    device->PinsOwned = TRUE;
    Write(device->Pins, 78 * 0x1000 + 4, 0);
    Write(device->Pins, 78 * 0x1000, 0x200);
    Delay(3);
    Write(device->Pins, 78 * 0x1000 + 4, 2);
    Delay(20);
    JfpQteeTrace(&device->Tee, 23, NULL, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

NTSTATUS JfpSensorOff(JFP_DEVICE *device) {
    if (device->Tee.Pending) return STATUS_IO_TIMEOUT;
    JfpQteeTrace(&device->Tee, 26, NULL, STATUS_SUCCESS);
    Restore(device);
    JfpQteeTrace(&device->Tee, 27, NULL, STATUS_SUCCESS);
    NTSTATUS status = JfpPowerOff(device);
    JfpQteeTrace(&device->Tee, 29, NULL, status);
    return status;
}

NTSTATUS JfpInitialize(JFP_DEVICE *device) {
    NTSTATUS status = Initialize(device);
    if (!NT_SUCCESS(status)) JfpSensorOff(device);
    return status;
}

VOID JfpRelease(JFP_DEVICE *device) {
    if (device->Tee.VerifyStarted) JfpQteeSessionEnd(device);
    else JfpQteeRelease(device);
    device->SessionOwner = NULL;
    JfpSensorOff(device);
    PUCHAR *mapped[] = {&device->Se, &device->Gcc, &device->Pins, &device->Rsc, &device->Db};
    static const ULONG sizes[] = {0x4000, 0x60000, 0x50000, 0x10000, 0x20000};
    for (ULONG i = 0; i < RTL_NUMBER_OF(mapped); i++) {
        if (*mapped[i]) MmUnmapIoSpace(*mapped[i], sizes[i]);
        *mapped[i] = NULL;
    }
    device->Info.Flags = 0;
    device->Info.Status = STATUS_DEVICE_NOT_READY;
    device->Powered = FALSE;
    device->RegistersOwned = FALSE;
    RtlZeroMemory(&device->Power, sizeof(device->Power));
}
