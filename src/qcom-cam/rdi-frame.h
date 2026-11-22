#pragma once

#define CAM_RDI_FATAL_IRQ 0x241c6001u

static BOOLEAN CamRdiFrameComplete(ULONG packets,ULONG rdiIrq,ULONG busIrq) {
    return packets>=QCAM_HEIGHT&&!(rdiIrq&CAM_RDI_FATAL_IRQ)&&((rdiIrq&8)||(busIrq&0x10000));
}
