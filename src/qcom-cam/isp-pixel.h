#pragma once

#include "isp-common.h"
#include "isp-bayer.h"
#include "isp-color.h"
#include "isp-demosaic.h"
#include "isp-mnds.h"
#include "isp-crop.h"

#define CAM_ISP_PIXEL_REGS 227u
typedef struct { ULONG Offset,Value; } CAM_ISP_PIXEL_REG;
typedef struct {
    ULONG Common[13],Demux[92],ChannelGain[14],Wb[11],Demosaic[20],Cc[11],Gamma[3],Cst[19],Mnds[20],Crop[27];
} CAM_ISP_PIXEL_PACKED;
typedef struct { ULONG Count; CAM_ISP_PIXEL_REG Registers[CAM_ISP_PIXEL_REGS]; } CAM_ISP_PIXEL_PLAN;

/* Native WriteRegRange destinations. Packed words are NOT contiguous MMIO.
   These ranges contain module controls only: no DMA enable, image address,
   interrupt acknowledge or DMI data port. */
static BOOLEAN CamIspPixelRange(CAM_ISP_PIXEL_PLAN *plan,ULONG offset,const ULONG *words,ULONG count) {
    if(!plan||!words||!count||(offset&3)||offset>0xfffc||count>(0x10000-offset)/4||
       plan->Count>CAM_ISP_PIXEL_REGS||count>CAM_ISP_PIXEL_REGS-plan->Count)return FALSE;
    for(ULONG i=0;i<count;i++) {
        plan->Registers[plan->Count].Offset=offset+i*4;
        plan->Registers[plan->Count++].Value=words[i];
    }
    return TRUE;
}

static BOOLEAN CamIspPixelBuild(CAM_ISP_PIXEL_PLAN *plan,const CAM_ISP_PIXEL_PACKED *p) {
    if(!plan||!p)return FALSE;
    plan->Count=0;
#define CAM_ISP_RANGE(offset,member,first,count) \
    if(!CamIspPixelRange(plan,offset,p->member+first,count))return FALSE
    CAM_ISP_RANGE(0x44,Common,0,10);CAM_ISP_RANGE(0x24,Common,10,3);
    CAM_ISP_RANGE(0xd058,Demux,0,8);CAM_ISP_RANGE(0xd07c,Demux,8,84);
    CAM_ISP_RANGE(0xdbf4,ChannelGain,0,1);CAM_ISP_RANGE(0xda60,ChannelGain,1,1);
    CAM_ISP_RANGE(0xda68,ChannelGain,2,12);
    CAM_ISP_RANGE(0xe1f4,Wb,0,1);CAM_ISP_RANGE(0xe060,Wb,1,1);CAM_ISP_RANGE(0xe068,Wb,2,9);
    CAM_ISP_RANGE(0x78f4,Demosaic,19,1);CAM_ISP_RANGE(0x7760,Demosaic,0,1);
    CAM_ISP_RANGE(0x7794,Demosaic,12,1);CAM_ISP_RANGE(0x7768,Demosaic,1,11);
    CAM_ISP_RANGE(0x78a0,Demosaic,13,4);
    /* 0x78e4 is conditional on native input+0x210==1; ordinary capture omits it. */
    CAM_ISP_RANGE(0x6460,Cc,0,1);CAM_ISP_RANGE(0x6468,Cc,1,10);
    CAM_ISP_RANGE(0x6b58,Gamma,0,3);
    CAM_ISP_RANGE(0x6d60,Cst,0,1);CAM_ISP_RANGE(0x6d68,Cst,1,18);
    CAM_ISP_RANGE(0x6f60,Mnds,0,10);CAM_ISP_RANGE(0x7160,Mnds,10,10);
    /* FD CRC13 resource17 skips its shared context register. */
    CAM_ISP_RANGE(0x7360,Crop,1,1);CAM_ISP_RANGE(0x7368,Crop,2,2);CAM_ISP_RANGE(0x7370,Crop,4,10);
    CAM_ISP_RANGE(0x7560,Crop,14,1);CAM_ISP_RANGE(0x7568,Crop,15,2);CAM_ISP_RANGE(0x7570,Crop,17,10);
#undef CAM_ISP_RANGE
    return plan->Count==CAM_ISP_PIXEL_REGS;
}

