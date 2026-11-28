#pragma once
#include "isp-bus.h"
#include "csid-commit.h"

/* Whole hardware frames, never pixel merging. Warmup writes three separate
   existing arena slots. The returned frame uses slot0, initialized before
   DMA and untouched by every discarded frame, so coverage cannot accumulate
   across exposures. Slot2/3 match the existing MDL's 256-byte displacement. */
static inline BOOLEAN CamIspFdFrameLayout(CAM_ISP_NV12_OUTPUT *layout,
    const QCAM_ISP_FD_STATE *state,ULONG frame,ULONG *offset) {
    if(!layout||!state||!offset||(state->BufferOffset!=0&&state->BufferOffset!=128&&state->BufferOffset!=256)||
       (state->WarmupFrames!=0&&state->WarmupFrames!=3)||
       frame>state->WarmupFrames||(state->WarmupFrames&&
       (state->Profile!=33||state->BufferOffset||state->Width!=1280||state->Height!=720)))return FALSE;
    ULONG displacement=frame<state->WarmupFrames?
        QCAM_FRAME_BYTES*(frame+1)+(frame?256u:0u):0;
    if(displacement>QCAM_ARENA_BYTES-4096-state->BufferOffset||
       QCAM_FRAME_BYTES>QCAM_ARENA_BYTES-4096-state->BufferOffset-displacement)return FALSE;
    ULONGLONG address=state->Address+displacement;
    if(address<state->Address||!CamIspNv12Output(layout,state->Width,state->Height,
       address,QCAM_FRAME_BYTES-state->BufferOffset)||layout->Bytes!=state->ImageBytes||layout->Stride!=state->Stride)return FALSE;
    *offset=state->BufferOffset+displacement;
    return TRUE;
}

static inline BOOLEAN CamIspFdRecordWarmup(QCAM_ISP_FD_STATE *state) {
    if(!state||state->WarmupFrames!=3||state->CompletedFrames>=3||
       (state->Stage&31)!=31||!state->FrameSof||state->FrameEof<=state->FrameSof||
       state->FrameSof<=state->StartSof||state->FrameEof<=state->StartEof||
       state->AtFrameCount!=state->Count||state->AtFrameMismatch)return FALSE;
    ULONG frame=state->CompletedFrames;
    if(frame&&(state->FrameSof<=state->WarmupSof[frame-1]||
       state->FrameEof<=state->WarmupEof[frame-1]))return FALSE;
    state->WarmupSof[frame]=state->FrameSof;state->WarmupEof[frame]=state->FrameEof;
    state->CompletedFrames++;
    return TRUE;
}

/* Caller retains exclusive ISP/DMA ownership and has no pending firmware,
   CDM or TBU work. Refuse stale completion, mismatched plane addresses and
   faults before any acknowledge/address write. Never re-enable a writer. */
static inline NTSTATUS CamIspFdRequeueWarmup(PUCHAR csid,PUCHAR tfe,QCAM_ISP_FD_STATE *state) {
    CAM_ISP_NV12_OUTPUT layout;ULONG offset;
    if(!csid||!tfe||!state||state->CompletedFrames>=state->WarmupFrames||
       !CamIspFdFrameLayout(&layout,state,state->CompletedFrames,&offset)||
       CamRead(tfe,0x1400)!=1||CamRead(tfe,0x1500)!=1||
       (CamRead(csid,0xd4)&CAM_ISP_IPP_FATAL)||(CamRead(csid,0xb4)&0x25fff000u)||
       CamRead(tfe,0x868)||CamRead(tfe,0x870)||
       !CamIspFdFrameReady(CamRead(csid,0xa4),state->IppIrq,state->FrameSof,state->FrameEof,
           state->StartEof,CamRead(tfe,0x1490),CamRead(tfe,0x1590),&layout)||
       !CamIspFdRecordWarmup(state)||
       !CamIspFdFrameLayout(&layout,state,state->CompletedFrames,&offset))return STATUS_DEVICE_NOT_READY;
    CamWrite(csid,0xac,CAM_ISP_FD_DONE);CamWrite(csid,0x14,1);
    if(CamRead(csid,0xa4)&CAM_ISP_FD_DONE)return STATUS_IO_DEVICE_ERROR;
    state->StartSof=state->FrameSof;state->StartEof=state->FrameEof;
    state->FrameSof=state->FrameEof=0;state->Stage&=~16u;
    CamWrite(tfe,0x1404,layout.ImageAddress[0]);CamWrite(tfe,0x1504,layout.ImageAddress[1]);
    CamCsidCommit(csid,1);
    return STATUS_SUCCESS;
}
