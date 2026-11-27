#pragma once
#include "isp-gtm.h"

/* Original TFE IQ21bc00 mode11 no-histogram reference: all257 tuning
   gains1024, reserve2/5/0, native input90800/10. Complete native packet
   SHA256ae313c20e2d0e704a8e96052353df5e1fbf366e6be2bd19be8c319b55b729c56.
   Segment boundaries below are native output, not generated substitutes.
   This helper does not select production tuning or load any hardware LUT. */
static inline BOOLEAN CamIspGtmNativeReference(CAM_ISP_GTM *output,ULONG bank) {
    static const ULONG segments[256]={
        0,0,0,0,0,1,1,1,
        2,2,2,3,4,4,5,5,
        6,7,8,9,10,10,11,12,
        13,15,16,17,18,19,20,22,
        23,24,26,27,29,30,31,33,
        35,36,38,39,41,43,45,46,
        48,50,52,54,56,58,60,62,
        64,66,68,70,72,75,77,79,
        82,84,86,89,91,94,96,99,
        102,104,107,110,112,115,118,121,
        124,127,130,133,136,139,143,146,
        149,152,156,159,163,166,170,174,
        177,181,185,189,193,197,201,205,
        209,213,218,222,226,231,236,240,
        245,250,255,260,265,270,275,280,
        285,291,296,302,308,314,319,325,
        331,338,344,350,357,363,370,377,
        384,391,398,406,413,421,428,436,
        444,452,461,469,478,486,495,504,
        513,523,532,542,552,562,572,583,
        593,604,615,627,638,650,662,674,
        686,699,712,725,739,752,766,781,
        795,810,825,840,856,872,889,905,
        922,940,958,976,994,1013,1033,1052,
        1073,1093,1114,1136,1158,1180,1203,1227,
        1251,1275,1300,1326,1352,1379,1406,1434,
        1463,1492,1522,1553,1585,1617,1650,1683,
        1718,1753,1789,1826,1864,1903,1942,1983,
        2024,2067,2111,2156,2201,2248,2297,2346,
        2396,2448,2502,2556,2612,2669,2728,2788,
        2850,2914,2979,3046,3114,3185,3257,3331,
        3408,3486,3566,3649,3733,3820,3910,4002,
    };
    if(!output||bank>1)return FALSE;
    output->Registers[0]=output->Registers[1]=bank;output->Registers[2]=1;
    output->Registers[3]=16;output->Registers[4]=32;output->Registers[5]=16;output->Registers[6]=0;
    for(ULONG i=0;i<256;i++) {
        output->Registers[7+i]=segments[i];
        output->Table[i]=1024ULL|(30ULL<<44);
    }
    return TRUE;
}
