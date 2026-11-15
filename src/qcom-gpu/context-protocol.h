#pragma once
#include "uapi.h"
#define GPU_CONTEXT_PREAMBLE_MAX_WORDS 11u
static inline GPU_U32 GpuContextPacket(GPU_U32 operation,GPU_U32 words) {
    GPU_U32 a=operation,b=words;a^=a>>4;a^=a>>2;a^=a>>1;b^=b>>8;b^=b>>4;b^=b>>2;b^=b>>1;
    return 0x70000000u|(operation<<16)|words|((~a&1)<<23)|((~b&1)<<15);
}
/* Linux a6xx_set_pagetable's A7xx/A8xx BR/BV synchronization reset. The VM
   does not change here; no SMMU table update or undocumented event is needed.
   WAIT_TIMESTAMP executes on the GPU and never blocks the submitting CPU. */
static inline unsigned GpuBuildContextPreamble(GPU_U32 *out,unsigned capacity,GPU_U64 fenceAddress,GPU_U32 previous) {
    unsigned required=previous?GPU_CONTEXT_PREAMBLE_MAX_WORDS:6u,at=0;
    if(capacity<required || (previous && (!fenceAddress || (fenceAddress&3))))return 0;
    if(previous) {
        out[at++]=GpuContextPacket(0x14,4);out[at++]=0;
        out[at++]=(GPU_U32)fenceAddress;out[at++]=(GPU_U32)(fenceAddress>>32);out[at++]=previous;
    }
    out[at++]=GpuContextPacket(0x17,1);out[at++]=0x80000003u; /* SYNC_THREADS | BOTH */
    out[at++]=GpuContextPacket(0x1f,1);out[at++]=0xf; /* all four context sync bits */
    out[at++]=GpuContextPacket(0x17,1);out[at++]=0x80000003u;
    return at;
}
