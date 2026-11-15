#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wddm/gpu-scanout-builder.h"
int main(void) {
    const unsigned width=1264,height=2780,sp=5120,dp=5056;
    const unsigned long long src=0x300005000ULL,dst=0xb0000000ULL;
    unsigned *commands=malloc(128*1024);assert(commands);
    unsigned char *s=malloc((size_t)sp*height),*d=malloc((size_t)dp*height+4096);assert(s && d);
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<sp/4;x++)
        ((unsigned *)(s+(size_t)y*sp))[x]=0x83000000u^(y<<12)^x;
    memset(d,0xa5,(size_t)dp*height+4096);
    unsigned words=A830BuildGpuCopy(commands,32768,src,(unsigned long long)sp*height,sp,
        dst,(unsigned long long)dp*height,dp,width,height);
    assert(words==6*height+8 && words*4<128*1024);
    unsigned copied=0;
    for(unsigned at=0;at<words;) {
        unsigned header=commands[at++],op=(header>>16)&0x7f,n=header&0x3fff;
        assert((header>>28)==7 && at+n<=words);
        if(op==0x75) {
            assert(n==5);unsigned count=commands[at];
            uint64_t a=commands[at+1]|((uint64_t)commands[at+2]<<32);
            uint64_t b=commands[at+3]|((uint64_t)commands[at+4]<<32);
            assert(a>=src && a+count*4<=src+(uint64_t)sp*height);
            assert(b>=dst && b+count*4<=dst+(uint64_t)dp*height);
            memcpy(d+(size_t)(b-dst),s+(size_t)(a-src),count*4);copied++;
        } else assert(op==0x17 || op==0x26 || op==0x12 || op==0x10);
        at+=n;
    }
    assert(copied==height);
    for(unsigned y=0;y<height;y++)assert(!memcmp(s+(size_t)y*sp,d+(size_t)y*dp,width*4));
    for(unsigned i=0;i<4096;i++)assert(d[(size_t)dp*height+i]==0xa5);
    /* Padded destinations must retain every untouched byte. */
    const unsigned padded=5200;
    unsigned char small[4*5200];memset(small,0xa5,sizeof(small));
    words=A830BuildGpuCopy(commands,32768,src,sp*4,sp,dst,sizeof(small),padded,width,4);
    assert(words);
    for(unsigned at=0;at<words;) {
        unsigned header=commands[at++],op=(header>>16)&0x7f,n=header&0x3fff;
        if(op==0x75) {
            uint64_t a=commands[at+1]|((uint64_t)commands[at+2]<<32);
            uint64_t b=commands[at+3]|((uint64_t)commands[at+4]<<32);
            assert(b-dst+commands[at]*4<=sizeof(small));
            memcpy(small+(size_t)(b-dst),s+(size_t)(a-src),commands[at]*4);
        }
        at+=n;
    }
    for(unsigned y=0;y<4;y++) {
        assert(!memcmp(small+y*padded,s+y*sp,width*4));
        for(unsigned x=width*4;x<padded;x++)assert(small[y*padded+x]==0xa5);
    }
    assert(!A830BuildGpuCopy(commands,32768,src,sp*height,sp,dst,dp*height-1,dp,width,height));
    assert(!A830BuildGpuCopy(commands,100,src,sp*height,sp,dst,dp*height,dp,width,height));
    assert(!A830BuildGpuCopy(commands,32768,src,sp*height,sp,src+4,dp*height,dp,width,height));
    assert(!A830BuildGpuCopy(commands,32768,src,sp*height,sp-1,dst,dp*height,dp,width,height));
    assert(!A830BuildGpuCopy(commands,32768,UINT64_MAX-12,512,64,dst,512,64,16,2));
    free(commands);free(s);free(d);
    puts("GPU scanout PM4: stride5120 to5056, 2780 rows, >4GB source, guard/padding/bounds PASS");return 0;
}
