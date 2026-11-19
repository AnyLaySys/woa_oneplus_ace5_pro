#pragma once
#include "uapi.h"
#define GPU_CONTEXT_PREAMBLE_MAX_WORDS 11u
#define GPU_PAGETABLE_PREAMBLE_MAX_WORDS 35u
static inline GPU_U32 GpuContextPacket(GPU_U32 operation,GPU_U32 words) {
    GPU_U32 a=operation,b=words;a^=a>>4;a^=a>>2;a^=a>>1;b^=b>>8;b^=b>>4;b^=b>>2;b^=b>>1;
    return 0x70000000u|(operation<<16)|words|((~a&1)<<23)|((~b&1)<<15);
}
/* Linux a6xx_set_pagetable's A7xx/A8xx BR/BV synchronization reset. The VM
   does not change here; no SMMU table update or undocumented event is needed.
   WAIT_TIMESTAMP executes on the GPU and never blocks the submitting CPU. */
static inline unsigned GpuBuildContextPreamble(GPU_U32 *out,unsigned capacity,GPU_U64 fenceAddress,GPU_U32 previous) {
    unsigned required=previous?GPU_CONTEXT_PREAMBLE_MAX_WORDS:6u,at=0;
    if(!out || capacity<required || (previous && (!fenceAddress || (fenceAddress&3))))return 0;
    if(previous) {
        out[at++]=GpuContextPacket(0x14,4);out[at++]=0;
        out[at++]=(GPU_U32)fenceAddress;out[at++]=(GPU_U32)(fenceAddress>>32);out[at++]=previous;
    }
    out[at++]=GpuContextPacket(0x17,1);out[at++]=0x80000003u; /* SYNC_THREADS | BOTH */
    out[at++]=GpuContextPacket(0x1f,1);out[at++]=0xf; /* all four context sync bits */
    out[at++]=GpuContextPacket(0x17,1);out[at++]=0x80000003u;
    return at;
}
static inline GPU_U32 GpuContextRegisterPacket(GPU_U32 reg) {
    GPU_U32 parity=reg;parity^=parity>>16;parity^=parity>>8;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
    return 0x40000001u|(reg<<8)|((~parity&1)<<27);
}
static inline unsigned GpuBuildPageTablePreamble(GPU_U32 *out,unsigned capacity,
    GPU_U64 fenceAddress,GPU_U32 previous,GPU_U64 root) {
    unsigned required=previous?GPU_PAGETABLE_PREAMBLE_MAX_WORDS:GPU_PAGETABLE_PREAMBLE_MAX_WORDS-5;
    if(!out || capacity<required || !root || (root&4095) || (root>>44))return 0;
    unsigned at=GpuBuildContextPreamble(out,capacity,fenceAddress,previous);if(!at)return 0;
    out[at++]=GpuContextPacket(0x46,1);out[at++]=0x26; /* LRZ cache flush */
    out[at++]=GpuContextPacket(0x17,1);out[at++]=0x80000001u;
    out[at++]=GpuContextRegisterPacket(0x449);out[at++]=1;
    out[at++]=GpuContextRegisterPacket(0x5e8);out[at++]=1;
    out[at++]=GpuContextPacket(0x53,4);out[at++]=(GPU_U32)root;out[at++]=(GPU_U32)(root>>32);out[at++]=0;out[at++]=0;
    out[at++]=GpuContextPacket(0x17,1);out[at++]=0x80000001u;
    out[at++]=GpuContextPacket(0x46,1);out[at++]=0x33; /* A7xx/A8xx UCHE invalidate */
    out[at++]=GpuContextPacket(0x3c,6);out[at++]=3;out[at++]=0x19f;out[at++]=0;out[at++]=1;out[at++]=1;out[at++]=0;
    return at;
}
