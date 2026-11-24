#include "fingerprint.h"

static ULONG Read(JFP_DEVICE *device, ULONG offset) {
    return JfpRegisterRead(device, offset);
}

static VOID Write(JFP_DEVICE *device, ULONG offset, ULONG value) {
    JfpRegisterWrite(device, offset, value);
}

static VOID Delay(ULONG milliseconds) {
    LARGE_INTEGER delay;
    delay.QuadPart = -(LONGLONG)milliseconds * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

static NTSTATUS Abort(JFP_DEVICE *device) {
    for (ULONG command = 4; command >= 2; command >>= 1) {
        Write(device, 0x604, command);
        for (ULONG i = 0; i < 100; i++) {
            if (!(Read(device, 0x40) & 1u)) {
                Write(device, 0x618, MAXULONG);
                return STATUS_SUCCESS;
            }
            Delay(1);
        }
    }
    device->Faulted = TRUE;
    return STATUS_IO_TIMEOUT;
}

NTSTATUS JfpTransfer(JFP_DEVICE *device, const UCHAR *tx, UCHAR *rx, ULONG size) {
    if (!tx || !rx || !size || size > 4 || !device->RegistersOwned || device->Faulted)
        return STATUS_INVALID_PARAMETER;
    if (Read(device, 0x40) & 1u) return STATUS_DEVICE_BUSY;
    ULONG depth = (Read(device, 0xe24) >> 16) & 255u;
    if (!depth || (Read(device, 0x800) & 0x0fffffffu)) return STATUS_DEVICE_DATA_ERROR;
    if (Read(device, 0x804) & 0x01ffffffu) return STATUS_DEVICE_DATA_ERROR;
    ULONG word = 0;
    for (ULONG i = 0; i < size; i++) word |= (ULONG)tx[i] << (i * 8);
    RtlZeroMemory(rx, size);
    RtlZeroMemory(device->Info.LastTx, sizeof(device->Info.LastTx));
    RtlZeroMemory(device->Info.LastRx, sizeof(device->Info.LastRx));
    RtlCopyMemory(device->Info.LastTx, tx, size);
    Write(device, 0x618, MAXULONG);
    Write(device, 0x26c, size);
    Write(device, 0x270, size);
    Write(device, 0x600, 7u << 27);
    Write(device, 0x700, word);
    if (device->Faulted) return device->Info.Status;
    ULONG received = 0;
    NTSTATUS status = STATUS_IO_TIMEOUT;
    ULONGLONG deadline = KeQueryInterruptTime() + 1000000ull;
    while (KeQueryInterruptTime() < deadline) {
        ULONG irq = Read(device, 0x610);
        if (irq == MAXULONG || (irq & 0x00007e0eu)) {
            status = STATUS_IO_DEVICE_ERROR;
            break;
        }
        ULONG fifo = Read(device, 0x804);
        ULONG count = fifo & 0x01ffffffu;
        if (count > depth) { status = STATUS_DEVICE_DATA_ERROR; break; }
        while (count--) {
            ULONG value = Read(device, 0x780);
            ULONG remaining = min(4u, size - received);
            for (ULONG i = 0; i < remaining; i++) rx[received++] = (UCHAR)(value >> (i * 8));
            if (received == size && count) { status = STATUS_DEVICE_DATA_ERROR; break; }
        }
        if (status == STATUS_DEVICE_DATA_ERROR) break;
        if (irq & 1u) {
            status = received == size && !(Read(device, 0x40) & 1u)
                ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
            break;
        }
        KeStallExecutionProcessor(10);
    }
    device->Info.IrqStatus = Read(device, 0x610);
    device->Info.TransferCount++;
    RtlCopyMemory(device->Info.LastRx, rx, size);
    if (!NT_SUCCESS(status)) {
        NTSTATUS aborted = Abort(device);
        return NT_SUCCESS(aborted) ? status : aborted;
    }
    Write(device, 0x618, MAXULONG);
    return status;
}

NTSTATUS JfpReadChipId(JFP_DEVICE *device) {
    static const UCHAR sequence[][4] = {{0x08, 0, 0, 0}, {0x13, 0xfd, 0, 0},
                                       {0x13, 0x20, 0, 0}, {0x13, 0x6f, 0x10, 0}};
    UCHAR rx[4];
    for (ULONG i = 0; i < RTL_NUMBER_OF(sequence); i++) {
        NTSTATUS status = JfpTransfer(device, sequence[i], rx, 4);
        if (!NT_SUCCESS(status)) return status;
        if (i == 0 || i == 2) Delay(6);
    }
    UCHAR query[4] = {0x15, 2, 0, 0};
    NTSTATUS status = JfpTransfer(device, query, rx, 4);
    if (!NT_SUCCESS(status)) return status;
    ULONG chip = rx[3];
    query[1] = 3;
    status = JfpTransfer(device, query, rx, 4);
    if (!NT_SUCCESS(status)) return status;
    device->Info.ChipId = chip | ((ULONG)rx[3] << 8);
    return device->Info.ChipId == 0x5446 ? STATUS_SUCCESS : STATUS_DEVICE_PROTOCOL_ERROR;
}
