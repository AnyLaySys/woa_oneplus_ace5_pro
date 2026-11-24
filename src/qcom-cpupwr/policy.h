#pragma once
#include "../qcom-dvfs.h"
#define SM_DOMAIN_COUNT 2u
#define SM_LEVEL_COUNT 16u

static const ULONG SmFrequencyKhz[SM_DOMAIN_COUNT][SM_LEVEL_COUNT] = {
    {384000,556800,748800,960000,1152000,1363200,1555200,1785600,
     1996800,2227200,2400000,2745600,2918400,3072000,3321600,3532800},
    {1017600,1209600,1401600,1689600,1958400,2246400,2438400,2649600,
     2841600,3072000,3283200,3513600,3801600,4089600,4204800,4320000}
};
static inline ULONG SmDomain(ULONG core) { return core < 6 ? 0u : 1u; }
static inline ULONG SmClampLevel(ULONG level) {
    return level < SM_LEVEL_COUNT ? level : SM_LEVEL_COUNT-1;
}
static inline ULONG SmLevelFromState(ULONG state) {
    return SM_LEVEL_COUNT-1-SmClampLevel(state);
}
static inline ULONG SmChooseState(ULONG desired,
                                    ULONG minimum, ULONG maximum) {
    if (minimum >= SM_LEVEL_COUNT || maximum >= SM_LEVEL_COUNT ||
        desired >= SM_LEVEL_COUNT || minimum < maximum) return 0xffffffffu;
    if (desired < maximum) desired = maximum;
    if (desired > minimum) desired = minimum;
    return SmLevelFromState(desired);
}
/* Discrete states are ordered fastest to slowest by the PEP contract. */
static inline ULONG SmChooseLevel(ULONG domain, ULONG desired,
                                    ULONG maximum) {
    ULONG level;
    if (desired > maximum) desired = maximum;
    for (level = 0; level < SM_LEVEL_COUNT-1; ++level)
        if (SmFrequencyKhz[domain][level] >= desired) break;
    while (level > 0 && SmFrequencyKhz[domain][level] > maximum) --level;
    return level;
}

static inline ULONG SmSustainableLevel(ULONG domain,ULONG band) {
    return (domain?13u:14u)-min(band,2u);
}
static inline ULONG SmDemand(ULONG domain,ULONG current,ULONG busy,ULONG ceiling,ULONG sweet,int holdRaised) {
    if(current>ceiling)return ceiling;
    ULONG target=busy<=DVFS_IDLE_PERCENT?0:busy>=63?current+2:
        SmChooseLevel(domain,SmFrequencyKhz[domain][current]*busy/63,SmFrequencyKhz[domain][sweet]);
    if(holdRaised && target<current)target=current;
    return min(target,sweet);
}
