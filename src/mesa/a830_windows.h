#pragma once
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stdlib.h>
#include "util/macros.h"
#include "util/os_time.h"
#include "qcom_gpu_uapi.h"
#include "a830_wddm_abi.h"
#include "a830_completion.h"
#include "a830_host.h"

typedef struct {
   HMODULE gdi;
   D3DKMT_HANDLE adapter, device, context;
   HANDLE completion;
   void *commands;
   UINT command_size, generation;
   uint64_t lease, submitted;
   A830_HOST host;
   SRWLOCK submit_lock;
   volatile A830_COMPLETION *page;
   __typeof__(&D3DKMTEnumAdapters2) EnumAdapters2;
   __typeof__(&D3DKMTQueryAdapterInfo) QueryAdapterInfo;
   __typeof__(&D3DKMTCloseAdapter) CloseAdapter;
   __typeof__(&D3DKMTCreateDevice) CreateDevice;
   __typeof__(&D3DKMTCreateContext) CreateContext;
   __typeof__(&D3DKMTRender) Render;
   __typeof__(&D3DKMTEscape) Escape;
   __typeof__(&D3DKMTDestroyContext) DestroyContext;
   __typeof__(&D3DKMTDestroyDevice) DestroyDevice;
} windows_context;

static int a830_ioctl(HANDLE handle, DWORD code, const void *input, DWORD input_size, void *output, DWORD output_size) {
   DWORD bytes;
   return DeviceIoControl(handle,code,(void *)input,input_size,output,output_size,&bytes,NULL)!=FALSE;
}
static int a830_context_status(windows_context *ctx, uint64_t fence) {
   A830_COMPLETION page;
   if(!a830_read_completion(ctx->page,&page) || page.Status || page.Generation!=ctx->generation)return -4;
   return page.Completed>=fence?0:1;
}
static int a830_context_wait(windows_context *ctx, uint64_t fence, uint64_t deadline) {
   for(;;) {
      int result=a830_context_status(ctx,fence);if(result!=1)return result;
      uint64_t now=os_time_get_nano();if(now>=deadline)return 2;
      ResetEvent(ctx->completion);
      result=a830_context_status(ctx,fence);if(result!=1)return result;
      DWORD ms=deadline==UINT64_MAX?10:(DWORD)MIN2((deadline-now+999999)/1000000,10);
      DWORD status=WaitForSingleObject(ctx->completion,ms);
      if(status!=WAIT_OBJECT_0 && status!=WAIT_TIMEOUT)return -4;
   }
}
static void a830_context_finish(windows_context *ctx) {
   if(!ctx)return;
   if(ctx->page && ctx->submitted)a830_context_wait(ctx,ctx->submitted,os_time_get_nano()+10000000000ull);
   if(ctx->host.User)ctx->host.Destroy(ctx->host.User);
   else if(ctx->context){D3DKMT_DESTROYCONTEXT args={ctx->context};ctx->DestroyContext(&args);}
   if(ctx->device){D3DKMT_DESTROYDEVICE args={ctx->device};ctx->DestroyDevice(&args);}
   if(ctx->adapter){D3DKMT_CLOSEADAPTER args={ctx->adapter};ctx->CloseAdapter(&args);}
   if(ctx->completion)CloseHandle(ctx->completion);
   if(ctx->gdi)FreeLibrary(ctx->gdi);
   free(ctx);
}
static windows_context *a830_context_new(HANDLE handle, const A830_HOST *host) {
   windows_context *ctx=(windows_context *)calloc(1,sizeof(*ctx));
   if(!ctx)return NULL;
   InitializeSRWLock(&ctx->submit_lock);
   if(host) {
      if(host->Version!=A830_HOST_VERSION || host->Size!=sizeof(A830_HOST) || !host->Synchronize ||
         !host->OpenImage || !host->QueryImage || !host->CloseImage)goto fail;
      ctx->host=*host;ctx->completion=CreateEventW(NULL,TRUE,FALSE,NULL);
      HANDLE context=NULL;void *page=NULL;
      if(!ctx->completion || !a830_ioctl(handle,GPU_IOCTL_CLIENT_LEASE,NULL,0,&ctx->lease,sizeof(ctx->lease)) ||
          FAILED(ctx->host.Create(ctx->host.User,ctx->completion,&context,&page)))goto fail;
      ctx->page=(volatile A830_COMPLETION *)page;if(!ctx->page)goto fail;
   } else {
      ctx->gdi=LoadLibraryExW(L"gdi32.dll",NULL,LOAD_LIBRARY_SEARCH_SYSTEM32);
      if(!ctx->gdi)goto fail;
#define LOAD_KMT(name) ctx->name=(__typeof__(ctx->name))GetProcAddress(ctx->gdi,"D3DKMT" #name);if(!ctx->name)goto fail;
      LOAD_KMT(EnumAdapters2);LOAD_KMT(QueryAdapterInfo);LOAD_KMT(CloseAdapter);
      LOAD_KMT(CreateDevice);LOAD_KMT(CreateContext);LOAD_KMT(Render);LOAD_KMT(Escape);
      LOAD_KMT(DestroyContext);LOAD_KMT(DestroyDevice);
#undef LOAD_KMT
      D3DKMT_ADAPTERINFO adapters[32]={0};D3DKMT_ENUMADAPTERS2 enumeration={32,adapters};
      if(ctx->EnumAdapters2(&enumeration)<0)goto fail;
      for(UINT i=0;i<enumeration.NumAdapters;i++) {
         A830_ADAPTER_INFO info={0};D3DKMT_QUERYADAPTERINFO query={adapters[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&info,sizeof(info)};
         if(!ctx->adapter && ctx->QueryAdapterInfo(&query)>=0 && info.Version==A830_ABI_VERSION &&
             info.Chip==0x44050001 && (info.Flags&1) && !info.Status)
            ctx->adapter=adapters[i].hAdapter;
         else {D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};ctx->CloseAdapter(&close);}
      }
      if(!ctx->adapter || !a830_ioctl(handle,GPU_IOCTL_CLIENT_LEASE,NULL,0,&ctx->lease,sizeof(ctx->lease)))goto fail;
      D3DKMT_CREATEDEVICE device={0};device.hAdapter=ctx->adapter;
      if(ctx->CreateDevice(&device)<0)goto fail;
      ctx->device=device.hDevice;ctx->completion=CreateEventW(NULL,TRUE,FALSE,NULL);
      if(!ctx->completion)goto fail;
      A830_CONTEXT_DATA data={A830_ABI_VERSION,0,(uint64_t)(uintptr_t)ctx->completion,0};
      D3DKMT_CREATECONTEXT context={0};context.hDevice=ctx->device;context.ClientHint=D3DKMT_CLIENTHINT_UNKNOWN;
      context.pPrivateDriverData=&data;context.PrivateDriverDataSize=sizeof(data);
      if(ctx->CreateContext(&context)<0)goto fail;
      ctx->context=context.hContext;ctx->commands=context.pCommandBuffer;ctx->command_size=context.CommandBufferSize;
      if(!data.CompletionPage) {
         A830_CONTEXT_QUERY query={A830_ABI_VERSION,0,0};D3DKMT_ESCAPE escape={0};
         escape.hAdapter=ctx->adapter;escape.hDevice=ctx->device;escape.hContext=ctx->context;
         escape.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;escape.pPrivateDriverData=&query;escape.PrivateDriverDataSize=sizeof(query);
         if(ctx->Escape(&escape)<0)goto fail;
         data.CompletionPage=query.CompletionPage;
      }
      ctx->page=(volatile A830_COMPLETION *)(uintptr_t)data.CompletionPage;
      if(!ctx->page)goto fail;
   }
   A830_COMPLETION page;
   if(!a830_read_completion(ctx->page,&page) || page.Status)goto fail;
   ctx->generation=page.Generation;
   return ctx;
fail:
   a830_context_finish(ctx);return NULL;
}
static int a830_context_submit(windows_context *ctx, const GPU_SUBMIT *entries, unsigned count, uint64_t *fence) {
   AcquireSRWLockExclusive(&ctx->submit_lock);
   int result=a830_context_status(ctx,0);
   for(unsigned at=0;result==0 && at<count;) {
      unsigned n=MIN2(count-at,256u);
      if(ctx->submitted>=INT32_MAX){result=-4;break;}
      A830_BATCH_COMMAND batch={0};batch.Version=A830_ABI_VERSION;batch.Operation=A830_OP_CLIENT;
      batch.Count=n;batch.Lease=ctx->lease;batch.Fence=ctx->submitted+1;
      memcpy(batch.Commands,entries+at,n*sizeof(*entries));
      UINT bytes=(UINT)(offsetof(A830_BATCH_COMMAND,Commands)+n*sizeof(*entries));
      if(ctx->host.User) {
         if(FAILED(ctx->host.Render(ctx->host.User,&batch,bytes))){result=-4;break;}
      } else {
         if(!ctx->commands || ctx->command_size<bytes){result=-4;break;}
         memcpy(ctx->commands,&batch,bytes);
         D3DKMT_RENDER render={0};render.hContext=ctx->context;render.CommandLength=bytes;
         if(ctx->Render(&render)<0){result=-4;break;}
         ctx->commands=render.pNewCommandBuffer;ctx->command_size=render.NewCommandBufferSize;
      }
      ctx->submitted=batch.Fence;at+=n;
   }
   *fence=ctx->submitted;ReleaseSRWLockExclusive(&ctx->submit_lock);return result;
}
