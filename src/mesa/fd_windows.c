#include "drm/freedreno_ringbuffer_sp.h"
#include "a830_windows.h"
#include "a830_native.h"

struct windows_device { struct fd_device base; HANDLE handle; A830_NATIVE_HOST host; };
struct windows_pipe { struct fd_pipe base; windows_context *context; A830_NATIVE_HOST host;void *native; };
struct windows_bo { struct fd_bo base;bool imported; };
FD_DEFINE_CAST(fd_device, windows_device);
FD_DEFINE_CAST(fd_pipe, windows_pipe);
FD_DEFINE_CAST(fd_bo, windows_bo);
static _Thread_local const A830_NATIVE_HOST *native_host;
void fd_windows_set_native_host(const A830_NATIVE_HOST *host){native_host=host;}

static uint64_t bo_iova(struct fd_bo *bo) { return bo->iova; }
static void *bo_map(struct fd_bo *bo) { return bo->map; }
static int bo_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op) { return 0; }
static int bo_madvise(struct fd_bo *bo, int willneed) { return 1; }
static int bo_dmabuf(struct fd_bo *bo) { return -ENOTSUP; }
static const struct fd_bo_funcs bo_funcs = {
   .map=bo_map,.cpu_prep=bo_prep,.madvise=bo_madvise,.iova=bo_iova,
   .dmabuf=bo_dmabuf,.destroy=fd_bo_fini_common,
};
static struct fd_bo *bo_new(struct fd_device *dev, uint32_t size, uint32_t flags) {
   struct windows_device *d=to_windows_device(dev);
   struct windows_bo *b=calloc(1,sizeof(*b));GPU_ALLOCATION allocation;
   if(!b)return NULL;struct fd_bo *bo=&b->base;
   if(d->host.Device?FAILED(d->host.Allocate(d->host.Device,size,flags,&allocation)):
       !a830_ioctl(d->handle,GPU_IOCTL_ALLOC,&size,sizeof(size),&allocation,sizeof(allocation))) {
      free(b);return NULL;
   }
   bo->size=allocation.Size;bo->handle=allocation.Handle;bo->iova=allocation.Address;
   bo->map=(void *)(uintptr_t)allocation.Map;bo->funcs=&bo_funcs;
   fd_bo_init_common(bo,dev);return bo;
}
static void bo_close(struct fd_bo *bo) {
   struct windows_device *d=to_windows_device(bo->dev);
   if(to_windows_bo(bo)->imported)return;
   if(d->host.Device){GPU_ALLOCATION a={bo->handle,bo->size,bo->iova,(uint64_t)(uintptr_t)bo->map};d->host.Free(d->host.Device,&a);}
   else a830_ioctl(d->handle,GPU_IOCTL_FREE,&bo->handle,sizeof(bo->handle),NULL,0);
}
struct fd_bo *fd_windows_bo_import(struct fd_device *dev,const GPU_ALLOCATION *a,uint64_t offset) {
   if(!to_windows_device(dev)->host.Device || offset>=a->Size)return NULL;
   struct windows_bo *b=calloc(1,sizeof(*b));if(!b)return NULL;
   b->imported=true;b->base.size=a->Size-(uint32_t)offset;b->base.handle=a->Handle;b->base.iova=a->Address+offset;
   b->base.map=a->Map?(void *)(uintptr_t)(a->Map+offset):NULL;b->base.funcs=&bo_funcs;fd_bo_init_common(&b->base,dev);return &b->base;
}
static int pipe_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t *value) {
   switch(param) {
   case FD_DEVICE_ID:case FD_GPU_ID:*value=830;return 0;
   case FD_CHIP_ID:*value=0x44050001;return 0;
   case FD_GMEM_SIZE:*value=12*1024*1024;return 0;
   case FD_GMEM_BASE:*value=0;return 0;
   case FD_NR_PRIORITIES:*value=1;return 0;
   case FD_VA_SIZE:*value=GPU_CLIENT_VA_SIZE;return 0;
   case FD_UCHE_TRAP_BASE:*value=0x1fffffffff000ull;return 0;
   case FD_CTX_FAULTS:case FD_GLOBAL_FAULTS: {
      struct windows_pipe *p=to_windows_pipe(pipe);
      if(p->native){*value=FAILED(p->host.Wait(p->native,0,0));return 0;}
      GPU_INFO info;
      if(!a830_ioctl(to_windows_device(pipe->dev)->handle,GPU_IOCTL_QUERY,NULL,0,&info,sizeof(info)))return -EIO;
      *value=info.Status;return 0;
   }
   default:return -ENOTSUP;
   }
}
static int pipe_set_param(struct fd_pipe *pipe, enum fd_param_id param, uint64_t value) { return -ENOTSUP; }
static int pipe_wait(struct fd_pipe *pipe, const struct fd_fence *fence, uint64_t timeout) {
   struct windows_pipe *p=to_windows_pipe(pipe);
   if(p->native){HRESULT hr=p->host.Wait(p->native,fence->kfence,timeout);return hr==S_OK?0:hr==S_FALSE?-ETIMEDOUT:-EIO;}
   uint64_t deadline=timeout==UINT64_MAX?UINT64_MAX:os_time_get_absolute_timeout(timeout);
   int result=a830_context_wait(to_windows_pipe(pipe)->context,fence->kfence,deadline);
   return result==0?0:result==2?-ETIMEDOUT:-EIO;
}
static int submit_list(struct list_head *list) {
   struct fd_submit_sp *last=to_fd_submit_sp(last_submit(list));
   unsigned count=0;
   foreach_submit(submit,list)count+=to_fd_ringbuffer_sp(submit->primary)->u.nr_cmds;
   GPU_SUBMIT *entries=malloc(count*sizeof(*entries));
   if(!entries)return -ENOMEM;
   unsigned at=0;
   foreach_submit_safe(submit,list) {
      struct fd_ringbuffer_sp *ring=to_fd_ringbuffer_sp(submit->primary);
      for(unsigned i=0;i<ring->u.nr_cmds;i++) {
         struct fd_bo *bo=ring->u.cmds[i].ring_bo;
         fd_submit_append_bo(last,bo);
         entries[at++]=(GPU_SUBMIT){suballoc_bo(bo)?fd_bo_heap_block(bo)->handle:bo->handle,submit_offset(bo,ring->offset),ring->u.cmds[i].size/4,0};
      }
      if(submit==&last->base)break;
      struct fd_submit_sp *prior=to_fd_submit_sp(submit);
      for(unsigned i=0;i<prior->nr_bos;i++)fd_submit_append_bo(last,prior->bos[i]);
      list_del(&submit->node);fd_submit_del(submit);
   }
   uint64_t fence;
   struct windows_pipe *pipe=to_windows_pipe(last->base.pipe);
   int result=pipe->native?(FAILED(pipe->host.Submit(pipe->native,entries,count,&fence))?-EIO:0):
      a830_context_submit(pipe->context,entries,count,&fence);
   free(entries);last->out_fence->kfence=(uint32_t)fence;
   return result==0?0:-EIO;
}
static struct fd_submit *submit_new(struct fd_pipe *pipe) { return fd_submit_sp_new(pipe,submit_list); }
static void pipe_destroy(struct fd_pipe *pipe) {
   struct windows_pipe *p=to_windows_pipe(pipe);
   if(p->native)p->host.Destroy(p->native);else a830_context_finish(p->context);
   fd_pipe_sp_ringpool_fini(pipe);free(pipe);
}
static const struct fd_pipe_funcs pipe_funcs = {
   .ringbuffer_new_object=fd_ringbuffer_sp_new_object,.submit_new=submit_new,.flush=fd_pipe_sp_flush,
   .get_param=pipe_param,.set_param=pipe_set_param,.wait=pipe_wait,.destroy=pipe_destroy,
};
static struct fd_pipe *pipe_new(struct fd_device *dev, enum fd_pipe_id id, unsigned prio) {
   if(id!=FD_PIPE_3D)return NULL;
   struct windows_pipe *pipe=calloc(1,sizeof(*pipe));
   if(!pipe)return NULL;
   struct windows_device *d=to_windows_device(dev);
   if(d->host.Device) {
      pipe->host=native_host && native_host->Device==d->host.Device?*native_host:d->host;
      if(FAILED(pipe->host.Create(pipe->host.Device,pipe->host.Queue,&pipe->native))){free(pipe);return NULL;}
   } else {pipe->context=a830_context_new(d->handle,NULL);if(!pipe->context){free(pipe);return NULL;}}
   pipe->base.funcs=&pipe_funcs;pipe->base.dev=dev;fd_pipe_sp_ringpool_init(&pipe->base);
   return &pipe->base;
}
static void device_destroy(struct fd_device *dev) { if(!to_windows_device(dev)->host.Device)CloseHandle(to_windows_device(dev)->handle); }
static const struct fd_device_funcs device_funcs = {
   .bo_new=bo_new,.bo_close_handle=bo_close,.pipe_new=pipe_new,.destroy=device_destroy,
};
struct fd_device *fd_windows_device_new(void) {
   if(native_host) {
      if(native_host->Version!=1 || native_host->Size!=sizeof(*native_host))return NULL;
      struct windows_device *dev=calloc(1,sizeof(*dev));if(!dev)return NULL;
      dev->host=*native_host;dev->base.funcs=&device_funcs;dev->base.version=FD_VERSION_SOFTPIN;
      dev->base.has_cached_coherent=true;return &dev->base;
   }
   HANDLE handle=CreateFileW(L"\\\\.\\QcomGpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
   if(handle==INVALID_HANDLE_VALUE)return NULL;
   GPU_INFO info;
   if(!a830_ioctl(handle,GPU_IOCTL_QUERY,NULL,0,&info,sizeof(info)) || info.Version!=GPU_UAPI_VERSION || info.Chip!=0x44050001 || info.Status) {
      CloseHandle(handle);return NULL;
   }
   struct windows_device *dev=calloc(1,sizeof(*dev));
   if(!dev){CloseHandle(handle);return NULL;}
   dev->handle=handle;dev->base.funcs=&device_funcs;dev->base.version=FD_VERSION_SOFTPIN;
   dev->base.has_cached_coherent=true;return &dev->base;
}
