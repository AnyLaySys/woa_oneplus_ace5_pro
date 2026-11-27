#pragma once
#if defined(_M_ARM64)
#include <arm64_neon.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

static inline VOID CamNv12Yuy2(const UCHAR *input,UCHAR *output,ULONG width,ULONG height) {
    const UCHAR *uv=input+width*height;
    for(ULONG y=0;y<height;y++) {
        const UCHAR *luma=input+y*width,*chroma=uv+(y/2)*width;UCHAR *row=output+y*width*2;
        for(ULONG x=0;x<width;x+=8) {
#if defined(_M_ARM64)||defined(__aarch64__)
            uint8x8x2_t values={{vld1_u8(luma+x),vld1_u8(chroma+x)}};vst2_u8(row+x*2,values);
#else
            for(ULONG i=0;i<8;i++){row[(x+i)*2]=luma[x+i];row[(x+i)*2+1]=chroma[x+i];}
#endif
        }
    }
}
