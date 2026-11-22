#include "a830_windows.h"
#include <cfgmgr32.h>
#include <new>
#include "tu_device.h"
#include "tu_knl.h"
#include "vk_sync_timeline.h"
#include "vk_fence.h"
#include "vk_semaphore.h"
#include "tu_wsi_windows.h"
static thread_local const A830_HOST *current_host;
static thread_local windows_context **context_sink;
void tu_windows_capture_context(windows_context **sink){context_sink=sink;}
extern "C" __declspec(dllexport) void WINAPI A830SetHost(const A830_HOST *host){current_host=host;}

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
      if(ctx)return flags&VK_SYNC_WAIT_PENDING?VK_SUCCESS:(VkResult)a830_context_wait(ctx,fence,deadline);
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

static VkResult acquire_sync(tu_device *dev,VkFence previous,vk_sync **out) {
   VkResult result=vk_sync_create(&dev->vk,&windows_sync_type,(vk_sync_flags)0,previous?0:1,out);
   if(result!=VK_SUCCESS || !previous)return result;
   VK_FROM_HANDLE(vk_fence, fence, previous);
   auto source=(windows_sync *)vk_fence_get_active_sync(fence);
   AcquireSRWLockShared(&source->lock);
   auto ctx=source->ctx;uint64_t value=source->fence;
   bool signaled=WaitForSingleObject(source->event,0)==WAIT_OBJECT_0;
   ReleaseSRWLockShared(&source->lock);
   if(ctx)sync_point((windows_sync *)*out,ctx,value);
   else if(signaled)result=sync_signal(&dev->vk,*out,0);
   else result=VK_ERROR_DEVICE_LOST;
   return result;
}
VkResult tu_windows_wsi_signal_acquire(VkDevice handle,VkFence previous,VkSemaphore semaphore,VkFence ready) {
   VK_FROM_HANDLE(tu_device, dev, handle);
   if(semaphore) {
      VK_FROM_HANDLE(vk_semaphore, sem, semaphore);vk_semaphore_reset_temporary(&dev->vk,sem);
      VkResult result=acquire_sync(dev,previous,&sem->temporary);if(result!=VK_SUCCESS)return result;
   }
   if(ready) {
      VK_FROM_HANDLE(vk_fence, fence, ready);vk_fence_reset_temporary(&dev->vk,fence);
      return acquire_sync(dev,previous,&fence->temporary);
   }
   return VK_SUCCESS;
}

static VkResult device_init(tu_device *dev) {
   dev->fd=-1;
   if(!DuplicateHandle(GetCurrentProcess(),dev->physical_device->windows_handle,GetCurrentProcess(),&dev->windows_handle,0,FALSE,DUPLICATE_SAME_ACCESS))
      return VK_ERROR_INITIALIZATION_FAILED;
   dev->windows_context=a830_context_new(dev->windows_handle,current_host);
   if(dev->windows_context) {
      if(context_sink && current_host)*context_sink=(windows_context *)dev->windows_context;
      return VK_SUCCESS;
   }
   CloseHandle(dev->windows_handle);dev->windows_handle=nullptr;return VK_ERROR_INITIALIZATION_FAILED;
}
bool tu_windows_cached_memory(tu_physical_device *dev) {
   GPU_INFO info={};
   return a830_ioctl(dev->windows_handle,GPU_IOCTL_QUERY,nullptr,0,&info,sizeof(info)) && info.Version>=3;
}
static void device_finish(tu_device *dev) { tu_windows_wsi_finish(dev);a830_context_finish((windows_context *)dev->windows_context);CloseHandle(dev->windows_handle); }
static int gpu_timestamp(tu_device *,uint64_t *) { return -1; }
static int suspend_count(tu_device *,uint64_t *value) { *value=0;return 0; }
static VkResult check_status(tu_device *dev) {
   GPU_INFO info;
   return a830_ioctl(dev->windows_handle,GPU_IOCTL_QUERY,nullptr,0,&info,sizeof(info)) && !info.Status?VK_SUCCESS:vk_device_set_lost(&dev->vk,nullptr);
}
static int queue_new(tu_device *,tu_queue *queue) { queue->msm_queue_id=1;return 0; }

