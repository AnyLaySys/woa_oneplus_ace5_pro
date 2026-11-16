#include <windows.h>
#include <winioctl.h>
#include <cfgmgr32.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <new>
#include "tu_device.h"
#include "tu_knl.h"
#include "vk_sync_timeline.h"
#include "util/os_time.h"
#include "qcom_gpu_uapi.h"
#include "a830_wddm_abi.h"
#include "a830_host.h"
static thread_local const A830_HOST *current_host;
extern "C" __declspec(dllexport) void WINAPI A830SetHost(const A830_HOST *host){current_host=host;}

struct windows_context {
   HMODULE gdi;
   D3DKMT_HANDLE adapter,device,context;
   HANDLE completion;
   void *commands;
   UINT command_size;
   uint64_t lease;
   UINT generation;
   A830_HOST host;
   SRWLOCK submit_lock;
   volatile A830_COMPLETION *page;
   uint64_t submitted;
   decltype(&D3DKMTEnumAdapters2) EnumAdapters2;
   decltype(&D3DKMTQueryAdapterInfo) QueryAdapterInfo;
   decltype(&D3DKMTCloseAdapter) CloseAdapter;
   decltype(&D3DKMTCreateDevice) CreateDevice;
   decltype(&D3DKMTCreateContext) CreateContext;
   decltype(&D3DKMTRender) Render;
   decltype(&D3DKMTEscape) Escape;
   decltype(&D3DKMTDestroyContext) DestroyContext;
   decltype(&D3DKMTDestroyDevice) DestroyDevice;
};
static VkResult point_wait(windows_context *,uint64_t,uint64_t);
static void context_finish(windows_context *ctx) {
   if(!ctx)return;
   if(ctx->page && ctx->submitted)point_wait(ctx,ctx->submitted,os_time_get_nano()+10000000000ull);
   if(ctx->host.User)ctx->host.Destroy(ctx->host.User);
   else if(ctx->context){D3DKMT_DESTROYCONTEXT args={ctx->context};ctx->DestroyContext(&args);}
   if(ctx->device){D3DKMT_DESTROYDEVICE args={ctx->device};ctx->DestroyDevice(&args);}
   if(ctx->adapter){D3DKMT_CLOSEADAPTER args={ctx->adapter};ctx->CloseAdapter(&args);}
   if(ctx->completion)CloseHandle(ctx->completion);
   if(ctx->gdi)FreeLibrary(ctx->gdi);
   delete ctx;
}
/* Each point refers to an immutable sequence on this device's ordered context.
 * The page is mapped read-only: aligned volatile reads plus barriers, never
 * interlocked RMW instructions (which would write the read-only mapping).
 */
