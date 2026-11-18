#pragma once

static inline ULONG GpuCoolingBand(ULONG band,int temperature) {
    if(temperature>=990)return 2;
    if(band>=2)return temperature<=810?1u:2u;
    if(band==1)return temperature<=780?0u:1u;
    return temperature>=870?1u:0u;
}
static inline ULONG GpuCoolingKhz(ULONG band) {
    return band==0?0:band==1?734000:160000;
}
static inline ULONG GpuChooseLevel(const ULONG *khz,ULONG maximum,ULONG desired) {
    ULONG level=1;
    while(level<maximum && khz[level]<desired)++level;
    return level;
}
static inline ULONG GpuDelayDown(ULONG target,ULONG current,ULONG busy,ULONG ceiling,ULONG *samples) {
    if(target<current && busy>2 && current<=ceiling && ++*samples<2)return current;
    *samples=0;return target;
}
/* Thermal/admin ceilings override the 342 MHz floor and pending-work boost. */
static inline ULONG GpuResponsiveDemand(const ULONG *khz,
    ULONG current,ULONG busy,ULONG ceiling,
    int slowPending,int holdRaised) {
    if(current>ceiling)return ceiling;
    ULONG target=GpuChooseLevel(khz,ceiling,342000);
    if(busy>2 || slowPending) {
        ULONG demand=(slowPending || busy>=80)?khz[current]*2:khz[current]*busy/70;
        ULONG level=!slowPending && demand>=khz[current]*7/8 && demand<=khz[current]?
            current:GpuChooseLevel(khz,ceiling,demand);
        if(target<level)target=level;
    }
    if(holdRaised && target<current)target=current;
    return target;
}
