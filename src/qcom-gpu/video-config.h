#pragma once
#include "uapi.h"

static const GPU_U32 GpuVideoLimits[][3]={{1620,40500,10000000},{3600,108000,14000000},{5120,216000,20000000},
    {8192,245760,20000000},{8192,245760,50000000},{8704,522240,50000000},
    {22080,589824,135000000},{36864,983040,240000000}};
static inline GPU_U32 GpuVideoOutputSize(GPU_U32 width,GPU_U32 height) {
    GPU_U32 mbs=((width+31)&~31u)*((height+31)&~31u)/256,bytes=width*height*3;
    bytes=mbs<3680?bytes*2:bytes/4;
    return (bytes*5/4+4095)&~4095u;
}
static inline GPU_U32 GpuVideoMaxBitrate(const GPU_VIDEO_CONFIG *c) {
    if(c->Level<8 || c->Level>15)return 0;
    GPU_U32 value=GpuVideoLimits[c->Level-8][2];if(c->Profile==4)value=value*5/4;
    return value<40000000?value:40000000;
}
static inline int GpuVideoConfigValid(const GPU_VIDEO_CONFIG *c) {
    if(c->Width<128 || c->Height<128 || c->Width>4096 || c->Height>4096 || c->Width*c->Height>4096*2160 ||
       ((c->Width|c->Height)&1) || !c->Rate || c->Rate>60 || c->Bitrate<64000 || c->Reserved ||
       c->Profile>4 || c->Profile==3 || c->Level<8 || c->Level>15 || c->Gop>c->Rate*600)return 0;
    GPU_U32 mbs=((c->Width+15)&~15u)*((c->Height+15)&~15u)/256;
    return mbs<=GpuVideoLimits[c->Level-8][0] && mbs*c->Rate<=GpuVideoLimits[c->Level-8][1] && c->Bitrate<=GpuVideoMaxBitrate(c);
}
