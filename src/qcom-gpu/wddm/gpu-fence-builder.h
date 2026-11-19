#pragma once
#include "gpu-scanout-builder.h"

static inline unsigned A830GpuFenceAddress(unsigned long long address) {
    return address && !(address&7) && address<=0x7ffffffff8ULL;
}
static inline unsigned A830BuildGpuSignal(unsigned *out,unsigned capacity,
    unsigned long long address,unsigned long long value) {
    if(!out || capacity<12 || !A830GpuFenceAddress(address) || value==~0ULL)return 0;
    out[0]=A830CopyPacket(0x17,1);out[1]=1;
    out[2]=A830CopyPacket(0x26,0);
    out[3]=A830CopyPacket(0x12,0);
    /* A7xx/A8xx CACHE_CLEAN, USER_64B, RAM destination. */
    out[4]=A830CopyPacket(0x46,5);out[5]=0x08100031u;
    out[6]=(unsigned)address;out[7]=(unsigned)(address>>32);
    out[8]=(unsigned)value;out[9]=(unsigned)(value>>32);
    out[10]=A830CopyPacket(0x17,1);out[11]=3;
    return 12;
}
