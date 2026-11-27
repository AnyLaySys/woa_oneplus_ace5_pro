#pragma once

/* A bad image still requires route teardown. Only hardware halt/drain and
   verified register restoration authorize it; completion alone does not. */
static inline BOOLEAN CamIspFdReleased(const QCAM_ISP_FD_STATE *state,
    BOOLEAN dmaActive,BOOLEAN rxActive,BOOLEAN registerOwner) {
    return state&&!dmaActive&&!rxActive&&!registerOwner&&
        (state->Stage&0x60)==0x60&&!state->DrainStatus&&!state->RestoreStatus;
}
