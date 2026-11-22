#pragma once
#define DVFS_SAMPLE_100NS 90000

static inline ULONG DvfsCoolingBand(ULONG band,int temperature) {
    if(temperature>=1080)return 2;
    if(temperature<=960)return 0;
    if(band==2 && temperature>1020)return 2;
    return temperature>=990 || band?1u:0u;
}
static inline ULONG DvfsSustainableBand(ULONG band,int temperature) {
    if(temperature>=960 || (band>=2 && temperature>930))return 2;
    if(temperature>=930 || (band>=1 && temperature>900))return 1;
    return 0;
}
static inline LONG DvfsTemperatureAverage(LONG average,LONG temperature) {
    return average==MINLONG?temperature*8:average+temperature-average/8;
}
static inline BOOLEAN DvfsHold(ULONGLONG now,ULONGLONG raised,int pending,ULONGLONG last) {
    return raised && now-raised<900000 && (pending || (last && now-last<=1800000));
}
static inline ULONG DvfsDelayDown(ULONG target,ULONG current,ULONG busy,ULONG ceiling,ULONG *samples) {
    if(target<current && busy>2 && current<=ceiling) {
        if(++*samples<10)return current;
        if(target+1<current)target=current-1;
    }
    *samples=0;return target;
}
