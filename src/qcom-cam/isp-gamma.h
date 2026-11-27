#pragma once

/* Apollo980 FD GLut: 0x6b58..0x6b60; three 256-byte DMI selectors at 0x6b08.
   The stock native IQ repeats this curve in all three channel tables. */
typedef struct { ULONG Config[3],Table[64]; } CAM_ISP_GAMMA;

static BOOLEAN CamIspGammaBuild(CAM_ISP_GAMMA *plan,const ULONG values[65],ULONG bank) {
    if(!plan||!values||bank>1)return FALSE;
    for(ULONG i=0;i<65;i++)if(values[i]>1023||(i&&values[i]<values[i-1]))return FALSE;
    CAM_ISP_GAMMA result;
    result.Config[0]=result.Config[1]=bank;result.Config[2]=1;
    for(ULONG i=0;i<64;i++) {
        ULONG delta=values[i+1]-values[i];if(delta>511)delta=511;
        result.Table[i]=values[i]|(delta<<10);
    }
    *plan=result;return TRUE;
}
