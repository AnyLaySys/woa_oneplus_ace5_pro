#pragma once

static int A830DpuCommitComplete(unsigned initialFrame,unsigned frame,
    int active,unsigned autorefreshDone,unsigned pingpongDone,int flushed) {
    return !active && (autorefreshDone&1u) && pingpongDone &&
        frame!=initialFrame && flushed;
}
