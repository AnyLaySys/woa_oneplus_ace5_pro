#pragma once

static ULONG CamIspPhase(ULONG input,ULONG output) {
    ULONG exponent=0;
    while(((ULONGLONG)output<<(exponent+1))<=(ULONGLONG)input)exponent++;
    ULONG shift=23-exponent;
    ULONGLONG scaled=(ULONGLONG)input<<shift;
    ULONG mantissa=(ULONG)(scaled/output),remainder=(ULONG)(scaled%output);
    if(remainder*2>output||(remainder*2==output&&(mantissa&1)))mantissa++;
    ULONG phase=shift>21?(mantissa+(1u<<(shift-22)))>>(shift-21):mantissa<<(21-shift);
    ULONG precision=input<=output*16?3:input<=output*32?2:input<=output*64?1:0;
    return phase|(precision<<30);
}

static BOOLEAN CamIspMndsBuild(ULONG words[20],ULONG width,ULONG height,ULONG outWidth,ULONG outHeight) {
    if(!words||width<2||height<2||width>16384||height>16384||outWidth<2||outHeight<2||
       ((width|height|outWidth|outHeight)&1)||outWidth>width||outHeight>height||
       width>outWidth*64||height>outHeight*64)return FALSE;
    RtlZeroMemory(words,20*sizeof(ULONG));
    for(ULONG plane=0;plane<2;plane++) {
        ULONG *r=words+plane*10;
        r[0]=1;r[1]=0x600;r[2]=((width-1)<<16)|(height-1);
        r[3]=CamIspPhase(width,outWidth>>plane);r[5]=CamIspPhase(height,outHeight>>plane);
    }
    return TRUE;
}

/* Original module 0x171d00, ordinary NV12/full-source rectangle: the four
   supported 4:3 modes scale to 1280x960 before the FD CRC crops to 720 lines.
   Keep the native luma interpolation offsets for ratios between one and two. */
static BOOLEAN CamIspMndsFdBuild(ULONG words[20],ULONG width,ULONG height) {
    if(!((width==4096&&height==3072)||(width==2048&&height==1536)||(width==3264&&height==2448)||
         (width==1600&&height==1200)||(width==2304&&height==1728)))return FALSE;
    if(!CamIspMndsBuild(words,width,height,1280,960))return FALSE;
    if(width<2560) {
        ULONG offset=((2560-width)*16+640)/1280;
        words[9]=offset|(offset<<16);
    }
    return TRUE;
}
