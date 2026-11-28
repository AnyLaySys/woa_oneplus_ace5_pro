#pragma once

#include "isp-input.h"
#include "isp-pixel.h"
#include "isp-bus.h"
#include "isp-precision.h"
#include "isp-gtm-reference.h"
#include "dma-idle.h"
#include "csid-commit.h"

/* Only configuration registers are retained here. Image-address writes
   enqueue buffers and must not be used for standby checks or restoration. */
static BOOLEAN CamIspFdAdd(QCAM_ISP_FD_STATE *state,ULONG region,ULONG offset,ULONG value) {
    if(state->Count>=QCAM_ISP_FD_REG_COUNT)return FALSE;
    ULONG i=state->Count++;
    state->Region[i]=region;state->Offset[i]=offset;state->Expected[i]=value;
    return TRUE;
}

static BOOLEAN CamIspFdBuildMode(QCAM_ISP_FD_STATE *state,ULONG width,ULONG height,ULONGLONG address,ULONG capacity,ULONG profile) {
    CAM_ISP_RAW10_INPUT input;CAM_ISP_PIXEL_PLAN pixel;CAM_ISP_FD_PLAN bus;
    BOOLEAN half=profile==1,exact=profile==2;
    ULONG outWidth=exact?1024:QCAM_ISP_FD_WIDTH,outHeight=exact?768:QCAM_ISP_FD_HEIGHT;
    if(!state||!CamIspRaw10Input(&input,0,0,0,0,width,height)||
       profile>33||((exact||profile>=7)&&(width!=4096||height!=3072))||
       !(((profile>=17&&profile<=21)||profile==24||profile==25)?CamIspPixelLumaDiagnostic(&pixel,width,height,profile>=24?profile-19:profile-17):(profile==13||profile==16||profile==23)?CamIspPixelBds2Reference(&pixel):profile==6?CamIspPixelCstBiasDiagnostic(&pixel,width,height):profile==5?CamIspPixelChromaDiagnostic(&pixel,width,height):profile==4?CamIspPixelConstant(&pixel,width,height):half?CamIspPixelReference(&pixel,width>>1,height>>1):
           CamIspPixelReferenceOutput(&pixel,width,height,outWidth,outHeight,!exact))||
       !(profile==3?CamIspFdPlanPacked(&bus,outWidth,outHeight,address,capacity,1):
           CamIspFdPlan(&bus,outWidth,outHeight,address,capacity)))return FALSE;
    if(profile==33&&!CamIspPixelMainBlacklevelReference(&pixel))return FALSE;
    RtlZeroMemory(state,sizeof(*state));state->Size=sizeof(*state);state->Abi=QCAM_ABI;
    state->Width=outWidth;state->Height=outHeight;
    state->Stride=bus.Output.Stride;state->ImageBytes=bus.Output.Bytes;state->Address=address;
    state->ScalerWidth=width>>(half||profile==13||profile==16||profile==23);state->ScalerHeight=height>>(half||profile==13||profile==16||profile==23);
    if(!CamIspFdAdd(state,CamTfe,0x924,0)||!CamIspFdAdd(state,CamTfe,0x6260,0)||
       /* Original GTM disable emitter 0xea0e0 targets CONTROL at 0x6660.
          0x6658 is the LUT bank selector and cannot disable the module. */
       !CamIspFdAdd(state,CamTfe,0x6660,0)||!CamIspFdAdd(state,CamTfe,0x1ec,3)||
       !CamIspFdAdd(state,CamTfe,0xc1ec,3)||!CamIspFdAdd(state,CamTfe,0x858,0x1036))return FALSE;
    for(ULONG resource=1;resource<=2;resource++) {
        ULONG registers[4];
        if(!CamIspCompDecompDisabled(registers,resource,0,1,0))return FALSE;
        for(ULONG i=0;i<4;i+=2)
            if(!CamIspFdAdd(state,CamTfe,registers[i],registers[i+1]))return FALSE;
    }
    for(ULONG i=0;i<CAM_ISP_INPUT_REGS;i++)
        if(!CamIspFdAdd(state,CamCsid,input.Registers[i].Offset,input.Registers[i].Value))return FALSE;
    for(ULONG i=0;i<pixel.Count;i++)
        if(!CamIspFdAdd(state,CamTfe,pixel.Registers[i].Offset,pixel.Registers[i].Value))return FALSE;
    /* Native CC/BLS/WB disable emitters change only control bit0 after
       selecting the retained context. These explicit diagnostics keep
       actual RAW and every other module unchanged. */
    ULONG bypassOffset=profile==9?0x6460:profile==10?0xda60:profile==11?0xe060:(profile==26||profile==28||profile==29)?0x6d60:0;
    if(bypassOffset)for(ULONG i=0;i<state->Count;i++)
        if(state->Region[i]==CamTfe&&state->Offset[i]==bypassOffset)state->Expected[i]&=~1u;
    /* Expose the unsigned native internal range through the hardware CRC.
       No CPU pixels or production-color interpretation in these probes. */
    if(profile==28||profile==29)for(ULONG i=0;i<state->Count;i++)
        if(state->Region[i]==CamTfe&&(state->Offset[i]==0x7374||state->Offset[i]==0x7574||state->Offset[i]==0x757c))
            state->Expected[i]=(profile==28?8u:6u)<<3;
    /* Keep the native matrix and real sensor input. A separately labelled
       diagnostic adds the verified native neutral bias512 to Y, so signed
       luma arithmetic can be observed before clamp. Not production color. */
    if(profile==12)for(ULONG i=0;i<state->Count;i++)
        if(state->Region[i]==CamTfe&&state->Offset[i]==0x6d78)state->Expected[i]=512;
    for(ULONG i=0;i<38;i++) {
        ULONG offset=bus.Registers[i].Offset;
        if((offset&255)==4)continue;
        if(!CamIspFdAdd(state,CamTfe,offset,bus.Registers[i].Value))return FALSE;
    }
    if(state->Count!=QCAM_ISP_FD_BASE_REG_COUNT)return FALSE;
    if(profile==7||profile==22||profile==23) {
        ULONG words[49],offsets[49],first;
        if(!CamIspCompandRaw10Reference(words)||!CamIspCompDecompRegisters(offsets,17,&first)||first!=1)return FALSE;
        /* The FD resource has no shared context. Replace its retained
           disabled control and append the complete native curve packet. */
        BOOLEAN control=FALSE;
        for(ULONG i=0;i<state->Count;i++)if(state->Region[i]==CamTfe&&state->Offset[i]==offsets[1]) {
            state->Expected[i]=words[1];control=TRUE;
        }
        if(!control)return FALSE;
        for(ULONG i=2;i<49;i++)if(!CamIspFdAdd(state,CamTfe,offsets[i],words[i]))return FALSE;
    }
    if(profile>=14) {
        CAM_ISP_GTM gtm;
        if(!CamIspGtmNativeReference(&gtm,0))return FALSE;
        for(ULONG i=0;i<state->Count;i++)if(state->Region[i]==CamTfe&&state->Offset[i]==0x6660)state->Expected[i]=1;
        for(ULONG i=0;i<263;i++)if(i!=2)
            if(!CamIspFdAdd(state,CamTfe,0x6658+i*4,gtm.Registers[i]))return FALSE;
    }
    state->Profile=profile;state->Stage=1;
    return state->Count==QCAM_ISP_FD_PROFILE_REG_COUNT(profile);
}

