#pragma once
#include <windows.h>

static __declspec(noinline) int a830_read_completion(const volatile A830_COMPLETION *page,A830_COMPLETION *out) {
    __try {
        if(!page)return 0;
        MemoryBarrier();out->Submitted=page->Submitted;out->Completed=page->Completed;MemoryBarrier();
        out->Status=page->Status;out->Generation=page->Generation;return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
