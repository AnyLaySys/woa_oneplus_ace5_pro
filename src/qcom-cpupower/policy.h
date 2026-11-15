#pragma once
#if defined(_KERNEL_MODE)
typedef unsigned int uint32_t;
#else
#include <stdint.h>
#endif

#define SM_DOMAIN_COUNT 2u
#define SM_LEVEL_COUNT 16u

static const uint32_t SmFrequencyKhz[SM_DOMAIN_COUNT][SM_LEVEL_COUNT] = {
    {384000,556800,748800,960000,1152000,1363200,1555200,1785600,
     1996800,2227200,2400000,2745600,2918400,3072000,3321600,3532800},
    {1017600,1209600,1401600,1689600,1958400,2246400,2438400,2649600,
     2841600,3072000,3283200,3513600,3801600,4089600,4204800,4320000}
};


static inline uint32_t SmDomain(uint32_t core) { return core < 6 ? 0u : 1u; }
static inline uint32_t SmClampLevel(uint32_t level) {
    return level < SM_LEVEL_COUNT ? level : SM_LEVEL_COUNT-1;
}
static inline uint32_t SmLevelFromState(uint32_t state) {
    return SM_LEVEL_COUNT-1-SmClampLevel(state);
}
static inline uint32_t SmChooseState(uint32_t desired,
                                    uint32_t minimum, uint32_t maximum) {
    if (minimum >= SM_LEVEL_COUNT || maximum >= SM_LEVEL_COUNT ||
        desired >= SM_LEVEL_COUNT || minimum < maximum) return 0xffffffffu;
    if (desired < maximum) desired = maximum;
    if (desired > minimum) desired = minimum;
    return SmLevelFromState(desired);
}
/* Discrete states are ordered fastest to slowest by the PEP contract. */
static inline uint32_t SmChooseLevel(uint32_t domain, uint32_t desired,
                                    uint32_t maximum) {
    uint32_t level;
    if (desired > maximum) desired = maximum;
    for (level = 0; level < SM_LEVEL_COUNT-1; ++level)
        if (SmFrequencyKhz[domain][level] >= desired) break;
    while (level > 0 && SmFrequencyKhz[domain][level] > maximum) --level;
    return level;
}

/* A single busy core can raise its domain. OS requests bound active demand;
 * they never force an idle domain to run at a high operating point. */
static inline uint32_t SmDemand(uint32_t domain,uint32_t current,uint32_t busy,uint32_t ceiling) {
    uint32_t khz,target;
    current=SmClampLevel(current);ceiling=SmClampLevel(ceiling);
    if(busy<=2)return 0;
    if(busy>100)busy=100;
    khz=SmFrequencyKhz[domain][current];
    target=busy>=80?khz*2:khz*busy/70;
    if(target>=khz*7/8 && target<=khz && current<=ceiling)return current;
    return SmChooseLevel(domain,target,SmFrequencyKhz[domain][ceiling]);
}
