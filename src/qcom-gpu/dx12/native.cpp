#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3d12umddi.h>
#include <new>
#include <vector>
#include <deque>
#include <unordered_map>
#include <cstring>
#include <array>
#include <algorithm>
#include <memory>
#include <functional>
#ifndef LIBVKD3D_SHADER_SOURCE
#define LIBVKD3D_SHADER_SOURCE
#endif
#include <vkd3d_shader.h>
#include "../../mesa/signature.h"
#include "../wddm/abi.h"
#include "../../mesa/a830_completion.h"
#include "../wddm/gpu-fence-builder.h"
#include "../../mesa/a830_native.h"
#include "../../mesa/umd/module.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"

namespace {
template<class T,class H> T *object(H h){return static_cast<T *>(h.pDrvPrivate);}
struct Adapter { HANDLE runtime;D3D12DDI_HRTTABLE command_table{}; };
struct Queue;
struct Resource;
struct State;
struct Recording { std::vector<std::function<HRESULT(Queue *)>> ops; };
struct Device {
    Adapter *adapter;
    D3D12DDI_HRTDEVICE runtime;
    D3DDDI_DEVICECALLBACKS kernel;
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0003 core;
    D3DDDICB_CREATEPAGINGQUEUE paging{};
    SRWLOCK memory_lock{};
    struct Memory { GPU_ALLOCATION allocation; HANDLE resource;bool owned=true,mapped=true; };
    std::unordered_map<UINT,Memory> allocations;
    std::vector<Resource *> resources;
    HMODULE backend=nullptr;
    pipe_screen *screen=nullptr;
    pipe_context *compiler=nullptr;
    SRWLOCK compiler_lock{};
    decltype(&A830GpuScreen) gpu_screen=nullptr;
    decltype(&A830GpuDestroyScreen) gpu_destroy_screen=nullptr;
    decltype(&A830GpuContext) gpu_context=nullptr;
    decltype(&A830GpuResource) gpu_resource=nullptr;
    decltype(&A830GpuShader) gpu_shader=nullptr;
    decltype(&A830GpuShaderStreamOutput) gpu_shader_so=nullptr;
    decltype(&A830GpuStreamTarget) gpu_stream_target=nullptr;
    decltype(&A830GpuViews) gpu_views=nullptr;
    decltype(&A830GpuClear) gpu_clear=nullptr;
};
struct Heap {
    Device *device;
    GPU_ALLOCATION memory;
    UINT offset;
    UINT64 size;
    D3D12DDI_CPU_PAGE_PROPERTY cpu;
};
struct Resource { Heap *heap;UINT64 offset,size;pipe_resource *pipe;A830_NATIVE_LAYOUT layout;D3D12DDIARG_CREATERESOURCE_0003 desc; };
struct Fence { Device *device; UINT64 address; };
struct Allocator {
    Device *device;
    UINT64 generation=1;
};
struct List {
    Device *device;
    Allocator *allocator;
    D3D12DDI_HRTCOMMANDLIST runtime;
    UINT64 generation;
    HRESULT failure=S_OK;
    bool closed=false;
    std::shared_ptr<Recording> recording;
};
struct Work {
    enum Type { Execute,Signal,Wait } type;
    UINT64 address=0,value=0;
    std::vector<std::shared_ptr<Recording>> recordings;
};
struct Queue {
    Device *device;
    D3D12DDI_HRTCOMMANDQUEUE runtime;
    HANDLE context,event;
    UINT generation;
    UINT64 submitted;
    volatile A830_COMPLETION *page;
    SRWLOCK lock;
    PTP_WORK worker;
    bool working;
    volatile LONG stopping,failure;
    std::deque<Work> pending;
    pipe_context *draw=nullptr;
    std::shared_ptr<State> state;
    std::vector<void *> samplers;
    void *vertex_layout=nullptr;
    std::shared_ptr<void> bound_pipeline;
    void *shaders[6]={};
    struct Retired {GPU_ALLOCATION memory;UINT64 fence;};
    std::deque<Retired> retired;
};
void release(Device *d,GPU_ALLOCATION &a) {
    if(!a.Handle)return;
    AcquireSRWLockExclusive(&d->memory_lock);auto it=d->allocations.find(a.Handle);
    HANDLE resource=it==d->allocations.end()?nullptr:it->second.resource;
    bool owned=it==d->allocations.end() || it->second.owned,mapped=it==d->allocations.end() || it->second.mapped;
    if(it!=d->allocations.end())d->allocations.erase(it);ReleaseSRWLockExclusive(&d->memory_lock);
    if(mapped){D3DDDICB_FREEGPUVIRTUALADDRESS free_va{a.Address,a.Size};d->kernel.pfnFreeGpuVirtualAddressCb(d->runtime.handle,&free_va);}
    if(mapped && a.Map){D3DDDICB_UNLOCK2 unlock{a.Handle};d->kernel.pfnUnlock2Cb(d->runtime.handle,&unlock);}
    D3DDDICB_DEALLOCATE2 deallocate{};deallocate.hResource=resource;deallocate.NumAllocations=1;deallocate.HandleList=&a.Handle;
    if(owned)d->kernel.pfnDeallocate2Cb(d->runtime.handle,&deallocate);
    a={};
}
HRESULT paging_wait(Device *d,UINT64 value) {
    volatile UINT64 *completed=static_cast<volatile UINT64 *>(d->paging.FenceValueCPUVirtualAddress);
    if(!completed)return DXGI_ERROR_DEVICE_REMOVED;
    ULONGLONG deadline=GetTickCount64()+10000;
    __try {
        while(*completed<value){if(GetTickCount64()>=deadline)return DXGI_ERROR_DEVICE_HUNG;Sleep(1);MemoryBarrier();}
        return S_OK;
    } __except(EXCEPTION_EXECUTE_HANDLER){return DXGI_ERROR_DEVICE_REMOVED;}
}
HRESULT map_memory(Device *d,GPU_ALLOCATION &a,HANDLE resource,bool owned) {
    D3DDDI_MAPGPUVIRTUALADDRESS map{};map.hPagingQueue=d->paging.hPagingQueue;map.hAllocation=a.Handle;
    map.MinimumAddress=GPU_CLIENT_VA_BASE;map.MaximumAddress=0x7fffffffffULL;map.SizeInPages=a.Size/4096;
    map.Protection.Write=1;map.Protection.Execute=1;
    HRESULT hr=d->kernel.pfnMapGpuVirtualAddressCb(d->runtime.handle,&map);
    if(hr==E_PENDING || SUCCEEDED(hr)){a.Address=map.VirtualAddress;hr=paging_wait(d,map.PagingFenceValue);}
    if(SUCCEEDED(hr)) {
        D3DDDI_MAKERESIDENT resident{};resident.hPagingQueue=d->paging.hPagingQueue;resident.NumAllocations=1;resident.AllocationList=&a.Handle;
        hr=d->kernel.pfnMakeResidentCb(d->runtime.handle,&resident);
        if(hr==E_PENDING || SUCCEEDED(hr))hr=paging_wait(d,resident.PagingFenceValue);
    }
    if(SUCCEEDED(hr)) {
        D3DDDICB_LOCK2 lock{};lock.hAllocation=a.Handle;hr=d->kernel.pfnLock2Cb(d->runtime.handle,&lock);
        if(SUCCEEDED(hr))a.Map=static_cast<UINT64>(reinterpret_cast<uintptr_t>(lock.pData));
    }
    if(SUCCEEDED(hr) && (!a.Handle || !a.Map || (a.Address&65535) || a.Address<GPU_CLIENT_VA_BASE ||
        a.Address>=0x8000000000ULL || a.Size>0x8000000000ULL-a.Address))hr=DXGI_ERROR_DEVICE_REMOVED;
    if(SUCCEEDED(hr))try {AcquireSRWLockExclusive(&d->memory_lock);d->allocations.emplace(a.Handle,Device::Memory{a,resource,owned,true});ReleaseSRWLockExclusive(&d->memory_lock);}
    catch(const std::bad_alloc &){ReleaseSRWLockExclusive(&d->memory_lock);hr=E_OUTOFMEMORY;}
    if(FAILED(hr)) {
        if(a.Address){D3DDDICB_FREEGPUVIRTUALADDRESS free_va{a.Address,a.Size};d->kernel.pfnFreeGpuVirtualAddressCb(d->runtime.handle,&free_va);}
        if(a.Map){D3DDDICB_UNLOCK2 unlock{a.Handle};d->kernel.pfnUnlock2Cb(d->runtime.handle,&unlock);}
        D3DDDICB_DEALLOCATE2 deallocate{};deallocate.hResource=resource;deallocate.NumAllocations=1;deallocate.HandleList=&a.Handle;
        if(owned)d->kernel.pfnDeallocate2Cb(d->runtime.handle,&deallocate);a={};
    }
    return hr;
}
HRESULT allocate(Device *d,UINT bytes,GPU_ALLOCATION &a,D3D12DDI_CPU_PAGE_PROPERTY cpu=D3D12DDI_CPU_PAGE_PROPERTY_WRITE_COMBINE,
    HANDLE resource=nullptr,const A830_DX12_RESOURCE *description=nullptr) {
    if(!bytes || bytes>GPU_MAX_BUFFER_SIZE)return E_INVALIDARG;
    if(bytes>GPU_MAX_BUFFER_SIZE-65535)return E_OUTOFMEMORY;
    bytes=(bytes+65535)&~65535u;
    A830_DX12_RESOURCE data=description?*description:A830_DX12_RESOURCE{};
    data.Resource.Version=A830_ABI_VERSION;data.Resource.Size=bytes;data.Resource.Flags|=A830_RESOURCE_NATIVE;data.Resource.Rotation=cpu;
    D3DDDI_ALLOCATIONINFO2 info{};info.pPrivateDriverData=&data;info.PrivateDriverDataSize=sizeof(data);
    info.Flags.Primary=(data.Resource.Flags&A830_RESOURCE_PRIMARY)!=0;
    D3DDDICB_ALLOCATE request{};request.hResource=resource;request.NumAllocations=1;request.pAllocationInfo2=&info;
    request.pPrivateDriverData=&data;request.PrivateDriverDataSize=sizeof(data);
    HRESULT hr=d->kernel.pfnAllocateCb(d->runtime.handle,&request);if(FAILED(hr))return hr;
    a={info.hAllocation,bytes,0,0};
    return map_memory(d,a,resource,true);
}
void error(Device *d,HRESULT hr){d->core.pfnSetErrorCb(D3D10DDI_HRTDEVICE{d->runtime.handle},hr);}
void error(List *l,HRESULT hr) {
    if(SUCCEEDED(l->failure)){l->failure=hr;l->device->core.pfnSetCommandListErrorCb(l->runtime,hr);}
}
UINT64 address(const Resource *r){return r->heap->memory.Address+r->heap->offset+r->offset;}
#include "resources.h"
SIZE_T APIENTRY allocator_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATECOMMANDALLOCATOR *){return sizeof(Allocator);}
HRESULT APIENTRY create_allocator(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATECOMMANDALLOCATOR *p) {
    if(p->Type>D3D12DDI_COMMAND_LIST_TYPE_BUNDLE || (p->QueueFlags&~7u))return DXGI_ERROR_UNSUPPORTED;
    auto *a=new(p->hDrvCommandAllocator.pDrvPrivate) Allocator{};a->device=object<Device>(hd);return S_OK;
}
void APIENTRY reset_allocator(D3D12DDI_HCOMMANDALLOCATOR h) {
    Allocator *a=object<Allocator>(h);++a->generation;
}
void APIENTRY destroy_allocator(D3D12DDI_HDEVICE,D3D12DDI_HCOMMANDALLOCATOR h){reset_allocator(h);object<Allocator>(h)->~Allocator();}
SIZE_T APIENTRY list_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *){return sizeof(List);}
HRESULT APIENTRY create_list(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_COMMAND_LIST_0001 *p) {
    if(p->Type>D3D12DDI_COMMAND_LIST_TYPE_BUNDLE || (p->QueueFlags&~7u) || p->NodeMask!=1)return DXGI_ERROR_UNSUPPORTED;
    Allocator *a=object<Allocator>(p->hDrvCommandAllocator);
    if(!a || a->device!=object<Device>(hd))return E_INVALIDARG;
    List *l=new(p->hDrvCommandList.pDrvPrivate) List{a->device,a,p->hRTCommandList,a->generation};
    try {l->recording=std::make_shared<Recording>();}catch(const std::bad_alloc &){l->~List();return E_OUTOFMEMORY;}
    l->device->core.pfnSetCommandListDDITableCb(l->runtime,l->device->adapter->command_table);return S_OK;
}
void APIENTRY destroy_list(D3D12DDI_HDEVICE,D3D12DDI_HCOMMANDLIST h){object<List>(h)->~List();}
void APIENTRY reset_list(D3D12DDI_HCOMMANDLIST h,const D3D12DDIARG_RESETCOMMANDLIST *p) {
    List *l=object<List>(h);Allocator *a=object<Allocator>(p->hDrvCommandAllocator);
    if(!l->closed || !a || a->device!=l->device){error(l,E_INVALIDARG);return;}
    try {l->recording=std::make_shared<Recording>();}catch(const std::bad_alloc &){error(l,E_OUTOFMEMORY);return;}
    l->allocator=a;l->generation=a->generation;l->failure=S_OK;l->closed=false;
}
void APIENTRY close_list(D3D12DDI_HCOMMANDLIST h) {
    List *l=object<List>(h);if(l->closed || l->generation!=l->allocator->generation){error(l,E_INVALIDARG);return;}l->closed=true;
}
template<class F> void record(List *l,F &&op) {
    if(l->closed || l->generation!=l->allocator->generation){error(l,E_INVALIDARG);return;}
    if(FAILED(l->failure))return;
    try {l->recording->ops.emplace_back(std::forward<F>(op));}catch(const std::bad_alloc &){error(l,E_OUTOFMEMORY);}
}
#include "shaders.h"
#include "descriptors.h"
#include "state.h"
#include "stream-output.h"
#include "queries.h"
#include "drawing.h"
#include "indirect.h"
#include "texture-copy.h"
#include "residency.h"
#include "interop.h"
#include "capabilities.h"
HRESULT queue_status(Queue *q,UINT64 *completed=nullptr) {
    HRESULT failure=InterlockedCompareExchange(&q->failure,0,0);if(FAILED(failure))return failure;
    A830_COMPLETION page;
    if(!a830_read_completion(q->page,&page) || page.Status || page.Generation!=q->generation)return DXGI_ERROR_DEVICE_REMOVED;
    if(completed)*completed=page.Completed;return S_OK;
}
void flush(Queue *q) {
    pipe_fence_handle *fence=nullptr;q->draw->flush(q->draw,&fence,0);
    if(fence) {
        q->device->screen->fence_finish(q->device->screen,q->draw,fence,0);
        q->device->screen->fence_reference(q->device->screen,&fence,nullptr);
    }
}
HRESULT WINAPI host_allocate(void *user,unsigned bytes,unsigned,GPU_ALLOCATION *out){return allocate(static_cast<Device *>(user),bytes,*out);}
void WINAPI host_free(void *user,const GPU_ALLOCATION *memory){GPU_ALLOCATION copy=*memory;release(static_cast<Device *>(user),copy);}
HRESULT WINAPI host_create(void *device,void *queue,void **out) {
    if(queue){*out=queue;return S_OK;}
    Device *d=static_cast<Device *>(device);Queue *q;
    try {q=new Queue{};}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
    q->device=d;q->event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    A830_CONTEXT_DATA data{A830_ABI_VERSION,0,static_cast<UINT64>(reinterpret_cast<uintptr_t>(q->event)),0};
    D3DDDICB_CREATECONTEXTVIRTUAL context{};context.NodeOrdinal=1;context.pPrivateDriverData=&data;context.PrivateDriverDataSize=sizeof(data);
    HRESULT hr=q->event?d->kernel.pfnCreateContextVirtualCb(d->runtime.handle,&context):E_OUTOFMEMORY;
    if(SUCCEEDED(hr)) {
        q->context=context.hContext;
        if(!data.CompletionPage) {
            A830_CONTEXT_QUERY query{A830_ABI_VERSION,0,0};D3DDDICB_ESCAPE e{};e.hDevice=d->runtime.handle;e.hContext=q->context;
            e.pPrivateDriverData=&query;e.PrivateDriverDataSize=sizeof(query);hr=d->kernel.pfnEscapeCb(d->adapter->runtime,&e);data.CompletionPage=query.CompletionPage;
        }
        q->page=reinterpret_cast<volatile A830_COMPLETION *>(static_cast<uintptr_t>(data.CompletionPage));
        A830_COMPLETION page;
        if(SUCCEEDED(hr) && a830_read_completion(q->page,&page) && !page.Status){q->generation=page.Generation;*out=q;return S_OK;}
        if(SUCCEEDED(hr))hr=DXGI_ERROR_DEVICE_REMOVED;
    }
    if(q->context){D3DDDICB_DESTROYCONTEXT destroy{q->context};d->kernel.pfnDestroyContextCb(d->runtime.handle,&destroy);}
    if(q->event)CloseHandle(q->event);delete q;return hr;
}
void WINAPI host_destroy(void *user) {
    Queue *q=static_cast<Queue *>(user);
    D3DDDICB_DESTROYCONTEXT destroy{q->context};q->device->kernel.pfnDestroyContextCb(q->device->runtime.handle,&destroy);
    CloseHandle(q->event);delete q;
}
HRESULT submit(Queue *q,const GPU_SUBMIT *commands,UINT count) {
    HRESULT hr=queue_status(q);if(FAILED(hr))return hr;
    if(!count || count>256 || q->submitted>=INT32_MAX)return E_INVALIDARG;
    for(UINT i=0;i<count && SUCCEEDED(hr);i++) {
        Device *d=q->device;AcquireSRWLockShared(&d->memory_lock);auto it=d->allocations.find(commands[i].Handle);
        GPU_ALLOCATION memory=it==d->allocations.end()?GPU_ALLOCATION{}:it->second.allocation;ReleaseSRWLockShared(&d->memory_lock);
        if(!memory.Handle || !commands[i].Words || commands[i].Words>0xfffff || commands[i].Offset>memory.Size ||
            commands[i].Words>(memory.Size-commands[i].Offset)/4)return E_INVALIDARG;
        D3DDDICB_SUBMITCOMMAND request{};request.Commands=memory.Address+commands[i].Offset;request.CommandLength=commands[i].Words*4;
        request.BroadcastContextCount=1;request.BroadcastContext[0]=q->context;
        hr=d->kernel.pfnSubmitCommandCb(d->runtime.handle,&request);if(SUCCEEDED(hr))++q->submitted;
    }
    return hr;
}
HRESULT WINAPI host_submit(void *user,const GPU_SUBMIT *commands,unsigned count,UINT64 *fence) {
    Queue *q=static_cast<Queue *>(user);HRESULT hr=submit(q,commands,count);*fence=q->submitted;
    if(FAILED(hr))InterlockedCompareExchange(&q->failure,hr,0);return hr;
}
HRESULT WINAPI host_wait(void *user,UINT64 fence,UINT64 timeout) {
    Queue *q=static_cast<Queue *>(user);ULONGLONG start=GetTickCount64();
    for(;;) {
        UINT64 completed;HRESULT hr=queue_status(q,&completed);if(FAILED(hr))return hr;
        if(completed>=fence)return S_OK;
        if(timeout!=UINT64_MAX && (GetTickCount64()-start)*1000000>=timeout)return S_FALSE;
        ResetEvent(q->event);DWORD status=WaitForSingleObject(q->event,1);
        if(status!=WAIT_OBJECT_0 && status!=WAIT_TIMEOUT)return DXGI_ERROR_DEVICE_REMOVED;
    }
}
A830_NATIVE_HOST native_host(Device *d,Queue *q=nullptr) {
    return {1,sizeof(A830_NATIVE_HOST),d,q,host_allocate,host_free,host_create,host_submit,host_wait,host_destroy};
}
bool load_backend(Device *d) {
    HMODULE self=nullptr;WCHAR path[MAX_PATH];
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
       reinterpret_cast<LPCWSTR>(load_backend),&self) || !GetModuleFileNameW(self,path,MAX_PATH))return false;
    WCHAR *tail=wcsrchr(path,L'\\');if(!tail)return false;tail[1]=0;if(wcscat_s(path,A830_MODULE(L"a830_gpu")))return false;
    d->backend=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!d->backend)return false;
