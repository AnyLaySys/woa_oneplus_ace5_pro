#include "fingerprint.h"

VOID JfpScmCall(ULONGLONG *registers);

static NTSTATUS RegisterIo(JFP_DEVICE *device, ULONG offset, ULONG *value, BOOLEAN write) {
    if (!device || !value || offset >= 0x4000 || (offset & 3) || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    device->Info.RegisterAddress = device->Info.SpiAddress + offset;
    ULONGLONG registers[8] = {write ? 0xc2000502u : 0xc2000501u, write ? 2u : 1u,
        device->Info.SpiAddress + offset, write ? *value : 0u};
    JfpScmCall(registers);
    RtlCopyMemory(device->Info.ScmReturn, registers, sizeof(device->Info.ScmReturn));
    device->Info.RegisterStatus = (LONG)registers[0];
    if (registers[0]) {
        device->Info.Flags &= ~JFP_FLAG_REGISTER_IO;
        switch ((LONG)registers[0]) {
        case -2:
        case -3: return STATUS_INVALID_PARAMETER;
        case -4: return STATUS_NOT_SUPPORTED;
        case -5: return STATUS_INSUFFICIENT_RESOURCES;
        case -12: return STATUS_DEVICE_BUSY;
        default: return STATUS_IO_DEVICE_ERROR;
        }
    }
    device->Info.Flags |= JFP_FLAG_REGISTER_IO;
    if (!write) *value = (ULONG)registers[1];
    return STATUS_SUCCESS;
}

ULONG JfpRegisterRead(JFP_DEVICE *device, ULONG offset) {
    ULONG value = MAXULONG;
    NTSTATUS status = RegisterIo(device, offset, &value, FALSE);
    if (!NT_SUCCESS(status)) device->Info.Status = status;
    return value;
}

VOID JfpRegisterWrite(JFP_DEVICE *device, ULONG offset, ULONG value) {
    NTSTATUS status = RegisterIo(device, offset, &value, TRUE);
    if (!NT_SUCCESS(status)) {
        device->Info.Status = status;
        device->Faulted = TRUE;
    }
}
