#include "cpupower.h"
VOID SmThermalInitialize(VOID) {
    for(ULONG d=0;d<2;d++) {
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
    ULONGLONG now=KeQueryInterruptTime();
    for(ULONG d=0;d<2;d++) {
        LONG temperature=MINLONG;
        /* Read-only TSENS0/1 CPU and cache sensors; firmware owns setup. */
        if(Sm.Thermal[d])for(ULONG i=1;i<=(d?6u:14u);i++) {
            ULONG raw=READ_REGISTER_ULONG((PULONG)(Sm.Thermal[d]+0xa0+4*i));
            LONG value=(LONG)(raw<<20)>>20;
            if(raw!=MAXULONG && (raw&0x200000u) && value>=-400 && value<=2000)
                temperature=max(temperature,value);
        }
        ULONG previous=(ULONG)Sm.CoolingBand[d];
        ULONG band=temperature==MINLONG?2:SmCoolingBand(previous,temperature);
        /* Only recovery waits for 500 ms; heating and sensor loss act now. */
        if(band<previous) {
            if(!Sm.CoolingSince[d])Sm.CoolingSince[d]=now;
            if(now-Sm.CoolingSince[d]<5000000)band=previous;
            else Sm.CoolingSince[d]=0;
        } else Sm.CoolingSince[d]=0;
        InterlockedExchange(&Sm.CoolingBand[d],(LONG)band);
    }
}
ULONG SmEffectiveCeiling(ULONG domain) {
    if(domain>=2 || Sm.Fault)return 0;
    ULONG band=(ULONG)Sm.CoolingBand[domain];
    ULONG limit=band>=2?0:band==1?(domain?6u:10u):15u;
    return min(limit,(ULONG)Sm.Ceiling[domain]);
}
