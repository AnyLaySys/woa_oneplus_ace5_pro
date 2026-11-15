#include <kxarm64.h>
    LEAF_ENTRY SmPsci, |.text|
    mov x2, #0
    mov x3, #0
    smc #0
    ret
    LEAF_END SmPsci
    END
