#pragma once

#include "isp-pixel.h"
#include "dma-idle.h"

static NTSTATUS CamIspPixelStandbyCheck(PUCHAR csid,PUCHAR tfe,ULONG width,ULONG height,QCAM_ISP_PIXEL_STATE *state) {
    CAM_ISP_PIXEL_PLAN plan;
    if(!state||!CamIspPixelReference(&plan,width,height))return STATUS_INVALID_PARAMETER;
    if(CamRead(csid,0)!=0x30000006||CamRead(tfe,0)!=0x10000000)return STATUS_REVISION_MISMATCH;
    if(!CamDmaResourcesIdle(csid,tfe)||(CamRead(csid,0x3f0)&31))return STATUS_DEVICE_BUSY;
    const ULONG contexts[]={0xdbf4,0xe1f4,0x78f4};
    for(ULONG i=0;i<3;i++)if(CamRead(tfe,contexts[i])&0x41f)return STATUS_DEVICE_BUSY;
    RtlZeroMemory(state,sizeof(*state));state->Size=sizeof(*state);state->Abi=QCAM_ABI;
    state->Width=width;state->Height=height;state->Count=plan.Count;
    for(ULONG i=0;i<plan.Count;i++) {
        state->Offsets[i]=plan.Registers[i].Offset;state->Expected[i]=plan.Registers[i].Value;
        state->Before[i]=CamRead(tfe,plan.Registers[i].Offset);
    }
    for(ULONG i=0;i<plan.Count;i++)CamWrite(tfe,plan.Registers[i].Offset,plan.Registers[i].Value);
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<plan.Count;i++) {
        state->Configured[i]=CamRead(tfe,plan.Registers[i].Offset);
        if(state->Configured[i]!=state->Expected[i])status=STATUS_IO_DEVICE_ERROR;
    }
    for(ULONG i=plan.Count;i;i--)CamWrite(tfe,plan.Registers[i-1].Offset,state->Before[i-1]);
    for(ULONG i=0;i<plan.Count;i++) {
        state->Restored[i]=CamRead(tfe,plan.Registers[i].Offset);
        if(state->Restored[i]!=state->Before[i])status=STATUS_IO_DEVICE_ERROR;
    }
    if(!CamDmaResourcesIdle(csid,tfe))status=STATUS_DEVICE_BUSY;
    state->Status=(ULONG)status;return status;
}
