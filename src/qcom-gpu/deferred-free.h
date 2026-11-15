#pragma once
#include "uapi.h"

/* A marked slot keeps its original BO/import and cannot be reallocated until
   both prepared-command ownership and the captured hardware fence retire. */
typedef struct {
    unsigned char Buffers[GPU_MAX_BUFFERS],Imports[GPU_MAX_BUFFERS];
    GPU_U64 Fence;
} GPU_DEFERRED_FREE;

static inline int GpuDeferFree(GPU_DEFERRED_FREE *state,unsigned slot,int imported,GPU_U64 fence) {
    if(slot>=GPU_MAX_BUFFERS)return 0;
    unsigned char *pending=imported?state->Imports:state->Buffers;
    if(pending[slot])return 0;
    pending[slot]=1;if(state->Fence<fence)state->Fence=fence;return 1;
}
static inline int GpuDeferredReady(const GPU_DEFERRED_FREE *state,unsigned pendingRefs,int online,int completionMapped,GPU_U64 completed) {
    return !pendingRefs && (!online || (completionMapped && completed>=state->Fence));
}
