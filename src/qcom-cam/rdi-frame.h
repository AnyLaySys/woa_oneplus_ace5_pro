#pragma once

#define CAM_RDI_FATAL_IRQ 0x241c6001u

static BOOLEAN CamRdiFrameCompleteHeight(ULONG packets,ULONG height,ULONG rdiIrq,ULONG busIrq) {
    return packets>=height&&!(rdiIrq&CAM_RDI_FATAL_IRQ)&&(busIrq&0x10000);
}

static inline BOOLEAN CamRdiFrameComplete(ULONG packets,ULONG rdiIrq,ULONG busIrq) {return CamRdiFrameCompleteHeight(packets,QCAM_HEIGHT,rdiIrq,busIrq);}

static BOOLEAN CamRdiPairReady(ULONG bus,ULONG mask,ULONGLONG sof0,ULONGLONG sof1,ULONGLONG eof0,ULONGLONG eof1,ULONGLONG previous) {
    return (bus&mask)==mask&&eof0&&eof0==eof1&&eof0!=previous&&sof0==sof1&&
        sof0<=eof0;
}
