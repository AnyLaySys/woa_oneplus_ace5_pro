#include "umd.h"

static uint64_t newid(){static volatile LONG64 n=0;return (uint64_t)InterlockedIncrement64(&n);}
#define STAGE(name,index) \
void APIENTRY name##SetShader(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER s){dev(h)->shaders[index]=object<Shader>(s);} \
void APIENTRY name##SetConstantBuffers(D3D10DDI_HDEVICE h,UINT start,UINT count,const D3D10DDI_HRESOURCE *r){Device *d=dev(h);if(start+count>16){error(d,E_INVALIDARG);return;}for(UINT i=0;i<count;i++)d->cb[index][start+i]=res(r[i]);} \
void APIENTRY name##SetSamplers(D3D10DDI_HDEVICE h,UINT start,UINT count,const D3D10DDI_HSAMPLER *s){Device *d=dev(h);if(start+count>16){error(d,E_INVALIDARG);return;}for(UINT i=0;i<count;i++)d->samplers[index][start+i]=object<Sampler>(s[i]);} \
void APIENTRY name##SetShaderResources(D3D10DDI_HDEVICE h,UINT start,UINT count,const D3D10DDI_HSHADERRESOURCEVIEW *v){Device *d=dev(h);if(start+count>128){error(d,E_INVALIDARG);return;}for(UINT i=0;i<count;i++)d->textures[index][start+i]=object<View>(v[i]);}
STAGE(Vs,0)
STAGE(Ps,1)
STAGE(Gs,2)
#undef STAGE

SIZE_T APIENTRY CalcPrivateShaderSize(D3D10DDI_HDEVICE,const UINT *,const D3D10DDIARG_STAGE_IO_SIGNATURES *){return sizeof(Shader);}
void APIENTRY CreateVertexShader(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER,const D3D10DDIARG_STAGE_IO_SIGNATURES *io){shader_create(dev(h),new(s.pDrvPrivate) Shader{},code,io,0);}
void APIENTRY CreatePixelShader(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER,const D3D10DDIARG_STAGE_IO_SIGNATURES *io){shader_create(dev(h),new(s.pDrvPrivate) Shader{},code,io,1);}
void APIENTRY CreateGeometryShader(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER,const D3D10DDIARG_STAGE_IO_SIGNATURES *io){shader_create(dev(h),new(s.pDrvPrivate) Shader{},code,io,2);}
void APIENTRY DestroyShader(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER s){Device *d=dev(h);Shader *p=object<Shader>(s);if(p->module)d->vk.DestroyShaderModule(d->device,p->module,nullptr);if(p->dual_module)d->vk.DestroyShaderModule(d->device,p->dual_module,nullptr);p->~Shader();}
SIZE_T APIENTRY CalcPrivateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *,const D3D10DDIARG_STAGE_IO_SIGNATURES *){return sizeof(Shader);}
void APIENTRY CreateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *p,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER rt,const D3D10DDIARG_STAGE_IO_SIGNATURES *io){if(p->NumEntries){new(s.pDrvPrivate) Shader{};error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}CreateGeometryShader(h,p->pShaderCode,s,rt,io);}
void APIENTRY SoSetTargets(D3D10DDI_HDEVICE h,UINT count,UINT,const D3D10DDI_HRESOURCE *r,const UINT *){for(UINT i=0;i<count;i++)if(res(r[i])){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}}
SIZE_T APIENTRY CalcPrivateSamplerSize(D3D10DDI_HDEVICE,const D3D10_DDI_SAMPLER_DESC *){return sizeof(Sampler);}
static VkSamplerAddressMode address(D3D10_DDI_TEXTURE_ADDRESS_MODE m){switch(m){case D3D10_DDI_TEXTURE_ADDRESS_WRAP:return VK_SAMPLER_ADDRESS_MODE_REPEAT;case D3D10_DDI_TEXTURE_ADDRESS_MIRROR:return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;case D3D10_DDI_TEXTURE_ADDRESS_BORDER:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;case D3D10_DDI_TEXTURE_ADDRESS_MIRRORONCE:return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;default:return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;}}
void APIENTRY CreateSampler(D3D10DDI_HDEVICE h,const D3D10_DDI_SAMPLER_DESC *p,D3D10DDI_HSAMPLER hs,D3D10DDI_HRTSAMPLER) {
 Device *d=dev(h);auto *s=new(hs.pDrvPrivate) Sampler{};VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
 ci.magFilter=D3D10_DDI_DECODE_MAG_FILTER(p->Filter)==D3D10_DDI_FILTER_TYPE_LINEAR?VK_FILTER_LINEAR:VK_FILTER_NEAREST;ci.minFilter=D3D10_DDI_DECODE_MIN_FILTER(p->Filter)==D3D10_DDI_FILTER_TYPE_LINEAR?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
 ci.mipmapMode=D3D10_DDI_DECODE_MIP_FILTER(p->Filter)==D3D10_DDI_FILTER_TYPE_LINEAR?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;ci.addressModeU=address(p->AddressU);ci.addressModeV=address(p->AddressV);ci.addressModeW=address(p->AddressW);
 ci.mipLodBias=p->MipLODBias;ci.anisotropyEnable=D3D10_DDI_DECODE_IS_ANISOTROPIC_FILTER(p->Filter);ci.maxAnisotropy=(float)p->MaxAnisotropy;ci.compareEnable=D3D10_DDI_DECODE_IS_COMPARISON_FILTER(p->Filter);ci.compareOp=(VkCompareOp)(p->ComparisonFunc-1);ci.minLod=p->MinLOD;ci.maxLod=p->MaxLOD;
 ci.borderColor=p->BorderColor[3]==0?VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:p->BorderColor[0]==1?VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
 check(d,d->vk.CreateSampler(d->device,&ci,nullptr,&s->handle));
}
void APIENTRY DestroySampler(D3D10DDI_HDEVICE h,D3D10DDI_HSAMPLER hs){Device *d=dev(h);Sampler *s=object<Sampler>(hs);if(s->handle){if((d->recording || d->submitted || d->pending.submitted) && begin(d))d->retired_samplers.push_back(s->handle);else d->vk.DestroySampler(d->device,s->handle,nullptr);}s->~Sampler();}