static VkResult point_status(windows_context *ctx,uint64_t fence) {
   if(!ctx->page)return VK_ERROR_DEVICE_LOST;
   MemoryBarrier();
   uint64_t completed=ctx->page->Completed;MemoryBarrier();
   if(ctx->page->Status || ctx->page->Generation!=ctx->generation)return VK_ERROR_DEVICE_LOST;
   return completed>=fence?VK_SUCCESS:VK_NOT_READY;
}
static VkResult point_wait(windows_context *ctx,uint64_t fence,uint64_t deadline) {
   for(;;) {
      VkResult result=point_status(ctx,fence);if(result!=VK_NOT_READY)return result;
      uint64_t now=os_time_get_nano();if(now>=deadline)return VK_TIMEOUT;
      /* Reset, then recheck to close the completion-before-wait race. A short
       * bounded sleep also handles multiple waiters sharing the event. */
      ResetEvent(ctx->completion);
      result=point_status(ctx,fence);if(result!=VK_NOT_READY)return result;
      DWORD ms=deadline==UINT64_MAX?10:(DWORD)MIN2((deadline-now+999999)/1000000,10);
      DWORD status=WaitForSingleObject(ctx->completion,ms);
      if(status!=WAIT_OBJECT_0 && status!=WAIT_TIMEOUT)return VK_ERROR_DEVICE_LOST;
   }
}
struct windows_sync { vk_sync base; HANDLE event; SRWLOCK lock; windows_context *ctx; uint64_t fence; };
static VkResult sync_init(vk_device *,vk_sync *sync,uint64_t value) {
   auto s=(windows_sync *)sync;s->ctx=nullptr;s->fence=0;InitializeSRWLock(&s->lock);
   s->event=CreateEventW(nullptr,TRUE,value!=0,nullptr);
   return s->event?VK_SUCCESS:VK_ERROR_OUT_OF_HOST_MEMORY;
}
static void sync_finish(vk_device *,vk_sync *sync) {CloseHandle(((windows_sync *)sync)->event);}
static VkResult sync_signal(vk_device *,vk_sync *sync,uint64_t) {
   auto s=(windows_sync *)sync;AcquireSRWLockExclusive(&s->lock);s->ctx=nullptr;s->fence=0;
   BOOL ok=SetEvent(s->event);ReleaseSRWLockExclusive(&s->lock);return ok?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
}
static VkResult sync_reset(vk_device *,vk_sync *sync) {
   auto s=(windows_sync *)sync;AcquireSRWLockExclusive(&s->lock);s->ctx=nullptr;s->fence=0;
   BOOL ok=ResetEvent(s->event);ReleaseSRWLockExclusive(&s->lock);return ok?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
}
static VkResult sync_move(vk_device *,vk_sync *dst,vk_sync *src) {
   auto d=(windows_sync *)dst;auto s=(windows_sync *)src;
   if(d==s)return VK_SUCCESS;
   auto first=(uintptr_t)d<(uintptr_t)s?d:s;auto second=first==d?s:d;
   AcquireSRWLockExclusive(&first->lock);AcquireSRWLockExclusive(&second->lock);
   HANDLE fresh=d->event;ResetEvent(fresh);d->event=s->event;d->ctx=s->ctx;d->fence=s->fence;
   s->event=fresh;s->ctx=nullptr;s->fence=0;
   ReleaseSRWLockExclusive(&second->lock);ReleaseSRWLockExclusive(&first->lock);return VK_SUCCESS;
}
static VkResult sync_wait(vk_device *,vk_sync *sync,uint64_t,vk_sync_wait_flags flags,uint64_t deadline) {
   auto s=(windows_sync *)sync;
   for(;;) {
      AcquireSRWLockShared(&s->lock);auto ctx=s->ctx;uint64_t fence=s->fence;
      bool signaled=ctx || WaitForSingleObject(s->event,0)==WAIT_OBJECT_0;ReleaseSRWLockShared(&s->lock);
      if(ctx)return flags&VK_SYNC_WAIT_PENDING?VK_SUCCESS:point_wait(ctx,fence,deadline);
      if(signaled)return VK_SUCCESS;
      uint64_t now=os_time_get_nano();if(now>=deadline)return VK_TIMEOUT;
      DWORD ms=deadline==UINT64_MAX?10:(DWORD)MIN2((deadline-now+999999)/1000000,10);
      if(WaitForSingleObject(s->event,ms)==WAIT_FAILED)return VK_ERROR_DEVICE_LOST;
   }
}
static void sync_point(windows_sync *sync,windows_context *ctx,uint64_t fence) {
   AcquireSRWLockExclusive(&sync->lock);sync->ctx=ctx;sync->fence=fence;SetEvent(sync->event);ReleaseSRWLockExclusive(&sync->lock);
}
static const vk_sync_type windows_sync_type={
   .size=sizeof(windows_sync),
   .features=(vk_sync_features)(VK_SYNC_FEATURE_BINARY|VK_SYNC_FEATURE_GPU_WAIT|VK_SYNC_FEATURE_GPU_MULTI_WAIT|VK_SYNC_FEATURE_CPU_WAIT|VK_SYNC_FEATURE_CPU_RESET|VK_SYNC_FEATURE_CPU_SIGNAL|VK_SYNC_FEATURE_WAIT_PENDING),
   .init=sync_init,.finish=sync_finish,.signal=sync_signal,.reset=sync_reset,.move=sync_move,.wait=sync_wait,
};

