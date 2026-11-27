#pragma once

/* Original Common10 packer 0xccce0. Flags retain the native byte order;
   do not infer a pixel mode from arbitrary sensor metadata. */
static BOOLEAN CamIspCommonBuild(ULONG words[13],const ULONG throttles[6],
    const ULONG flags[5],const ULONG pdaf[2],BOOLEAN hdr) {
    if(!words||!throttles||!flags||!pdaf||(hdr!=FALSE&&hdr!=TRUE))return FALSE;
    for(ULONG i=0;i<6;i++)if(throttles[i]>(i&1?65535u:1u))return FALSE;
    for(ULONG i=0;i<5;i++)if(flags[i]>(i==4?3u:1u))return FALSE;
    RtlZeroMemory(words,13*sizeof(ULONG));
    words[0]=throttles[4]|(throttles[5]<<4);
    words[7]=words[8]=throttles[2]|(throttles[3]<<4);
    words[9]=throttles[0]|(throttles[1]<<4);
    words[10]=(ULONG)hdr|(flags[0]<<8)|(flags[1]<<2)|(flags[2]<<4)|(flags[3]<<6);
    words[11]=flags[4];words[12]=((pdaf[0]|pdaf[1])==0)|((pdaf[1]==0)<<4);
    return TRUE;
}
