#pragma once

/* Verify one exact, unique SID route without rejecting unrelated clients of
   the same TBU. Masked or duplicate routes matching this SID are conflicts. */
static BOOLEAN CamSmmuExactRoute(PUCHAR base,ULONG sid,ULONG slot,ULONG control) {
    if(!base||sid>0x7fff||slot>=128)return FALSE;
    BOOLEAN found=FALSE;
    for(ULONG i=0;i<128;i++) {
        ULONG smr=CamRead(base,0x800+i*4),mask=(smr>>16)&0x7fff;
        if(!(smr&0x80000000u)||((smr^sid)&~mask&0x7fff))continue;
        if(found||i!=slot||smr!=(0x80000000u|sid)||CamRead(base,0xc00+i*4)!=control)return FALSE;
        found=TRUE;
    }
    return found;
}
