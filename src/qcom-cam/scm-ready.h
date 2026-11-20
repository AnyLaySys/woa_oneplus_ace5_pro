#pragma once
#include "dma-ready.h"

static BOOLEAN CamScmDiagnosticReady(CAM *c) {
    if(!CamDiagnosticDmaReady(c))return FALSE;
    ULONG nrt=CamRead(c->Base[CamCc],0x11310);
    if(!(nrt&1)||(nrt&0x80000000u))return FALSE;
    for(ULONG i=0;i<2;i++) {
        ULONG state=CamRead(c->Base[CamCrm],0x220+i*4);
        if(!(state&0x20000000u)||!(state&0x3fff))return FALSE;
    }
    const ULONG domains[]={0,6,7};
    for(ULONG i=0;i<RTL_NUMBER_OF(domains);i++)
        for(ULONG state=0;state<3;state++)
            if(!(CamRead(c->Base[CamCrm],0x11c+domains[i]*0x20+state*4)&7))return FALSE;
    return TRUE;
}
