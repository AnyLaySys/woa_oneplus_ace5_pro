#include "a830_windows.h"
#include "tu_device.h"
#include "tu_wsi_windows.h"
#include "vk_fence.h"
#include <d3d11.h>
#include <dxgi1_4.h>
#include <new>
#include "a830_composition.h"

struct windows_wsi {
   HMODULE d3d11;
   ID3D11Device *device;
   ID3D11DeviceContext *immediate;
   windows_context *context;
   CRITICAL_SECTION lock;
   D3DKMT_HANDLE ready,available,present_ready,present_available;
   uint64_t ready_serial,available_serial;
};

static void destroy_state(windows_wsi *state) {
   for(auto object:{state->ready,state->available,state->present_ready,state->present_available})if(object) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT drop={object};D3DKMTDestroySynchronizationObject(&drop);
   }
   if(state->immediate)state->immediate->Release();if(state->device)state->device->Release();
   if(state->d3d11)FreeLibrary(state->d3d11);DeleteCriticalSection(&state->lock);delete state;
}
static bool create_fences(windows_context *ctx,windows_wsi *state) {
   D3DKMT_HANDLE *targets[]={&state->ready,&state->available};
   D3DKMT_HANDLE *present_targets[]={&state->present_ready,&state->present_available};
   for(unsigned i=0;i<2;i++) {
      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create{};create.hDevice=ctx->device;create.Info.Type=D3DDDI_FENCE;create.Info.Flags.Shared=1;
      if(D3DKMTCreateSynchronizationObject2(&create)<0)return false;
      *targets[i]=create.hSyncObject;
      D3DKMT_OPENSYNCHRONIZATIONOBJECT open{};open.hSharedHandle=create.Info.SharedHandle;
      if(D3DKMTOpenSynchronizationObject(&open)<0)return false;
      *present_targets[i]=open.hSyncObject;
   }
   return true;
}
static void *get_device(VkDevice handle) {
   VK_FROM_HANDLE(tu_device, dev, handle);
   auto ctx=(windows_context *)dev->windows_context;
   AcquireSRWLockExclusive(&ctx->submit_lock);
   auto state=(windows_wsi *)dev->windows_wsi;
   if(!state) {
      state=new(std::nothrow) windows_wsi{};
      if(state) {
         InitializeCriticalSection(&state->lock);
         state->d3d11=LoadLibraryExW(L"d3d11.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
         auto create=state->d3d11?(decltype(&D3D11CreateDevice))GetProcAddress(state->d3d11,"D3D11CreateDevice"):nullptr;
         IDXGIFactory4 *factory=nullptr;IDXGIAdapter1 *adapter=nullptr;
         HMODULE dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
         auto make=dxgi?(decltype(&CreateDXGIFactory1))GetProcAddress(dxgi,"CreateDXGIFactory1"):nullptr;
         LUID luid{};
         D3DKMT_ADAPTERINFO info[32]{};D3DKMT_ENUMADAPTERS2 list={32,info};
         if(ctx->EnumAdapters2 && ctx->EnumAdapters2(&list)>=0)for(UINT i=0;i<list.NumAdapters;i++) {
            A830_ADAPTER_INFO data{};D3DKMT_QUERYADAPTERINFO query={info[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&data,sizeof(data)};
            if(ctx->QueryAdapterInfo(&query)>=0 && data.Version==A830_ABI_VERSION && data.Chip==0x44050001)luid=info[i].AdapterLuid;
            D3DKMT_CLOSEADAPTER close={info[i].hAdapter};ctx->CloseAdapter(&close);
         }
         HRESULT hr=make?make(IID_PPV_ARGS(&factory)):E_FAIL;
         if(SUCCEEDED(hr))hr=factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter));
         const D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_10_0;
         tu_windows_capture_context(&state->context);
         if(SUCCEEDED(hr) && create)hr=create(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &level,1,D3D11_SDK_VERSION,&state->device,nullptr,&state->immediate);
         else hr=E_FAIL;
         tu_windows_capture_context(nullptr);
         if(adapter)adapter->Release();if(factory)factory->Release();if(dxgi)FreeLibrary(dxgi);
         if(FAILED(hr) || !state->context || !create_fences(ctx,state)) {
            destroy_state(state);state=nullptr;
         } else dev->windows_wsi=state;
      }
   }
   ReleaseSRWLockExclusive(&ctx->submit_lock);
   return state?state->device:nullptr;
}
static NTSTATUS fence_signal(windows_context *ctx,D3DKMT_HANDLE object,uint64_t value) {
   if(ctx->host.User)return ctx->host.Synchronize(ctx->host.User,object,value,TRUE);
   D3DKMT_SIGNALSYNCHRONIZATIONOBJECT2 signal{};
   signal.hContext=ctx->context;signal.ObjectCount=1;signal.ObjectHandleArray[0]=object;signal.Fence.FenceValue=value;
   return D3DKMTSignalSynchronizationObject2(&signal);
}
static NTSTATUS fence_wait(windows_context *ctx,D3DKMT_HANDLE object,uint64_t value) {
   if(ctx->host.User)return ctx->host.Synchronize(ctx->host.User,object,value,FALSE);
   D3DKMT_WAITFORSYNCHRONIZATIONOBJECT2 wait{};
   wait.hContext=ctx->context;wait.ObjectCount=1;wait.ObjectHandleArray[0]=object;wait.Fence.FenceValue=value;
   return D3DKMTWaitForSynchronizationObject2(&wait);
}
static void destroy_image(VkDevice handle,void *data) {
   VK_FROM_HANDLE(tu_device, dev, handle);auto state=(windows_wsi *)dev->windows_wsi;
   auto image=(A830_IMAGE *)data;if(!image)return;
   EnterCriticalSection(&state->lock);
   state->context->host.CloseImage(state->context->host.User,image);
   LeaveCriticalSection(&state->lock);delete image;
}
static VkResult create_image(VkDevice handle,void *resource,VkImage vk_image,
                            const VkAllocationCallbacks *alloc,VkDeviceMemory *memory,void **out) {
   VK_FROM_HANDLE(tu_device, dev, handle);auto state=(windows_wsi *)dev->windows_wsi;
   auto image=new(std::nothrow) A830_IMAGE{};if(!image)return VK_ERROR_OUT_OF_HOST_MEMORY;
   EnterCriticalSection(&state->lock);
   HRESULT hr=state->context->host.OpenImage(state->context->host.User,resource,image);
   LeaveCriticalSection(&state->lock);
   VkResult result=VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if(SUCCEEDED(hr)) {
      VkMemoryRequirements req{};dev->vk.dispatch_table.GetImageMemoryRequirements(handle,vk_image,&req);
      VkImageSubresource sub={VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};
      dev->vk.dispatch_table.GetImageSubresourceLayout(handle,vk_image,&sub,&layout);
      const auto &address=*image;
      if(address.Address && req.size<=address.Size && !layout.offset && layout.rowPitch==address.Pitch) {
         uint32_t type=0;while(type<dev->physical_device->memory.type_count && !(req.memoryTypeBits&(1u<<type)))type++;
         VkMemoryOpaqueCaptureAddressAllocateInfo replay={VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO};replay.opaqueCaptureAddress=address.Address;
         VkMemoryDedicatedAllocateInfo dedicated={VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.pNext=&replay;dedicated.image=vk_image;
         VkMemoryAllocateInfo info={VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};info.pNext=&dedicated;info.allocationSize=address.Size;info.memoryTypeIndex=type;
         result=type<dev->physical_device->memory.type_count?tu_AllocateMemory(handle,&info,alloc,memory):VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }
   if(result!=VK_SUCCESS){destroy_image(handle,image);return result;}
   *out=image;return VK_SUCCESS;
}
static VkResult prepare_image(VkDevice handle,void *first,void *second,uint32_t *selected) {
   VK_FROM_HANDLE(tu_device, dev, handle);auto ctx=(windows_context *)dev->windows_context;
   auto state=(windows_wsi *)dev->windows_wsi;
   auto a=(A830_IMAGE *)first;auto b=(A830_IMAGE *)second;
   EnterCriticalSection(&state->lock);
   A830_IMAGE current=*a;
   HRESULT hr=state->context->host.QueryImage(state->context->host.User,&current);
   if(FAILED(hr) || (current.Address!=a->Address && current.Address!=b->Address)) {
      LeaveCriticalSection(&state->lock);return VK_ERROR_OUT_OF_DATE_KHR;
   }
   *selected=current.Address==a->Address?0:1;
   /* DXGI queued the next buffer's reuse dependencies on its native context.
      Put the wait after application rendering, immediately before the transfer. */
   state->immediate->Flush();
   NTSTATUS status=fence_signal(state->context,state->present_available,++state->available_serial);
   if(status>=0) {
      AcquireSRWLockExclusive(&ctx->submit_lock);
      status=fence_wait(ctx,state->available,state->available_serial);
      ReleaseSRWLockExclusive(&ctx->submit_lock);
   }
   if(status<0) {
      LeaveCriticalSection(&state->lock);
      return vk_device_set_lost(&dev->vk,nullptr);
   }
   return VK_SUCCESS;
}
static void finish_present(VkDevice handle) {
   VK_FROM_HANDLE(tu_device, dev, handle);auto state=(windows_wsi *)dev->windows_wsi;
   LeaveCriticalSection(&state->lock);
}
static VkResult present_image(VkDevice handle,VkFence ready) {
   VK_FROM_HANDLE(tu_device, dev, handle);auto ctx=(windows_context *)dev->windows_context;
   auto state=(windows_wsi *)dev->windows_wsi;
   VK_FROM_HANDLE(vk_fence, fence, ready);
   VkResult result=vk_device_flush(&dev->vk);
   if(result==VK_SUCCESS)result=vk_sync_wait(&dev->vk,vk_fence_get_active_sync(fence),0,VK_SYNC_WAIT_PENDING,UINT64_MAX);
   if(result!=VK_SUCCESS)return result;
   AcquireSRWLockExclusive(&ctx->submit_lock);
   NTSTATUS status=fence_signal(ctx,state->ready,++state->ready_serial);
   ReleaseSRWLockExclusive(&ctx->submit_lock);
   if(status>=0)status=fence_wait(state->context,state->present_ready,state->ready_serial);
   return status>=0 && SUCCEEDED(state->device->GetDeviceRemovedReason())?
      VK_SUCCESS:vk_device_set_lost(&dev->vk,nullptr);
}
void tu_windows_wsi_init(wsi_device *wsi) {
   wsi->win32.get_d3d11_device=get_device;
   wsi->win32.create_d3d11_image=create_image;
   wsi->win32.destroy_d3d11_image=destroy_image;
   wsi->win32.prepare_d3d11_image=prepare_image;
   wsi->win32.finish_d3d11_present=finish_present;
   wsi->win32.present_d3d11_image=present_image;
   wsi->win32.signal_d3d11_acquire=tu_windows_wsi_signal_acquire;
   wsi->win32.requires_blits=[](VkDevice){return true;};
   wsi->disable_unordered_submits=true;
}
void tu_windows_wsi_finish(tu_device *dev) {
   auto state=(windows_wsi *)dev->windows_wsi;if(!state)return;
   destroy_state(state);dev->windows_wsi=nullptr;
}

static HRESULT WINAPI composition_create(VkDevice device,const void *descriptor,void **out) {
   if(!out || !descriptor)return E_INVALIDARG;
   *out=nullptr;
   auto native=(ID3D11Device *)get_device(device);
   if(!native)return DXGI_ERROR_DEVICE_REMOVED;
   HMODULE dxgi=LoadLibraryExW(L"dxgi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
   auto create=dxgi?(decltype(&CreateDXGIFactory1))GetProcAddress(dxgi,"CreateDXGIFactory1"):nullptr;
   IDXGIFactory2 *factory=nullptr;
   HRESULT hr=create?create(IID_PPV_ARGS(&factory)):E_FAIL;
   if(SUCCEEDED(hr)) {
      hr=factory->CreateSwapChainForComposition(native,
         (const DXGI_SWAP_CHAIN_DESC1 *)descriptor,nullptr,(IDXGISwapChain1 **)out);
      factory->Release();
   }
   if(dxgi)FreeLibrary(dxgi);
   return hr;
}
static void WINAPI composition_close_image(VkDevice handle,A830_COMPOSITION_IMAGE *image) {
   if(!image)return;
   VK_FROM_HANDLE(tu_device,dev,handle);
   dev->vk.dispatch_table.DestroyImage(handle,image->image,nullptr);
   dev->vk.dispatch_table.FreeMemory(handle,image->memory,nullptr);
   if(image->data)destroy_image(handle,image->data);
   *image={};
}
static VkResult WINAPI composition_open_image(VkDevice handle,void *swapchain,unsigned index,
                                               A830_COMPOSITION_IMAGE *out) {
   if(!out || !swapchain)return VK_ERROR_INITIALIZATION_FAILED;
   *out={};
   VK_FROM_HANDLE(tu_device,dev,handle);
   ID3D11Texture2D *texture=nullptr;
   HRESULT hr=((IDXGISwapChain1 *)swapchain)->GetBuffer(index,IID_PPV_ARGS(&texture));
   if(FAILED(hr))return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
   VkFormat format=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM?VK_FORMAT_B8G8R8A8_UNORM:
      desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_UNDEFINED;
   VkImageCreateInfo info={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
   info.imageType=VK_IMAGE_TYPE_2D;info.format=format;
   info.extent={desc.Width,desc.Height,1};info.mipLevels=info.arrayLayers=1;
   info.samples=VK_SAMPLE_COUNT_1_BIT;info.tiling=VK_IMAGE_TILING_LINEAR;
   info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
   VkResult result=format==VK_FORMAT_UNDEFINED?VK_ERROR_FORMAT_NOT_SUPPORTED:
      dev->vk.dispatch_table.CreateImage(handle,&info,nullptr,&out->image);
   if(result==VK_SUCCESS)result=create_image(handle,texture,out->image,nullptr,&out->memory,&out->data);
   if(result==VK_SUCCESS)result=dev->vk.dispatch_table.BindImageMemory(handle,out->image,out->memory,0);
   texture->Release();
   if(result!=VK_SUCCESS)composition_close_image(handle,out);
   return result;
}
static VkResult WINAPI composition_ready(VkDevice handle) {
   VK_FROM_HANDLE(tu_device,dev,handle);
   auto ctx=(windows_context *)dev->windows_context;
   auto state=(windows_wsi *)dev->windows_wsi;
   /* The caller drains the D3D12 submission worker, then holds its Vulkan
    * queue lock. Order the native present behind the GPU copy, without a
    * CPU wait for pixel data or GPU completion.
    */
   VkResult result=vk_device_flush(&dev->vk);
   if(result!=VK_SUCCESS)return result;
   AcquireSRWLockExclusive(&ctx->submit_lock);
   NTSTATUS status=fence_signal(ctx,state->ready,++state->ready_serial);
   ReleaseSRWLockExclusive(&ctx->submit_lock);
   if(status>=0)status=fence_wait(state->context,state->present_ready,state->ready_serial);
   return status>=0 && SUCCEEDED(state->device->GetDeviceRemovedReason())?
      VK_SUCCESS:vk_device_set_lost(&dev->vk,nullptr);
}
const void *VKAPI_CALL tu_windows_get_composition_api() {
   static const A830_COMPOSITION_API api={A830_COMPOSITION_VERSION,sizeof(A830_COMPOSITION_API),
      composition_create,composition_open_image,composition_close_image,prepare_image,
      composition_ready,finish_present};
   return &api;
}
