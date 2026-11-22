#pragma once

static inline ULONG GpuCoolingKhz(ULONG band) {
    return band==0?0:band==1?832000:160000;
}
static inline ULONG GpuSustainableKhz(ULONG band) {
    return band==0?1050000:band==1?967000:900000;
}
static inline ULONG GpuChooseLevel(const ULONG *khz,ULONG maximum,ULONG desired) {
    ULONG level=1;
    while(level<maximum && khz[level]<desired)++level;
    return level;
}
static inline ULONG GpuResponsiveDemand(const ULONG *khz,
    ULONG current,ULONG busy,ULONG ceiling,ULONG sweet,
    int slowPending,int holdRaised) {
    if(current>ceiling)return ceiling;
    ULONG target=GpuChooseLevel(khz,ceiling,342000);
    if(busy>2 || slowPending) {
        ULONG level=(slowPending || busy>=63)?current+2:
            GpuChooseLevel(khz,sweet,khz[current]*busy/63);
        if(target<level)target=level;
    }
    if(holdRaised && target<current)target=current;
    return target<sweet?target:sweet;
}