/* Explicit standby reference configuration, not selected sensor tuning.
   Used to validate MMIO/context access before any real pixel DMA is allowed.
   Gamma stays disabled because this diagnostic does not load its LUTs. */
static BOOLEAN CamIspPixelReferenceOutput(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height,ULONG outWidth,ULONG outHeight,BOOLEAN nativeGeometry) {
    CAM_ISP_PIXEL_PACKED p;
    const ULONG throttles[6]={0},flags[5]={0},pdaf[2]={0};
    const LONG offsets[6]={0},cc[9]={128,0,0,0,128,0,0,0,128};
    /* Stock FD CST root: Q10 coefficients in G/B/R order, 10-bit output. */
    const LONG cst[9]={601,117,306,-338,510,-172,-427,-83,510},bias[3]={0,512,512};
    const ULONG low[3]={0},high[3]={1023,1023,1023};
    RtlZeroMemory(&p,sizeof(p));
    if(!CamIspCommonBuild(p.Common,throttles,flags,pdaf,FALSE)||!CamIspDemuxBayerBuild(p.Demux)||
       !CamIspChannelGainReference(p.ChannelGain,0)||!CamIspWbBuild(p.Wb,1024,1024,1024,offsets)||
       !CamIspDemosaicNative(p.Demosaic)||!CamIspCcBuild(p.Cc,cc,offsets,0)||
       !CamIspCstBuild(p.Cst,cst,bias,offsets,low,high))return FALSE;
    if(nativeGeometry) {
        if(outWidth!=1280||outHeight!=720)return FALSE;
        if(!CamIspMndsFdBuild(p.Mnds,width,height)||!CamIspFdCenteredCropBuild(p.Crop))return FALSE;
    } else if(!CamIspMndsBuild(p.Mnds,width,height,outWidth,outHeight)||!CamIspFdCropBuild(p.Crop,outWidth,outHeight,2))return FALSE;
    return CamIspPixelBuild(plan,&p);
}

static BOOLEAN CamIspPixelReferenceProfile(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height,BOOLEAN nativeGeometry) {
    return CamIspPixelReferenceOutput(plan,width,height,1280,720,nativeGeometry);
}

static inline BOOLEAN CamIspPixelMainBlacklevelReference(CAM_ISP_PIXEL_PLAN *plan) {
    ULONG words[14],found=0;
    if(!plan||plan->Count!=CAM_ISP_PIXEL_REGS||!CamIspChannelGainMainBlacklevelReference(words))return FALSE;
    for(ULONG i=0;i<plan->Count;i++) {
        ULONG offset=plan->Registers[i].Offset;
        if(offset==0xdbf4){plan->Registers[i].Value=words[0];found++;}
        else if(offset==0xda60){plan->Registers[i].Value=words[1];found++;}
        else if(offset>=0xda68&&offset<=0xda94){plan->Registers[i].Value=words[2+(offset-0xda68)/4];found++;}
    }
    return found==14;
}

/* Historical input-offset diagnostic, retained for direct comparison.
   With a zero matrix this group did not generate nonzero pixel values.
   The separately selected CstBiasDiagnostic is the verified fixed output. */
static inline BOOLEAN CamIspPixelConstant(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) {
    ULONG words[19];
    const LONG matrix[9]={0},subtract[3]={0},add[3]={256,512,768};
    const ULONG low[3]={0},high[3]={1023,1023,1023};
    if(!CamIspCstBuild(words,matrix,subtract,add,low,high)||
       !CamIspPixelReferenceOutput(plan,width,height,1280,720,TRUE))return FALSE;
    for(ULONG i=0;i<plan->Count;i++) {
        ULONG offset=plan->Registers[i].Offset;
        if(offset==0x6d60)plan->Registers[i].Value=words[0];
        else if(offset>=0x6d68&&offset<=0x6dac)plan->Registers[i].Value=words[1+(offset-0x6d68)/4];
    }
    return TRUE;
}

