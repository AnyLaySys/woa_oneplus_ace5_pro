#include "umd.h"
#include "rotation.h"

void APIENTRY DestroyDevice(D3D10DDI_HDEVICE h){Device *d=dev(h);destroy(d);d->~Device();}
template<class T> static void device_functions(T *f) {
#define DDI(slot,name) f->slot=name;
#define DXGI(slot,name)
#include "callbacks.inc"
#undef DXGI
#undef DDI
}
void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE,D3D10DDI_DEVICEFUNCS *f){device_functions(f);}
void APIENTRY RelocateDeviceFuncs(D3D10DDI_HDEVICE,D3D11_1DDI_DEVICEFUNCS *f){device_functions(f);d3d11_functions(f);}
static void process(Device *d){if(d->amortized && !d->processing){d->processing=true;d->amortized(d->core);d->processing=false;}}
BOOL APIENTRY Flush(D3D10DDI_HDEVICE h,UINT){Device *d=dev(h);BOOL result=flush(d);if(result)process(d);return result;}
void APIENTRY Flush(D3D10DDI_HDEVICE h){Device *d=dev(h);if(flush(d))process(d);}
void APIENTRY CheckFormatSupport(D3D10DDI_HDEVICE h,DXGI_FORMAT f,UINT *caps) {
 *caps=0;VkFormat vk=format(f);if(vk==VK_FORMAT_UNDEFINED){*caps=D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;return;}
 if(f==DXGI_FORMAT_P010){*caps=D3D11_1DDI_FORMAT_SUPPORT_DECODER_OUTPUT|D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT|D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;return;}
 if(f==DXGI_FORMAT_NV12){*caps=D3D11_1DDI_FORMAT_SUPPORT_DECODER_OUTPUT|D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT|D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT|D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET|D3D11_1DDI_FORMAT_SUPPORT_CAPTURE;return;}
 if(f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM)*caps|=D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT|D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT|D3D11_1DDI_FORMAT_SUPPORT_CAPTURE;
 Device *d=dev(h);VkFormatProperties properties{};d->vk.GetPhysicalDeviceFormatProperties(d->physical,vk,&properties);
 if(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)*caps|=D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET;
 if(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)*caps|=D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
 if(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)*caps|=D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE;
}
void APIENTRY CheckMultisampleQualityLevels(D3D10DDI_HDEVICE,DXGI_FORMAT,UINT count,UINT *levels){*levels=count==1?1:0;}
void APIENTRY SetTextFilterSize(D3D10DDI_HDEVICE h,UINT w,UINT height){if(w||height)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
SIZE_T APIENTRY CalcPrivateDeviceSize(D3D10DDI_HADAPTER,const D3D10DDIARG_CALCPRIVATEDEVICESIZE *){return sizeof(Device);}
HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER ha,D3D10DDIARG_CREATEDEVICE *p) {
 if(p->Interface!=D3D10_0_DDI_INTERFACE_VERSION && p->Interface!=D3D10_0_x_DDI_INTERFACE_VERSION && p->Interface!=D3D10_0_7_DDI_INTERFACE_VERSION && p->Interface!=D3D11_1_DDI_INTERFACE_VERSION)return E_NOINTERFACE;
 auto *a=object<Adapter>(ha);auto *d=new(p->hDrvDevice.pDrvPrivate) Device{};
 d->runtime=p->hRTDevice.handle;d->adapter=a->runtime;d->core=p->hRTCoreLayer;d->callbacks=*p->pKTCallbacks;d->errors=*p->pUMCallbacks;d->present=p->DXGIBaseDDI.pDXGIBaseCallbacks;
 if(p->Interface==D3D11_1_DDI_INTERFACE_VERSION)d->amortized=p->p11UMCallbacks->pfnPerformAmortizedProcessingCb;
 InitializeCriticalSection(&d->lock);
 if(!initialize(d)){HRESULT hr=FAILED(d->failure)?d->failure:E_FAIL;destroy(d);d->~Device();return hr;}
 if(p->Interface==D3D11_1_DDI_INTERFACE_VERSION) {
   device_functions(p->p11_1DeviceFuncs);d3d11_functions(p->p11_1DeviceFuncs);
   *p->ppfnRetrieveSubObject=retrieve_video;
 } else device_functions(p->pDeviceFuncs);
#define DDI(slot,name)
#define DXGI(slot,name) p->DXGIBaseDDI.pDXGIDDIBaseFunctions->slot=name;
#include "callbacks.inc"
#undef DXGI
#undef DDI
 if(p->Interface==D3D10_0_7_DDI_INTERFACE_VERSION || p->Interface==D3D11_1_DDI_INTERFACE_VERSION)p->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnResolveSharedResource=_ResolveSharedResource;
 return S_OK;
}
static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER h){delete object<Adapter>(h);return S_OK;}
static HRESULT open_adapter(D3D10DDIARG_OPENADAPTER *p) {
 A830_ADAPTER_INFO info{};D3DDDICB_QUERYADAPTERINFO query{&info,sizeof(info)};
 if(!p->pAdapterCallbacks || !p->pAdapterCallbacks->pfnQueryAdapterInfoCb)return E_INVALIDARG;
 HRESULT hr=p->pAdapterCallbacks->pfnQueryAdapterInfoCb(p->hRTAdapter.handle,&query);
 if(FAILED(hr) || info.Version!=A830_ABI_VERSION || !(info.Flags&2))return DXGI_ERROR_UNSUPPORTED;
 auto *a=new(std::nothrow) Adapter{p->hRTAdapter.handle};if(!a)return E_OUTOFMEMORY;
 p->hAdapter.pDrvPrivate=a;p->pAdapterFuncs->pfnCalcPrivateDeviceSize=CalcPrivateDeviceSize;p->pAdapterFuncs->pfnCreateDevice=CreateDevice;p->pAdapterFuncs->pfnCloseAdapter=CloseAdapter;
 return S_OK;
}
extern "C" HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *p){return open_adapter(p);}
static HRESULT APIENTRY versions(D3D10DDI_HADAPTER,UINT32 *count,UINT64 *values) {
 const UINT64 supported[]={D3D11_1_DDI_SUPPORTED,D3D10_0_DDI_SUPPORTED,D3D10_0_x_DDI_SUPPORTED,D3D10_0_7_DDI_SUPPORTED};
 if(values && *count<std::size(supported))return E_OUTOFMEMORY;*count=(UINT32)std::size(supported);if(values)memcpy(values,supported,sizeof(supported));return S_OK;
}
static HRESULT APIENTRY caps(D3D10DDI_HADAPTER,const D3D10_2DDIARG_GETCAPS *p) {
 if(!p->pData)return E_INVALIDARG;memset(p->pData,0,p->DataSize);
 if(p->Type==D3D11DDICAPS_3DPIPELINESUPPORT) {
   if(p->DataSize!=sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS))return E_INVALIDARG;
   ((D3D11DDI_3DPIPELINESUPPORT_CAPS *)p->pData)->Caps=D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0);
 }
 return S_OK;
}
extern "C" HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *p){HRESULT hr=open_adapter(p);if(SUCCEEDED(hr)){p->pAdapterFuncs_2->pfnGetSupportedVersions=versions;p->pAdapterFuncs_2->pfnGetCaps=caps;}return hr;}
HRESULT APIENTRY _Present(DXGI_DDI_ARG_PRESENT *p) {
 Device *d=dev(p->hDevice);Resource *r=res(p->hSurfaceToPresent);if(!r || !r->allocation)return E_INVALIDARG;
 if(!flush(d))return FAILED(d->failure)?d->failure:E_FAIL;
 DXGIDDICB_PRESENT present{};present.hSrcAllocation=r->allocation;if(p->hDstResource)present.hDstAllocation=res(p->hDstResource)->allocation;present.hContext=d->context;present.pDXGIContext=p->pDXGIContext;
 EnterCriticalSection(&d->lock);HRESULT hr=d->present->pfnPresentCb(d->runtime,&present);LeaveCriticalSection(&d->lock);
 if(FAILED(hr))error(d,hr);return hr;
}
HRESULT APIENTRY _ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *p){Device *d=dev(p->hDevice);return flush(d,true)?S_OK:d->failure;}
HRESULT APIENTRY _SetDisplayMode(DXGI_DDI_ARG_SETDISPLAYMODE *p){Device *d=dev(p->hDevice);Resource *r=res(p->hResource);if(!r || !r->allocation)return E_INVALIDARG;D3DDDICB_SETDISPLAYMODE mode{};mode.hPrimaryAllocation=r->allocation;return d->callbacks.pfnSetDisplayModeCb(d->runtime,&mode);}
HRESULT APIENTRY _SetResourcePriority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *){return S_OK;}
HRESULT APIENTRY _QueryResourceResidency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *p){for(UINT i=0;i<p->Resources;i++)p->pStatus[i]=DXGI_DDI_RESIDENCY_FULLY_RESIDENT;return S_OK;}
HRESULT APIENTRY _GetGammaCaps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *){return DXGI_ERROR_UNSUPPORTED;}
HRESULT APIENTRY _RotateResourceIdentities(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *p) {
 Device *d=dev(p->hDevice);if(p->Resources<2)return S_OK;if(!flush(d))return d->failure;
 EnterCriticalSection(&d->lock);
 for(UINT i=0;i<p->Resources;i++)if(!res(p->pResources[i])){LeaveCriticalSection(&d->lock);return E_INVALIDARG;}
 Resource *first=res(p->pResources[0]);
 Storage saved=first->storage;D3DKMT_HANDLE allocation=first->allocation,kmresource=first->kmresource;
 for(UINT i=0;i<p->Resources;i++) {
   Resource *dst=res(p->pResources[i]);Resource *src=i+1<p->Resources?res(p->pResources[i+1]):nullptr;
   dst->storage=src?src->storage:saved;dst->allocation=src?src->allocation:allocation;dst->kmresource=src?src->kmresource:kmresource;dst->generation++;d->resource_revision++;
 }
 LeaveCriticalSection(&d->lock);
 return S_OK;
}
HRESULT blit(Device *d,View *target,const std::array<View *,2> &inputs,Shader *pixel,const float *values,UINT size,const RECT &rect) {
 if(!d->blit_vertex.module && !shader_create(d,&d->blit_vertex,rotation_vs_code,&rotation_vs_io,0))return d->failure;
 if(!begin(d))return d->failure;
 UINT alignment=(UINT)std::max(d->properties.limits.minUniformBufferOffsetAlignment,d->properties.limits.nonCoherentAtomSize);
 UINT offset=(d->constants_offset+alignment-1)/alignment*alignment;
 if(!d->blit_constants.storage.buffer || offset+128>d->blit_constants.width) {
   if(d->blit_constants.storage.buffer){d->transient_bytes+=d->blit_constants.storage.size;d->temporary.push_back(std::move(d->blit_constants.storage));}d->blit_constants={};
   d->blit_constants.dimension=D3D10DDIRESOURCE_BUFFER;d->blit_constants.width=65536;
   if(!buffer_create(d,d->blit_constants.storage,65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))return d->failure;offset=0;d->resource_revision++;
 }
 d->constants_offset=offset+128;
 memcpy((char *)d->blit_constants.storage.map+offset,values,size);if(!sync_memory(d,d->blit_constants.storage,false,offset,size))return d->failure;
 Raster raster{};raster.desc.FillMode=D3D10_DDI_FILL_SOLID;raster.desc.CullMode=D3D10_DDI_CULL_NONE;raster.desc.DepthClipEnable=TRUE;
 auto shaders=d->shaders;auto cb=d->cb;auto cb_first=d->cb_first;auto cb_count=d->cb_count;auto samplers=d->samplers;auto textures=d->textures;auto vertices=d->vertices;auto targets=d->targets;
 Layout *layout=d->layout;Blend *blend=d->blend;Depth *depth=d->depth;Raster *old_raster=d->raster;View *depth_view=d->depth_view;
 UINT target_count=d->target_count,sample_mask=d->sample_mask;auto topology=d->topology;auto viewport=d->viewport;
 end_pass(d);d->shaders={&d->blit_vertex,pixel,nullptr};d->cb={};d->cb_first={};d->cb_count={};d->cb[0][0]=d->cb[1][0]=&d->blit_constants;d->cb_first[0][0]=d->cb_first[1][0]=offset;d->cb_count[0][0]=d->cb_count[1][0]=128;d->pipeline_dirty=true;d->descriptor_dirty.fill(true);
 d->samplers={};d->textures={};d->textures[1][0]=inputs[0];d->textures[1][1]=inputs[1];d->vertices={};
 d->targets={target};d->target_count=1;d->depth_view=nullptr;d->layout=nullptr;d->blend=nullptr;d->depth=nullptr;d->raster=&raster;d->sample_mask=~0u;
 d->topology=D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST;d->viewport={(FLOAT)rect.left,(FLOAT)rect.top,(FLOAT)(rect.right-rect.left),(FLOAT)(rect.bottom-rect.top),0,1};
 bool okay=draw(d,3,0,0,1,0,false);end_pass(d);
 d->shaders=shaders;d->cb=cb;d->cb_first=cb_first;d->cb_count=cb_count;d->samplers=samplers;d->textures=textures;d->vertices=vertices;d->targets=targets;
 d->layout=layout;d->blend=blend;d->depth=depth;d->raster=old_raster;d->depth_view=depth_view;d->target_count=target_count;d->sample_mask=sample_mask;d->topology=topology;d->viewport=viewport;d->pipeline_dirty=true;d->descriptor_dirty.fill(true);
 return okay?S_OK:FAILED(d->failure)?d->failure:E_FAIL;
}
static HRESULT rotated_blt(Device *d,DXGI_DDI_ARG_BLT *p) {
 Resource *src=res(p->hSrcResource),*dst=res(p->hDstResource);
 if(!src || !dst || src==dst || p->SrcSubresource>=src->levels*src->layers || p->DstSubresource>=dst->levels*dst->layers ||
    p->DstRight<=p->DstLeft || p->DstBottom<=p->DstTop)return E_INVALIDARG;
 if(!d->rotation_pixel.module && !shader_create(d,&d->rotation_pixel,rotation_ps_code,&rotation_ps_io,1))return d->failure;
 View source{};source.resource=src;source.format=src->format;source.level=p->SrcSubresource%src->levels;source.layer=p->SrcSubresource/src->levels;
 View target{};target.resource=dst;target.format=dst->format;target.level=p->DstSubresource%dst->levels;target.layer=p->DstSubresource/dst->levels;
 HRESULT hr=blit(d,&target,{&source,nullptr},&d->rotation_pixel,blit_matrices[(UINT)p->Rotate-1],32,{(LONG)p->DstLeft,(LONG)p->DstTop,(LONG)p->DstRight,(LONG)p->DstBottom});
 view_destroy(d,&source);view_destroy(d,&target);return hr;
}
HRESULT APIENTRY _Blt(DXGI_DDI_ARG_BLT *p) {
 Device *d=dev(p->hDevice);Resource *src=res(p->hSrcResource),*dst=res(p->hDstResource);
 if(!src || !dst)return E_INVALIDARG;
 if(p->Rotate>=DXGI_DDI_MODE_ROTATION_ROTATE90 && p->Rotate<=DXGI_DDI_MODE_ROTATION_ROTATE270)return rotated_blt(d,p);
 if(p->Rotate!=DXGI_DDI_MODE_ROTATION_IDENTITY && p->Rotate!=DXGI_DDI_MODE_ROTATION_UNSPECIFIED)return DXGI_ERROR_UNSUPPORTED;
 UINT sl=p->SrcSubresource%src->levels,dl=p->DstSubresource%dst->levels;
 if(src->format==dst->format && (src==dst || (src->storage.external && src->storage.address==dst->storage.address)) && p->SrcSubresource==p->DstSubresource && !p->DstLeft && !p->DstTop && p->DstRight==std::max(1u,src->width>>sl) && p->DstBottom==std::max(1u,src->height>>sl))return S_OK;
 if(src->format==dst->format && p->DstRight-p->DstLeft==std::max(1u,src->width>>sl) && p->DstBottom-p->DstTop==std::max(1u,src->height>>sl)) {
   ResourceCopyRegion(D3D10DDI_HDEVICE{d},D3D10DDI_HRESOURCE{dst},p->DstSubresource,p->DstLeft,p->DstTop,0,D3D10DDI_HRESOURCE{src},p->SrcSubresource,nullptr);return d->failure;
 }
 access(d,src->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,false,sl,1,p->SrcSubresource/src->levels,1);access(d,dst->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,false,dl,1,p->DstSubresource/dst->levels,1);
 VkImageBlit b{};b.srcSubresource={aspects(src->format),sl,p->SrcSubresource/src->levels,1};b.dstSubresource={aspects(dst->format),dl,p->DstSubresource/dst->levels,1};b.srcOffsets[1]={(INT)std::max(1u,src->width>>sl),(INT)std::max(1u,src->height>>sl),1};b.dstOffsets[0]={(INT)p->DstLeft,(INT)p->DstTop,0};b.dstOffsets[1]={(INT)p->DstRight,(INT)p->DstBottom,1};
 d->vk.CmdBlitImage(d->cmd,src->storage.image,VK_IMAGE_LAYOUT_GENERAL,dst->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_NEAREST);return S_OK;
}
