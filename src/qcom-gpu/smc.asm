#include <kxarm64.h>
    NESTED_ENTRY GpuSmc, |.text|
    PROLOG_SAVE_REG_PAIR x19, x20, #-32!
    PROLOG_SAVE_REG_PAIR fp, lr, #16
    mov x19, x0
    ldp x0, x1, [x19]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
    smc #0
    stp x0, x1, [x19]
    stp x2, x3, [x19, #16]
    stp x4, x5, [x19, #32]
    stp x6, x7, [x19, #48]
    EPILOG_RESTORE_REG_PAIR fp, lr, #16
    EPILOG_RESTORE_REG_PAIR x19, x20, #32!
    EPILOG_RETURN
    NESTED_END GpuSmc
    END