static inline BOOLEAN CamIspFdValidPlan(const QCAM_ISP_FD_STATE *state) {
    return state&&state->Profile<=33&&state->Count==QCAM_ISP_FD_PROFILE_REG_COUNT(state->Profile);
}

/* Load only the bank inactive before FD configuration. Before[] retains
   the original selection until stop/drain restores it; the overwritten
   inactive LUT is never silently substituted for the original active one. */
static inline BOOLEAN CamIspFdGammaBank(const QCAM_ISP_FD_STATE *state,ULONG *bank) {
    if(!bank||!CamIspFdValidPlan(state)||(state->Profile!=8&&(state->Profile<15||state->Profile>33))||(state->Stage&3)!=3)return FALSE;
    ULONG before[3]={0},found=0;
    for(ULONG i=0;i<state->Count;i++)if(state->Region[i]==CamTfe&&state->Offset[i]>=0x6b58&&state->Offset[i]<=0x6b60) {
        ULONG n=(state->Offset[i]-0x6b58)/4;
        if(found&(1u<<n))return FALSE;
        before[n]=state->Before[i];found|=1u<<n;
    }
    if(found!=7||before[0]>1||before[1]!=before[0]||before[2]>1)return FALSE;
    *bank=before[0]^1u;return TRUE;
}