static BOOLEAN CamIspPixelReference(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) {
    return CamIspPixelReferenceProfile(plan,width,height,FALSE);
}

static inline BOOLEAN CamIspPixelBds2Reference(CAM_ISP_PIXEL_PLAN *plan) {
    ULONG demo[20];
    if(!CamIspDemosaicBds2(demo)||!CamIspPixelReferenceOutput(plan,2048,1536,1280,720,TRUE))return FALSE;
    for(ULONG i=0;i<plan->Count;i++) {
        ULONG offset=plan->Registers[i].Offset;
        if(offset==0x78f4)plan->Registers[i].Value=demo[19];
        else if(offset==0x7760)plan->Registers[i].Value=demo[0];
        else if(offset==0x7794)plan->Registers[i].Value=demo[12];
        else if(offset>=0x7768&&offset<=0x7790)plan->Registers[i].Value=demo[1+(offset-0x7768)/4];
        else if(offset>=0x78a0&&offset<=0x78ac)plan->Registers[i].Value=demo[13+(offset-0x78a0)/4];
    }
    return TRUE;
}

static inline BOOLEAN CamIspPixelChromaDiagnostic(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) {
    ULONG words[27];
    if(!CamIspPixelConstant(plan,width,height)||!CamIspFdChromaCropBuild(words))return FALSE;
    for(ULONG i=0;i<plan->Count;i++) {
        ULONG offset=plan->Registers[i].Offset;
        if(offset>=0x7570&&offset<=0x7594)plan->Registers[i].Value=words[17+(offset-0x7570)/4];
    }
    return TRUE;
}

/* The two native CST offset groups are tested independently. No production
   interpretation of add/subtract follows from packer agreement alone. */
static inline BOOLEAN CamIspPixelCstBiasDiagnostic(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) {
    if(!CamIspPixelConstant(plan,width,height))return FALSE;
    for(ULONG i=0;i<plan->Count;i++) {
        ULONG offset=plan->Registers[i].Offset;
        if(offset==0x6d78)plan->Registers[i].Value=256;
        else if(offset==0x6d90)plan->Registers[i].Value=512;
        else if(offset==0x6da8)plan->Registers[i].Value=768;
        else if(offset==0x6d7c||offset==0x6d94||offset==0x6dac)plan->Registers[i].Value=0;
    }
    return TRUE;
}

/* Arithmetic isolation only: real input and all non-Y rows remain native.
   Reusing native Cb in Y tests the Y matrix/output path independently of
   ordinary positive luma. Inverting luma tests signed/clamped input.
   Neither diagnostic is production color or a valid sensor photograph. */
static inline BOOLEAN CamIspPixelLumaDiagnostic(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height,ULONG mode) {
    const LONG matrix[9]={-338,510,-172,-338,510,-172,-427,-83,510};
    LONG selected[9];ULONG words[19];
    LONG bias[3]={512,512,512};const LONG offset[3]={0};
    ULONG low[3]={0};const ULONG high[3]={1023,1023,1023};
    for(ULONG i=0;i<9;i++)selected[i]=matrix[i];
    if(mode>6)return FALSE;
    if(mode==1){selected[0]=-601;selected[1]=-117;selected[2]=-306;}
    else if(mode==2){selected[0]=601;selected[1]=117;selected[2]=306;}
    else if(mode<=4&&mode>=3){selected[0]=mode==3?1024:-1024;selected[1]=selected[2]=0;}
    else if(mode>=5){
        selected[0]=mode==5?601:0;selected[1]=mode==5?117:0;selected[2]=mode==5?306:0;
        low[0]=1023;bias[0]=0;
    }
    if(!CamIspCstBuild(words,selected,bias,offset,low,high)||
       !CamIspPixelReferenceOutput(plan,width,height,1280,720,TRUE))return FALSE;
    for(ULONG i=0;i<plan->Count;i++) {
        ULONG o=plan->Registers[i].Offset;
        if(o>=0x6d68&&o<=0x6d7c)plan->Registers[i].Value=words[1+(o-0x6d68)/4];
    }
    return TRUE;
}
