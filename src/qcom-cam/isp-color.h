#pragma once

/* WB141 packs all six signed24 offsets; the emitter reads eleven words. */
static BOOLEAN CamIspWbBuild(ULONG words[11],ULONG red,ULONG green,ULONG blue,const LONG offsets[6]) {
    if(!words||!offsets||!red||!green||!blue||red>32767||green>32767||blue>32767)return FALSE;
    for(ULONG i=0;i<6;i++)if(offsets[i]<-8388608||offsets[i]>8388607)return FALSE;
    RtlZeroMemory(words,11*sizeof(ULONG));words[1]=1;
    words[2]=green;words[3]=blue;words[4]=red;
    for(ULONG i=0;i<6;i++)words[5+i]=(ULONG)offsets[i]&0xffffff;
    return TRUE;
}

/* Native field groups are output bias, then input offset. Real zero-matrix
   diagnostic verified bias256/512/768 becomes NV12 64/128/192 after CRC.
   Earlier add/subtract names reversed the two hardware field groups. */
static BOOLEAN CamIspCstBuild(ULONG words[19],const LONG matrix[9],const LONG outputBias[3],
    const LONG inputOffset[3],const ULONG low[3],const ULONG high[3]) {
    if(!words||!matrix||!outputBias||!inputOffset||!low||!high)return FALSE;
    for(ULONG i=0;i<9;i++)if(matrix[i]<-4096||matrix[i]>4095)return FALSE;
    for(ULONG i=0;i<3;i++)if(outputBias[i]<-8192||outputBias[i]>8191||inputOffset[i]<-8192||inputOffset[i]>8191||
        low[i]>high[i]||high[i]>32767)return FALSE;
    RtlZeroMemory(words,19*sizeof(ULONG));words[0]=1;
    for(ULONG i=0;i<3;i++) {
        ULONG *r=words+1+i*6;
        r[0]=((ULONG)matrix[i*3]&0x1fff)|(((ULONG)matrix[i*3+1]&0x1fff)<<16);
        r[1]=(ULONG)matrix[i*3+2]&0x1fff;r[2]=low[i];r[3]=high[i];
        r[4]=(ULONG)outputBias[i]&0x3fff;r[5]=(ULONG)inputOffset[i]&0x3fff;
    }
    return TRUE;
}
