#pragma once
#include <d3d12.h>
#include "a830_composition.h"

/* These are the published vkd3d-proton interop interfaces. Keep their complete
 * base vtables: applications still receive ordinary ID3D12Resource objects.
 */
MIDL_INTERFACE("099a73fd-2199-4f45-bf48-0eb86f6fdb65") A830DeviceExt : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance*,VkPhysicalDevice*,VkDevice*)=0;
  virtual BOOL STDMETHODCALLTYPE GetExtensionSupport(UINT)=0;
  virtual HRESULT STDMETHODCALLTYPE CreateCubinComputeShaderWithName(const void*,UINT,UINT,UINT,UINT,const char*,void**)=0;
  virtual HRESULT STDMETHODCALLTYPE DestroyCubinComputeShader(void*)=0;
  virtual HRESULT STDMETHODCALLTYPE GetCudaTextureObject(D3D12_CPU_DESCRIPTOR_HANDLE,D3D12_CPU_DESCRIPTOR_HANDLE,UINT*)=0;
  virtual HRESULT STDMETHODCALLTYPE GetCudaSurfaceObject(D3D12_CPU_DESCRIPTOR_HANDLE,UINT*)=0;
  virtual HRESULT STDMETHODCALLTYPE CaptureUAVInfo(void*)=0;
  virtual HRESULT STDMETHODCALLTYPE CreateResourceFromBorrowedHandle(const D3D12_RESOURCE_DESC1*,UINT64,ID3D12Resource**)=0;
};
MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd") A830DeviceInterop : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID,void**)=0;
  virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT*,const char**)=0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT*,const char**)=0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2**)=0;
  virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance*,VkPhysicalDevice*,VkDevice*)=0;
  virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue*,VkQueue*,UINT*)=0;
  virtual void STDMETHODCALLTYPE GetVulkanImageLayout(ID3D12Resource*,D3D12_RESOURCE_STATES,VkImageLayout*)=0;
  virtual HRESULT STDMETHODCALLTYPE GetVulkanResourceInfo(ID3D12Resource*,UINT64*,UINT64*)=0;
  virtual HRESULT STDMETHODCALLTYPE LockCommandQueue(ID3D12CommandQueue*)=0;
  virtual HRESULT STDMETHODCALLTYPE UnlockCommandQueue(ID3D12CommandQueue*)=0;
};

namespace dxvk {
class DxgiCompositionPresenter : public ComObject<IDXGIVkSwapChain1> {
  struct Buffer {
    Com<ID3D12Resource> image;
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> commands;
    UINT64 serial=0;
  };
  struct Target {
    A830_COMPOSITION_IMAGE imported={};
    Com<ID3D12Resource> resource;
  };
  Com<ID3D12CommandQueue> m_queue;
  Com<ID3D12Device> m_device;
  Com<A830DeviceExt> m_ext;
  Com<A830DeviceInterop> m_interop;
  Com<IDXGISwapChain4> m_native;
  Com<ID3D12Fence> m_fence;
  const A830_COMPOSITION_API *m_api=nullptr;
  VkDevice m_vkDevice=VK_NULL_HANDLE;
  DXGI_SWAP_CHAIN_DESC1 m_desc={};
  std::vector<Buffer> m_buffers;
  Target m_targets[2];
  HANDLE m_event=nullptr;
  UINT m_index=0;
  UINT64 m_serial=0, m_presentCount=0, m_presentTime=0;

