#include <windows.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include "../uapi.h"

static GUID guid={0x19c45a12,0xc057,0x4903,{0xa8,0x30,0x61,0x19,0x45,0x61,0x20,0x26}};
static int Io(HANDLE h,DWORD code,void *in,DWORD size,void *out,DWORD length) {
    DWORD bytes;return DeviceIoControl(h,code,in,size,out,length,&bytes,NULL);
}
static int Rejected(HANDLE h,DWORD code,GPU_SUBMIT *entries,DWORD size) {
    GPU_INFO before,after;ULONGLONG fence;
    if(!Io(h,GPU_IOCTL_QUERY,NULL,0,&before,sizeof(before)))return 0;
    if(Io(h,code,entries,size,&fence,sizeof(fence)) || GetLastError()!=ERROR_INVALID_PARAMETER)return 0;
    return Io(h,GPU_IOCTL_QUERY,NULL,0,&after,sizeof(after)) && after.Submitted==before.Submitted && !after.Status;
}
int main(int argc,char **argv) {
    int cached=argc>1 && !strcmp(argv[1],"cached");
    WCHAR path[512];GPU_ALLOCATION a={0};ULONG size=cached?0x4000:4096,target=cached?2080:32;int result=1;
    if(CM_Get_Device_Interface_ListW(&guid,NULL,path,512,CM_GET_DEVICE_INTERFACE_LIST_PRESENT) || !path[0])
        memcpy(path,L"\\\\.\\A830Gpu",sizeof(L"\\\\.\\A830Gpu"));
    HANDLE h=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE)return 1;
    if(!Io(h,cached?GPU_IOCTL_ALLOC_CACHED:GPU_IOCTL_ALLOC,&size,sizeof(size),&a,sizeof(a)))goto done;
    GPU_CACHE cache={a.Handle,0,a.Size,0};
    ULONG *map=(ULONG *)(uintptr_t)a.Map;
    map[0]=0x703D8003;map[1]=(ULONG)a.Address+target*4;map[2]=(ULONG)(a.Address>>32);map[3]=0x83002500;
    GPU_SUBMIT valid={a.Handle,0,4,0};GPU_WAIT wait={0,10000000};
    GPU_SUBMIT bad[]={ {0,0,4,0},{GPU_MAX_BUFFERS+1,0,4,0},{a.Handle,0,4,1},
        {a.Handle,1,4,0},{a.Handle,0,0,0},{a.Handle,0,0x100000,0},
        {a.Handle,a.Size,4,0},{a.Handle,0xFFFFFFFC,4,0},{a.Handle,0,a.Size/4+1,0} };
    for(ULONG i=0;i<sizeof(bad)/sizeof(*bad);i++) {
        GPU_SUBMIT batch[2]={valid,bad[i]};
        if(!Rejected(h,GPU_IOCTL_SUBMIT,bad+i,sizeof(*bad)) || !Rejected(h,GPU_IOCTL_SUBMIT_BATCH,batch,sizeof(batch)))goto done;
    }
    MemoryBarrier();
    if(cached) {
        if(!Io(h,GPU_IOCTL_CACHE,&cache,sizeof(cache),NULL,0))goto done;
        cache.Offset=0x1000;cache.Size=0x2000;
    }
    for(ULONG i=0;i<32;i++) {
        map[target]=0;MemoryBarrier();
        cache.Readback=0;if(cached && !Io(h,GPU_IOCTL_CACHE,&cache,sizeof(cache),NULL,0))goto done;
        if(!Io(h,GPU_IOCTL_SUBMIT,&valid,sizeof(valid),&wait.Fence,sizeof(wait.Fence)) ||
            !Io(h,GPU_IOCTL_WAIT,&wait,sizeof(wait),NULL,0))goto done;
        cache.Readback=1;if(cached && !Io(h,GPU_IOCTL_CACHE,&cache,sizeof(cache),NULL,0))goto done;
        MemoryBarrier();if(map[target]!=0x83002500)goto done;
    }
    GPU_SUBMIT batch[256];for(ULONG i=0;i<256;i++)batch[i]=valid;
    map[target]=0;MemoryBarrier();
    cache.Readback=0;if(cached && !Io(h,GPU_IOCTL_CACHE,&cache,sizeof(cache),NULL,0))goto done;
    if(!Io(h,GPU_IOCTL_SUBMIT_BATCH,batch,sizeof(batch),&wait.Fence,sizeof(wait.Fence)) ||
        !Io(h,GPU_IOCTL_WAIT,&wait,sizeof(wait),NULL,0))goto done;
    cache.Readback=1;if(cached && !Io(h,GPU_IOCTL_CACHE,&cache,sizeof(cache),NULL,0))goto done;
    MemoryBarrier();if(map[target]!=0x83002500)goto done;
    if(!Io(h,GPU_IOCTL_FREE,&a.Handle,sizeof(a.Handle),NULL,0))goto done;
    a.Handle=0;
    if(!Rejected(h,GPU_IOCTL_SUBMIT,&valid,sizeof(valid)))goto done;
    GPU_INFO info;if(!Io(h,GPU_IOCTL_QUERY,NULL,0,&info,sizeof(info)) || info.Status || info.Completed!=info.Submitted)goto done;
    printf("{\"gpu_address\":\"%llx\",\"single_writes\":32,\"batch_entries\":256,\"invalid_submissions\":19,\"completed\":%llu,\"irqs\":%u,\"dpcs\":%u,\"result\":\"PASS\"}\n",a.Address,info.Completed,info.Snapshot[0],info.Snapshot[1]);
    result=0;
done:
    if(result)printf("{\"result\":\"FAIL\",\"error\":%lu}\n",GetLastError());
    if(a.Handle)Io(h,GPU_IOCTL_FREE,&a.Handle,sizeof(a.Handle),NULL,0);
    CloseHandle(h);return result;
}