static inline BOOLEAN CamIspFdGammaConfigured(QCAM_ISP_FD_STATE *state) {
    ULONG bank;
    if(!CamIspFdGammaBank(state,&bank)||!state->GammaLoaded||state->GammaStatus||state->GammaBank!=bank)return FALSE;
    const ULONG expected[]={bank,bank,1};
    for(ULONG i=0;i<3;i++)if(state->GammaConfig[i]!=expected[i])return FALSE;
    for(ULONG i=0;i<state->Count;i++)if(state->Region[i]==CamTfe&&state->Offset[i]>=0x6b58&&state->Offset[i]<=0x6b60) {
        ULONG n=(state->Offset[i]-0x6b58)/4;
        state->Expected[i]=state->Configured[i]=expected[n];
    }
    return TRUE;
}

static BOOLEAN CamIspFdBuildProfile(QCAM_ISP_FD_STATE *state,ULONG width,ULONG height,ULONGLONG address,ULONG capacity,BOOLEAN half) {
    if(half!=TRUE&&half!=FALSE)return FALSE;
    return CamIspFdBuildMode(state,width,height,address,capacity,half?1:0);
}

static BOOLEAN CamIspFdBuild(QCAM_ISP_FD_STATE *state,ULONG width,ULONG height,ULONGLONG address,ULONG capacity) {
    return CamIspFdBuildProfile(state,width,height,address,capacity,FALSE);
}

static PUCHAR CamIspFdBase(PUCHAR csid,PUCHAR tfe,ULONG region) {
    return region==CamCsid?csid:tfe;
}

/* Read only after a fresh paired FD completion. Standby readback alone
   does not establish that the same controls survive receiver startup.
   IPP CFG0 gains its documented path-enable bit when streaming starts. */
static inline BOOLEAN CamIspFdFrameRegisters(PUCHAR csid,PUCHAR tfe,QCAM_ISP_FD_STATE *state) {
    if(!CamIspFdValidPlan(state)||(state->Stage&0x1f)!=0x1f||
       !state->FrameSof||state->FrameEof<=state->FrameSof)return FALSE;
    state->AtFrameCount=state->AtFrameMismatch=0;
    for(ULONG i=0;i<state->Count;i++) {
        ULONG expected=state->Expected[i];
        if(state->Region[i]==CamCsid&&state->Offset[i]==0x600)expected|=0x80000000u;
        state->AtFrame[i]=CamRead(CamIspFdBase(csid,tfe,state->Region[i]),state->Offset[i]);
        if(state->AtFrame[i]!=expected)state->AtFrameMismatch++;
        state->AtFrameCount++;
    }
    return TRUE;
}

