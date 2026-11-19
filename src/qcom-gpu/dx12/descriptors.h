#pragma once
struct Descriptor {
    enum Kind { Empty,SRV,UAV,RTV,DSV,CBV,Sampler } kind=Empty;
    UINT stride=0;
    union {
        pipe_sampler_view srv;
        pipe_image_view uav;
        pipe_surface surface;
        pipe_sampler_state sampler;
        D3D12DDI_CONSTANT_BUFFER_VIEW_DESC cb;
    };
    Descriptor(){memset(&srv,0,sizeof(srv));}
};
struct DescriptorHeap { Device *device;D3D12DDI_DESCRIPTOR_HEAP_TYPE type;std::vector<Descriptor> entries; };
Descriptor *descriptor(D3D12DDI_CPU_DESCRIPTOR_HANDLE h){return reinterpret_cast<Descriptor *>(h.ptr);}
UINT format_bytes(DXGI_FORMAT f) {
    UINT n=static_cast<UINT>(f);
    if(n>=1 && n<=4)return 16;if(n>=5 && n<=8)return 12;if(n>=9 && n<=22)return 8;
    if(n>=23 && n<=47)return 4;if(n>=48 && n<=59)return 2;if(n>=60 && n<=65)return 1;
    if(n==67 || n==68 || n==69 || (n>=87 && n<=93))return 4;
    if(n>=70 && n<=84)return n<=72 || (n>=79 && n<=81)?8:16;
    if(n>=94 && n<=99)return 16;if(n==85 || n==86 || n==115)return 2;return 0;
}
SIZE_T APIENTRY descriptor_heap_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *){return sizeof(DescriptorHeap);}
HRESULT APIENTRY create_descriptor_heap(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 *p,D3D12DDI_HDESCRIPTORHEAP h) {
    if(!p || !p->NumDescriptors || p->Type>=D3D12DDI_DESCRIPTOR_HEAP_TYPE_NUM_TYPES || p->NodeMask!=1)return E_INVALIDARG;
    auto *heap=new(h.pDrvPrivate) DescriptorHeap{object<Device>(hd),p->Type};
    try {heap->entries.resize(p->NumDescriptors);return S_OK;}catch(const std::bad_alloc &){heap->~DescriptorHeap();return E_OUTOFMEMORY;}
}
void APIENTRY destroy_descriptor_heap(D3D12DDI_HDEVICE,D3D12DDI_HDESCRIPTORHEAP h){object<DescriptorHeap>(h)->~DescriptorHeap();}
UINT APIENTRY descriptor_size(D3D12DDI_HDEVICE,D3D12DDI_DESCRIPTOR_HEAP_TYPE){return sizeof(Descriptor);}
D3D12DDI_CPU_DESCRIPTOR_HANDLE APIENTRY cpu_start(D3D12DDI_HDEVICE,D3D12DDI_HDESCRIPTORHEAP h){return {reinterpret_cast<SIZE_T>(object<DescriptorHeap>(h)->entries.data())};}
D3D12DDI_GPU_DESCRIPTOR_HANDLE APIENTRY gpu_start(D3D12DDI_HDEVICE,D3D12DDI_HDESCRIPTORHEAP h){return {reinterpret_cast<UINT64>(object<DescriptorHeap>(h)->entries.data())};}
template<class S> pipe_sampler_state sampler_state(const S &s) {
    pipe_sampler_state p{};static const UINT wraps[]={0,PIPE_TEX_WRAP_REPEAT,PIPE_TEX_WRAP_MIRROR_REPEAT,
        PIPE_TEX_WRAP_CLAMP_TO_EDGE,PIPE_TEX_WRAP_CLAMP_TO_BORDER,PIPE_TEX_WRAP_MIRROR_CLAMP_TO_EDGE};
    p.wrap_s=wraps[s.AddressU];p.wrap_t=wraps[s.AddressV];p.wrap_r=wraps[s.AddressW];
    p.min_img_filter=(s.Filter>>4)&1;p.mag_img_filter=(s.Filter>>2)&1;p.min_mip_filter=(s.Filter&1)?PIPE_TEX_MIPFILTER_LINEAR:PIPE_TEX_MIPFILTER_NEAREST;
    p.max_anisotropy=(s.Filter&64)?std::min(s.MaxAnisotropy,16u):0;p.compare_mode=((s.Filter>>7)&3)==1;
    p.compare_func=s.ComparisonFunc-1;p.reduction_mode=((s.Filter>>7)&3)>=2?((s.Filter>>7)&3)-1:0;
    p.lod_bias=s.MipLODBias;p.min_lod=s.MinLOD;p.max_lod=s.MaxLOD;p.seamless_cube_map=1;return p;
}
void APIENTRY create_sampler(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_SAMPLER *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h) {
    if(!p || !p->pSamplerDesc || !h.ptr){error(object<Device>(hd),E_INVALIDARG);return;}const auto &s=*p->pSamplerDesc;
    if(s.AddressU<1 || s.AddressU>5 || s.AddressV<1 || s.AddressV>5 || s.AddressW<1 || s.AddressW>5){error(object<Device>(hd),E_INVALIDARG);return;}
    Descriptor *v=descriptor(h);*v={};v->kind=Descriptor::Sampler;v->sampler=sampler_state(s);memcpy(v->sampler.border_color.f,s.BorderColor,16);
}
void APIENTRY create_cbv(D3D12DDI_HDEVICE hd,const D3D12DDI_CONSTANT_BUFFER_VIEW_DESC *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h) {
    if(!p || !h.ptr || (p->BufferLocation&255) || (p->SizeInBytes&255) || p->SizeInBytes>65536){error(object<Device>(hd),E_INVALIDARG);return;}
    Descriptor *v=descriptor(h);*v={};v->kind=Descriptor::CBV;v->cb=*p;
}
void APIENTRY create_srv(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h) {
    Device *d=object<Device>(hd);if(!p || !h.ptr){error(d,E_INVALIDARG);return;}Descriptor out;out.kind=Descriptor::SRV;
    Resource *r=object<Resource>(p->hDrvResource);auto &s=out.srv;s.texture=r?r->pipe:nullptr;s.format=format(p->Format);s.target=r?r->pipe->target:PIPE_TEXTURE_2D;
    UINT swizzles[4];for(UINT i=0;i<4;i++)swizzles[i]=(p->Shader4ComponentMapping>>(i*3))&7;
    s.swizzle_r=swizzles[0];s.swizzle_g=swizzles[1];s.swizzle_b=swizzles[2];s.swizzle_a=swizzles[3];
    UINT first=0,layers=1,level=0,levels=1;
    switch(p->ResourceDimension) {
    case D3D12DDI_RD_BUFFER:{
        UINT stride=p->Buffer.StructureByteStride?p->Buffer.StructureByteStride:format_bytes(p->Format);
        if(p->Buffer.Flags || p->Buffer.StructureByteStride){stride=p->Buffer.StructureByteStride?p->Buffer.StructureByteStride:4;s.format=PIPE_FORMAT_R32_UINT;}
        UINT64 offset=p->Buffer.FirstElement*stride,size=static_cast<UINT64>(p->Buffer.NumElements)*stride;
        if(!stride || offset>UINT_MAX || size>UINT_MAX || (r && (offset>r->size || size>r->size-offset))){error(d,E_INVALIDARG);return;}
        s.target=PIPE_BUFFER;s.u.buf.offset=static_cast<UINT>(offset);s.u.buf.size=static_cast<UINT>(size);break;
    }
    case D3D12DDI_RD_TEXTURE1D:first=p->Tex1D.FirstArraySlice;layers=p->Tex1D.ArraySize;level=p->Tex1D.MostDetailedMip;levels=p->Tex1D.MipLevels;s.u.tex.min_lod_clamp=p->Tex1D.ResourceMinLODClamp;break;
    case D3D12DDI_RD_TEXTURE2D:first=p->Tex2D.FirstArraySlice;layers=p->Tex2D.ArraySize;level=p->Tex2D.MostDetailedMip;levels=p->Tex2D.MipLevels;s.u.tex.min_lod_clamp=p->Tex2D.ResourceMinLODClamp;break;
    case D3D12DDI_RD_TEXTURE3D:level=p->Tex3D.MostDetailedMip;levels=p->Tex3D.MipLevels;s.u.tex.min_lod_clamp=p->Tex3D.ResourceMinLODClamp;break;
    case D3D12DDI_RD_TEXTURECUBE:first=p->TexCube.First2DArrayFace;layers=p->TexCube.NumCubes*6;level=p->TexCube.MostDetailedMip;levels=p->TexCube.MipLevels;
        s.target=layers>6?PIPE_TEXTURE_CUBE_ARRAY:PIPE_TEXTURE_CUBE;s.u.tex.min_lod_clamp=p->TexCube.ResourceMinLODClamp;break;
    default:error(d,E_INVALIDARG);return;
    }
    if(s.target!=PIPE_BUFFER) {
        if(!layers || !levels || (r && (level>=r->desc.MipLevels || levels>r->desc.MipLevels-level || first>=r->pipe->array_size || layers>r->pipe->array_size-first))){error(d,E_INVALIDARG);return;}
        s.u.tex.first_level=level;s.u.tex.last_level=level+levels-1;s.u.tex.first_layer=first;s.u.tex.last_layer=first+layers-1;
    }
    *descriptor(h)=out;
}
template<class V> bool view_layers(Device *d,const V &p,UINT &level,UINT &first,UINT &count) {
    switch(p.ResourceDimension) {
    case D3D12DDI_RD_TEXTURE1D:level=p.Tex1D.MipSlice;first=p.Tex1D.FirstArraySlice;count=p.Tex1D.ArraySize;return true;
    case D3D12DDI_RD_TEXTURE2D:level=p.Tex2D.MipSlice;first=p.Tex2D.FirstArraySlice;count=p.Tex2D.ArraySize;return true;
    case D3D12DDI_RD_TEXTURE3D:
        if constexpr(!std::is_same_v<V,D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW>){level=p.Tex3D.MipSlice;first=p.Tex3D.FirstW;count=p.Tex3D.WSize;return true;}
        break;
    case D3D12DDI_RD_TEXTURECUBE:
        if constexpr(!std::is_same_v<V,D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002>){level=p.TexCube.MipSlice;first=p.TexCube.FirstArraySlice;count=p.TexCube.ArraySize;return true;}
        break;
    default:error(d,DXGI_ERROR_UNSUPPORTED);return false;
    }
    error(d,DXGI_ERROR_UNSUPPORTED);return false;
}
template<class V> void create_surface_view(Device *d,const V *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h,Descriptor::Kind kind) {
    if(!p || !h.ptr){error(d,E_INVALIDARG);return;}UINT level=0,first=0,count=0;if(!view_layers(d,*p,level,first,count))return;
    Resource *r=object<Resource>(p->hDrvResource);Descriptor out;out.kind=kind;
    if(r && level>=r->desc.MipLevels){error(d,E_INVALIDARG);return;}
    UINT layers=r?(r->pipe->target==PIPE_TEXTURE_3D?std::max(1u,static_cast<UINT>(r->pipe->depth0)>>level):r->pipe->array_size):0;
    if(r && (!count || level>=r->desc.MipLevels || first>=layers || count>layers-first)){error(d,E_INVALIDARG);return;}
    if(!r){out.surface.format=format(p->Format,true);*descriptor(h)=out;return;}
    out.surface.texture=r->pipe;out.surface.format=format(p->Format,true);out.surface.level=level;out.surface.first_layer=first;out.surface.last_layer=first+count-1;*descriptor(h)=out;
}
void APIENTRY create_rtv(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h){create_surface_view(object<Device>(hd),p,h,Descriptor::RTV);}
void APIENTRY create_dsv(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_DEPTH_STENCIL_VIEW *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h){create_surface_view(object<Device>(hd),p,h,Descriptor::DSV);}
void APIENTRY create_uav(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 *p,D3D12DDI_CPU_DESCRIPTOR_HANDLE h) {
    Device *d=object<Device>(hd);if(!p || !h.ptr){error(d,E_INVALIDARG);return;}Resource *r=object<Resource>(p->hDrvResource);
    Descriptor out;out.kind=Descriptor::UAV;auto &v=out.uav;v.resource=r?r->pipe:nullptr;v.format=format(p->Format);v.access=v.shader_access=PIPE_IMAGE_ACCESS_READ_WRITE;
    if(p->ResourceDimension==D3D12DDI_RD_BUFFER) {
        out.stride=p->Buffer.StructureByteStride;
        UINT stride=p->Buffer.StructureByteStride?p->Buffer.StructureByteStride:format_bytes(p->Format);
        if(p->Buffer.Flags || p->Buffer.StructureByteStride){stride=p->Buffer.StructureByteStride?p->Buffer.StructureByteStride:4;v.format=PIPE_FORMAT_R32_UINT;}
        UINT64 offset=p->Buffer.FirstElement*stride,size=static_cast<UINT64>(p->Buffer.NumElements)*stride;
        if(!stride || offset>UINT_MAX || size>UINT_MAX || (r && (offset>r->size || size>r->size-offset))){error(d,E_INVALIDARG);return;}
        if(p->Buffer.hDrvCounterResource.pDrvPrivate){error(d,DXGI_ERROR_UNSUPPORTED);return;}
        v.u.buf.offset=static_cast<UINT>(offset);v.u.buf.size=static_cast<UINT>(size);
    } else {
        UINT level=0,first=0,count=0;if(!view_layers(d,*p,level,first,count))return;
        if(r && level>=r->desc.MipLevels){error(d,E_INVALIDARG);return;}
        UINT layers=r?(r->pipe->target==PIPE_TEXTURE_3D?std::max(1u,static_cast<UINT>(r->pipe->depth0)>>level):r->pipe->array_size):0;
        if(!count || (r && (level>=r->desc.MipLevels || first>=layers || count>layers-first))){error(d,E_INVALIDARG);return;}
        v.u.tex.level=level;v.u.tex.first_layer=first;v.u.tex.last_layer=first+count-1;
    }
    *descriptor(h)=out;
}
void APIENTRY copy_descriptors_simple(D3D12DDI_HDEVICE,UINT count,D3D12DDI_CPU_DESCRIPTOR_HANDLE dst,D3D12DDI_CPU_DESCRIPTOR_HANDLE src,D3D12DDI_DESCRIPTOR_HEAP_TYPE) {
    memmove(descriptor(dst),descriptor(src),static_cast<size_t>(count)*sizeof(Descriptor));
}
void APIENTRY copy_descriptors(D3D12DDI_HDEVICE hd,UINT nd,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *dst,const UINT *ds,
    UINT ns,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *src,const UINT *ss,D3D12DDI_DESCRIPTOR_HEAP_TYPE type) {
    UINT d=0,s=0,di=0,si=0;
    while(d<nd && s<ns) {
        UINT dn=ds?ds[d]:1,sn=ss?ss[s]:1;if(di==dn){++d;di=0;continue;}if(si==sn){++s;si=0;continue;}
        UINT count=std::min(dn-di,sn-si);copy_descriptors_simple(hd,count,{dst[d].ptr+di*sizeof(Descriptor)},{src[s].ptr+si*sizeof(Descriptor)},type);di+=count;si+=count;
    }
}
