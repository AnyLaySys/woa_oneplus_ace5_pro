#pragma once

static BOOLEAN CamControlDmaReady(CAM *c) {
    ULONG clock=CamRead(c->Base[CamCc],0x11300),root=CamRead(c->Base[CamCc],0x112e8);
    ULONG bus=CamRead(c->Base[CamGcc],0x26014),vote=CamRead(c->Base[CamCrm],0x228);
    return (clock&1)&&!(clock&0x80000000u)&&!(root&0x80000000u)&&
        (bus&1)&&!(bus&0x80000000u)&&(vote&0x20000000u)&&(vote&0x3fff)&&
        (CamRead(c->Base[CamCc],0x11350)&0x10000)&&(CamRead(c->Base[CamCc],0x110f0)&0x10000)&&
        (CamRead(c->Base[CamCrm],0x2a8c)&7)&&(CamRead(c->Base[CamCrm],0x32cc)&7)&&
        (CamRead(c->Base[CamCrm],0x34dc)&7);
}

static BOOLEAN CamDiagnosticDmaReady(CAM *c) {
    return c->ReceiverVerified&&CamControlDmaReady(c);
}

static inline BOOLEAN CamIspPixelDmaReady(CAM *c) {
    return CamDiagnosticDmaReady(c)&&
        (CamRead(c->Base[CamCc],0x1112c)&0x80000001u)==1&&
        (CamRead(c->Base[CamCc],0x1113c)&0x80000001u)==1;
}
