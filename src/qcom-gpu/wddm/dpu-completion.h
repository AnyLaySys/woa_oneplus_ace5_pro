#pragma once

static int A830DpuTimingValid(unsigned sync,unsigned autorefresh) {
    return sync!=~0u && autorefresh!=~0u && (sync&0x180000u)==0x180000u &&
        (autorefresh&0x80000000u) && (autorefresh&0xffffu);
}

static int A830DpuCommitComplete(unsigned initialFrame,unsigned frame,
    int active,unsigned autorefreshDone,unsigned pingpongDone,int flushed) {
    return !active && (autorefreshDone&1u) && pingpongDone &&
        frame!=initialFrame && flushed;
}
