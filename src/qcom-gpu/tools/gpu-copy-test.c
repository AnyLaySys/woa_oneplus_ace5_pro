#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include "../uapi.h"
#include "../wddm/gpu-scanout-builder.h"
static BOOL Io(HANDLE h,DWORD code,void *in,DWORD size,void *out,DWORD length) {
    DWORD bytes;return DeviceIoControl(h,code,in,size,out,length,&bytes,NULL);
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    HANDLE h=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
    GPU_ALLOCATION b[3]={0};int result=1;const unsigned sp=5120,dp=5056,height=2780,guard=4096;
    ULONG sizes[]={sp*height+8192,dp*height+8192,128*1024};
    for(unsigned i=0;i<3;i++)if(!Io(h,GPU_IOCTL_ALLOC,sizes+i,sizeof(ULONG),b+i,sizeof(b[i])))goto done;
    unsigned char *src=(unsigned char *)(uintptr_t)b[0].Map,*dst=(unsigned char *)(uintptr_t)b[1].Map;
    unsigned *cmd=(unsigned *)(uintptr_t)b[2].Map;
    double warm[5]={0};
    for(unsigned test=0;test<7;test++) {
        unsigned width=test==6?1231:1264;
        for(unsigned i=0;i<b[0].Size/4;i++)((unsigned *)src)[i]=0x83000000u^(i*17u)^test;
        memset(dst,0xa5,b[1].Size);
        unsigned words=A830BuildGpuCopy(cmd,b[2].Size/4,b[0].Address+guard,sp*height,sp,
            b[1].Address+guard,dp*height,dp,width,height);
        if(!words)goto done;
        GPU_SUBMIT submit={b[2].Handle,0,words,0};GPU_WAIT wait={0,10000000};MemoryBarrier();
        GPU_POWER_INFO power={0};Io(h,GPU_IOCTL_POWER,NULL,0,&power,sizeof(power));
        unsigned khz=power.Index<GPU_POWER_LEVELS?power.Khz[power.Index]:0;
        LARGE_INTEGER begin,end,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&begin);
        printf("{\"case\":%u,\"width\":%u,\"height\":%u,\"source\":\"%llx\",\"destination\":\"%llx\",\"command_dwords\":%u}\n",
            test,width,height,b[0].Address+guard,b[1].Address+guard,words);
        if(!Io(h,GPU_IOCTL_SUBMIT,&submit,sizeof(submit),&wait.Fence,sizeof(wait.Fence)) ||
           !Io(h,GPU_IOCTL_WAIT,&wait,sizeof(wait),NULL,0))goto done;
        QueryPerformanceCounter(&end);
        MemoryBarrier();
        for(unsigned y=0;y<height;y++) {
            const unsigned *s=(unsigned *)(src+guard+(size_t)y*sp);
            const unsigned *d=(unsigned *)(dst+guard+(size_t)y*dp);
            for(unsigned x=0;x<width;x++)if(s[x]!=d[x]) {
                printf("{\"mismatch_y\":%u,\"x\":%u,\"expected\":\"%08x\",\"actual\":\"%08x\"}\n",y,x,s[x],d[x]);goto done;
            }
            for(unsigned x=width*4;x<dp;x++)if(dst[guard+(size_t)y*dp+x]!=0xa5){puts("{\"padding\":\"CORRUPT\"}");goto done;}
        }
        for(unsigned i=0;i<guard;i++)if(dst[i]!=0xa5 || dst[guard+(size_t)dp*height+i]!=0xa5){puts("{\"guard\":\"CORRUPT\"}");goto done;}
        for(unsigned i=0;i<b[0].Size/4;i++)if(((unsigned *)src)[i]!=(0x83000000u^(i*17u)^test)){puts("{\"source\":\"CORRUPT\"}");goto done;}
        double ms=(double)(end.QuadPart-begin.QuadPart)*1000.0/(double)frequency.QuadPart;
        if(test>=1 && test<=5)warm[test-1]=ms;
        printf("{\"case\":%u,\"fence\":%llu,\"submit_to_complete_ms\":%.3f,\"voted_khz_before\":%u,\"result\":\"PASS\"}\n",
            test,wait.Fence,ms,khz);
    }
    for(unsigned i=0;i<5;i++)for(unsigned j=i+1;j<5;j++)if(warm[j]<warm[i]){double t=warm[j];warm[j]=warm[i];warm[i]=t;}
    printf("{\"full_frame_repeats\":5,\"median_ms\":%.3f,\"minimum_ms\":%.3f,\"maximum_ms\":%.3f}\n",warm[2],warm[0],warm[4]);
    result=0;
done:
    if(result)printf("{\"gpu_copy\":\"FAIL\",\"error\":%lu}\n",GetLastError());
    else puts("{\"gpu_copy\":\"PASS\",\"framebuffer_mapped\":false}");
    for(unsigned i=0;i<3;i++)if(b[i].Handle)Io(h,GPU_IOCTL_FREE,&b[i].Handle,sizeof(b[i].Handle),NULL,0);
    CloseHandle(h);return result;
}