static bool ioctl(HANDLE handle,DWORD code,const void *input,DWORD input_size,void *output,DWORD output_size) {
   DWORD bytes;
   return DeviceIoControl(handle,code,(void *)input,input_size,output,output_size,&bytes,nullptr)!=FALSE;
}
static VkResult device_init(tu_device *dev) {
   dev->fd=-1;
   if(!DuplicateHandle(GetCurrentProcess(),dev->physical_device->windows_handle,GetCurrentProcess(),&dev->windows_handle,0,FALSE,DUPLICATE_SAME_ACCESS))
      return VK_ERROR_INITIALIZATION_FAILED;
   windows_context *ctx=new(std::nothrow) windows_context{};
   if(!ctx){CloseHandle(dev->windows_handle);dev->windows_handle=nullptr;return VK_ERROR_OUT_OF_HOST_MEMORY;}
   auto fail=[&](VkResult result) {context_finish(ctx);CloseHandle(dev->windows_handle);dev->windows_handle=nullptr;return result;};
   InitializeSRWLock(&ctx->submit_lock);
   if(current_host) {
      if(current_host->Version!=2 || current_host->Size!=sizeof(A830_HOST))return fail(VK_ERROR_INITIALIZATION_FAILED);
      ctx->host=*current_host;
      ctx->completion=CreateEventW(nullptr,TRUE,FALSE,nullptr);
      HANDLE context=nullptr;void *page=nullptr;
      if(!ctx->completion || !ioctl(dev->windows_handle,GPU_IOCTL_CLIENT_LEASE,nullptr,0,&ctx->lease,sizeof(ctx->lease)) ||
          FAILED(ctx->host.Create(ctx->host.User,ctx->completion,&context,&page)))return fail(VK_ERROR_INITIALIZATION_FAILED);
      ctx->page=(volatile A830_COMPLETION *)page;if(!ctx->page)return fail(VK_ERROR_INITIALIZATION_FAILED);
      ctx->generation=ctx->page->Generation;
      dev->windows_context=ctx;
      return VK_SUCCESS;
   }
   ctx->gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
   if(!ctx->gdi)return fail(VK_ERROR_INITIALIZATION_FAILED);
#define LOAD_KMT(name) ctx->name=(decltype(ctx->name))GetProcAddress(ctx->gdi,"D3DKMT" #name);if(!ctx->name)return fail(VK_ERROR_INITIALIZATION_FAILED);
   LOAD_KMT(EnumAdapters2);LOAD_KMT(QueryAdapterInfo);LOAD_KMT(CloseAdapter);
   LOAD_KMT(CreateDevice);LOAD_KMT(CreateContext);LOAD_KMT(Render);LOAD_KMT(Escape);
   LOAD_KMT(DestroyContext);LOAD_KMT(DestroyDevice);
#undef LOAD_KMT
   D3DKMT_ADAPTERINFO adapters[32]={};D3DKMT_ENUMADAPTERS2 enumeration={32,adapters};
   NTSTATUS status=ctx->EnumAdapters2(&enumeration);
   if(status<0)return fail(VK_ERROR_INITIALIZATION_FAILED);
   for(UINT i=0;i<enumeration.NumAdapters;i++) {
      A830_ADAPTER_INFO info={};D3DKMT_QUERYADAPTERINFO query={adapters[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&info,sizeof(info)};
      if(!ctx->adapter && ctx->QueryAdapterInfo(&query)>=0 && info.Version==A830_ABI_VERSION &&
          info.Chip==0x44050001 && (info.Flags&1) && !info.Status)
         {ctx->adapter=adapters[i].hAdapter;ctx->generation=info.Flags>>16;}
      else {D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};ctx->CloseAdapter(&close);}
   }
   if(!ctx->adapter)return fail(VK_ERROR_INITIALIZATION_FAILED);
   if(!ioctl(dev->windows_handle,GPU_IOCTL_CLIENT_LEASE,nullptr,0,&ctx->lease,sizeof(ctx->lease)) || !ctx->lease) {
      return fail(VK_ERROR_INITIALIZATION_FAILED);
   }
   D3DKMT_CREATEDEVICE device={};device.hAdapter=ctx->adapter;
   status=ctx->CreateDevice(&device);
   if(status<0){return fail(VK_ERROR_INITIALIZATION_FAILED);}
   ctx->device=device.hDevice;ctx->completion=CreateEventW(nullptr,TRUE,FALSE,nullptr);
   if(!ctx->completion)return fail(VK_ERROR_OUT_OF_HOST_MEMORY);
   A830_CONTEXT_DATA data={A830_ABI_VERSION,0,(uint64_t)(uintptr_t)ctx->completion,0};
   D3DKMT_CREATECONTEXT context={};context.hDevice=ctx->device;context.ClientHint=D3DKMT_CLIENTHINT_UNKNOWN;
   context.pPrivateDriverData=&data;context.PrivateDriverDataSize=sizeof(data);
   status=ctx->CreateContext(&context);
   if(status<0){return fail(VK_ERROR_INITIALIZATION_FAILED);}
   ctx->context=context.hContext;ctx->commands=context.pCommandBuffer;ctx->command_size=context.CommandBufferSize;
   if(!data.CompletionPage) {
      A830_CONTEXT_QUERY query={A830_ABI_VERSION,0,0};
      D3DKMT_ESCAPE escape={};escape.hAdapter=ctx->adapter;escape.hDevice=ctx->device;
      escape.hContext=ctx->context;escape.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;
      escape.pPrivateDriverData=&query;escape.PrivateDriverDataSize=sizeof(query);
      if(ctx->Escape(&escape)<0)return fail(VK_ERROR_INITIALIZATION_FAILED);
      data.CompletionPage=query.CompletionPage;
   }
   ctx->page=(volatile A830_COMPLETION *)(uintptr_t)data.CompletionPage;
   if(!ctx->page)return fail(VK_ERROR_INITIALIZATION_FAILED);
   ctx->generation=ctx->page->Generation;
   dev->windows_context=ctx;

   return VK_SUCCESS;
}
bool tu_windows_cached_memory(tu_physical_device *dev) {
   GPU_INFO info={};
   return ioctl(dev->windows_handle,GPU_IOCTL_QUERY,nullptr,0,&info,sizeof(info)) && info.Version>=3;
}
static void device_finish(tu_device *dev) { context_finish((windows_context *)dev->windows_context);CloseHandle(dev->windows_handle); }
static int gpu_timestamp(tu_device *,uint64_t *) { return -1; }
static int suspend_count(tu_device *,uint64_t *value) { *value=0;return 0; }
static VkResult check_status(tu_device *dev) {
   GPU_INFO info;
   return ioctl(dev->windows_handle,GPU_IOCTL_QUERY,nullptr,0,&info,sizeof(info)) && !info.Status?VK_SUCCESS:vk_device_set_lost(&dev->vk,"GPU fault");
}
static int queue_new(tu_device *,tu_queue *queue) { queue->msm_queue_id=1;return 0; }

