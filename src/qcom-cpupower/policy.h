#pragma once
#define SM_DOMAIN_COUNT 2u
#define SM_LEVEL_COUNT 16u

static const ULONG SmFrequencyKhz[SM_DOMAIN_COUNT][SM_LEVEL_COUNT] = {
    {384000,556800,748800,960000,1152000,1363200,1555200,1785600,
     1996800,2227200,2400000,2745600,2918400,3072000,3321600,3532800},
    {1017600,1209600,1401600,1689600,1958400,2246400,2438400,2649600,
     2841600,3072000,3283200,3513600,3801600,4089600,4204800,4320000}
};
static inline ULONG SmCoolingBand(ULONG band,int temperature) {
    if(temperature>=990)return 2;
    if(band>=2)return temperature<=810?1u:2u;
    if(band==1)return temperature<=780?0u:1u;
    return temperature>=870?1u:0u;
}

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

/* A single busy core can raise its domain. OS requests bound active demand;
 * they never force an idle domain to run at a high operating point. */
static inline ULONG SmDemand(ULONG domain,ULONG current,ULONG busy,ULONG ceiling) {
    ULONG khz,target;
    current=SmClampLevel(current);ceiling=SmClampLevel(ceiling);
    if(busy<=2)return 0;
    if(busy>100)busy=100;
    khz=SmFrequencyKhz[domain][current];
    target=busy>=80?khz*2:khz*busy/70;
    if(target>=khz*7/8 && target<=khz && current<=ceiling)return current;
    return SmChooseLevel(domain,target,SmFrequencyKhz[domain][ceiling]);
}