static NTSTATUS CamIspFdApply(PUCHAR csid,PUCHAR tfe,QCAM_ISP_FD_STATE *state) {
    if(!CamIspFdValidPlan(state))return STATUS_INVALID_PARAMETER;
    if(CamRead(csid,0)!=0x30000006||CamRead(tfe,0)!=0x10000000)return STATUS_REVISION_MISMATCH;
    if(!CamDmaResourcesIdle(csid,tfe)||(CamRead(csid,0x3f0)&31)||CamRead(tfe,0x924))return STATUS_DEVICE_BUSY;
    const ULONG contexts[]={0xdbf4,0xe1f4,0x78f4,0x8ef4,0xe3f4};
    for(ULONG i=0;i<sizeof(contexts)/sizeof(contexts[0]);i++)
        if(CamRead(tfe,contexts[i])&0x41f)return STATUS_DEVICE_BUSY;
    for(ULONG i=0;i<state->Count;i++)
        state->Before[i]=CamRead(CamIspFdBase(csid,tfe,state->Region[i]),state->Offset[i]);
    /* Disable processing with the native bit0 update. Keep compression-mode
       and other control fields; zeroing the whole word changed reset mode1
       (live readback0x4) into mode0 even while the module was disabled. */
    for(ULONG i=0;i<state->Count;i++)
        if(state->Region[i]==CamTfe&&((state->Offset[i]==0x6260&&state->Profile!=7&&state->Profile!=22&&state->Profile!=23)||state->Offset[i]==0xe260||state->Offset[i]==0x8d60||(state->Offset[i]==0x6660&&state->Profile<14)||(state->Profile==9&&state->Offset[i]==0x6460)||(state->Profile==10&&state->Offset[i]==0xda60)||(state->Profile==11&&state->Offset[i]==0xe060)||((state->Profile==26||state->Profile==28||state->Profile==29)&&state->Offset[i]==0x6d60)))
            state->Expected[i]=state->Before[i]&~1u;
        else if(state->Region[i]==CamTfe&&state->Offset[i]==0x858)
            /* Stock group1 FD startup ORs the sun LPDDR5(X) static value,
               including plain NV12/PLAIN8 outputs with no UBWC metadata. */
            state->Expected[i]=state->Before[i]|0x1036;
    if(state->Profile>=14) {
        ULONG bank=MAXULONG,second=MAXULONG;
        for(ULONG i=0;i<state->Count;i++)if(state->Region[i]==CamTfe) {
            if(state->Offset[i]==0x6658)bank=state->Before[i];
            if(state->Offset[i]==0x665c)second=state->Before[i];
        }
        if(bank>1||second!=bank)return STATUS_NOT_SUPPORTED;
        state->GtmBank=bank^1u;
        for(ULONG i=0;i<state->Count;i++)if(state->Region[i]==CamTfe&&(state->Offset[i]==0x6658||state->Offset[i]==0x665c))
            state->Expected[i]=state->GtmBank;
    }
    state->Stage|=2;
    for(ULONG i=0;i<state->Count;i++)
        CamWrite(CamIspFdBase(csid,tfe,state->Region[i]),state->Offset[i],state->Expected[i]);
    CamCsidCommit(csid,1);
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<state->Count;i++) {
        state->Configured[i]=CamRead(CamIspFdBase(csid,tfe,state->Region[i]),state->Offset[i]);
        if(state->Configured[i]!=state->Expected[i])status=STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

static NTSTATUS CamIspFdRestoreRegisters(PUCHAR csid,PUCHAR tfe,QCAM_ISP_FD_STATE *state) {
    if(!state||!(state->Stage&2))return STATUS_SUCCESS;
    if(!CamIspFdValidPlan(state))return STATUS_INVALID_PARAMETER;
    if(!CamDmaResourcesIdle(csid,tfe))return STATUS_DEVICE_BUSY;
    for(ULONG i=state->Count;i;i--)
        CamWrite(CamIspFdBase(csid,tfe,state->Region[i-1]),state->Offset[i-1],state->Before[i-1]);
    CamCsidCommit(csid,1);
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<state->Count;i++) {
        state->Restored[i]=CamRead(CamIspFdBase(csid,tfe,state->Region[i]),state->Offset[i]);
        if(state->Restored[i]!=state->Before[i])status=STATUS_IO_DEVICE_ERROR;
    }
    state->RestoreStatus=(ULONG)status;
    if(status==STATUS_SUCCESS)state->Stage|=32;
    return status;
}
