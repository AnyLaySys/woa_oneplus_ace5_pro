#pragma once
UINT depth_view_flags(DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS || f==DXGI_FORMAT_R24_UNORM_X8_TYPELESS)
        return D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE|D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER|D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    if(f==DXGI_FORMAT_X32_TYPELESS_G8X24_UINT || f==DXGI_FORMAT_X24_TYPELESS_G8_UINT)return D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    return 0;
}
template<class T> HRESULT capability(const D3D12DDIARG_GETCAPS *p,const T &value) {
    if(!p->pData || p->DataSize!=sizeof(T))return E_INVALIDARG;
    memcpy(p->pData,&value,sizeof(value));return S_OK;
}
HRESULT APIENTRY caps(D3D12DDI_HADAPTER,const D3D12DDIARG_GETCAPS *p) {
    if(!p)return E_INVALIDARG;
    switch(p->Type) {
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE:
        if(!p->pInfo || *static_cast<const UINT *>(p->pInfo))return E_INVALIDARG;
        return capability(p,D3D12DDI_MEMORY_ARCHITECTURE_CAPS{TRUE,FALSE,FALSE});
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT:
        if(p->DataSize==sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS))return capability(p,D3D12DDI_TEXTURE_LAYOUT_CAPS{});
        return capability(p,D3D12DDI_TEXTURE_LAYOUT_CAPS_0001{});
    case D3D12DDICAPS_TYPE_SHADER:return capability(p,D3D12DDI_SHADER_CAPS_0003{});
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO:return capability(p,D3D12DDI_ARCHITECTURE_INFO_DATA{TRUE});
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT:return capability(p,D3D12DDI_3DPIPELINELEVEL_11_0);
    case D3D12DDICAPS_TYPE_GPUVA_CAPS:
        if(!p->pInfo || *static_cast<const UINT *>(p->pInfo))return E_INVALIDARG;
        return capability(p,D3D12DDI_GPUVA_CAPS_0004{29});
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS:{
        D3D12DDI_D3D12_OPTIONS_DATA_0003 options{};
        options.ResourceBindingTier=D3D12DDI_RESOURCE_BINDING_TIER_1;
        options.ResourceHeapTier=D3D12DDI_RESOURCE_HEAP_TIER_1;
        return capability(p,options);
    }
    default:return DXGI_ERROR_UNSUPPORTED;
    }
}
void APIENTRY check_format(D3D12DDI_HDEVICE h,DXGI_FORMAT f,UINT *out) {
    *out=0;Device *d=object<Device>(h);pipe_format pf=format(f);
    if(!pf){if(f==DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM)*out=D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED;return;}
    auto supported=[&](pipe_texture_target target,UINT samples,UINT bind){return d->screen->is_format_supported(d->screen,pf,target,samples,samples,bind);};
    if(depth_format(f)) {
        for(UINT samples:{2u,4u,8u})if(supported(PIPE_TEXTURE_2D,samples,PIPE_BIND_DEPTH_STENCIL))*out|=D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        return;
    }
    if(supported(PIPE_BUFFER,1,PIPE_BIND_VERTEX_BUFFER))*out|=D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER;
    if(supported(PIPE_BUFFER,1,PIPE_BIND_SAMPLER_VIEW))*out|=D3D12DDI_FORMAT_SUPPORT_BUFFER;
    if(supported(PIPE_TEXTURE_2D,1,PIPE_BIND_SAMPLER_VIEW))*out|=D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE|D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
    if(supported(PIPE_TEXTURE_2D,1,PIPE_BIND_RENDER_TARGET))*out|=D3D12DDI_FORMAT_SUPPORT_RENDERTARGET;
    if(supported(PIPE_TEXTURE_2D,1,PIPE_BIND_RENDER_TARGET|PIPE_BIND_BLENDABLE))*out|=D3D12DDI_FORMAT_SUPPORT_BLENDABLE;
    if(supported(PIPE_TEXTURE_2D,1,PIPE_BIND_SHADER_IMAGE))*out|=D3D12DDI_FORMAT_SUPPORT_UAV_WRITES;
    if(f==DXGI_FORMAT_R32_FLOAT || f==DXGI_FORMAT_R32_UINT || f==DXGI_FORMAT_R32_SINT)*out|=D3D12DDI_FORMAT_SUPPORT_UAV_READS;
    if(f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM)*out|=D3D12DDI_FORMAT_SUPPORT_DISPLAY;
    for(UINT samples:{2u,4u,8u}) {
        if(supported(PIPE_TEXTURE_2D,samples,PIPE_BIND_RENDER_TARGET))*out|=D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET;
        if(supported(PIPE_TEXTURE_2D,samples,PIPE_BIND_SAMPLER_VIEW))*out|=D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;
    }
    UINT view=depth_view_flags(f);if(view)*out&=view;
}
void APIENTRY multisample_levels(D3D12DDI_HDEVICE h,DXGI_FORMAT f,UINT samples,D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags,UINT *out) {
    *out=0;if(flags || depth_view_flags(f) || (samples!=1 && samples!=2 && samples!=4 && samples!=8))return;
    Device *d=object<Device>(h);pipe_format pf=format(f);if(!pf)return;
    UINT bind=depth_format(f)?PIPE_BIND_DEPTH_STENCIL:PIPE_BIND_RENDER_TARGET;
    *out=d->screen->is_format_supported(d->screen,pf,PIPE_TEXTURE_2D,samples,samples,bind)?1:0;
}
UINT APIENTRY adapter_mask(D3D12DDI_HDEVICE){return 1;}
void APIENTRY node_map(D3D12DDI_HDEVICE,UINT count,UINT *out){for(UINT i=0;i<count;i++)out[i]=i?D3D12DDI_NODE_MAP_HIDE_NODE:0;}
UINT APIENTRY present_data_size(D3D12DDI_HDEVICE,const D3D12DDIARG_PRESENT_0001 *){return 0;}
