#pragma once
#include "isp-gamma.h"

/* Exact shared mode11 native Gamma reference from allfour tuning roots.
   Original IQ/config/DMI packing are tested; production selection is separate. */
static inline BOOLEAN CamIspGammaNativeReference(CAM_ISP_GAMMA *output,ULONG bank) {
    static const ULONG points[65]={
        0,64,108,144,176,205,232,258,282,304,326,347,367,
        386,405,423,441,458,475,491,507,523,538,553,568,583,
        597,611,625,638,651,665,678,690,703,715,728,740,752,
        764,775,787,798,810,821,832,843,854,865,875,886,896,
        907,917,927,937,947,957,967,977,987,996,1006,1015,1023,
    };
    return CamIspGammaBuild(output,points,bank);
}
