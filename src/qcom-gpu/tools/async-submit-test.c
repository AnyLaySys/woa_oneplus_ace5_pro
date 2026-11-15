#include <windows.h>
#include <d3dkmthk.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "../wddm/abi.h"

/* Bounded: at most 66 MiB of CP writes, no sleeps between submissions. */
#define MAX_BATCHES 64u
#define PAYLOAD_WORDS 1024u
#define COMMAND_STRIDE 8192u
static int Io(HANDLE h,DWORD code,void *in,DWORD size,void *out,DWORD length) {
    DWORD bytes;return DeviceIoControl(h,code,in,size,out,length,&bytes,NULL)!=FALSE;
}
static ULONG Packet(ULONG words) {
    ULONG parity=words;parity^=parity>>8;parity^=parity>>4;parity^=parity>>2;parity^=parity>>1;
    return 0x703D0000|words|((~parity&1)<<15);
}
static int Compare(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;return x<y?-1:x>y?1:0;
}
static int Ready(volatile A830_COMPLETION *page,ULONG generation,ULONGLONG fence) {
    ULONGLONG completed=page->Completed;MemoryBarrier();
    if(page->Status || page->Generation!=generation)return -1;
    return completed>=fence;
}
static int Wait(volatile A830_COMPLETION *page,ULONG generation,ULONGLONG fence,HANDLE event) {
    ULONGLONG end=GetTickCount64()+5000;
    for(;;) {
        int ready=Ready(page,generation,fence);if(ready)return ready>0;
        if(GetTickCount64()>=end)return 0;
        ResetEvent(event);ready=Ready(page,generation,fence);if(ready)return ready>0;
        if(WaitForSingleObject(event,10)==WAIT_FAILED)return 0;
    }
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    UINT batches=argc>1?(UINT)strtoul(argv[1],NULL,10):32;
    UINT entries=argc>2?(UINT)strtoul(argv[2],NULL,10):256;
    if(batches<2 || batches>MAX_BATCHES || !entries || entries>256)return 2;
    int result=1;NTSTATUS status=0;DWORD error=0;ULONG size,generation=0;
    HANDLE gpu=INVALID_HANDLE_VALUE,event=NULL;
    GPU_ALLOCATION commands={0},data={0};ULONGLONG lease=0,peak=0;
    GPU_TELEMETRY metricsBefore={0},metricsAfter={0};
    volatile A830_COMPLETION *page=NULL;
    D3DKMT_HANDLE adapter=0;D3DKMT_CREATEDEVICE device={0};D3DKMT_CREATECONTEXT context={0};
    D3DKMT_ADAPTERINFO adapters[16]={0};D3DKMT_ENUMADAPTERS2 enumeration={16,adapters};
    LARGE_INTEGER frequency,before,after;double durations[MAX_BATCHES];
    QueryPerformanceFrequency(&frequency);
    status=D3DKMTEnumAdapters2(&enumeration);if(status<0)goto done;
    for(UINT i=0;i<enumeration.NumAdapters;i++) {
        A830_ADAPTER_INFO info={0};D3DKMT_QUERYADAPTERINFO query={adapters[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&info,sizeof(info)};
        if(!adapter && D3DKMTQueryAdapterInfo(&query)>=0 && info.Version==A830_ABI_VERSION && info.Chip==0x44050001 && (info.Flags&1) && !info.Status)adapter=adapters[i].hAdapter;
        else {D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};D3DKMTCloseAdapter(&close);}
    }
    if(!adapter){puts("{\"adapter\":\"NOT_FOUND\"}");goto done;}
    gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(gpu==INVALID_HANDLE_VALUE || !Io(gpu,GPU_IOCTL_CLIENT_LEASE,NULL,0,&lease,sizeof(lease)))goto done;
    event=CreateEventW(NULL,TRUE,FALSE,NULL);if(!event)goto done;
    device.hAdapter=adapter;status=D3DKMTCreateDevice(&device);if(status<0)goto done;
    A830_CONTEXT_DATA privateData={A830_ABI_VERSION,0,(GPU_U64)(uintptr_t)event,0};
    context.hDevice=device.hDevice;context.pPrivateDriverData=&privateData;context.PrivateDriverDataSize=sizeof(privateData);
    status=D3DKMTCreateContext(&context);if(status<0)goto done;
    if(!privateData.CompletionPage) {
        A830_CONTEXT_QUERY query={A830_ABI_VERSION,0,0};D3DKMT_ESCAPE escape={0};
        escape.hAdapter=adapter;escape.hDevice=device.hDevice;escape.hContext=context.hContext;
        escape.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;escape.pPrivateDriverData=&query;escape.PrivateDriverDataSize=sizeof(query);
        status=D3DKMTEscape(&escape);if(status<0)goto done;privateData.CompletionPage=query.CompletionPage;
    }
    page=(volatile A830_COMPLETION *)(uintptr_t)privateData.CompletionPage;if(!page)goto done;
    generation=page->Generation;
    MEMORY_BASIC_INFORMATION mapping={0};
    if(!VirtualQuery((void *)page,&mapping,sizeof(mapping)) || (mapping.Protect&0xff)!=PAGE_READONLY) {
        printf("{\"completion_page_not_readonly\":%lu}\n",mapping.Protect);goto done;
    }
    size=(batches+1)*COMMAND_STRIDE;if(!Io(gpu,GPU_IOCTL_ALLOC,&size,sizeof(size),&commands,sizeof(commands)))goto done;
    size=(batches+2)*PAYLOAD_WORDS*4;if(!Io(gpu,GPU_IOCTL_ALLOC,&size,sizeof(size),&data,sizeof(data)))goto done;
    for(UINT b=0;b<=batches;b++) {
        ULONG *cmd=(ULONG *)(uintptr_t)(commands.Map+(GPU_U64)b*COMMAND_STRIDE);
        GPU_U64 address=data.Address+(GPU_U64)b*PAYLOAD_WORDS*4;
        cmd[0]=Packet(PAYLOAD_WORDS+2);cmd[1]=(ULONG)address;cmd[2]=(ULONG)(address>>32);
        for(UINT j=0;j<PAYLOAD_WORDS;j++)cmd[3+j]=0x83003000u+b;
    }
    MemoryBarrier();
    if(!Io(gpu,GPU_IOCTL_TELEMETRY,NULL,0,&metricsBefore,sizeof(metricsBefore)) || metricsBefore.Version!=GPU_TELEMETRY_VERSION)goto done;
    for(UINT b=0;b<batches;b++) {
        A830_BATCH_COMMAND batch={0};batch.Version=A830_ABI_VERSION;batch.Operation=A830_OP_CLIENT;
        batch.Lease=lease;batch.Fence=b+1;batch.Count=entries;
        for(UINT j=0;j<entries;j++){batch.Commands[j].Handle=commands.Handle;batch.Commands[j].Offset=b*COMMAND_STRIDE;batch.Commands[j].Words=PAYLOAD_WORDS+3;}
        UINT bytes=(UINT)(offsetof(A830_BATCH_COMMAND,Commands)+entries*sizeof(GPU_SUBMIT));
        if(!context.pCommandBuffer || context.CommandBufferSize<bytes)goto done;
        memcpy(context.pCommandBuffer,&batch,bytes);D3DKMT_RENDER render={0};render.hContext=context.hContext;render.CommandLength=bytes;
        QueryPerformanceCounter(&before);status=D3DKMTRender(&render);QueryPerformanceCounter(&after);
        if(status<0)goto done;
        durations[b]=(double)(after.QuadPart-before.QuadPart)*1e6/(double)frequency.QuadPart;
        context.pCommandBuffer=render.pNewCommandBuffer;context.CommandBufferSize=render.NewCommandBufferSize;
        MemoryBarrier();ULONGLONG submitted=page->Submitted,completed=page->Completed;
        if(page->Status || submitted!=b+1 || completed>submitted)goto done;
        if(submitted-completed>peak)peak=submitted-completed;
    }
    if(!Wait(page,generation,batches,event))goto done;
    if(!Io(gpu,GPU_IOCTL_TELEMETRY,NULL,0,&metricsAfter,sizeof(metricsAfter)))goto done;
    printf("{\"hardware_issued_delta\":%llu,\"hardware_completed_delta\":%llu,\"ring_peak_before\":%llu,\"ring_peak_after\":%llu}\n",metricsAfter.HardwareIssued-metricsBefore.HardwareIssued,metricsAfter.HardwareCompleted-metricsBefore.HardwareCompleted,metricsBefore.MaxHardwareInflight,metricsAfter.MaxHardwareInflight);
    volatile ULONG *pixels=(volatile ULONG *)(uintptr_t)data.Map;
    for(UINT b=0;b<batches;b++)for(UINT j=0;j<PAYLOAD_WORDS;j++)
        if(pixels[b*PAYLOAD_WORDS+j]!=0x83003000u+b){printf("{\"mismatch_batch\":%u,\"word\":%u}\n",b,j);goto done;}
    /* This is deliberately a page-only wait(0), with no event/IOCTL call. */
    QueryPerformanceCounter(&before);
    for(UINT j=0;j<10000;j++)if(Ready(page,generation,batches)!=1)goto done;
    QueryPerformanceCounter(&after);
    double fastNs=(double)(after.QuadPart-before.QuadPart)*1e9/(double)frequency.QuadPart/10000;
    qsort(durations,batches,sizeof(*durations),Compare);
    printf("{\"batches\":%u,\"entries_per_batch\":%u,\"submitted\":%llu,\"completed\":%llu,\"peak_pending_sequences\":%llu,\"submit_us_p50\":%.3f,\"submit_us_p95\":%.3f,\"submit_us_max\":%.3f,\"completed_page_check_ns\":%.1f,\"all_data\":\"PASS\"}\n",batches,entries,page->Submitted,page->Completed,peak,durations[batches/2],durations[(batches-1)*95/100],durations[batches-1],fastNs);
    /* FREE releases the handle without waiting for execution. The driver must
     * preserve backing pages referenced by a pending batch until completion. */
    A830_BATCH_COMMAND tail={0};tail.Version=A830_ABI_VERSION;tail.Operation=A830_OP_CLIENT;tail.Lease=lease;tail.Fence=batches+1;tail.Count=entries;
    for(UINT j=0;j<entries;j++){tail.Commands[j].Handle=commands.Handle;tail.Commands[j].Offset=batches*COMMAND_STRIDE;tail.Commands[j].Words=PAYLOAD_WORDS+3;}
    UINT bytes=(UINT)(offsetof(A830_BATCH_COMMAND,Commands)+entries*sizeof(GPU_SUBMIT));memcpy(context.pCommandBuffer,&tail,bytes);
    D3DKMT_RENDER render={0};render.hContext=context.hContext;render.CommandLength=bytes;status=D3DKMTRender(&render);if(status<0)goto done;
    context.pCommandBuffer=render.pNewCommandBuffer;context.CommandBufferSize=render.NewCommandBufferSize;
    ULONGLONG freePending=page->Submitted-page->Completed;
    QueryPerformanceCounter(&before);
    if(!Io(gpu,GPU_IOCTL_FREE,&commands.Handle,sizeof(commands.Handle),NULL,0))goto done;commands.Handle=0;
    QueryPerformanceCounter(&after);
    double freeUs=(double)(after.QuadPart-before.QuadPart)*1e6/(double)frequency.QuadPart;
    int completedAtFree=Ready(page,generation,batches+1);
    if(completedAtFree<0 || !Wait(page,generation,batches+1,event)) {
        puts("{\"free_pending_completion\":\"FAIL\"}");goto done;
    }
    MemoryBarrier();for(UINT j=0;j<PAYLOAD_WORDS;j++)if(pixels[batches*PAYLOAD_WORDS+j]!=0x83003000u+batches)goto done;
    printf("{\"free_data\":\"PASS\",\"free_pending_sequences\":%llu,\"completed_at_free\":%s,\"free_us\":%.3f,\"completion_after_free\":\"PASS\"}\n",freePending,completedAtFree?"true":"false",freeUs);
    /* A separate final batch checks DestroyContext with no explicit wait or
     * preceding FREE; its command BO remains owned until destruction returns. */
    size=COMMAND_STRIDE;if(!Io(gpu,GPU_IOCTL_ALLOC,&size,sizeof(size),&commands,sizeof(commands)))goto done;
    ULONG *last=(ULONG *)(uintptr_t)commands.Map;GPU_U64 address=data.Address+(GPU_U64)(batches+1)*PAYLOAD_WORDS*4;
    last[0]=Packet(PAYLOAD_WORDS+2);last[1]=(ULONG)address;last[2]=(ULONG)(address>>32);
    for(UINT j=0;j<PAYLOAD_WORDS;j++)last[3+j]=0x83003000u+batches+1;
    MemoryBarrier();
    tail.Fence=batches+2;for(UINT j=0;j<entries;j++){tail.Commands[j].Handle=commands.Handle;tail.Commands[j].Offset=0;}
    memcpy(context.pCommandBuffer,&tail,bytes);memset(&render,0,sizeof(render));render.hContext=context.hContext;render.CommandLength=bytes;
    status=D3DKMTRender(&render);if(status<0)goto done;
    ULONGLONG destroyPending=page->Submitted-page->Completed;
    QueryPerformanceCounter(&before);
    D3DKMT_DESTROYCONTEXT destroyContext={context.hContext};status=D3DKMTDestroyContext(&destroyContext);context.hContext=0;page=NULL;
    QueryPerformanceCounter(&after);if(status<0)goto done;
    MemoryBarrier();for(UINT j=0;j<PAYLOAD_WORDS;j++)if(pixels[(batches+1)*PAYLOAD_WORDS+j]!=0x83003000u+batches+1)goto done;
    printf("{\"queued_destroy_result\":\"PASS\",\"destroy_pending_sequences\":%llu,\"destroy_us\":%.3f,\"multiple_inflight\":\"%s\",\"async_submit\":\"%s\"}\n",destroyPending,(double)(after.QuadPart-before.QuadPart)*1e6/(double)frequency.QuadPart,peak>1 && metricsAfter.MaxHardwareInflight>1?"OBSERVED":"NOT_OBSERVED",peak>1 && metricsAfter.MaxHardwareInflight>1 && freePending && destroyPending?"PASS":"INCONCLUSIVE");
    result=peak>1 && metricsAfter.MaxHardwareInflight>1 && freePending && destroyPending?0:3;
done:
    error=GetLastError();
    if(result==1)printf("{\"async_submit\":\"FAIL\",\"ntstatus\":\"%08lx\",\"error\":%lu}\n",status,error);
    if(context.hContext){D3DKMT_DESTROYCONTEXT destroy={context.hContext};D3DKMTDestroyContext(&destroy);}
    if(commands.Handle)Io(gpu,GPU_IOCTL_FREE,&commands.Handle,sizeof(commands.Handle),NULL,0);
    if(data.Handle)Io(gpu,GPU_IOCTL_FREE,&data.Handle,sizeof(data.Handle),NULL,0);
    if(device.hDevice){D3DKMT_DESTROYDEVICE destroy={device.hDevice};D3DKMTDestroyDevice(&destroy);}
    if(adapter){D3DKMT_CLOSEADAPTER close={adapter};D3DKMTCloseAdapter(&close);}
    if(event)CloseHandle(event);if(gpu!=INVALID_HANDLE_VALUE)CloseHandle(gpu);
    return result;
}
