#pragma once

static BOOLEAN CamIspCsidCrop(ULONG words[24],ULONG x,ULONG y,ULONG width,ULONG height,BOOLEAN measure) {
    if(!words||width<2||height<2||((x|y|width|height)&1)||width>65535||height>65535||
       x>65536-width||y>65536-height)return FALSE;
    RtlZeroMemory(words,24*sizeof(ULONG));
    words[1]=0x3000;words[2]=9;
    words[3]=x|((x+width-1)<<16);words[4]=y|((y+height-1)<<16);
    /* IPP0 uses context 0 at both MC_CFG0 (word 0) and IPP_CFG2 (word 22). */
    words[10]=words[11]=words[18]=1;
    if(measure){words[20]=0x1f;words[21]=(height<<16)|width;}
    return TRUE;
}
