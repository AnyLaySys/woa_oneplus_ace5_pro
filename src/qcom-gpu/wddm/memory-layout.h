#pragma once
#include "../uapi.h"

#define A830_SEGMENT_BYTES (64u*1024*1024)
#define A830_COMMAND_BYTES 0x10000u
#define A830_SHARED_ADDRESS GPU_CLIENT_VA_END

static inline GPU_U64 A830SharedBudget(GPU_U64 systemBytes,GPU_U64 dedicatedBytes) {
    GPU_U64 eighty=(systemBytes/5)*4+(systemBytes%5)*4/5;
    GPU_U64 half=systemBytes/2;
    GPU_U64 reserve=systemBytes>0x400000000ULL?systemBytes-0x400000000ULL:0;
    GPU_U64 available=reserve>half?reserve:half;
    if(available>eighty)available=eighty;
    if(available<=dedicatedBytes)return 0;
    available-=dedicatedBytes;
    GPU_U64 addressBytes=GPU_VA_END-A830_SHARED_ADDRESS;
    if(available>addressBytes)available=addressBytes;
    return available&~0xfffULL;
}
