#pragma once
#define SIA_VOLUME_MIN (-96L*65536)
#define SIA_VOLUME_STEP 32768L
#define SIA_GAIN_ONE 1073741824L
static __inline LONG SiaGain(LONG level) {
    LONGLONG gain=SIA_GAIN_ONE;
    for(LONG i=0;i<-level/SIA_VOLUME_STEP;i++)gain=(gain*2027355295LL+1073741824LL)>>31;
    return (LONG)gain;
}
static __inline VOID SiaScale(UCHAR *data,ULONG bytes,const LONG gains[2]) {
    for(ULONG i=0;i+3<=bytes;i+=3) {
        LONG sample=(LONG)(((ULONG)data[i]|((ULONG)data[i+1]<<8)|((ULONG)data[i+2]<<16))<<8)>>8;
        sample=(LONG)(((LONGLONG)sample*gains[(i/3)&1])/SIA_GAIN_ONE);
        data[i]=(UCHAR)sample;data[i+1]=(UCHAR)(sample>>8);data[i+2]=(UCHAR)(sample>>16);
    }
}
