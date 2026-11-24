#include "fingerprint.h"
#include <stdio.h>

static struct {
    ULONGLONG Time;
    ULONG Writes;
    ULONG Exchanges;
    ULONG AbortWrites;
    ULONG Mode;
    ULONG Depth;
    ULONG Chip;
    ULONG Word;
    ULONG Remaining;
    BOOLEAN Busy;
    BOOLEAN Stuck;
    UCHAR Tx[8][4];
} Model;

static JFP_DEVICE Device(void) {
    JFP_DEVICE device = {0};
    RtlZeroMemory(&Model, sizeof(Model));
    Model.Depth = 16;
    Model.Chip = 0x5446;
    device.RegistersOwned = TRUE;
    return device;
}

ULONG JfpRegisterRead(JFP_DEVICE *device, ULONG offset) {
    (void)device;
    Model.Time += 1000;
    switch (offset) {
    case 0x40: return Model.Busy;
    case 0xe24: return Model.Depth << 16;
    case 0x800: return 0;
    case 0x804: return Model.Remaining;
    case 0x610:
        if (!Model.Busy) return 1;
        if (Model.Mode == 1) return 0;
        if (Model.Mode == 3) return 2;
        return 1;
    case 0x780:
        assert(Model.Remaining);
        Model.Remaining--;
        Model.Busy = FALSE;
        return Model.Word;
    default: assert(0); return 0;
    }
}

VOID JfpRegisterWrite(JFP_DEVICE *device, ULONG offset, ULONG value) {
    Model.Writes++;
    if (offset == 0x600) {
        assert(value == (7u << 27));
        Model.Busy = TRUE;
    } else if (offset == 0x700) {
        ULONG n = Model.Exchanges++;
        assert(n < RTL_NUMBER_OF(Model.Tx));
        for (ULONG i = 0; i < 4; i++) Model.Tx[n][i] = (UCHAR)(value >> (8 * i));
        Model.Word = (Model.Tx[n][0] == 0x15
            ? (Model.Tx[n][1] == 2 ? Model.Chip & 255u : Model.Chip >> 8) : 0u) << 24;
        Model.Remaining = Model.Mode == 1 || Model.Mode == 2 || Model.Mode == 3 ? 0 : 1;
        if (Model.Mode == 4) Model.Remaining = Model.Depth + 1;
        if (Model.Mode == 5) {
            device->Faulted = TRUE;
            device->Info.Status = STATUS_ACCESS_DENIED;
        }
    } else if (offset == 0x604) {
        assert(value == 4 || value == 2);
        Model.AbortWrites++;
        if (!Model.Stuck) { Model.Busy = FALSE; Model.Remaining = 0; }
    } else {
        assert(offset == 0x618 || offset == 0x26c || offset == 0x270);
    }
}

ULONGLONG KeQueryInterruptTime(void) { return Model.Time; }
VOID KeStallExecutionProcessor(ULONG microseconds) { Model.Time += (ULONGLONG)microseconds * 10; }
NTSTATUS KeDelayExecutionThread(int mode, BOOLEAN alertable, LARGE_INTEGER *delay) {
    assert(mode == KernelMode && !alertable && delay->QuadPart < 0);
    Model.Time += (ULONGLONG)-delay->QuadPart;
    return STATUS_SUCCESS;
}

int main(void) {
    static const UCHAR expected[][4] = {{8, 0, 0, 0}, {0x13, 0xfd, 0, 0},
        {0x13, 0x20, 0, 0}, {0x13, 0x6f, 0x10, 0}, {0x15, 2, 0, 0}, {0x15, 3, 0, 0}};
    JFP_DEVICE device = Device();
    assert(JfpReadChipId(&device) == STATUS_SUCCESS);
    assert(device.Info.ChipId == 0x5446 && Model.Exchanges == 6 && device.Info.TransferCount == 6);
    assert(!memcmp(expected, Model.Tx, sizeof(expected)));
    assert(!Model.AbortWrites && !device.Faulted);
    device = Device(); Model.Chip = 0x1234;
    assert(JfpReadChipId(&device) == STATUS_DEVICE_PROTOCOL_ERROR);
    assert(device.Info.ChipId == 0x1234);
    UCHAR rx[8] = {0}, tx[8] = {0x15, 2};
    device = Device();
    assert(JfpTransfer(&device, NULL, rx, 4) == STATUS_INVALID_PARAMETER);
    assert(JfpTransfer(&device, tx, NULL, 4) == STATUS_INVALID_PARAMETER);
    assert(JfpTransfer(&device, tx, rx, 0) == STATUS_INVALID_PARAMETER);
    assert(JfpTransfer(&device, tx, rx, 5) == STATUS_INVALID_PARAMETER);
    assert(!Model.Writes);
    device = Device(); Model.Busy = TRUE;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_DEVICE_BUSY && !Model.Writes);
    device = Device(); Model.Depth = 0;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_DEVICE_DATA_ERROR && !Model.Writes);
    device = Device(); Model.Mode = 1;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_IO_TIMEOUT);
    assert(Model.AbortWrites == 1 && !device.Faulted);
    device = Device(); Model.Mode = 1; Model.Stuck = TRUE;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_IO_TIMEOUT);
    assert(Model.AbortWrites == 2 && device.Faulted);
    device = Device(); Model.Mode = 2;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_DEVICE_DATA_ERROR);
    assert(Model.AbortWrites == 1);
    device = Device(); Model.Mode = 3;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_IO_DEVICE_ERROR);
    device = Device(); Model.Mode = 4;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_DEVICE_DATA_ERROR);
    device = Device(); Model.Mode = 5;
    assert(JfpTransfer(&device, tx, rx, 4) == STATUS_ACCESS_DENIED && device.Faulted);
    puts("JV0301 protocol, identity, FIFO, timeout and register-access checks passed");
    return 0;
}