static VkResult bo_init(tu_device *dev,vk_object_base *base,tu_bo **out,uint64_t size,uint64_t iova,VkMemoryPropertyFlags properties,tu_bo_alloc_flags,tu_sparse_vma *,const char *name) {
   if(!size || size>GPU_MAX_BUFFER_SIZE || (iova && (iova<A830_SHARED_BASE || iova>=A830_SHARED_END || size>A830_SHARED_END-iova)))return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   auto bo=(tu_bo *)vk_zalloc(&dev->vk.alloc,sizeof(tu_bo),8,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if(!bo)return VK_ERROR_OUT_OF_HOST_MEMORY;
   if(iova) {
      GPU_SHARED_IMPORT import={iova,size};ULONG handle=0;
      if(!ioctl(dev->windows_handle,GPU_IOCTL_SHARED_RETAIN,&import,sizeof(import),&handle,sizeof(handle))){vk_free(&dev->vk.alloc,bo);return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
      bo->gem_handle=handle;
      bo->size=size;bo->iova=iova;bo->refcnt=1;bo->base=base;
      bo->never_unmap=true;*out=bo;return VK_SUCCESS;
   }
   uint32_t bytes=(uint32_t)size;
   GPU_ALLOCATION allocation;
   DWORD allocate=(properties&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?GPU_IOCTL_ALLOC_CACHED:GPU_IOCTL_ALLOC;
   if(!ioctl(dev->windows_handle,allocate,&bytes,sizeof(bytes),&allocation,sizeof(allocation))) {


      vk_free(&dev->vk.alloc,bo);return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }
   bo->gem_handle=allocation.Handle;bo->size=allocation.Size;bo->iova=allocation.Address;
   bo->map=(void *)(uintptr_t)allocation.Map;bo->refcnt=1;bo->never_unmap=true;
   bo->base=base;bo->unique_id=bo->gem_handle;
   *out=bo;
   return VK_SUCCESS;
}
static VkResult bo_map(tu_device *,tu_bo *bo,void *placed) {
   return !placed && bo->map?VK_SUCCESS:VK_ERROR_MEMORY_MAP_FAILED;
}
void tu_windows_bo_sync_cache(tu_device *dev,tu_bo *bo,VkDeviceSize offset,VkDeviceSize size,tu_mem_sync_op op) {
   if(size==VK_WHOLE_SIZE)size=bo->size-offset;
   if(!size || offset>bo->size || size>bo->size-offset)return;
   GPU_CACHE range={bo->gem_handle,(uint32_t)offset,(uint32_t)size,op==TU_MEM_SYNC_CACHE_FROM_GPU};
   if(!ioctl(dev->windows_handle,GPU_IOCTL_CACHE,&range,sizeof(range),nullptr,0))
      vk_device_set_lost(&dev->vk,"CPU cache synchronization failed");
}
static void bo_finish(tu_device *dev,tu_bo *bo) {
   if(!p_atomic_dec_zero(&bo->refcnt))return;
   if(bo->gem_handle)ioctl(dev->windows_handle,(bo->gem_handle&0x80000000u)?GPU_IOCTL_SHARED_RELEASE:GPU_IOCTL_FREE,&bo->gem_handle,sizeof(bo->gem_handle),nullptr,0);

   vk_free(&dev->vk.alloc,bo);
}
static VkResult bo_import(tu_device *,tu_bo **,uint64_t,tu_bo_alloc_flags,int) { return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
static int bo_export(tu_device *,tu_bo *) { return -1; }
static VkResult bo_lazy(tu_device *,tu_bo *) { return VK_ERROR_FEATURE_NOT_PRESENT; }
static int metadata_get(tu_device *,tu_bo *,void *,uint32_t) { return -1; }

static void *submit_create(tu_device *) {
   auto array=new(std::nothrow) util_dynarray;
   if(array)util_dynarray_init(array,nullptr);
   return array;
}
static void submit_finish(tu_device *,void *data) {
   auto array=(util_dynarray *)data;util_dynarray_fini(array);delete array;
}
static void submit_entries(tu_device *,void *data,tu_cs_entry *entries,unsigned count) {
   for(unsigned i=0;i<count;i++) {
      GPU_SUBMIT entry={entries[i].bo->gem_handle,entries[i].offset,entries[i].size/4,0};
      util_dynarray_append((util_dynarray *)data,entry);
   }
}
static void submit_bind(tu_device *,void *,tu_sparse_vma *,uint64_t,tu_bo *,uint64_t,uint64_t) { UNREACHABLE("Sparse unsupported"); }
static VkResult sparse_init(tu_device *,vk_object_base *,tu_sparse_vma *,uint64_t *,tu_sparse_vma_flags,uint64_t,uint64_t) { return VK_ERROR_FEATURE_NOT_PRESENT; }

static VkResult queue_wait(tu_queue *queue,uint32_t fence,uint64_t timeout) {
   if(fence==UINT32_MAX)return VK_SUCCESS;
   auto ctx=(windows_context *)queue->device->windows_context;
   return point_wait(ctx,fence,timeout==UINT64_MAX?UINT64_MAX:os_time_get_nano()+timeout);
}
static VkResult queue_submit(tu_queue *queue,void *data,struct vk_sync_wait *waits,uint32_t wait_count,struct vk_sync_signal *signals,uint32_t signal_count,tu_u_trace_submission_data *) {
   auto dev=queue->device;auto ctx=(windows_context *)dev->windows_context;
   /* All Vulkan queues on this device use the same ordered WDDM context.
    * A semaphore already submitted there needs no CPU completion wait. */
   for(unsigned i=0;i<wait_count;i++) {
      bool ordered=false;
      if(ctx && waits[i].sync->type==&windows_sync_type) {
         auto sync=(windows_sync *)waits[i].sync;
         AcquireSRWLockShared(&sync->lock);ordered=sync->ctx==ctx;ReleaseSRWLockShared(&sync->lock);
      }
      if(!ordered) {
         VkResult result=vk_sync_wait(&dev->vk,waits[i].sync,waits[i].wait_value,VK_SYNC_WAIT_COMPLETE,UINT64_MAX);
         if(result!=VK_SUCCESS)return result;
      }
   }
   auto array=(util_dynarray *)data;auto entries=(GPU_SUBMIT *)array->data;
   unsigned count=util_dynarray_num_elements(array,GPU_SUBMIT);
   if(!ctx || !ctx->page)return vk_device_set_lost(&dev->vk,"Missing WDDM completion state");

      AcquireSRWLockExclusive(&ctx->submit_lock);
      VkResult result=point_status(ctx,0);
      for(unsigned at=0;result==VK_SUCCESS && at<count;) {
         unsigned n=MIN2(count-at,256u);
         /* queue_wait_fence has a 32-bit API; never silently wrap its token. */
         if(ctx->submitted>=INT32_MAX){result=VK_ERROR_DEVICE_LOST;break;}
         A830_BATCH_COMMAND batch={};batch.Version=A830_ABI_VERSION;batch.Operation=A830_OP_CLIENT;
         batch.Count=n;batch.Lease=ctx->lease;batch.Fence=ctx->submitted+1;
         memcpy(batch.Commands,entries+at,n*sizeof(*entries));
         UINT bytes=(UINT)(offsetof(A830_BATCH_COMMAND,Commands)+n*sizeof(*entries));
         if(ctx->host.User) {
            if(FAILED(ctx->host.Render(ctx->host.User,&batch,bytes))){result=VK_ERROR_DEVICE_LOST;break;}
         } else {
            if(!ctx->commands || ctx->command_size<bytes){result=VK_ERROR_DEVICE_LOST;break;}
            memcpy(ctx->commands,&batch,bytes);
            D3DKMT_RENDER render={};render.hContext=ctx->context;render.CommandLength=bytes;
            NTSTATUS status=ctx->Render(&render);
            if(status<0){result=VK_ERROR_DEVICE_LOST;break;}
            ctx->commands=render.pNewCommandBuffer;ctx->command_size=render.NewCommandBufferSize;
         }
         ctx->submitted=batch.Fence;at+=n;
      }
      uint64_t fence=ctx->submitted;queue->fence=(uint32_t)fence;
      if(result==VK_SUCCESS)for(unsigned i=0;i<signal_count;i++) {
         assert(signals[i].sync->type==&windows_sync_type);
         sync_point((windows_sync *)signals[i].sync,ctx,fence);
      }
      ReleaseSRWLockExclusive(&ctx->submit_lock);
      return result==VK_SUCCESS?VK_SUCCESS:vk_device_set_lost(&dev->vk,"Asynchronous WDDM submission failed");

}
static const tu_knl windows_knl={
   .name="windows",.device_init=device_init,.device_finish=device_finish,
   .device_get_gpu_timestamp=gpu_timestamp,.device_get_suspend_count=suspend_count,.device_check_status=check_status,
   .submitqueue_new=queue_new,.bo_init=bo_init,
   .bo_init_dmabuf=bo_import,.bo_export_dmabuf=bo_export,.bo_alloc_lazy=bo_lazy,.bo_map=bo_map,
   .bo_finish=bo_finish,.bo_get_metadata=metadata_get,
   .submit_create=submit_create,.submit_finish=submit_finish,.submit_add_entries=submit_entries,.submit_add_bind=submit_bind,
   .queue_submit=queue_submit,.queue_wait_fence=queue_wait,.sparse_vma_init=sparse_init,
};

VkResult tu_knl_windows_load(tu_instance *instance) {
   const GUID guid={0x19c45a12,0xc057,0x4903,{0xa8,0x30,0x61,0x19,0x45,0x61,0x20,0x26}};
   HANDLE handle=CreateFileW(L"\\\\.\\QcomGpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
   ULONG length=0;
   if(handle==INVALID_HANDLE_VALUE) {
   if(CM_Get_Device_Interface_List_SizeW(&length,(GUID *)&guid,nullptr,CM_GET_DEVICE_INTERFACE_LIST_PRESENT)!=CR_SUCCESS || length<2)return VK_ERROR_INCOMPATIBLE_DRIVER;
   auto path=(WCHAR *)vk_alloc(&instance->vk.alloc,length*sizeof(WCHAR),8,VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if(!path)return VK_ERROR_OUT_OF_HOST_MEMORY;
   CONFIGRET status=CM_Get_Device_Interface_ListW((GUID *)&guid,nullptr,path,length,CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
   handle=status==CR_SUCCESS?CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr):INVALID_HANDLE_VALUE;
   vk_free(&instance->vk.alloc,path);
   }
   if(handle==INVALID_HANDLE_VALUE){return VK_ERROR_INITIALIZATION_FAILED;}
   GPU_INFO info;
   if(!ioctl(handle,GPU_IOCTL_QUERY,nullptr,0,&info,sizeof(info)) || info.Version!=GPU_UAPI_VERSION) {
      CloseHandle(handle);return VK_ERROR_INCOMPATIBLE_DRIVER;
   }
   auto device=(tu_physical_device *)vk_zalloc(&instance->vk.alloc,sizeof(tu_physical_device),8,VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if(!device){CloseHandle(handle);return VK_ERROR_OUT_OF_HOST_MEMORY;}
   device->instance=instance;device->windows_handle=handle;
   device->local_fd=device->master_fd=device->kgsl_dma_fd=-1;
   device->dev_id={.gpu_id=830,.chip_id=info.Chip};
   device->gmem_size=12*1024*1024;device->gmem_base=0;
   device->uche_trap_base=0x1FFFFFFFFF000ull;
   device->va_start=GPU_CLIENT_VA_BASE;device->va_size=GPU_CLIENT_VA_SIZE;
   device->ubwc_config.highest_bank_bit=16;device->ubwc_config.bank_swizzle_levels=6;device->ubwc_config.macrotile_mode=FDL_MACROTILE_8_CHANNEL;
   device->submitqueue_priority_count=1;
   device->heap.size=device->va_size;device->heap.flags=VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
   device->sync_types[0]=&windows_sync_type;
   device->timeline_type=vk_sync_timeline_get_type(&windows_sync_type);
   device->sync_types[1]=&device->timeline_type.sync;
   instance->knl=&windows_knl;
   VkResult result=tu_physical_device_init(device,instance);
   if(result!=VK_SUCCESS){CloseHandle(handle);vk_free(&instance->vk.alloc,device);return result;}
   auto &ext=device->vk.supported_extensions;
   ext.KHR_external_fence_fd=ext.KHR_external_memory_fd=ext.KHR_external_semaphore_fd=false;
   ext.EXT_external_memory_dma_buf=ext.EXT_image_drm_format_modifier=ext.EXT_map_memory_placed=false;
   ext.KHR_calibrated_timestamps=ext.EXT_calibrated_timestamps=false;
   list_addtail(&device->vk.link,&instance->vk.physical_devices.list);
   return VK_SUCCESS;
}
