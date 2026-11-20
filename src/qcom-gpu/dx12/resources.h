#pragma once
#include "formats.h"
UINT display_format(DXGI_FORMAT f) {
    switch(f) {case DXGI_FORMAT_B8G8R8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return D3DDDIFMT_A8R8G8B8;
        case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return D3DDDIFMT_A8B8G8R8;
        case DXGI_FORMAT_R10G10B10A2_UNORM:return D3DDDIFMT_A2B10G10R10;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:return D3DDDIFMT_A16B16G16R16F;default:return 0;}
}
void drop_resource(Device *d,pipe_resource *p) {
    if(p && !InterlockedDecrement(reinterpret_cast<volatile LONG *>(&p->reference.count)))d->screen->resource_destroy(d->screen,p);
}
bool resource_template(Device *d,const D3D12DDIARG_CREATERESOURCE_0003 *r,pipe_resource &p) {
    if(!r || !r->Width || r->Width>UINT_MAX || !r->Height || !r->DepthOrArraySize || !r->MipLevels || r->MipLevels>15 ||
       !r->SampleDesc.Count || r->SampleDesc.Quality || (r->Flags&~0xffu))return false;
    p={};p.width0=static_cast<UINT>(r->Width);p.height0=r->Height;p.depth0=1;p.array_size=r->DepthOrArraySize;
    p.last_level=r->MipLevels-1;p.nr_samples=p.nr_storage_samples=r->SampleDesc.Count;p.usage=PIPE_USAGE_DEFAULT;
    bool depth=(r->Flags&D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL)!=0;
    p.format=format(r->Format,depth);
    switch(r->ResourceType) {
    case D3D12DDI_RT_BUFFER:
        if(r->Width>GPU_MAX_BUFFER_SIZE || r->Height!=1 || r->DepthOrArraySize!=1 || r->MipLevels!=1 ||
           r->SampleDesc.Count!=1 || r->Format!=DXGI_FORMAT_UNKNOWN || r->pRowMajorLayout)return false;
        p.target=PIPE_BUFFER;p.format=PIPE_FORMAT_R8_UNORM;
        p.bind=PIPE_BIND_VERTEX_BUFFER|PIPE_BIND_INDEX_BUFFER|PIPE_BIND_CONSTANT_BUFFER|PIPE_BIND_SHADER_BUFFER;break;
    case D3D12DDI_RT_TEXTURE1D:
        if(r->Height!=1)return false;p.target=r->DepthOrArraySize>1?PIPE_TEXTURE_1D_ARRAY:PIPE_TEXTURE_1D;break;
    case D3D12DDI_RT_TEXTURE2D:p.target=r->DepthOrArraySize>1?PIPE_TEXTURE_2D_ARRAY:PIPE_TEXTURE_2D;break;
    case D3D12DDI_RT_TEXTURE3D:p.target=PIPE_TEXTURE_3D;p.depth0=r->DepthOrArraySize;p.array_size=1;break;
    default:return false;
    }
    if(p.target!=PIPE_BUFFER) {
        if(p.width0>16384 || p.height0>16384 || p.depth0>2048 || p.array_size>2048 || !p.format)return false;
        if(r->Layout==D3D12DDI_TL_ROW_MAJOR)p.bind|=PIPE_BIND_LINEAR;
        else if(r->Layout!=D3D12DDI_TL_UNDEFINED)return false;
        if(r->pRowMajorLayout)return false;
        if(r->ResourceType==D3D12DDI_RT_TEXTURE2D && r->MipLevels==1 && r->DepthOrArraySize==1 && r->SampleDesc.Count==1 && display_format(r->Format))p.bind|=PIPE_BIND_LINEAR;
    }
    if(r->Flags&D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE)p.bind|=PIPE_BIND_SAMPLER_VIEW;
    if(r->Flags&D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS)p.bind|=PIPE_BIND_SHADER_IMAGE;
    if(r->Flags&D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET)p.bind|=PIPE_BIND_RENDER_TARGET;
    if(depth)p.bind|=PIPE_BIND_DEPTH_STENCIL;
    return p.target==PIPE_BUFFER || d->screen->is_format_supported(d->screen,p.format,p.target,p.nr_samples,p.nr_storage_samples,p.bind&~PIPE_BIND_LINEAR);
}
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY heap_sizes(D3D12DDI_HDEVICE,
    const D3D12DDIARG_CREATEHEAP_0001 *h,const D3D12DDIARG_CREATERESOURCE_0003 *r){return {h?sizeof(Heap):0,r?sizeof(Resource):0};}