SIZE_T APIENTRY CalcPrivateElementLayoutSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEELEMENTLAYOUT *){return sizeof(Layout);}
void APIENTRY CreateElementLayout(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEELEMENTLAYOUT *p,D3D10DDI_HELEMENTLAYOUT hl,D3D10DDI_HRTELEMENTLAYOUT){auto *l=new(hl.pDrvPrivate) Layout{};l->id=newid();l->elements.assign(p->pVertexElements,p->pVertexElements+p->NumElements);}
void APIENTRY DestroyElementLayout(D3D10DDI_HDEVICE,D3D10DDI_HELEMENTLAYOUT l){object<Layout>(l)->~Layout();}
void APIENTRY IaSetInputLayout(D3D10DDI_HDEVICE h,D3D10DDI_HELEMENTLAYOUT l){dev(h)->layout=object<Layout>(l);}
void APIENTRY IaSetTopology(D3D10DDI_HDEVICE h,D3D10_DDI_PRIMITIVE_TOPOLOGY t){dev(h)->topology=t;}
void APIENTRY IaSetVertexBuffers(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HRESOURCE *r,const UINT *strides,const UINT *offsets){Device *d=dev(h);if(first+count>32){error(d,E_INVALIDARG);return;}for(UINT i=0;i<count;i++)d->vertices[first+i]={res(r[i]),strides[i],offsets[i]};}
void APIENTRY IaSetIndexBuffer(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE r,DXGI_FORMAT f,UINT offset){Device *d=dev(h);d->indices=res(r);d->index_size=f==DXGI_FORMAT_R16_UINT?2:4;d->index_offset=offset;}
void APIENTRY Draw(D3D10DDI_HDEVICE h,UINT count,UINT first){draw(dev(h),count,first,0,1,0,false);}
void APIENTRY DrawIndexed(D3D10DDI_HDEVICE h,UINT count,UINT first,INT bias){draw(dev(h),count,first,bias,1,0,true);}
void APIENTRY DrawInstanced(D3D10DDI_HDEVICE h,UINT count,UINT instances,UINT first,UINT first_instance){draw(dev(h),count,first,0,instances,first_instance,false);}
void APIENTRY DrawIndexedInstanced(D3D10DDI_HDEVICE h,UINT count,UINT instances,UINT first,INT bias,UINT first_instance){draw(dev(h),count,first,bias,instances,first_instance,true);}
void APIENTRY DrawAuto(D3D10DDI_HDEVICE h){error(dev(h),DXGI_ERROR_UNSUPPORTED);}

