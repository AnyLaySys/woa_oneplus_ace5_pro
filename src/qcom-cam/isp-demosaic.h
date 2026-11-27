#pragma once

static BOOLEAN CamIspDemosaicNative(ULONG words[20]) {
    if(!words)return FALSE;
    RtlZeroMemory(words,20*sizeof(ULONG));
    words[0]=1;words[12]=0x100;words[13]=0x4001;
    words[14]=0x80;words[15]=0x80;words[16]=0x66;words[17]=1;
    return TRUE;
}

/* Explicit main-camera 4096x3072 -> 2048x1536 BDS2 request. Complete
   native 1d24b0 calculation and c3b60/c3480 packing/emission, including
   interpolation geometry, are oracle checked. Not a one-bit mode guess. */
static inline BOOLEAN CamIspDemosaicBds2(ULONG words[20]) {
    if(!words)return FALSE;
    static const ULONG native[20]={4097,2048,1536,7603216,0,0,0,0,0,0,0,0,16,0,0,0,0,1,0,0};
    for(ULONG i=0;i<20;i++)words[i]=native[i];
    return TRUE;
}
