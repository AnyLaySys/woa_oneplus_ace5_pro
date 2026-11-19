#pragma once

static ULONG CamCphyLaneMask(ULONG assignment,ULONG count) {
    if(!count||count>3)return 0;
    ULONG mask=0;
    for(ULONG i=0;i<count;i++) {
        ULONG lane=(assignment>>(4*i))&15;
        if(lane>2||(mask&(1u<<(2*lane+1))))return 0;
        mask|=1u<<(2*lane+1);
    }
    return mask;
}
