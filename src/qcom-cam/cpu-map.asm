#include <kxarm64.h>
    LEAF_ENTRY CamCpuTranslate, |.text|
    mrs x2, DAIF
    msr DAIFSet, #2
    mrs x1, PAR_EL1
    at S1E1R, x0
    isb
    mrs x0, PAR_EL1
    msr PAR_EL1, x1
    msr DAIF, x2
    ret
    LEAF_END CamCpuTranslate
    LEAF_ENTRY CamDmaInvalidate, |.text|
    dsb sy
    mrs x8, CTR_EL0
    ubfx x8, x8, #16, #4
    mov x9, #4
    lsl x8, x9, x8
DmaRow
    add x5, x0, x1
    add x6, x0, x4
DmaBlock
    add x7, x6, #256
    mov x9, x6
DmaLine
    dc ivac, x9
    add x9, x9, x8
    cmp x9, x7
    b.lo DmaLine
    add x6, x6, #512
    cmp x6, x5
    b.lo DmaBlock
    add x0, x0, x2
    subs w3, w3, #1
    b.ne DmaRow
    dsb sy
    ret
    LEAF_END CamDmaInvalidate
    END
