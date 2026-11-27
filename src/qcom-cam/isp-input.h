#pragma once

#include "isp-route.h"

#define CAM_ISP_INPUT_REGS 25u

typedef struct { ULONG Offset,Value; } CAM_ISP_INPUT_REG;
typedef struct { CAM_ISP_INPUT_REG Registers[CAM_ISP_INPUT_REGS]; } CAM_ISP_RAW10_INPUT;

static BOOLEAN CamIspRaw10Input(CAM_ISP_RAW10_INPUT *plan,ULONG vc,ULONG cid,
    ULONG x,ULONG y,ULONG width,ULONG height) {
    ULONG crop[24];
    if(!plan||vc>31||cid>15||x>0x3fff||y>0x3fff||
       !CamIspCsidCrop(crop,x,y,width,height,TRUE))return FALSE;
    const ULONG offsets[]={0x3f0,0x1800,0x1804,0x180c,0x1810,0x1814,0x1818,0x181c,0x1820,
        0x190c,0x1910,0x1914,0x1918,0x191c,0x1824,0x1828,0x182c,0x1830,
        0x688,0x68c,0x690,0x694,0x638};
    for(ULONG i=0;i<23;i++){plan->Registers[i].Offset=offsets[i];plan->Registers[i].Value=crop[i];}
    plan->Registers[23].Offset=0x60c;plan->Registers[23].Value=0;
    plan->Registers[24].Offset=0x600;
    plan->Registers[24].Value=(vc<<22)|(cid<<27)|(0x2bu<<16)|(2u<<12)|(2u<<8)|(1u<<6)|(1u<<5);
    return TRUE;
}