#define GPU_LOAD(field,name) d->field=reinterpret_cast<decltype(d->field)>(GetProcAddress(d->backend,name));if(!d->field)return false;
    GPU_LOAD(gpu_screen,"A830GpuScreen");GPU_LOAD(gpu_context,"A830GpuContext");
    GPU_LOAD(gpu_destroy_screen,"A830GpuDestroyScreen");
    GPU_LOAD(gpu_resource,"A830GpuResource");GPU_LOAD(gpu_shader,"A830GpuShader");
    GPU_LOAD(gpu_shader_so,"A830GpuShaderStreamOutput");GPU_LOAD(gpu_stream_target,"A830GpuStreamTarget");
    GPU_LOAD(gpu_views,"A830GpuViews");
    GPU_LOAD(gpu_clear,"A830GpuClear");
#undef GPU_LOAD
    A830_NATIVE_HOST host=native_host(d);d->screen=d->gpu_screen(&host);if(!d->screen)return false;
    d->compiler=d->gpu_context(d->screen,&host);return d->compiler!=nullptr;
}
SIZE_T APIENTRY fence_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_FENCE *){return sizeof(Fence);}
HRESULT APIENTRY create_fence(D3D12DDI_HDEVICE hd,D3D12DDI_HFENCE hf,const D3D12DDIARG_CREATE_FENCE *p) {
    if(!hf.pDrvPrivate || !p || p->FenceCount!=1 || !p->Fences)return E_INVALIDARG;
    const auto &f=p->Fences[0];
    if((f.Flags&~D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE) ||
       ((f.FenceValue.BaseAddress || f.FenceMonitoredValue.BaseAddress) &&
        (!A830GpuFenceAddress(f.FenceValue.BaseAddress) || !A830GpuFenceAddress(f.FenceMonitoredValue.BaseAddress))))return E_INVALIDARG;
    new(hf.pDrvPrivate) Fence{object<Device>(hd),f.FenceValue.BaseAddress};return S_OK;
}
void APIENTRY destroy_fence(D3D12DDI_HDEVICE,D3D12DDI_HFENCE h){object<Fence>(h)->~Fence();}
HRESULT signal(Queue *q,UINT64 address,UINT64 value) {
    flush(q);
    UINT64 completed;HRESULT hr=queue_status(q,&completed);if(FAILED(hr))return hr;
    while(!q->retired.empty() && q->retired.front().fence<=completed){release(q->device,q->retired.front().memory);q->retired.pop_front();}
    UINT words[12];if(!A830BuildGpuSignal(words,12,address,value))return E_INVALIDARG;
    GPU_ALLOCATION memory{};hr=allocate(q->device,sizeof(words),memory);
    if(SUCCEEDED(hr)) {
        memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(memory.Map)),words,sizeof(words));MemoryBarrier();
        try {q->retired.push_back({memory,UINT64_MAX});}catch(const std::bad_alloc &){release(q->device,memory);return E_OUTOFMEMORY;}
        GPU_SUBMIT command{memory.Handle,0,12,0};hr=submit(q,&command,1);
        if(SUCCEEDED(hr))q->retired.back().fence=q->submitted;
        else {q->retired.pop_back();release(q->device,memory);}
    }
    return hr;
}
HRESULT fence_ready(Queue *q,UINT64 address,UINT64 value) {
    HRESULT hr=queue_status(q);if(FAILED(hr))return hr;
    A830_FENCE_QUERY query{A830_ABI_VERSION,0,address,0};D3DDDICB_ESCAPE escape{};escape.hDevice=q->device->runtime.handle;escape.hContext=q->context;
    escape.pPrivateDriverData=&query;escape.PrivateDriverDataSize=sizeof(query);hr=q->device->kernel.pfnEscapeCb(q->device->adapter->runtime,&escape);
    return FAILED(hr)?hr:query.Value>=value?S_OK:S_FALSE;
}
HRESULT execute_work(Queue *q,const Work &work) {
    if(work.type==Work::Signal)return signal(q,work.address,work.value);
    for(const auto &recording:work.recordings) {
        try {q->state=std::make_shared<State>();}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
        for(const auto &op:recording->ops){HRESULT hr=op(q);if(FAILED(hr))return hr;}
    }
    flush(q);return queue_status(q);
}
void CALLBACK queue_worker(PTP_CALLBACK_INSTANCE,void *context,PTP_WORK) {
    Queue *q=static_cast<Queue *>(context);
    for(;;) {
        AcquireSRWLockExclusive(&q->lock);
        if(q->pending.empty() || q->stopping){q->pending.clear();q->working=false;ReleaseSRWLockExclusive(&q->lock);return;}
        Work work=std::move(q->pending.front());q->pending.pop_front();HRESULT hr;
        if(work.type==Work::Wait) {
            ReleaseSRWLockExclusive(&q->lock);
            for(;;) {
                if(InterlockedCompareExchange(&q->stopping,0,0)){hr=E_ABORT;break;}
                hr=fence_ready(q,work.address,work.value);if(hr!=S_FALSE)break;
                ResetEvent(q->event);
                DWORD result=WaitForSingleObject(q->event,10);
                if(result!=WAIT_OBJECT_0 && result!=WAIT_TIMEOUT){hr=DXGI_ERROR_DEVICE_REMOVED;break;}
            }
        } else {hr=execute_work(q,work);ReleaseSRWLockExclusive(&q->lock);}
        if(FAILED(hr)) {
            AcquireSRWLockExclusive(&q->lock);q->pending.clear();bool stopping=q->stopping!=0;
            if(!stopping)InterlockedCompareExchange(&q->failure,hr,0);
            q->working=false;ReleaseSRWLockExclusive(&q->lock);if(!stopping)error(q->device,hr);return;
        }
    }
}
HRESULT enqueue(Queue *q,Work &&work) {
    if(q->stopping)return DXGI_ERROR_DEVICE_REMOVED;
    HRESULT hr=queue_status(q);if(FAILED(hr))return hr;
    try {q->pending.push_back(std::move(work));}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
    if(!q->working){q->working=true;SubmitThreadpoolWork(q->worker);}return S_OK;
}
void fence_operation(D3D12DDI_HCOMMANDQUEUE h,D3D12DDIARG_FENCE_OPERATION *p,bool wait) {
    Queue *q=object<Queue>(h);Fence *f=p?object<Fence>(p->Fence):nullptr;
    if(!f || f->device!=q->device || p->Value==UINT64_MAX){error(q->device,E_INVALIDARG);return;}
    AcquireSRWLockExclusive(&q->lock);HRESULT hr=queue_status(q);
    if(SUCCEEDED(hr)) {
        if(q->working)hr=enqueue(q,Work{wait?Work::Wait:Work::Signal,f->address,p->Value});
        else if(!wait)hr=signal(q,f->address,p->Value);
        else {hr=fence_ready(q,f->address,p->Value);if(hr==S_FALSE)hr=enqueue(q,Work{Work::Wait,f->address,p->Value});}
    }
    ReleaseSRWLockExclusive(&q->lock);if(FAILED(hr))error(q->device,hr);else p->PhysicalAdapterMask=1;
}
void APIENTRY signal_fence(D3D12DDI_HCOMMANDQUEUE h,D3D12DDIARG_FENCE_OPERATION *p){fence_operation(h,p,false);}
void APIENTRY wait_fence(D3D12DDI_HCOMMANDQUEUE h,D3D12DDIARG_FENCE_OPERATION *p){fence_operation(h,p,true);}
HRESULT queue_wait(Queue *q) {
    HRESULT hr=host_wait(q,q->submitted,10000000000ULL);
    return hr==S_FALSE?DXGI_ERROR_DEVICE_HUNG:hr;
}
SIZE_T APIENTRY queue_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *){return sizeof(Queue);}
HRESULT APIENTRY create_queue(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATECOMMANDQUEUE_0001 *p) {
    if(!p->QueueFlags || (p->QueueFlags&~7u) || p->NodeMask!=1)return DXGI_ERROR_UNSUPPORTED;
    Device *d=object<Device>(hd);Queue *q;
    try {q=new(p->hDrvCommandQueue.pDrvPrivate) Queue{};}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
    q->device=d;q->runtime=p->hRTCommandQueue;InitializeSRWLock(&q->lock);
    q->worker=CreateThreadpoolWork(queue_worker,q,nullptr);
    q->event=CreateEventW(nullptr,TRUE,FALSE,nullptr);HRESULT hr=E_OUTOFMEMORY;
    if(q->worker && q->event) {
        hr=DXGI_ERROR_DEVICE_REMOVED;
        {
            A830_CONTEXT_DATA data{A830_ABI_VERSION,0,static_cast<UINT64>(reinterpret_cast<uintptr_t>(q->event)),0};
            D3DDDICB_CREATECONTEXTVIRTUAL context{};context.NodeOrdinal=1;context.pPrivateDriverData=&data;context.PrivateDriverDataSize=sizeof(data);
            hr=d->core.pfnCreateContextVirtualCb(q->runtime,&context);
            if(SUCCEEDED(hr)) {
                q->context=context.hContext;
                if(!data.CompletionPage) {
                    A830_CONTEXT_QUERY query{A830_ABI_VERSION,0,0};D3DDDICB_ESCAPE e{};
                    e.hDevice=d->runtime.handle;e.hContext=q->context;e.pPrivateDriverData=&query;e.PrivateDriverDataSize=sizeof(query);
                    hr=d->kernel.pfnEscapeCb(d->adapter->runtime,&e);data.CompletionPage=query.CompletionPage;
                }
                q->page=reinterpret_cast<volatile A830_COMPLETION *>(static_cast<uintptr_t>(data.CompletionPage));
                A830_COMPLETION page;
                if(SUCCEEDED(hr) && a830_read_completion(q->page,&page)) {
                    q->generation=page.Generation;hr=queue_status(q);
                    if(SUCCEEDED(hr)){A830_NATIVE_HOST host=native_host(d,q);q->draw=d->gpu_context(d->screen,&host);if(q->draw)return S_OK;hr=E_OUTOFMEMORY;}
                }
                if(SUCCEEDED(hr))hr=DXGI_ERROR_DEVICE_REMOVED;
            }
        }
    }
    if(q->context){D3DDDICB_DESTROYCONTEXT context{q->context};d->core.pfnDestroyContextCb(q->runtime,&context);}
    if(q->worker)CloseThreadpoolWork(q->worker);
    if(q->event)CloseHandle(q->event);q->~Queue();return hr;
}
void APIENTRY destroy_queue(D3D12DDI_HDEVICE,D3D12DDI_HCOMMANDQUEUE h) {
    Queue *q=object<Queue>(h);
    InterlockedExchange(&q->stopping,1);SetEvent(q->event);
    WaitForThreadpoolWorkCallbacks(q->worker,FALSE);CloseThreadpoolWork(q->worker);
    if(q->draw) {
        flush(q);release_samplers(q);
        delete_queue_shaders(q);
        q->draw->bind_vertex_elements_state(q->draw,nullptr);
        if(q->vertex_layout)q->draw->delete_vertex_elements_state(q->draw,q->vertex_layout);
        q->draw->destroy(q->draw);
    }
    if(q->submitted){HRESULT hr=queue_wait(q);if(FAILED(hr))error(q->device,hr);}
    if(q->context){D3DDDICB_DESTROYCONTEXT context{q->context};q->device->core.pfnDestroyContextCb(q->runtime,&context);}
    for(auto &retired:q->retired)release(q->device,retired.memory);
    if(q->event)CloseHandle(q->event);
    q->~Queue();
}
void APIENTRY execute(D3D12DDI_HCOMMANDQUEUE h,UINT count,const D3D12DDI_HCOMMANDLIST *lists) {
    Queue *q=object<Queue>(h);HRESULT hr=queue_status(q);
    if(count && !lists)hr=E_INVALIDARG;
    for(UINT i=0;SUCCEEDED(hr) && i<count;i++) {
        List *l=object<List>(lists[i]);
        if(!l || l->device!=q->device || !l->closed || l->generation!=l->allocator->generation)hr=E_INVALIDARG;
        else if(FAILED(l->failure))hr=l->failure;
    }
    AcquireSRWLockExclusive(&q->lock);
    if(SUCCEEDED(hr)) {
        Work work{Work::Execute};
        try {work.recordings.reserve(count);for(UINT i=0;i<count;i++)work.recordings.push_back(object<List>(lists[i])->recording);}
        catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
        if(SUCCEEDED(hr) && !work.recordings.empty())hr=q->working?enqueue(q,std::move(work)):execute_work(q,work);
    }
    ReleaseSRWLockExclusive(&q->lock);if(FAILED(hr))error(q->device,hr);
}
void device_table(D3D12DDI_DEVICE_FUNCS_CORE_0003 *f) {
    *f={};
    f->pfnCheckFormatSupport=check_format;f->pfnCheckMultisampleQualityLevels=multisample_levels;
    f->pfnGetImplicitPhysicalAdapterMask=adapter_mask;f->pfnQueryNodeMap=node_map;f->pfnGetPresentPrivateDriverDataSize=present_data_size;
    f->pfnCalcPrivateRootSignatureSize=root_size;f->pfnCreateRootSignature=create_root;f->pfnDestroyRootSignature=destroy_root;
    f->pfnCalcPrivateShaderSize=shader_size;f->pfnCreateVertexShader=create_shader;f->pfnCreatePixelShader=create_shader;
    f->pfnCreateGeometryShader=create_shader;f->pfnCreateComputeShader=create_compute;
    f->pfnCalcPrivateGeometryShaderWithStreamOutput=stream_shader_size;f->pfnCreateGeometryShaderWithStreamOutput=create_stream_shader;
    f->pfnCalcPrivateTessellationShaderSize=tess_size;f->pfnCreateHullShader=create_tess;f->pfnCreateDomainShader=create_tess;f->pfnDestroyShader=destroy_shader;
    f->pfnCalcPrivateElementLayoutSize=layout_size;f->pfnCreateElementLayout=create_layout;f->pfnDestroyElementLayout=destroy_layout;
    f->pfnCalcPrivateBlendStateSize=blend_size;f->pfnCreateBlendState=create_blend;f->pfnDestroyBlendState=destroy_blend;
    f->pfnCalcPrivateDepthStencilStateSize=depth_size;f->pfnCreateDepthStencilState=create_depth;f->pfnDestroyDepthStencilState=destroy_depth;
    f->pfnCalcPrivateRasterizerStateSize=raster_size;f->pfnCreateRasterizerState=create_raster;f->pfnDestroyRasterizerState=destroy_raster;
    f->pfnCalcPrivatePipelineStateSize=pipeline_size;f->pfnCreatePipelineState=create_pipeline;f->pfnDestroyPipelineState=destroy_pipeline;
    f->pfnCalcPrivateDescriptorHeapSize=descriptor_heap_size;f->pfnCreateDescriptorHeap=create_descriptor_heap;f->pfnDestroyDescriptorHeap=destroy_descriptor_heap;
    f->pfnGetDescriptorSizeInBytes=descriptor_size;f->pfnGetCPUDescriptorHandleForHeapStart=cpu_start;f->pfnGetGPUDescriptorHandleForHeapStart=gpu_start;
    f->pfnMakeResident=make_resident;f->pfnEvict=evict;f->pfnOfferResources=offer;f->pfnReclaimResources=reclaim;
    f->pfnCalcPrivateQueryHeapSize=query_size;f->pfnCreateQueryHeap=create_query_heap;f->pfnDestroyQueryHeap=destroy_query_heap;
    f->pfnCalcPrivateOpenedHeapAndResourceSizes=opened_sizes;f->pfnOpenHeapAndResource=open_heap;
    f->pfnCheckResourceAllocationHandle=allocation_handle;f->pfnCheckExistingResourceAllocationInfo=existing_resource_info;
    f->pfnCreateShaderResourceView=create_srv;f->pfnCreateUnorderedAccessView=create_uav;f->pfnCreateConstantBufferView=create_cbv;f->pfnCreateSampler=create_sampler;
    f->pfnCreateRenderTargetView=create_rtv;f->pfnCreateDepthStencilView=create_dsv;f->pfnCopyDescriptors=copy_descriptors;f->pfnCopyDescriptorsSimple=copy_descriptors_simple;
    f->pfnCalcPrivateHeapAndResourceSizes=heap_sizes;f->pfnCreateHeapAndResource=create_heap;f->pfnDestroyHeapAndResource=destroy_heap;
    f->pfnMapHeap=map_heap;f->pfnUnmapHeap=unmap_heap;f->pfnCheckResourceVirtualAddress=resource_address;
    f->pfnCheckResourceAllocationInfo=resource_info;f->pfnCheckSubresourceInfo=subresource_info;
    f->pfnCalcPrivateCommandAllocatorSize=allocator_size;f->pfnCreateCommandAllocator=create_allocator;
    f->pfnResetCommandAllocator=reset_allocator;f->pfnDestroyCommandAllocator=destroy_allocator;
    f->pfnCalcPrivateCommandListSize=list_size;f->pfnCreateCommandList=create_list;f->pfnDestroyCommandList=destroy_list;
    f->pfnCalcPrivateCommandQueueSize=queue_size;f->pfnCreateCommandQueue=create_queue;f->pfnDestroyCommandQueue=destroy_queue;
    f->pfnCalcPrivateFenceSize=fence_size;f->pfnCreateFence=create_fence;f->pfnDestroyFence=destroy_fence;
    f->pfnCalcPrivateCommandSignatureSize=signature_size;f->pfnCreateCommandSignature=create_signature;f->pfnDestroyCommandSignature=destroy_signature;
}
HRESULT APIENTRY fill_table(D3D12DDI_HADAPTER ha,D3D12DDI_TABLE_TYPE type,void *table,SIZE_T size,UINT instance,D3D12DDI_HRTTABLE runtime) {
    if(!table)return E_INVALIDARG;
    switch(type) {
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE:
        if(size!=sizeof(D3D12DDI_DEVICE_FUNCS_CORE_0003))return E_INVALIDARG;
        device_table(static_cast<D3D12DDI_DEVICE_FUNCS_CORE_0003 *>(table));return S_OK;
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D: {
        if(size!=sizeof(D3D12DDI_COMMAND_LIST_FUNCS_3D_0003))return E_INVALIDARG;
        auto *f=static_cast<D3D12DDI_COMMAND_LIST_FUNCS_3D_0003 *>(table);*f={};
        f->pfnCloseCommandList=close_list;f->pfnResetCommandList=reset_list;f->pfnCopyBufferRegion=copy_buffer;f->pfnResourceCopy=copy_resource;
        f->pfnDrawInstanced=draw_instanced;f->pfnDrawIndexedInstanced=draw_indexed;f->pfnDispatch=dispatch;
        f->pfnSetPipelineState=set_pipeline;f->pfnSetDescriptorHeaps=set_heaps;f->pfnSetComputeRootSignature=set_root<true>;f->pfnSetGraphicsRootSignature=set_root<false>;
        f->pfnSetComputeRootDescriptorTable=set_root_table<true>;f->pfnSetGraphicsRootDescriptorTable=set_root_table<false>;
        f->pfnSetComputeRoot32BitConstant=set_constant<true>;f->pfnSetGraphicsRoot32BitConstant=set_constant<false>;
        f->pfnSetComputeRoot32BitConstants=set_constants<true>;f->pfnSetGraphicsRoot32BitConstants=set_constants<false>;
        f->pfnSetComputeRootConstantBufferView=f->pfnSetComputeRootShaderResourceView=f->pfnSetComputeRootUnorderedAccessView=set_root_address<true>;
        f->pfnSetGraphicsRootConstantBufferView=f->pfnSetGraphicsRootShaderResourceView=f->pfnSetGraphicsRootUnorderedAccessView=set_root_address<false>;
        f->pfnIaSetTopology=set_topology;f->pfnRsSetViewports=set_viewports;f->pfnRsSetScissorRects=set_scissors;
        f->pfnOmSetBlendFactor=set_blend;f->pfnOmSetStencilRef=set_stencil;f->pfnIASetIndexBuffer=set_index;f->pfnIASetVertexBuffers=set_vertices;
        f->pfnOMSetRenderTargets=set_targets;f->pfnClearRenderTargetView=clear_rtv;f->pfnClearDepthStencilView=clear_dsv;f->pfnResourceBarrier=resource_barrier;f->pfnExecuteBundle=execute_bundle;
        f->pfnClearUnorderedAccessViewUint=clear_uav<true,UINT>;f->pfnClearUnorderedAccessViewFloat=clear_uav<false,FLOAT>;
        f->pfnCopyTextureRegion=copy_texture;f->pfnResourceResolveSubresource=resolve_texture;
        f->pfnBeginQuery=begin_end_query<false>;f->pfnEndQuery=begin_end_query<true>;f->pfnResolveQueryData=resolve_queries;f->pfnSetPredication=set_predication;
        f->pfnExecuteIndirect=execute_indirect;
        f->pfnDiscardResource=discard_resource;f->pfnSetMarker=set_marker;f->pfnClearRootArguments=clear_roots;
        f->pfnSOSetTargets=set_stream_targets;
        f->pfnBlt=blt;f->pfnPresent=present;
        if(!instance)object<Adapter>(ha)->command_table=runtime;
        return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D: {
        if(size!=sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001))return E_INVALIDARG;
        auto *f=static_cast<D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 *>(table);*f={};f->pfnExecuteCommandLists=execute;f->pfnSignalFence=signal_fence;f->pfnWaitForFence=wait_fence;return S_OK;
    }
    default:return DXGI_ERROR_UNSUPPORTED;
    }
}
SIZE_T APIENTRY device_size(D3D12DDI_HADAPTER,const D3D12DDIARG_CALCPRIVATEDEVICESIZE *){return sizeof(Device);}
HRESULT APIENTRY create_device(D3D12DDI_HADAPTER ha,const D3D12DDIARG_CREATEDEVICE_0003 *p) {
    if(p->Interface!=D3D12DDI_INTERFACE_VERSION || !p->hDrvDevice.pDrvPrivate || !p->pKTCallbacks || !p->p12UMCallbacks)return E_INVALIDARG;
    const auto *c=p->p12UMCallbacks;
    if(!c->pfnSetErrorCb || !c->pfnSetCommandListErrorCb || !c->pfnSetCommandListDDITableCb || !c->pfnCreateContextCb || !c->pfnDestroyContextCb ||
       !c->pfnCreateContextVirtualCb || !p->pKTCallbacks->pfnAllocateCb || !p->pKTCallbacks->pfnDeallocate2Cb ||
       !p->pKTCallbacks->pfnMapGpuVirtualAddressCb || !p->pKTCallbacks->pfnFreeGpuVirtualAddressCb ||
       !p->pKTCallbacks->pfnLock2Cb || !p->pKTCallbacks->pfnUnlock2Cb || !p->pKTCallbacks->pfnMakeResidentCb ||
       !p->pKTCallbacks->pfnCreatePagingQueueCb || !p->pKTCallbacks->pfnDestroyPagingQueueCb ||
       !p->pKTCallbacks->pfnCreateContextVirtualCb || !p->pKTCallbacks->pfnDestroyContextCb ||
       !p->pKTCallbacks->pfnSubmitCommandCb || !p->pKTCallbacks->pfnEscapeCb || !c->pfnMakeResidentCb || !c->pfnEvictCb ||
       !c->pfnOfferAllocationsCb || !c->pfnReclaimAllocations2Cb)return E_INVALIDARG;
    Device *d;
    try {d=new(p->hDrvDevice.pDrvPrivate) Device{object<Adapter>(ha),p->hRTDevice,*p->pKTCallbacks,*c};}
    catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
    InitializeSRWLock(&d->memory_lock);
    HRESULT hr=d->kernel.pfnCreatePagingQueueCb(d->runtime.handle,&d->paging);
    if(SUCCEEDED(hr) && !load_backend(d))hr=DXGI_ERROR_UNSUPPORTED;
    if(FAILED(hr)) {
        if(d->compiler)d->compiler->destroy(d->compiler);if(d->screen)d->gpu_destroy_screen(d->screen);if(d->backend)FreeLibrary(d->backend);
        if(d->paging.hPagingQueue){D3DDDI_DESTROYPAGINGQUEUE paging{d->paging.hPagingQueue};d->kernel.pfnDestroyPagingQueueCb(d->runtime.handle,&paging);}
        d->~Device();
    }return hr;
}
void APIENTRY destroy_device(D3D12DDI_HDEVICE h) {
    Device *d=object<Device>(h);D3DDDI_DESTROYPAGINGQUEUE paging{d->paging.hPagingQueue};
    if(d->compiler)d->compiler->destroy(d->compiler);if(d->screen)d->gpu_destroy_screen(d->screen);if(d->backend)FreeLibrary(d->backend);
    d->kernel.pfnDestroyPagingQueueCb(d->runtime.handle,&paging);d->~Device();
}
HRESULT APIENTRY close_adapter(D3D12DDI_HADAPTER h){delete object<Adapter>(h);return S_OK;}
HRESULT APIENTRY versions(D3D12DDI_HADAPTER,UINT32 *count,UINT64 *supported) {
    if(!count)return E_INVALIDARG;
    if(supported){if(!*count)return E_INVALIDARG;*supported=D3D12DDI_SUPPORTED_0003;}
    *count=1;return S_OK;
}
HRESULT APIENTRY optional_tables(D3D12DDI_HADAPTER,UINT32 *count,D3D12DDI_TABLE_REQUEST *) {
    if(!count)return E_INVALIDARG;*count=0;return S_OK;
}
}
extern "C" HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER *p) {
    if(!p || !p->pAdapterFuncs || !p->pAdapterCallbacks || !p->pAdapterCallbacks->pfnQueryAdapterInfoCb)return E_INVALIDARG;
    A830_ADAPTER_INFO info{};D3DDDICB_QUERYADAPTERINFO query{&info,sizeof(info)};
    HRESULT hr=p->pAdapterCallbacks->pfnQueryAdapterInfoCb(p->hRTAdapter.handle,&query);
    if(FAILED(hr))return hr;
    if(info.Version!=A830_ABI_VERSION || info.Chip!=0x44050001 || !(info.Flags&1) || info.Status)return DXGI_ERROR_UNSUPPORTED;
    auto *a=new(std::nothrow) Adapter{p->hRTAdapter.handle};if(!a)return E_OUTOFMEMORY;
    p->hAdapter.pDrvPrivate=a;
    *p->pAdapterFuncs={device_size,create_device,close_adapter,versions,caps,optional_tables,fill_table,destroy_device};return S_OK;
}
