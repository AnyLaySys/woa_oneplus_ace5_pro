#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
inline int NpuCountTrailingZeros(unsigned int value) {
    unsigned long index = 0;
    return _BitScanForward(&index, value) ? static_cast<int>(index) : 32;
}
#endif