static VkResult bo_init(tu_device *dev,vk_object_base *base,tu_bo **out,uint64_t size,uint64_t iova,VkMemoryPropertyFlags properties,tu_bo_alloc_flags,tu_sparse_vma *,const char *name) {
   if(!size || size>GPU_MAX_BUFFER_SIZE || (iova && (iova<A830_SHARED_BASE || iova>=A830_SHARED_END || size>A830_SHARED_END-iova)))return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   auto bo=(tu_bo *)vk_zalloc(&dev->vk.alloc,sizeof(tu_bo),8,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if(!bo)return VK_ERROR_OUT_OF_HOST_MEMORY;
   if(iova) {
      GPU_SHARED_IMPORT import={iova,size};ULONG handle=0;
      if(!a830_ioctl(dev->windows_handle,GPU_IOCTL_SHARED_RETAIN,&import,sizeof(import),&handle,sizeof(handle))){vk_free(&dev->vk.alloc,bo);return VK_ERROR_INVALID_EXTERNAL_HANDLE;}
      bo->gem_handle=handle;
      bo->size=size;bo->iova=iova;bo->refcnt=1;bo->base=base;
      bo->never_unmap=true;*out=bo;return VK_SUCCESS;
   }
   uint32_t bytes=(uint32_t)size;
   GPU_ALLOCATION allocation;
   DWORD allocate=(properties&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?GPU_IOCTL_ALLOC_CACHED:GPU_IOCTL_ALLOC;
   if(!a830_ioctl(dev->windows_handle,allocate,&bytes,sizeof(bytes),&allocation,sizeof(allocation))) {
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
   if(!bo->cached_non_coherent)return;
   if(size==VK_WHOLE_SIZE)size=bo->size-offset;
   if(!size || offset>bo->size || size>bo->size-offset)return;
   GPU_CACHE range={bo->gem_handle,(uint32_t)offset,(uint32_t)size,op==TU_MEM_SYNC_CACHE_FROM_GPU};
   if(!a830_ioctl(dev->windows_handle,GPU_IOCTL_CACHE,&range,sizeof(range),nullptr,0))
      vk_device_set_lost(&dev->vk,nullptr);
}
static void bo_finish(tu_device *dev,tu_bo *bo) {
   if(!p_atomic_dec_zero(&bo->refcnt))return;
   if(bo->gem_handle)a830_ioctl(dev->windows_handle,(bo->gem_handle&0x80000000u)?GPU_IOCTL_SHARED_RELEASE:GPU_IOCTL_FREE,&bo->gem_handle,sizeof(bo->gem_handle),nullptr,0);
   vk_free(&dev->vk.alloc,bo);
}
static VkResult bo_import(tu_device *,tu_bo **,uint64_t,tu_bo_alloc_flags,int) { return VK_ERROR_INVALID_EXTERNAL_HANDLE; }
static int bo_export(tu_device *,tu_bo *) { return -1; }
static VkResult bo_lazy(tu_device *,tu_bo *) { return VK_ERROR_FEATURE_NOT_PRESENT; }
static int metadata_get(tu_device *,tu_bo *,void *,uint32_t) { return -1; }

struct windows_submit { util_dynarray entries,binds; };
static void *submit_create(tu_device *) {
   auto data=new(std::nothrow) windows_submit;
   if(data){util_dynarray_init(&data->entries,nullptr);util_dynarray_init(&data->binds,nullptr);}
   return data;
}
static void submit_finish(tu_device *,void *data) {
   auto submit=(windows_submit *)data;util_dynarray_fini(&submit->entries);util_dynarray_fini(&submit->binds);delete submit;
}
static void submit_entries(tu_device *,void *data,tu_cs_entry *entries,unsigned count) {
   for(unsigned i=0;i<count;i++) {
      GPU_SUBMIT entry={entries[i].bo->gem_handle,entries[i].offset,entries[i].size/4,0};
      util_dynarray_append(&((windows_submit *)data)->entries,entry);
   }
}
static void submit_bind(tu_device *,void *data,tu_sparse_vma *vma,uint64_t offset,tu_bo *bo,uint64_t bo_offset,uint64_t size) {
   GPU_VA_BIND bind={vma->kgsl.virtual_bo->gem_handle,bo?bo->gem_handle:0,offset,bo_offset,size};
   util_dynarray_append(&((windows_submit *)data)->binds,bind);
}
static VkResult sparse_init(tu_device *dev,vk_object_base *base,tu_sparse_vma *vma,uint64_t *iova,tu_sparse_vma_flags flags,uint64_t size,uint64_t address) {
   auto bo=(tu_bo *)vk_zalloc(&dev->vk.alloc,sizeof(tu_bo),8,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);if(!bo)return VK_ERROR_OUT_OF_HOST_MEMORY;
   GPU_VA_RESERVE request={address,size,(flags&TU_SPARSE_VMA_MAP_ZERO)?1u:0u,0};GPU_VA_ALLOCATION allocation;
   if(!a830_ioctl(dev->windows_handle,GPU_IOCTL_VA_RESERVE,&request,sizeof(request),&allocation,sizeof(allocation))){vk_free(&dev->vk.alloc,bo);return VK_ERROR_OUT_OF_DEVICE_MEMORY;}
   bo->gem_handle=allocation.Handle;bo->size=allocation.Size;bo->iova=allocation.Address;bo->refcnt=1;bo->base=base;
   vma->kgsl.virtual_bo=bo;*iova=bo->iova;return VK_SUCCESS;
}
static void sparse_finish(tu_device *dev,tu_sparse_vma *vma) {
   auto bo=vma->kgsl.virtual_bo;
   a830_ioctl(dev->windows_handle,GPU_IOCTL_VA_RELEASE,&bo->gem_handle,sizeof(bo->gem_handle),nullptr,0);
   vk_free(&dev->vk.alloc,bo);
}

static VkResult queue_wait(tu_queue *queue,uint32_t fence,uint64_t timeout) {
   if(fence==UINT32_MAX)return VK_SUCCESS;
   auto ctx=(windows_context *)queue->device->windows_context;
   return (VkResult)a830_context_wait(ctx,fence,timeout==UINT64_MAX?UINT64_MAX:os_time_get_nano()+timeout);
}
static VkResult queue_submit(tu_queue *queue,void *data,struct vk_sync_wait *waits,uint32_t wait_count,struct vk_sync_signal *signals,uint32_t signal_count,tu_u_trace_submission_data *) {
   auto dev=queue->device;auto ctx=(windows_context *)dev->windows_context;
   auto submit=(windows_submit *)data;unsigned bind_count=util_dynarray_num_elements(&submit->binds,GPU_VA_BIND);
   for(unsigned i=0;i<wait_count;i++) {
      bool ordered=false;
      if(!bind_count && ctx && waits[i].sync->type==&windows_sync_type) {
         auto sync=(windows_sync *)waits[i].sync;
         AcquireSRWLockShared(&sync->lock);ordered=sync->ctx==ctx;ReleaseSRWLockShared(&sync->lock);
      }
      if(!ordered) {
         VkResult result=vk_sync_wait(&dev->vk,waits[i].sync,waits[i].wait_value,VK_SYNC_WAIT_COMPLETE,UINT64_MAX);
         if(result!=VK_SUCCESS)return result;
      }
   }
   if(bind_count) {
      if(!ctx)return VK_ERROR_DEVICE_LOST;
      AcquireSRWLockExclusive(&ctx->submit_lock);
      VkResult result=(VkResult)a830_context_wait(ctx,ctx->submitted,UINT64_MAX);
      if(result==VK_SUCCESS && !a830_ioctl(dev->windows_handle,GPU_IOCTL_VA_BIND,submit->binds.data,(DWORD)submit->binds.size,nullptr,0))result=VK_ERROR_DEVICE_LOST;
      ReleaseSRWLockExclusive(&ctx->submit_lock);
      if(result!=VK_SUCCESS)return result;
   }
   auto entries=(GPU_SUBMIT *)submit->entries.data;
   unsigned count=util_dynarray_num_elements(&submit->entries,GPU_SUBMIT);
   if(!ctx || !ctx->page)return vk_device_set_lost(&dev->vk,nullptr);
   uint64_t fence;
   VkResult result=(VkResult)a830_context_submit(ctx,entries,count,&fence);queue->fence=(uint32_t)fence;
   if(result==VK_SUCCESS)for(unsigned i=0;i<signal_count;i++) {
      sync_point((windows_sync *)signals[i].sync,ctx,fence);
   }
   return result==VK_SUCCESS?VK_SUCCESS:vk_device_set_lost(&dev->vk,nullptr);
}
static const tu_knl windows_knl={
   .name="windows",.device_init=device_init,.device_finish=device_finish,
   .device_get_gpu_timestamp=gpu_timestamp,.device_get_suspend_count=suspend_count,.device_check_status=check_status,
   .submitqueue_new=queue_new,.bo_init=bo_init,
   .bo_init_dmabuf=bo_import,.bo_export_dmabuf=bo_export,.bo_alloc_lazy=bo_lazy,.bo_map=bo_map,
   .bo_finish=bo_finish,.bo_get_metadata=metadata_get,
   .submit_create=submit_create,.submit_finish=submit_finish,.submit_add_entries=submit_entries,.submit_add_bind=submit_bind,
   .queue_submit=queue_submit,.queue_wait_fence=queue_wait,.sparse_vma_init=sparse_init,.sparse_vma_finish=sparse_finish,
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
   if(!a830_ioctl(handle,GPU_IOCTL_QUERY,nullptr,0,&info,sizeof(info)) || info.Version!=GPU_UAPI_VERSION) {
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
   ULONG sparse=0;
   device->has_sparse=device->has_sparse_prr=a830_ioctl(handle,GPU_IOCTL_SPARSE_CAPS,nullptr,0,&sparse,sizeof(sparse)) && sparse==1;
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
