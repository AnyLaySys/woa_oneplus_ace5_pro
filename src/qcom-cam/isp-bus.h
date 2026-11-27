#pragma once

#include "isp-output.h"

#define CAM_ISP_FD_DONE 8u
#define CAM_ISP_IPP_FATAL 0x241c6001u

typedef struct { ULONG Offset,Value; } CAM_ISP_BUS_REG;
typedef struct { CAM_ISP_NV12_OUTPUT Output; CAM_ISP_BUS_REG Registers[38]; } CAM_ISP_FD_PLAN;

static inline BOOLEAN CamIspFdPlanPacked(CAM_ISP_FD_PLAN *plan,ULONG width,ULONG height,ULONGLONG address,ULONG capacity,ULONG packer) {
    CAM_ISP_NV12_OUTPUT output;
    if(!plan||(packer!=1&&packer!=3)||!CamIspNv12Output(&output,width,height,address,capacity))return FALSE;
    const ULONG offsets[]={0x0c,0x10,0x14,0x18,0x20,0x24,0x28,0x30,0x34,0x38,0x3c,0x60,0x64,0x68,0x78,0x7c,0x70,0x08,0x04};
    plan->Output=output;
    for(ULONG plane=0;plane<2;plane++) {
        ULONG bytes=output.Stride*(plane?height/2:height);
        const ULONG values[]={output.ImageSize[plane],0,output.Stride,packer,0,0,0,0,1,0,1,
            1,MAXULONG,0,1,11u<<8,output.AddressOffset[plane],bytes>>8,output.ImageAddress[plane]};
        for(ULONG i=0;i<19;i++) {
            plan->Registers[plane*19+i].Offset=0x1400+plane*0x100+offsets[i];
            plan->Registers[plane*19+i].Value=values[i];
        }
    }
    return TRUE;
}

static BOOLEAN CamIspFdPlan(CAM_ISP_FD_PLAN *plan,ULONG width,ULONG height,ULONGLONG address,ULONG capacity) {
    return CamIspFdPlanPacked(plan,width,height,address,capacity,3);
}

static BOOLEAN CamIspFdFrameReady(ULONG busIrq,ULONG ippIrq,ULONGLONG sof,ULONGLONG eof,
    ULONGLONG previous,ULONG yAddress,ULONG cAddress,const CAM_ISP_NV12_OUTPUT *output) {
    return output&&(busIrq&CAM_ISP_FD_DONE)&&!(ippIrq&CAM_ISP_IPP_FATAL)&&
        sof&&eof>sof&&eof>previous&&yAddress==output->ImageAddress[0]&&cAddress==output->ImageAddress[1];
}

/* A polled completion may arrive after the next SOF. Select only a fresh
   native SOF preceding the captured EOF, with no inferred timestamp. */
static inline ULONGLONG CamIspFdFrameSof(ULONGLONG current,ULONGLONG previous,ULONGLONG eof,
    ULONGLONG startSof,ULONGLONG startEof) {
    if(!eof||eof<=startEof)return 0;
    if(current>startSof&&current<eof)return current;
    if(current>eof&&previous>startSof&&previous<eof)return previous;
    return 0;
}
