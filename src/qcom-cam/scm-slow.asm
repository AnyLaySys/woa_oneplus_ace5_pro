#include <kxarm64.h>
    NESTED_ENTRY CamScmSlow, |.text|
    PROLOG_SAVE_REG_PAIR x19, x20, #-48!
    PROLOG_SAVE_REG_PAIR fp, lr, #16
    str x18, [sp, #32]
    mov x19, x0
    mov x20, #64
    ldp x0, x1, [x19]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldp x6, x7, [x19, #48]
CamScmSlowResume
    smc #0
    cmp w0, #1
    b.ne CamScmSlowDone
    subs x20, x20, #1
    b.eq CamScmSlowDone
    ldr x1, [x19, #8]
    ldp x2, x3, [x19, #16]
    ldp x4, x5, [x19, #32]
    ldr x7, [x19, #56]
    ldr x18, [sp, #32]
    b CamScmSlowResume
CamScmSlowDone
    stp x0, x1, [x19]
    stp x2, x3, [x19, #16]
    stp x4, x5, [x19, #32]
    stp x6, x7, [x19, #48]
    ldr x18, [sp, #32]
    EPILOG_RESTORE_REG_PAIR fp, lr, #16
    EPILOG_RESTORE_REG_PAIR x19, x20, #48!
    EPILOG_RETURN
    NESTED_END CamScmSlow
    NESTED_ENTRY CamScmOnce, |.text|
    PROLOG_SAVE_REG_PAIR x19, x20, #-48!
    PROLOG_SAVE_REG_PAIR fp, lr, #16
    str x18, [sp, #32]
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
    ldr x18, [sp, #32]
    EPILOG_RESTORE_REG_PAIR fp, lr, #16
    EPILOG_RESTORE_REG_PAIR x19, x20, #48!
    EPILOG_RETURN
    NESTED_END CamScmOnce
    END
