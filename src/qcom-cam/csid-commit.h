#pragma once

static VOID CamCsidCommit(PUCHAR base,ULONG mask) {
    CamWrite(base,0x18,mask);
    CamWrite(base,0x1c,mask);
    CamWrite(base,0x20,1);
}
