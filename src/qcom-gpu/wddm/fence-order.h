#pragma once
#include "../uapi.h"

/* WDDM fence IDs are 32-bit serial numbers, not the GPU ring's private
   timestamps (which also include firmware scanout-copy submissions). */
static inline int A830FenceAfter(GPU_U32 candidate,GPU_U32 previous) {
    return candidate!=previous && (GPU_U32)(candidate-previous)<0x80000000u;
}
static inline GPU_U32 A830FenceLatest(GPU_U32 first,GPU_U32 second) {
    return A830FenceAfter(second,first)?second:first;
}
