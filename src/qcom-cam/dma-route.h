#pragma once

static BOOLEAN CamDmaIdentity(ULONG control,ULONG cbar,ULONG sctlr) {
    ULONG type=(control>>16)&3;
    return type==1||(type==0&&((cbar>>16)&3)==1&&!(sctlr&1));
}
