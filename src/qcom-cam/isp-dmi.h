#pragma once

#include "isp-gamma.h"
#include "isp-gtm.h"

#define CAM_ISP_GTM_CDM_WORDS 271u
#define CAM_ISP_GTM_DMA_BYTES 3328u

/* Original GTM emitter0xe9e60: 263 controls/segments then a2048-byte
   DMI payload at6608/selector1. The IRQ is appended for native completion.
   Retain the command page and original active bank through image teardown. */
static inline BOOLEAN CamIspGtmDmiBuildFormat(ULONG *buffer,ULONG capacity,ULONGLONG address,
    ULONG baseOffset,ULONG marker,const CAM_ISP_GTM *gtm,BOOLEAN dmi64) {
    if(!buffer||!gtm||capacity<CAM_ISP_GTM_DMA_BYTES||!address||(address&255)||
       (dmi64!=FALSE&&dmi64!=TRUE)||
       address>0x100000000ULL-CAM_ISP_GTM_DMA_BYTES||!baseOffset||(baseOffset&4095)||
       baseOffset>0xffffff-0x6a70||marker>255||gtm->Registers[0]>1||
       gtm->Registers[1]!=gtm->Registers[0]||gtm->Registers[2]!=1)return FALSE;
    for(ULONG i=3;i<7;i++)if(gtm->Registers[i]>(i==6?127u:63u))return FALSE;
    for(ULONG i=0;i<256;i++)if(gtm->Table[i]>>49)return FALSE;
    RtlZeroMemory(buffer,CAM_ISP_GTM_DMA_BYTES);
    buffer[0]=0x08000000|baseOffset;buffer[1]=0x03000107;buffer[2]=0x6658;
    for(ULONG i=0;i<263;i++)buffer[3+i]=gtm->Registers[i];
    /* Original PacketBuilder/coreutils + CSL encoder select opcode11 for
       CmdType::CDMDMI64, opcode1 for the native generic CDMDMI buffer.
       Both retain the exact 2048-byte payload and bank/selector geometry. */
    buffer[266]=(dmi64?0x0b000000u:0x01000000u)|0x7ff;buffer[267]=(ULONG)address+1280;buffer[268]=0x01006608;
    buffer[269]=0x06000001;buffer[270]=marker;
    for(ULONG i=0;i<256;i++) {
        buffer[320+i*2]=(ULONG)gtm->Table[i];buffer[321+i*2]=(ULONG)(gtm->Table[i]>>32);
    }
    return TRUE;
}

static inline BOOLEAN CamIspGtmDmiBuild(ULONG *buffer,ULONG capacity,ULONGLONG address,
    ULONG baseOffset,ULONG marker,const CAM_ISP_GTM *gtm) {
    return CamIspGtmDmiBuildFormat(buffer,capacity,address,baseOffset,marker,gtm,FALSE);
}

#define CAM_ISP_GAMMA_CDM_WORDS 19u
#define CAM_ISP_GAMMA_REG_WORDS 10u
#define CAM_ISP_GAMMA_DMA_BYTES 1024u

/* camera-kernel cam_cdm_util.c: CHANGE_BASE8, REG_RANDOM4, DMI1, GEN_IRQ6.
   Source addresses are 32-bit CDM addresses, not shifted36-bit WM addresses.
   The caller must establish/translate the command route, choose the actual
   TFE AHB base offset and retain this whole page until CDM completion/drain.
   This builder neither submits commands nor changes any hardware state. */
static BOOLEAN CamIspGammaCommandBuild(ULONG *buffer,ULONG capacity,ULONGLONG address,
    ULONG baseOffset,ULONG marker,const CAM_ISP_GAMMA *gamma,BOOLEAN lut) {
    if(!buffer||!gamma||capacity<CAM_ISP_GAMMA_DMA_BYTES||!address||(address&255)||
       address>0x100000000ULL-CAM_ISP_GAMMA_DMA_BYTES||!baseOffset||(baseOffset&4095)||
       baseOffset>0xffffff-0x6b60||marker>255||gamma->Config[0]>1||
       gamma->Config[1]!=gamma->Config[0]||gamma->Config[2]!=1||(lut!=FALSE&&lut!=TRUE))return FALSE;
    for(ULONG i=0;i<64;i++)if(gamma->Table[i]&~0x7ffffu)return FALSE;
    RtlZeroMemory(buffer,CAM_ISP_GAMMA_DMA_BYTES);
    buffer[0]=0x08000000|baseOffset;
    buffer[1]=0x04000003;
    for(ULONG i=0;i<3;i++){buffer[2+i*2]=0x6b58+i*4;buffer[3+i*2]=gamma->Config[i];}
    for(ULONG i=0;lut&&i<3;i++) {
        ULONG n=8+i*3;
        buffer[n]=0x010000ff;buffer[n+1]=(ULONG)address+256+i*256;
        buffer[n+2]=((i+1)<<24)|0x6b08;
        for(ULONG j=0;j<64;j++)buffer[64+i*64+j]=gamma->Table[j];
    }
    ULONG irq=lut?17:8;
    buffer[irq]=0x06000001;buffer[irq+1]=marker;
    return TRUE;
}

static BOOLEAN CamIspGammaDmiBuild(ULONG *buffer,ULONG capacity,ULONGLONG address,
    ULONG baseOffset,ULONG marker,const CAM_ISP_GAMMA *gamma) {
    return CamIspGammaCommandBuild(buffer,capacity,address,baseOffset,marker,gamma,TRUE);
}
