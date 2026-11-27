#pragma once

/* Original Apollo980 channel-gain packet: 22 halfwords, 14 register words. */
static BOOLEAN CamIspChannelGainBuild(ULONG words[14],const ULONG data[22]) {
    if(!words||!data||data[0]>1||data[1]>7)return FALSE;
    for(ULONG i=0;i<22;i++)if(data[i]>65535)return FALSE;
    for(ULONG i=6;i<10;i++)if(data[i]>32767||(data[0]&&!data[i]))return FALSE;
    RtlZeroMemory(words,14*sizeof(ULONG));
    words[1]=data[0]|(data[1]<<20);
    words[2]=data[4]|(data[5]<<16);words[3]=data[2]|(data[3]<<16);
    words[4]=data[7]|(data[6]<<16);words[5]=data[8]|(data[9]<<16);
    for(ULONG i=0;i<4;i++){words[6+i]=data[11+i*2];words[10+i]=data[10+i*2];}
    return TRUE;
}

/* Native IQ 0x1fdc90 emits Bayer selection words even for zero black
   levels and unity gains. Zero selectors are not a unity configuration.
   All four layouts are checked against the original calculation, separately
   from the packer. This remains an uncalibrated diagnostic reference. */
static inline BOOLEAN CamIspChannelGainReference(ULONG words[14],ULONG bayer) {
    static const ULONG selectors[4][2]={{0xac,0xc9},{0xca,0x9c},{0x9c,0xca},{0xc9,0xac}};
    if(!words||bayer>3)return FALSE;
    ULONG data[22]={1,0,0,0,0,0,1024,1024,1024,1024};
    data[2]=selectors[bayer][0];data[4]=selectors[bayer][1];
    return CamIspChannelGainBuild(words,data);
}

/* CC131 matrix is row-major Q(7+q); hardware packs columns, not rows. */
static BOOLEAN CamIspCcBuild(ULONG words[11],const LONG matrix[9],const LONG offsets[3],ULONG q) {
    if(!words||!matrix||!offsets||q>3)return FALSE;
    for(ULONG i=0;i<9;i++)if(matrix[i]<-2048||matrix[i]>2047)return FALSE;
    for(ULONG i=0;i<3;i++)if(offsets[i]<-16384||offsets[i]>16383)return FALSE;
    RtlZeroMemory(words,11*sizeof(ULONG));words[0]=1;
    for(ULONG i=0;i<3;i++) {
        words[1+i*2]=((ULONG)matrix[i]&0xfff)|(((ULONG)matrix[i+3]&0xfff)<<16);
        words[2+i*2]=(ULONG)matrix[i+6]&0xfff;
        words[7+i]=(ULONG)offsets[i]&0x7fff;
    }
    words[10]=q;return TRUE;
}

/* Non-HDR ordinary Bayer uses type8, with no grouped/white-pixel LUT. */
static BOOLEAN CamIspDemuxBayerBuild(ULONG words[92]) {
    if(!words)return FALSE;
    RtlZeroMemory(words,92*sizeof(ULONG));words[2]=0x805;
    return TRUE;
}
