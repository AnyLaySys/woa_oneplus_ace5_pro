#pragma once

/* FD CRC13: first coordinate in bits31:16, inclusive last in bits15:0.
   Original module 0x129130 passes {firstPixel,firstLine,lastPixel,lastLine}
   to packer 0xcfd70. This is the opposite order from the CSID crop words. */
static BOOLEAN CamIspFdCropBuild(ULONG words[27],ULONG width,ULONG height,ULONG roundBits) {
    if(!words||width<2||height<2||((width|height)&1)||width>16384||height>16384||roundBits>15)return FALSE;
    RtlZeroMemory(words,27*sizeof(ULONG));
    words[1]=0xe01;words[2]=height-1;words[3]=width-1;
    words[5]=roundBits<<3;words[10]=255;
    words[14]=0x3e01;words[15]=height/2-1;words[16]=width/2-1;
    words[18]=words[20]=roundBits<<3;words[23]=words[24]=255;
    return TRUE;
}

static BOOLEAN CamIspFdCenteredCropBuild(ULONG words[27]) {
    if(!CamIspFdCropBuild(words,1280,720,2))return FALSE;
    /* Inclusive luma 120..839 and chroma 60..419, inside 960/480 rows. */
    words[2]=(120u<<16)|839u;words[15]=(60u<<16)|419u;
    return TRUE;
}

/* Original CRC13 packer 0xcfd70, resource17, native input+0x20=1.
   Isolate this native chroma mode; its live effect is not inferred from
   packet arithmetic or substituted for a real sensor picture. */
static inline BOOLEAN CamIspFdChromaCropBuild(ULONG words[27]) {
    if(!CamIspFdCenteredCropBuild(words))return FALSE;
    words[17]|=0x80;words[19]|=0x80;
    words[23]=words[24]=128;
    return TRUE;
}
