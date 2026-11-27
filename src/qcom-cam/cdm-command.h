#pragma once

#include "cdm-target.h"

#define CAM_CDM_COMMAND_BYTES 24u
#define CAM_CDM_FETCH_BYTES 8u

static VOID CamCdmEncodeFetch(ULONG *words,ULONG marker) {
    words[0]=0x06000001;
    words[1]=marker;
}

static VOID CamCdmEncode(ULONG *words,ULONG marker) {
    words[0]=0x08000000|CAM_CDM_CSID2_BASE;
    words[1]=0x04000001;
    words[2]=0xe10;
    words[3]=0x8011;
    words[4]=0x06000001;
    words[5]=marker;
}

static BOOLEAN CamCdmFetched(PUCHAR base,ULONG marker) {
    return CamRead(base,0x80)==marker&&
           !((CamRead(base,0x6c)|CamRead(base,0x70))&0x01ff01ff)&&
           (CamRead(base,0x44)&0x70002)==2;
}

static BOOLEAN CamCdmCompleted(PUCHAR base,ULONG marker) {
    return CamCdmFetched(base,marker)&&
           ((CamRead(base,0xd0)&0xffffff)==CAM_CDM_CSID2_BASE+0xe10||CamRead(base,0xd0)==0xad2de10)&&
           CamRead(base,0xd4)==0x8011;
}