HRESULT APIENTRY create_heap(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATEHEAP_0001 *p,D3D12DDI_HHEAP hh,
    D3D12DDI_HRTRESOURCE rt,const D3D12DDIARG_CREATERESOURCE_0003 *r,const D3D12DDI_CLEAR_VALUES *,D3D12DDI_HRESOURCE hr) {
    Device *d=object<Device>(hd);Heap *h=object<Heap>(hh);pipe_resource tmpl{};A830_NATIVE_LAYOUT layout{};
    if(!h || (!p && !r) || (r && (!hr.pDrvPrivate || !resource_template(d,r,tmpl))))return E_INVALIDARG;
    bool implicit=p && p->ByteSize==UINT64_MAX;
    if(p && (!p->ByteSize || (implicit?!r:p->ByteSize>GPU_MAX_BUFFER_SIZE-65535) || (p->Alignment && p->Alignment!=65536) ||
       p->MemoryPool!=D3D12DDI_MEMORY_POOL_L0 || (p->Flags&~0x3eu) || p->CreationNodeMask!=1 || p->VisibleNodeMask!=1 ||
       static_cast<UINT>(p->CPUPageProperty)>D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK))return DXGI_ERROR_UNSUPPORTED;
    if(!p && h->device!=d)return E_INVALIDARG;
    UINT64 offset=r?r->ReuseBufferGPUVA.BaseAddress.UMD.Offset:0,size=p?p->ByteSize:h->size;
    if(r && (offset&65535))return E_INVALIDARG;
    if(r) {
        pipe_resource *query=d->gpu_resource(d->screen,&tmpl,nullptr,0,&layout);
        if(!query)return DXGI_ERROR_UNSUPPORTED;drop_resource(d,query);
        if(implicit) {
            if(offset>GPU_MAX_BUFFER_SIZE-65535 || layout.Size>GPU_MAX_BUFFER_SIZE-65535-offset)return E_OUTOFMEMORY;
            size=(offset+layout.Size+65535)&~UINT64(65535);
        }
        if(offset>size || layout.Size>size-offset)return E_INVALIDARG;
    }
    if(p) {
        A830_DX12_RESOURCE data{};
        if(r) {
            data.Resource.Width=static_cast<UINT>(r->Width);data.Resource.Height=r->Height;data.Resource.Pitch=layout.Mip[0].Pitch;data.Resource.Format=display_format(r->Format);
            data.Type=r->ResourceType;data.Depth=r->DepthOrArraySize;data.Levels=r->MipLevels;data.DxgiFormat=r->Format;
            data.Samples=r->SampleDesc.Count;data.Layout=r->Layout;data.BindFlags=r->Flags;data.Offset=offset;
        }
        if(p->Flags&D3D12DDI_HEAP_FLAG_PRIMARY){if(!data.Resource.Format || offset || data.Type!=D3D12DDI_RT_TEXTURE2D || data.Depth!=1 || data.Levels!=1 || data.Samples!=1)return DXGI_ERROR_UNSUPPORTED;data.Resource.Flags|=A830_RESOURCE_PRIMARY;}
        GPU_ALLOCATION memory{};HRESULT result=allocate(d,static_cast<UINT>(size),memory,p->CPUPageProperty,rt.handle,&data);
        if(FAILED(result))return result;new(h) Heap{d,memory,0,size,p->CPUPageProperty};
    }
    if(r) {
        pipe_resource *pipe=d->gpu_resource(d->screen,&tmpl,&h->memory,h->offset+offset,&layout);
        if(!pipe){if(p)release(d,h->memory);return E_OUTOFMEMORY;}
        Resource *resource=new(hr.pDrvPrivate) Resource{h,offset,r->ResourceType==D3D12DDI_RT_BUFFER?r->Width:layout.Size,pipe,layout,*r};
        resource->desc.pRowMajorLayout=nullptr;
        try {AcquireSRWLockExclusive(&d->memory_lock);d->resources.push_back(resource);ReleaseSRWLockExclusive(&d->memory_lock);}
        catch(const std::bad_alloc &){ReleaseSRWLockExclusive(&d->memory_lock);drop_resource(d,pipe);if(p)release(d,h->memory);return E_OUTOFMEMORY;}
    }
    return S_OK;
}
void APIENTRY destroy_heap(D3D12DDI_HDEVICE hd,D3D12DDI_HHEAP hh,D3D12DDI_HRESOURCE hr) {
    Device *d=object<Device>(hd);
    if(hr.pDrvPrivate) {
        Resource *r=object<Resource>(hr);AcquireSRWLockExclusive(&d->memory_lock);
        auto it=std::find(d->resources.begin(),d->resources.end(),r);if(it!=d->resources.end())d->resources.erase(it);
        ReleaseSRWLockExclusive(&d->memory_lock);drop_resource(d,r->pipe);r->~Resource();
    }
    if(hh.pDrvPrivate)release(d,object<Heap>(hh)->memory);
}
Resource *find_resource(Device *d,UINT64 va,UINT64 bytes,UINT *offset) {
    Resource *found=nullptr;AcquireSRWLockShared(&d->memory_lock);
    for(Resource *r:d->resources)if(r->desc.ResourceType==D3D12DDI_RT_BUFFER && va>=address(r) && va-address(r)<r->size && bytes<=r->size-(va-address(r))) {
        found=r;*offset=static_cast<UINT>(va-address(r));break;
    }
    ReleaseSRWLockShared(&d->memory_lock);return found;
}
HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE hd,D3D12DDI_HHEAP hh,void **out) {
    if(!hh.pDrvPrivate || !out)return E_INVALIDARG;Heap *h=object<Heap>(hh);*out=nullptr;
    if(h->cpu==D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE || h->device!=object<Device>(hd))return E_INVALIDARG;
    *out=reinterpret_cast<void *>(static_cast<uintptr_t>(h->memory.Map+h->offset));return S_OK;
}
void APIENTRY unmap_heap(D3D12DDI_HDEVICE,D3D12DDI_HHEAP){MemoryBarrier();}
D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY resource_address(D3D12DDI_HDEVICE,D3D12DDI_HRESOURCE h){return address(object<Resource>(h));}
void APIENTRY resource_info(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATERESOURCE_0003 *p,
    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS,UINT64 alignment,UINT nodes,D3D12DDI_RESOURCE_ALLOCATION_INFO *out) {
    Device *d=object<Device>(hd);pipe_resource tmpl{};A830_NATIVE_LAYOUT layout{};*out={};out->ResourceDataSize=UINT64_MAX;
    if((alignment && alignment!=65536) || nodes!=1 || !resource_template(d,p,tmpl)){error(d,DXGI_ERROR_UNSUPPORTED);return;}
    pipe_resource *query=d->gpu_resource(d->screen,&tmpl,nullptr,0,&layout);
    if(!query){error(d,DXGI_ERROR_UNSUPPORTED);return;}drop_resource(d,query);
    out->ResourceDataSize=(layout.Size+65535)&~UINT64(65535);out->ResourceDataAlignment=65536;out->Layout=p->Layout;
}
void APIENTRY subresource_info(D3D12DDI_HDEVICE hd,D3D12DDI_HRESOURCE hr,UINT sub,D3D12DDI_SUBRESOURCE_INFO *out) {
    Resource *r=object<Resource>(hr);*out={};UINT levels=r->desc.MipLevels;
    if(sub>=levels*r->layout.Layers){error(object<Device>(hd),E_INVALIDARG);return;}
    const auto &mip=r->layout.Mip[sub%levels];out->Offset=r->offset+mip.Offset+static_cast<UINT64>(sub/levels)*mip.LayerPitch;
    out->RowStride=mip.Pitch;out->DepthStride=mip.LayerPitch;
}
