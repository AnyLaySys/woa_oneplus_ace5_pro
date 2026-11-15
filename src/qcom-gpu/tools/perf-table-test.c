#include "../gpu.h"
#include "../perf-table.h"
#include <stdio.h>
int main(void) {
    static const ULONG fuses[]={0,252,242,232,221,190},maxima[]={1200000,1200000,1150000,1100000,1050000,900000};
    for(ULONG i=0;i<RTL_NUMBER_OF(fuses);i++) {
        ULONG count=1,previous=0,bandwidth=0,mask=GpuSpeedMask(fuses[i]);
        if(!mask)return 1;
        for(ULONG j=0;j<RTL_NUMBER_OF(GpuOpps);j++)if(GpuOpps[j].Mask&mask) {
            const GPU_OPP *p=GpuOpps+j;
            if(p->Khz<=previous || p->Bandwidth<bandwidth || count>=GPU_PERF_LEVELS)return 1;
            previous=p->Khz;bandwidth=p->Bandwidth;count++;
        }
        if(previous!=maxima[i] || 5+count*3+6>64 || 32+(count-1)*8+2>=160)return 1;
    }
    if(GpuSpeedMask(511) || GpuSpeedMask(1))return 1;
    puts("{\"speed_bin_filter\":\"PASS\",\"opp_order\":\"PASS\",\"hfi_packet_bounds\":\"PASS\"}");return 0;
}
