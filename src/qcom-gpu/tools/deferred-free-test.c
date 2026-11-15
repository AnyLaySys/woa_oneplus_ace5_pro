#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../deferred-free.h"
static void Check(int value){if(!value)abort();}
int main(void) {
    GPU_DEFERRED_FREE state={0};
    /* A successful Render that never reaches Submit still owns a ref. FREE
       records the original slot and returns; it neither waits nor revokes the
       context. Completing unrelated hardware cannot make that slot reusable. */
    Check(GpuDeferFree(&state,7,0,3));
    Check(state.Buffers[7] && !GpuDeferredReady(&state,1,1,1,100));
    Check(!GpuDeferFree(&state,7,0,5));Check(state.Fence==3);
    Check(GpuDeferFree(&state,8,1,9));Check(state.Imports[8] && state.Fence==9);
    Check(!GpuDeferredReady(&state,0,1,1,8));
    Check(!GpuDeferredReady(&state,0,1,0,100));
    Check(GpuDeferredReady(&state,0,1,1,9));
    /* Only the collector clears the original occupied slots after retirement. */
    state.Buffers[7]=0;state.Imports[8]=0;state.Fence=0;
    Check(GpuDeferFree(&state,7,0,10));Check(state.Fence==10);
    Check(!GpuDeferFree(&state,GPU_MAX_BUFFERS,0,10));
    Check(!GpuDeferredReady(&state,1,0,0,0));
    Check(GpuDeferredReady(&state,0,0,0,0));
    puts("Deferred FREE: unsubmitted Render stays pinned without waiting, duplicate/slot bounds, imported slot, max fence, quiesced hardware: PASS");return 0;
}
