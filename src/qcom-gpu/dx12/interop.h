#pragma once
DXGI_FORMAT imported_format(UINT f) {
    switch(f){case D3DDDIFMT_X8R8G8B8:case D3DDDIFMT_A8R8G8B8:return DXGI_FORMAT_B8G8R8A8_UNORM;
        case D3DDDIFMT_A8B8G8R8:return DXGI_FORMAT_R8G8B8A8_UNORM;case D3DDDIFMT_A2B10G10R10:return DXGI_FORMAT_R10G10B10A2_UNORM;
        case D3DDDIFMT_A16B16G16R16F:return DXGI_FORMAT_R16G16B16A16_FLOAT;default:return DXGI_FORMAT_UNKNOWN;}
}
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY opened_sizes(D3D12DDI_HDEVICE,const D3D12DDIARG_OPENHEAP_0003 *){return {sizeof(Heap),sizeof(Resource)};}
HRESULT APIENTRY open_heap(D3D12DDI_HDEVICE hd,const D3D12DDIARG_OPENHEAP_0003 *p,D3D12DDI_HHEAP hh,D3D12DDI_HRTRESOURCE rt,D3D12DDI_HRESOURCE hr) {
    Device *d=object<Device>(hd);
    if(!p || p->NumAllocations!=1 || !p->pOpenAllocationInfo || !p->pPrivateDriverData ||
       (p->PrivateDriverDataSize!=sizeof(A830_RESOURCE) && p->PrivateDriverDataSize!=sizeof(A830_DX12_RESOURCE)))return E_INVALIDARG;
    const A830_RESOURCE &data=*static_cast<const A830_RESOURCE *>(p->pPrivateDriverData);
    if(data.Version!=A830_ABI_VERSION || !data.Size || data.Size>GPU_MAX_BUFFER_SIZE)return E_INVALIDARG;
    A830_DX12_RESOURCE native{};
    if(p->PrivateDriverDataSize==sizeof(native))native=*static_cast<const A830_DX12_RESOURCE *>(p->pPrivateDriverData);
    else if(data.Width && data.Height){native.Type=D3D12DDI_RT_TEXTURE2D;native.Depth=native.Levels=native.Samples=1;native.DxgiFormat=imported_format(data.Format);native.Layout=D3D12DDI_TL_ROW_MAJOR;native.BindFlags=D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET|D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE;}
    if(hr.pDrvPrivate && !native.Type)return DXGI_ERROR_UNSUPPORTED;
    GPU_ALLOCATION memory{p->pOpenAllocationInfo[0].hAllocation,data.Size,0,0};HRESULT result;
    bool mapped=(data.Flags&A830_RESOURCE_NATIVE)!=0;
    if(mapped)result=map_memory(d,memory,rt.handle,false);
    else {
        A830_ALLOCATION_ADDRESS query{A830_ABI_VERSION,memory.Handle,0,0,0,0,0};D3DDDICB_ESCAPE escape{};
        escape.hDevice=d->runtime.handle;escape.pPrivateDriverData=&query;escape.PrivateDriverDataSize=sizeof(query);
        result=d->kernel.pfnEscapeCb(d->adapter->runtime,&escape);memory.Address=query.Address;memory.Size=query.Size;
        if(SUCCEEDED(result))try {AcquireSRWLockExclusive(&d->memory_lock);d->allocations.emplace(memory.Handle,Device::Memory{memory,rt.handle,false,false});ReleaseSRWLockExclusive(&d->memory_lock);}
        catch(const std::bad_alloc &){ReleaseSRWLockExclusive(&d->memory_lock);result=E_OUTOFMEMORY;}
    }
    if(FAILED(result))return result;
    Heap *heap=new(hh.pDrvPrivate) Heap{d,memory,0,memory.Size,mapped?static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(data.Rotation):D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE};
    if(hr.pDrvPrivate) {
        D3D12DDIARG_CREATERESOURCE_0003 desc{};desc.ResourceType=static_cast<D3D12DDI_RESOURCE_TYPE>(native.Type);desc.Width=data.Width;desc.Height=data.Height;
        desc.DepthOrArraySize=static_cast<UINT16>(native.Depth);desc.MipLevels=static_cast<UINT16>(native.Levels);desc.Format=static_cast<DXGI_FORMAT>(native.DxgiFormat);
        desc.SampleDesc.Count=native.Samples;desc.Layout=static_cast<D3D12DDI_TEXTURE_LAYOUT>(native.Layout);desc.Flags=static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(native.BindFlags);
        desc.ReuseBufferGPUVA.BaseAddress.UMD.Offset=native.Offset;
        result=create_heap(hd,nullptr,hh,rt,&desc,nullptr,hr);
        if(FAILED(result))release(d,heap->memory);
    }
    return result;
}
D3DKMT_HANDLE APIENTRY allocation_handle(D3D12DDI_HDEVICE,D3D10DDI_HRESOURCE h){return object<Resource>(h)->heap->memory.Handle;}
void APIENTRY existing_resource_info(D3D12DDI_HDEVICE,D3D12DDI_HRESOURCE h,D3D12DDI_RESOURCE_ALLOCATION_INFO *info,D3DKMT_HANDLE *handle) {
    Resource *r=object<Resource>(h);*info={};info->ResourceDataSize=r->layout.Size;info->ResourceDataAlignment=65536;info->Layout=r->desc.Layout;*handle=r->heap->memory.Handle;
}
void APIENTRY blt(D3D12DDI_HCOMMANDLIST h,const D3D12DDIARG_BLT *p) {
    List *l=object<List>(h);if(!p || p->Rotate!=DXGI_DDI_MODE_ROTATION_IDENTITY){error(l,DXGI_ERROR_UNSUPPORTED);return;}
    Resource *s=object<Resource>(p->hSrcResource),*d=object<Resource>(p->hDstResource);
    if(!s || !d){error(l,E_INVALIDARG);return;}
    D3D12DDIARG_BLT desc=*p;record(l,[=](Queue *q){pipe_blit_info info{};info.src.resource=s->pipe;info.dst.resource=d->pipe;
        info.src.level=desc.SrcSubresource%s->desc.MipLevels;info.dst.level=desc.DstSubresource%d->desc.MipLevels;
        info.src.format=s->pipe->format;info.dst.format=d->pipe->format;
        info.src.box.x=desc.SrcLeft;info.src.box.y=static_cast<int16_t>(desc.SrcTop);info.src.box.z=static_cast<int16_t>(desc.SrcSubresource/s->desc.MipLevels);
        info.src.box.width=desc.SrcRight-desc.SrcLeft;info.src.box.height=static_cast<int16_t>(desc.SrcBottom-desc.SrcTop);info.src.box.depth=1;
        info.dst.box.x=desc.DstLeft;info.dst.box.y=static_cast<int16_t>(desc.DstTop);info.dst.box.z=static_cast<int16_t>(desc.DstSubresource/d->desc.MipLevels);
        info.dst.box.width=desc.DstRight-desc.DstLeft;info.dst.box.height=static_cast<int16_t>(desc.DstBottom-desc.DstTop);info.dst.box.depth=1;
        info.mask=PIPE_MASK_RGBA;info.filter=PIPE_TEX_FILTER_LINEAR;q->draw->blit(q->draw,&info);return S_OK;});
}
void APIENTRY present(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HCOMMANDQUEUE queue,const D3D12DDIARG_PRESENT_0001 *p,D3D12DDI_PRESENT_0003 *out) {
    List *l=object<List>(h);Queue *q=object<Queue>(queue);*out={};
    if(!p || p->SurfacesToPresent!=1 || !p->phSurfacesToPresent || !q || q->device!=l->device){error(l,E_INVALIDARG);return;}
    Resource *s=object<Resource>(p->phSurfacesToPresent[0].hSurface),*d=object<Resource>(p->hDstResource);
    if(!s || p->phSurfacesToPresent[0].SubResourceIndex){error(l,E_INVALIDARG);return;}
    out->hSrcAllocation=s->heap->memory.Handle;out->hDstAllocation=d?d->heap->memory.Handle:0;out->hContext=q->context;
    out->BackBufferMultiplicity=1;out->AddedGpuWork=!l->recording->ops.empty();
}
