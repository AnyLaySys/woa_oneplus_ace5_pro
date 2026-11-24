#include "cpupower.h"
VOID SmThermalInitialize(VOID) {
    for(ULONG d=0;d<2;d++) {
        Sm.TemperatureAverage[d]=MINLONG;
        Sm.Temperature[d]=MINLONG;
        PHYSICAL_ADDRESS p;p.QuadPart=0x0c228000ULL+d*0x1000;
        Sm.Thermal[d]=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READONLY|PAGE_NOCACHE);
    }
}
VOID SmThermalClose(VOID) {
    for(ULONG d=0;d<2;d++)if(Sm.Thermal[d]) {
        MmUnmapIoSpace(Sm.Thermal[d],PAGE_SIZE);Sm.Thermal[d]=NULL;
    }
}
VOID SmThermalUpdate(VOID) {
    for(ULONG d=0;d<2;d++) {
        LONG temperature=MINLONG;
        /* Read-only TSENS0/1 CPU and cache sensors; firmware owns setup. */
        if(Sm.Thermal[d])for(ULONG i=1;i<=(d?6u:14u);i++) {
            ULONG raw=READ_REGISTER_ULONG((PULONG)(Sm.Thermal[d]+0xa0+4*i));
            LONG value=(LONG)(raw<<20)>>20;
            if(raw!=MAXULONG && (raw&0x200000u) && value>=-400 && value<=2000)
                temperature=max(temperature,value);
        }
        InterlockedExchange(&Sm.Temperature[d],temperature);
        if(temperature==MINLONG){InterlockedExchange(&Sm.TemperatureAverage[d],MINLONG);continue;}
        InterlockedExchange(&Sm.CoolingBand[d],(LONG)DvfsCoolingBand((ULONG)Sm.CoolingBand[d],temperature));
        LONG average=DvfsTemperatureAverage(Sm.TemperatureAverage[d],temperature);
        InterlockedExchange(&Sm.TemperatureAverage[d],average);
        InterlockedExchange(&Sm.SweetBand[d],(LONG)DvfsSustainableBand((ULONG)Sm.SweetBand[d],average/8));
    }
}
ULONG SmEffectiveCeiling(ULONG domain) {
    if(domain>=2 || Sm.Fault || Sm.TemperatureAverage[domain]==MINLONG)return 0;
    ULONG band=(ULONG)Sm.CoolingBand[domain];
    ULONG limit=band>=2?0:band==1?(domain?10u:11u):SM_LEVEL_COUNT-1;
    return min(limit,(ULONG)Sm.Ceiling[domain]);
}
