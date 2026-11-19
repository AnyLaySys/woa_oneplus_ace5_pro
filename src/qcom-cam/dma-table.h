#pragma once

#include "dma-address.h"

static BOOLEAN CamDmaBuild(PULONGLONG table,ULONGLONG physical,PPFN_NUMBER pfns,ULONG pages) {
    if(!table||!pfns||!physical||(physical&4095)||!pages||pages>4096)return FALSE;
    for(ULONG i=0;i<pages;i++)if(pfns[i]>(0xfffffffffffULL>>12))return FALSE;
    RtlZeroMemory(table,40960);
    table[0]=(physical+4096)|3;
    for(ULONG i=0;i<pages;i++) {
        ULONG leaf=i/512;
        table[512+((CAM_DMA_IOVA>>21)&511)+leaf]=(physical+(leaf+2)*4096)|3;
        table[(leaf+2)*512+(i&511)]=((ULONGLONG)pfns[i]<<12)|0xe43u;
    }
    return TRUE;
}