SIZE_T APIENTRY CalcPrivateBlendStateSize(D3D10DDI_HDEVICE,const D3D10_DDI_BLEND_DESC *){return sizeof(Blend);}
void APIENTRY CreateBlendState(D3D10DDI_HDEVICE,const D3D10_DDI_BLEND_DESC *p,D3D10DDI_HBLENDSTATE hs,D3D10DDI_HRTBLENDSTATE){auto *s=new(hs.pDrvPrivate) Blend{};s->id=newid();s->desc=*p;}
void APIENTRY DestroyBlendState(D3D10DDI_HDEVICE,D3D10DDI_HBLENDSTATE hs){object<Blend>(hs)->~Blend();}
void APIENTRY SetBlendState(D3D10DDI_HDEVICE h,D3D10DDI_HBLENDSTATE hs,const FLOAT color[4],UINT mask){Device *d=dev(h);d->blend=object<Blend>(hs);memcpy(d->blend_color,color,16);d->sample_mask=mask;}
SIZE_T APIENTRY CalcPrivateDepthStencilStateSize(D3D10DDI_HDEVICE,const D3D10_DDI_DEPTH_STENCIL_DESC *){return sizeof(Depth);}
void APIENTRY CreateDepthStencilState(D3D10DDI_HDEVICE,const D3D10_DDI_DEPTH_STENCIL_DESC *p,D3D10DDI_HDEPTHSTENCILSTATE hs,D3D10DDI_HRTDEPTHSTENCILSTATE){auto *s=new(hs.pDrvPrivate) Depth{};s->id=newid();s->desc=*p;}
void APIENTRY DestroyDepthStencilState(D3D10DDI_HDEVICE,D3D10DDI_HDEPTHSTENCILSTATE hs){object<Depth>(hs)->~Depth();}
void APIENTRY SetDepthStencilState(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILSTATE hs,UINT ref){Device *d=dev(h);d->depth=object<Depth>(hs);d->stencil_ref=ref;}
SIZE_T APIENTRY CalcPrivateRasterizerStateSize(D3D10DDI_HDEVICE,const D3D10_DDI_RASTERIZER_DESC *){return sizeof(Raster);}
void APIENTRY CreateRasterizerState(D3D10DDI_HDEVICE,const D3D10_DDI_RASTERIZER_DESC *p,D3D10DDI_HRASTERIZERSTATE hs,D3D10DDI_HRTRASTERIZERSTATE){auto *s=new(hs.pDrvPrivate) Raster{};s->id=newid();s->desc=*p;}
void APIENTRY DestroyRasterizerState(D3D10DDI_HDEVICE,D3D10DDI_HRASTERIZERSTATE hs){object<Raster>(hs)->~Raster();}
void APIENTRY SetRasterizerState(D3D10DDI_HDEVICE h,D3D10DDI_HRASTERIZERSTATE hs){dev(h)->raster=object<Raster>(hs);}
void APIENTRY SetViewports(D3D10DDI_HDEVICE h,UINT count,UINT,const D3D10_DDI_VIEWPORT *v){if(count>1){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}if(count)dev(h)->viewport=v[0];}
void APIENTRY SetScissorRects(D3D10DDI_HDEVICE h,UINT count,UINT,const D3D10_DDI_RECT *r){if(count>1){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}if(count)dev(h)->scissor=r[0];}
void APIENTRY SetRenderTargets(D3D10DDI_HDEVICE h,const D3D10DDI_HRENDERTARGETVIEW *views,UINT count,UINT clear,D3D10DDI_HDEPTHSTENCILVIEW depth){Device *d=dev(h);if(count+clear>8){error(d,E_INVALIDARG);return;}for(UINT i=0;i<count;i++)d->targets[i]=object<View>(views[i]);for(UINT i=0;i<clear;i++)d->targets[count+i]=nullptr;d->target_count=count;d->depth_view=object<View>(depth);}
void APIENTRY GenMips(D3D10DDI_HDEVICE h,D3D10DDI_HSHADERRESOURCEVIEW hv) {
 Device *d=dev(h);View *v=object<View>(hv);Resource *r=v->resource;
 for(UINT i=v->level+1;i<v->level+v->levels;i++) {
   barrier(d);VkImageBlit b{};b.srcSubresource={aspects(v->format),i-1,v->layer,v->layers};b.dstSubresource={aspects(v->format),i,v->layer,v->layers};b.srcOffsets[1]={(INT)std::max(1u,r->width>>(i-1)),(INT)std::max(1u,r->height>>(i-1)),(INT)std::max(1u,r->depth>>(i-1))};b.dstOffsets[1]={(INT)std::max(1u,r->width>>i),(INT)std::max(1u,r->height>>i),(INT)std::max(1u,r->depth>>i)};
   r->storage.used=d->next_submit;d->vk.CmdBlitImage(d->cmd,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
 }
}
