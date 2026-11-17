#pragma once

/* Both status bits were cleared after the old transfer drained and before
   this flush. A consumed flush plus a later completed frame proves that the
   new source was transferred, even if the timer never sampled active WRPTR. */
static int A830DpuCommitComplete(unsigned initialFrame,unsigned frame,
    int active,unsigned autorefreshDone,unsigned pingpongDone,int flushed) {
    return !active && (autorefreshDone&1u) && pingpongDone &&
        frame!=initialFrame && flushed;
}
