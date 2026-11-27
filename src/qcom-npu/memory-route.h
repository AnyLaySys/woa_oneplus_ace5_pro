#pragma once
static BOOLEAN NpuRouteInvalidate(PUCHAR smmu, ULONG slot, ULONG stream, ULONG activeS2cr, ULONG savedSmr) {
    if (NpuRead(smmu, 0x800 + slot * 4) != stream || NpuRead(smmu, 0xc00 + slot * 4) != activeS2cr)
        return FALSE;
    // The saved SMR was invalid. Once cleared, the S2CR cannot receive this
    // stream. Firmware's original S2CR can contain a protected VMID (observed
    // 0x0300006e); writing it back from Windows raises a fatal bus exception.
    // Leave the inactive S2CR in its permitted nonsecure configuration.
    NpuWrite(smmu, 0x800 + slot * 4, savedSmr);
    KeMemoryBarrier();
    return TRUE;
}
