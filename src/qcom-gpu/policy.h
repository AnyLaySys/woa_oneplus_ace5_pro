#pragma once
#ifdef _KERNEL_MODE
typedef unsigned long GPU_POLICY_U32;
#else
#include <stdint.h>
typedef uint32_t GPU_POLICY_U32;
#endif

static inline GPU_POLICY_U32 GpuCoolingBand(GPU_POLICY_U32 band,int temperature) {
    if(temperature>=990)return 2;
    if(band>=2)return temperature<=810?1u:2u;
    if(band==1)return temperature<=780?0u:1u;
    return temperature>=870?1u:0u;
}
static inline GPU_POLICY_U32 GpuCoolingKhz(GPU_POLICY_U32 band) {
    static const GPU_POLICY_U32 limits[]={0,660000,160000};
    return limits[band<3?band:2];
}
static inline GPU_POLICY_U32 GpuChooseLevel(const GPU_POLICY_U32 *khz,GPU_POLICY_U32 maximum,GPU_POLICY_U32 desired) {
    GPU_POLICY_U32 level=1;
    while(level<maximum && khz[level]<desired)++level;
    return level;
}
static inline GPU_POLICY_U32 GpuDemand(const GPU_POLICY_U32 *khz,GPU_POLICY_U32 current,GPU_POLICY_U32 busy,GPU_POLICY_U32 ceiling) {
    GPU_POLICY_U32 target;
    if(busy<=2)return 1;
    if(busy>100)busy=100;
    target=busy>=80?khz[current]*2:khz[current]*busy/70;
    if(target>=khz[current]*7/8 && target<=khz[current] && current<=ceiling)return current;
    return GpuChooseLevel(khz,ceiling,target);
}
static inline GPU_POLICY_U32 GpuDelayDown(GPU_POLICY_U32 target,GPU_POLICY_U32 current,GPU_POLICY_U32 busy,GPU_POLICY_U32 ceiling,GPU_POLICY_U32 *samples) {
    if(target<current && busy>2 && current<=ceiling && ++*samples<2)return current;
    *samples=0;return target;
}