  HRESULT Wait(UINT64 serial) {
    if(!serial || m_fence->GetCompletedValue()>=serial)return S_OK;
    HRESULT hr=m_fence->SetEventOnCompletion(serial,m_event);
    if(FAILED(hr))return hr;
    if(WaitForSingleObject(m_event,30000)!=WAIT_OBJECT_0)return DXGI_ERROR_DEVICE_REMOVED;
    return m_device->GetDeviceRemovedReason();
  }
  HRESULT Drain() {
    HRESULT hr=m_queue->Signal(m_fence.ptr(),++m_serial);
    return FAILED(hr)?hr:Wait(m_serial);
  }
  void CloseTargets() {
    for(auto &target:m_targets) {
      target.resource=nullptr;
      if(m_api)m_api->close_image(m_vkDevice,&target.imported);
    }
  }
  HRESULT CreateBuffers(const DXGI_SWAP_CHAIN_DESC1 &desc) {
    D3D12_RESOURCE_DESC resource={};
    resource.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width=desc.Width;resource.Height=desc.Height;
    resource.DepthOrArraySize=resource.MipLevels=1;resource.Format=desc.Format;
    resource.SampleDesc.Count=1;resource.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if(desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)resource.Flags|=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap={};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask=heap.VisibleNodeMask=1;
    std::vector<Buffer> buffers(desc.BufferCount);
    HRESULT hr=S_OK;
    for(auto &buffer:buffers) {
      hr=m_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&resource,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&buffer.image));
      if(FAILED(hr))return hr;
      hr=m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&buffer.allocator));
      if(FAILED(hr))return hr;
      hr=m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,buffer.allocator.ptr(),nullptr,IID_PPV_ARGS(&buffer.commands));
      if(FAILED(hr))return hr;
      hr=buffer.commands->Close();if(FAILED(hr))return hr;
    }
    D3D12_RESOURCE_DESC1 native={};
    native.Dimension=resource.Dimension;native.Width=resource.Width;native.Height=resource.Height;
    native.DepthOrArraySize=native.MipLevels=1;native.Format=resource.Format;
    native.SampleDesc.Count=1;native.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
    for(unsigned i=0;i<2;i++) {
      VkResult result=m_api->open_image(m_vkDevice,m_native.ptr(),i,&m_targets[i].imported);
      if(result!=VK_SUCCESS){CloseTargets();return result==VK_ERROR_DEVICE_LOST?DXGI_ERROR_DEVICE_REMOVED:E_FAIL;}
      hr=m_ext->CreateResourceFromBorrowedHandle(&native,(UINT64)m_targets[i].imported.image,&m_targets[i].resource);
      if(FAILED(hr)){CloseTargets();return hr;}
    }
    m_buffers=std::move(buffers);m_desc=desc;m_index=0;
    return S_OK;
  }
