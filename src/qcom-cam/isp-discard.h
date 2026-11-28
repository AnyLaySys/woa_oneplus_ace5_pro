#pragma once

/* Original cam_ife_csid_ver2_discard_sof_pix_bottom_half counts INPUT_SOF
   (ver2 BIT12), then ORs resume_frame_boundary into the path control.
   It does not recycle a downstream buffer or modify RUP/AUP during a frame.
   Caller owns the powered, streaming pipeline and checks all error IRQs. */
static inline NTSTATUS CamIspFdDiscardPoll(PUCHAR csid,PUCHAR tfe,
    QCAM_ISP_FD_STATE *state,ULONGLONG now) {
    if(!csid||!tfe||!state||state->Profile!=33||state->WarmupFrames||
       state->DiscardFrames!=3||state->DiscardSofCount>=3||(state->Stage&31)!=15||
       state->CompletedFrames||state->FrameSof||state->FrameEof||!now||
       CamRead(csid,0x604)!=0||CamRead(tfe,0x1400)!=1||CamRead(tfe,0x1500)!=1)
        return STATUS_DEVICE_NOT_READY;
    ULONG irq=CamRead(csid,0xd4);
    if(!(irq&0x1000u))return STATUS_PENDING;
    ULONG n=state->DiscardSofCount;
    if(n&&now<=state->DiscardPollTime[n-1])return STATUS_DEVICE_NOT_READY;
    CamWrite(csid,0xdc,0x1000);CamWrite(csid,0x14,1);
    if(CamRead(csid,0xd4)&0x1000u)return STATUS_IO_DEVICE_ERROR;
    state->DiscardInputIrq[n]=irq;state->DiscardPollTime[n]=now;state->DiscardSofCount++;
    if(state->DiscardSofCount==3) {
        CamWrite(csid,0x604,1);
        if(CamRead(csid,0x604)!=1)return STATUS_IO_DEVICE_ERROR;
        return STATUS_SUCCESS;
    }
    return STATUS_PENDING;
}
