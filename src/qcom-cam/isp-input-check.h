#pragma once

#include "isp-input.h"
#include "dma-idle.h"
#include "csid-commit.h"

static NTSTATUS CamIspRaw10Check(PUCHAR csid,PUCHAR tfe,ULONG width,ULONG height,QCAM_ISP_INPUT_STATE *state) {
    CAM_ISP_RAW10_INPUT plan;
    if(!state||!CamIspRaw10Input(&plan,0,0,0,0,width,height))return STATUS_INVALID_PARAMETER;
    if(CamRead(csid,0)!=0x30000006||CamRead(tfe,0)!=0x10000000)return STATUS_REVISION_MISMATCH;
    if(!CamDmaResourcesIdle(csid,tfe))return STATUS_DEVICE_BUSY;
    if(CamRead(csid,0x3f0)&31)return STATUS_DEVICE_BUSY;
    RtlZeroMemory(state,sizeof(*state));state->Size=sizeof(*state);state->Abi=QCAM_ABI;
    state->Width=width;state->Height=height;
    for(ULONG i=0;i<CAM_ISP_INPUT_REGS;i++)state->Before[i]=CamRead(csid,plan.Registers[i].Offset);
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<CAM_ISP_INPUT_REGS;i++)CamWrite(csid,plan.Registers[i].Offset,plan.Registers[i].Value);
    CamCsidCommit(csid,1);CamDelay(1);
    for(ULONG i=0;i<CAM_ISP_INPUT_REGS;i++) {
        state->Configured[i]=CamRead(csid,plan.Registers[i].Offset);
        if(state->Configured[i]!=plan.Registers[i].Value)status=STATUS_IO_DEVICE_ERROR;
    }
    for(ULONG n=CAM_ISP_INPUT_REGS;n;n--)CamWrite(csid,plan.Registers[n-1].Offset,state->Before[n-1]);
    CamCsidCommit(csid,1);CamDelay(1);
    for(ULONG i=0;i<CAM_ISP_INPUT_REGS;i++) {
        state->Restored[i]=CamRead(csid,plan.Registers[i].Offset);
        if(state->Restored[i]!=state->Before[i])status=STATUS_IO_DEVICE_ERROR;
    }
    if(!CamDmaResourcesIdle(csid,tfe))status=STATUS_DEVICE_BUSY;
    state->Status=(ULONG)status;
    return status;
}