public:
  ~DxgiCompositionPresenter() {
    if(m_fence != nullptr && m_queue != nullptr)Drain();
    m_buffers.clear();CloseTargets();
    if(m_event)CloseHandle(m_event);
  }
  HRESULT Init(IUnknown *queue,const DXGI_SWAP_CHAIN_DESC1 *desc,PFN_vkGetInstanceProcAddr loader) {
    if(!queue || !desc || !desc->Width || !desc->Height || desc->BufferCount<2 || desc->BufferCount>16 ||
       desc->SampleDesc.Count!=1 || desc->SampleDesc.Quality || desc->Stereo ||
       desc->Scaling!=DXGI_SCALING_STRETCH ||
       (desc->SwapEffect!=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL && desc->SwapEffect!=DXGI_SWAP_EFFECT_FLIP_DISCARD) ||
       (desc->AlphaMode!=DXGI_ALPHA_MODE_PREMULTIPLIED && desc->AlphaMode!=DXGI_ALPHA_MODE_IGNORE) ||
       (desc->Format!=DXGI_FORMAT_B8G8R8A8_UNORM && desc->Format!=DXGI_FORMAT_R8G8B8A8_UNORM))
      return DXGI_ERROR_INVALID_CALL;
    HRESULT hr=queue->QueryInterface(IID_PPV_ARGS(&m_queue));if(FAILED(hr))return DXGI_ERROR_UNSUPPORTED;
    if(m_queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)return DXGI_ERROR_UNSUPPORTED;
    hr=m_queue->GetDevice(IID_PPV_ARGS(&m_device));if(FAILED(hr))return hr;
    hr=m_device->QueryInterface(IID_PPV_ARGS(&m_ext));if(FAILED(hr))return hr;
    hr=m_device->QueryInterface(IID_PPV_ARGS(&m_interop));if(FAILED(hr))return hr;
    VkInstance instance=VK_NULL_HANDLE;VkPhysicalDevice physical=VK_NULL_HANDLE;
    hr=m_ext->GetVulkanHandles(&instance,&physical,&m_vkDevice);if(FAILED(hr))return hr;
    auto getApi=(A830_GET_COMPOSITION_API)loader(instance,"vkGetA830CompositionInterface");
    m_api=getApi?getApi():nullptr;
    if(!m_api || m_api->version!=A830_COMPOSITION_VERSION || m_api->size!=sizeof(*m_api))return DXGI_ERROR_UNSUPPORTED;
    DXGI_SWAP_CHAIN_DESC1 native=*desc;
    native.BufferCount=2;native.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    native.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    Com<IDXGISwapChain1> swapchain;
    hr=m_api->create(m_vkDevice,&native,reinterpret_cast<void**>(&swapchain));if(FAILED(hr))return hr;
    hr=swapchain->QueryInterface(IID_PPV_ARGS(&m_native));if(FAILED(hr))return hr;
    hr=m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence));if(FAILED(hr))return hr;
    m_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!m_event)return HRESULT_FROM_WIN32(GetLastError());
    return CreateBuffers(*desc);
  }
  IDXGISwapChain4 *Native(){return m_native.ptr();}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void **out) override {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid==__uuidof(IUnknown) || iid==__uuidof(IDXGIVkSwapChain) || iid==__uuidof(IDXGIVkSwapChain1)) {
      *out=ref(this);return S_OK;
    }
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC1 *desc) override {if(!desc)return E_POINTER;*desc=m_desc;return S_OK;}
  HRESULT STDMETHODCALLTYPE GetAdapter(REFIID iid,void **out) override {return m_interop->GetDXGIAdapter(iid,out);}
  HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void **out) override {return m_device->QueryInterface(iid,out);}
  HRESULT STDMETHODCALLTYPE GetImage(UINT index,REFIID iid,void **out) override {
    if(!out)return E_POINTER;*out=nullptr;
    return index<m_buffers.size()?m_buffers[index].image->QueryInterface(iid,out):DXGI_ERROR_INVALID_CALL;
  }
  UINT STDMETHODCALLTYPE GetImageIndex() override {return m_index;}
  UINT STDMETHODCALLTYPE GetFrameLatency() override {UINT latency=1;m_native->GetMaximumFrameLatency(&latency);return latency;}
  HANDLE STDMETHODCALLTYPE GetFrameLatencyEvent() override {return m_native->GetFrameLatencyWaitableObject();}
  HRESULT STDMETHODCALLTYPE SetFrameLatency(UINT latency) override {return m_native->SetMaximumFrameLatency(latency);}
  HRESULT STDMETHODCALLTYPE ChangeProperties(const DXGI_SWAP_CHAIN_DESC1 *desc,const UINT *nodes,IUnknown *const *queues) override {
    if(!desc || !desc->Width || !desc->Height || desc->BufferCount<2 || desc->BufferCount>16)return DXGI_ERROR_INVALID_CALL;
    if(nodes || queues)return DXGI_ERROR_UNSUPPORTED;
    HRESULT hr=Drain();if(FAILED(hr))return hr;
    /* Release every native texture reference before ResizeBuffers. Imported
     * VkImages and borrowed D3D12 resources do not own the native allocation.
     */
    CloseTargets();m_buffers.clear();
    hr=m_native->ResizeBuffers(2,desc->Width,desc->Height,desc->Format,desc->Flags);
    if(FAILED(hr))return hr;
    return CreateBuffers(*desc);
  }
  HRESULT STDMETHODCALLTYPE SetPresentRegion(const RECT *region) override {
    return region?m_native->SetSourceSize(region->right-region->left,region->bottom-region->top):E_INVALIDARG;
  }
  HRESULT STDMETHODCALLTYPE SetGammaControl(UINT,const DXGI_RGB*) override {return DXGI_ERROR_UNSUPPORTED;}
  HRESULT STDMETHODCALLTYPE Present(UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS *parameters) override {
    const DXGI_PRESENT_PARAMETERS empty={};
    if(!parameters)parameters=&empty;
    if(flags & DXGI_PRESENT_TEST)return m_native->Present1(interval,flags,parameters);
    if(m_buffers.empty())return DXGI_ERROR_INVALID_CALL;
    Buffer &buffer=m_buffers[m_index];HRESULT hr=Wait(buffer.serial);if(FAILED(hr))return hr;
    hr=m_interop->LockCommandQueue(m_queue.ptr());if(FAILED(hr))return hr;
    unsigned target=0;
    VkResult result=m_api->prepare(m_vkDevice,m_targets[0].imported.data,m_targets[1].imported.data,&target);
    m_interop->UnlockCommandQueue(m_queue.ptr());
    if(result!=VK_SUCCESS)return DXGI_ERROR_DEVICE_REMOVED;
    hr=buffer.allocator->Reset();
    if(SUCCEEDED(hr))hr=buffer.commands->Reset(buffer.allocator.ptr(),nullptr);
    if(SUCCEEDED(hr)) {
      D3D12_RESOURCE_BARRIER barriers[2]={};
      for(auto &barrier:barriers){barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
      barriers[0].Transition.pResource=buffer.image.ptr();barriers[0].Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
      barriers[1].Transition.pResource=m_targets[target].resource.ptr();barriers[1].Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
      buffer.commands->ResourceBarrier(2,barriers);
      D3D12_TEXTURE_COPY_LOCATION src={},dst={};
      src.pResource=buffer.image.ptr();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource=m_targets[target].resource.ptr();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      buffer.commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      for(auto &barrier:barriers)std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
      buffer.commands->ResourceBarrier(2,barriers);hr=buffer.commands->Close();
    }
    if(SUCCEEDED(hr)) {
      ID3D12CommandList *list=buffer.commands.ptr();m_queue->ExecuteCommandLists(1,&list);
      buffer.serial=++m_serial;hr=m_queue->Signal(m_fence.ptr(),buffer.serial);
    }
    if(SUCCEEDED(hr)) {
      hr=m_interop->LockCommandQueue(m_queue.ptr());
      if(SUCCEEDED(hr)) {
        result=m_api->ready(m_vkDevice);
        m_interop->UnlockCommandQueue(m_queue.ptr());
        if(result!=VK_SUCCESS)hr=DXGI_ERROR_DEVICE_REMOVED;
      }
    }
    if(SUCCEEDED(hr))hr=m_native->Present1(interval,flags,parameters);
    m_api->finish(m_vkDevice);
    if(SUCCEEDED(hr)) {
      m_index=(m_index+1)%m_buffers.size();m_presentCount++;
      LARGE_INTEGER time;QueryPerformanceCounter(&time);m_presentTime=time.QuadPart;
    }
    return hr;
  }
  UINT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE color) override {
    UINT support=0;m_native->CheckColorSpaceSupport(color,&support);return support;
  }
  HRESULT STDMETHODCALLTYPE SetColorSpace(DXGI_COLOR_SPACE_TYPE color) override {return m_native->SetColorSpace1(color);}
  HRESULT STDMETHODCALLTYPE SetHDRMetaData(const DXGI_VK_HDR_METADATA *metadata) override {
    return metadata?m_native->SetHDRMetaData(metadata->Type,metadata->Type==DXGI_HDR_METADATA_TYPE_HDR10?sizeof(metadata->HDR10):0,
      metadata->Type==DXGI_HDR_METADATA_TYPE_HDR10?const_cast<DXGI_HDR_METADATA_HDR10*>(&metadata->HDR10):nullptr):E_INVALIDARG;
  }
  void STDMETHODCALLTYPE GetLastPresentCount(UINT64 *count) override {if(count)*count=m_presentCount;}
  void STDMETHODCALLTYPE GetFrameStatistics(DXGI_VK_FRAME_STATISTICS *stats) override {
    if(stats){stats->PresentCount=m_presentCount;stats->PresentQPCTime=m_presentTime;}
  }
};
}
