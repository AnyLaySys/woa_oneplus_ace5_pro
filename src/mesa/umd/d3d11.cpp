#include "umd.h"

SIZE_T APIENTRY CalcPrivateResourceSize(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATERESOURCE *){return sizeof(Resource);}
void APIENTRY CreateResource(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATERESOURCE *p,D3D10DDI_HRESOURCE r,D3D10DDI_HRTRESOURCE rt) {
 D3D10DDIARG_CREATERESOURCE desc{};
 desc.pMipInfoList=p->pMipInfoList;desc.pInitialDataUP=p->pInitialDataUP;desc.ResourceDimension=p->ResourceDimension;
 desc.Usage=p->Usage;desc.BindFlags=p->BindFlags;desc.MapFlags=p->MapFlags;desc.MiscFlags=p->MiscFlags;
 desc.Format=p->Format;desc.SampleDesc=p->SampleDesc;desc.MipLevels=p->MipLevels;desc.ArraySize=p->ArraySize;desc.pPrimaryDesc=p->pPrimaryDesc;
 CreateResource(h,&desc,r,rt);
}
SIZE_T APIENTRY CalcPrivateShaderResourceViewSize(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATESHADERRESOURCEVIEW *){return sizeof(View);}
void APIENTRY CreateShaderResourceView(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATESHADERRESOURCEVIEW *p,D3D10DDI_HSHADERRESOURCEVIEW v,D3D10DDI_HRTSHADERRESOURCEVIEW rt) {
 D3D10DDIARG_CREATESHADERRESOURCEVIEW desc{};desc.hDrvResource=p->hDrvResource;desc.Format=p->Format;desc.ResourceDimension=p->ResourceDimension;
 memcpy(&desc.Buffer,&p->Buffer,sizeof(desc.Buffer)>sizeof(desc.Tex2D)?sizeof(desc.Buffer):sizeof(desc.Tex2D));CreateShaderResourceView(h,&desc,v,rt);
}
SIZE_T APIENTRY CalcPrivateDepthStencilViewSize(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *){return sizeof(View);}
void APIENTRY CreateDepthStencilView(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *p,D3D10DDI_HDEPTHSTENCILVIEW v,D3D10DDI_HRTDEPTHSTENCILVIEW rt) {
 D3D10DDIARG_CREATEDEPTHSTENCILVIEW desc{};desc.hDrvResource=p->hDrvResource;desc.Format=p->Format;desc.ResourceDimension=p->ResourceDimension;desc.Tex2D=p->Tex2D;CreateDepthStencilView(h,&desc,v,rt);
}
void APIENTRY ResourceCopyRegion(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT ds,UINT x,UINT y,UINT z,D3D10DDI_HRESOURCE src,UINT ss,const D3D10_DDI_BOX *box,UINT){ResourceCopyRegion(h,dst,ds,x,y,z,src,ss,box);}
void APIENTRY ResourceUpdateSubResourceUP(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE r,UINT sub,const D3D10_DDI_BOX *box,const void *data,UINT pitch,UINT slice,UINT){ResourceUpdateSubResourceUP(h,r,sub,box,data,pitch,slice);}
void APIENTRY SetRenderTargets(D3D10DDI_HDEVICE h,const D3D10DDI_HRENDERTARGETVIEW *v,UINT count,UINT clear,D3D10DDI_HDEPTHSTENCILVIEW depth,const D3D11DDI_HUNORDEREDACCESSVIEW *,const UINT *,UINT,UINT uavs,UINT,UINT) {
 if(uavs){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}SetRenderTargets(h,v,count,clear,depth);
}
#define CONSTANTS(stage,index) \
void APIENTRY stage##SetConstantBuffers(D3D10DDI_HDEVICE h,UINT start,UINT count,const D3D10DDI_HRESOURCE *r,const UINT *first,const UINT *sizes) { \
 stage##SetConstantBuffers(h,start,count,r);if(start+count>16)return;for(UINT i=0;i<count;i++){dev(h)->cb_first[index][start+i]=first?first[i]*16:0;dev(h)->cb_count[index][start+i]=sizes?sizes[i]*16:0;} \
}
CONSTANTS(Vs,0)
CONSTANTS(Ps,1)
CONSTANTS(Gs,2)
#undef CONSTANTS
SIZE_T APIENTRY CalcPrivateBlendStateSize(D3D10DDI_HDEVICE,const D3D11_1_DDI_BLEND_DESC *){return sizeof(Blend);}
void APIENTRY CreateBlendState(D3D10DDI_HDEVICE h,const D3D11_1_DDI_BLEND_DESC *p,D3D10DDI_HBLENDSTATE s,D3D10DDI_HRTBLENDSTATE rt) {
 D3D10_DDI_BLEND_DESC desc{};desc.AlphaToCoverageEnable=p->AlphaToCoverageEnable;
 for(UINT i=0;i<8;i++){desc.BlendEnable[i]=p->RenderTarget[i].BlendEnable;desc.RenderTargetWriteMask[i]=p->RenderTarget[i].RenderTargetWriteMask;if(p->RenderTarget[i].LogicOpEnable){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}}
 desc.SrcBlend=p->RenderTarget[0].SrcBlend;desc.DestBlend=p->RenderTarget[0].DestBlend;desc.BlendOp=p->RenderTarget[0].BlendOp;
 desc.SrcBlendAlpha=p->RenderTarget[0].SrcBlendAlpha;desc.DestBlendAlpha=p->RenderTarget[0].DestBlendAlpha;desc.BlendOpAlpha=p->RenderTarget[0].BlendOpAlpha;
 CreateBlendState(h,&desc,s,rt);
}
SIZE_T APIENTRY CalcPrivateRasterizerStateSize(D3D10DDI_HDEVICE,const D3D11_1_DDI_RASTERIZER_DESC *){return sizeof(Raster);}
void APIENTRY CreateRasterizerState(D3D10DDI_HDEVICE h,const D3D11_1_DDI_RASTERIZER_DESC *p,D3D10DDI_HRASTERIZERSTATE s,D3D10DDI_HRTRASTERIZERSTATE rt) {
 D3D10_DDI_RASTERIZER_DESC desc{};desc.FillMode=p->FillMode;desc.CullMode=p->CullMode;desc.FrontCounterClockwise=p->FrontCounterClockwise;
 desc.DepthBias=p->DepthBias;desc.DepthBiasClamp=p->DepthBiasClamp;desc.SlopeScaledDepthBias=p->SlopeScaledDepthBias;desc.DepthClipEnable=p->DepthClipEnable;
 desc.ScissorEnable=p->ScissorEnable;desc.MultisampleEnable=p->MultisampleEnable;desc.AntialiasedLineEnable=p->AntialiasedLineEnable;CreateRasterizerState(h,&desc,s,rt);
}
SIZE_T APIENTRY CalcPrivateShaderSize(D3D10DDI_HDEVICE,const UINT *,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *){return sizeof(Shader);}
static bool create_shader(Device *d,Shader *s,const UINT *code,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *p,UINT stage) {
 std::vector<D3D10DDIARG_SIGNATURE_ENTRY> input,output;
 if(p) {
   for(UINT i=0;i<p->NumInputSignatureEntries;i++){auto &e=p->pInputSignature[i];input.push_back({e.SystemValue,e.Register,e.Mask});}
   for(UINT i=0;i<p->NumOutputSignatureEntries;i++){auto &e=p->pOutputSignature[i];output.push_back({e.SystemValue,e.Register,e.Mask});}
 }
 D3D10DDIARG_STAGE_IO_SIGNATURES io{input.data(),(UINT)input.size(),output.data(),(UINT)output.size()};return shader_create(d,s,code,&io,stage);
}
void APIENTRY CreateVertexShader(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *p){create_shader(dev(h),new(s.pDrvPrivate) Shader{},code,p,0);}
void APIENTRY CreatePixelShader(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *p){create_shader(dev(h),new(s.pDrvPrivate) Shader{},code,p,1);}
void APIENTRY CreateGeometryShader(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *p){create_shader(dev(h),new(s.pDrvPrivate) Shader{},code,p,2);}
SIZE_T APIENTRY CalcPrivateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *){return sizeof(Shader);}
void APIENTRY CreateGeometryShaderWithStreamOutput(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT *p,D3D10DDI_HSHADER s,D3D10DDI_HRTSHADER rt,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *io) {
 if(p->NumEntries){new(s.pDrvPrivate) Shader{};error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}CreateGeometryShader(h,p->pShaderCode,s,rt,io);
}
static void APIENTRY Discard(D3D10DDI_HDEVICE h,D3D11DDI_HANDLETYPE type,void *handle,const D3D10_DDI_RECT *,UINT count) {
 if(count || !handle)return;Resource *r=nullptr;
 if(type==D3D10DDI_HT_RESOURCE)r=(Resource *)handle;
 else if(type==D3D10DDI_HT_RENDERTARGETVIEW || type==D3D10DDI_HT_DEPTHSTENCILVIEW || type==D3D10DDI_HT_SHADERRESOURCEVIEW)r=((View *)handle)->resource;
 if(r && r->levels==1 && r->layers==1 && r->storage.image){end_pass(dev(h));r->storage.discarded=r->storage.aspects;}
}
static void APIENTRY CheckDirectFlipSupport(D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE,D3D10DDI_HRESOURCE,UINT,BOOL *supported){*supported=FALSE;}
static void APIENTRY SetResourceMinLOD(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE,FLOAT lod){if(lod)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
#define SHADER(stage) \
static void APIENTRY stage##SetShaderWithIfaces(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER s,UINT count,const UINT *,const D3D11DDIARG_POINTERDATA *){if(count){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}stage##SetShader(h,s);}
SHADER(Vs)
SHADER(Ps)
SHADER(Gs)
#undef SHADER
static void APIENTRY ClearView(D3D10DDI_HDEVICE h,D3D11DDI_HANDLETYPE type,void *handle,const FLOAT color[4],const D3D10_DDI_RECT *rects,UINT count) {
 if(type==D3D11_1DDI_HT_VIDEODECODEROUTPUTVIEW){VideoDecode::Clear(dev(h),handle,color,rects,count);return;}
 if(type!=D3D10DDI_HT_RENDERTARGETVIEW){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}
 D3D10DDI_HRENDERTARGETVIEW hv{handle};if(!count){FLOAT value[4];memcpy(value,color,sizeof(value));ClearRenderTargetView(h,hv,value);return;}
 Device *d=dev(h);auto *v=(View *)handle;Resource *r=v->resource;
 if(v->format!=DXGI_FORMAT_A8_UNORM && v->format!=DXGI_FORMAT_B8G8R8A8_UNORM && v->format!=DXGI_FORMAT_R8G8B8A8_UNORM){error(dev(h),DXGI_ERROR_UNSUPPORTED);return;}
 UINT width=std::max(1u,r->width>>v->level),height=std::max(1u,r->height>>v->level);std::vector<VkClearRect> areas;
 areas.reserve(count);
 for(UINT i=0;i<count;i++) {
   LONG left=std::max(0L,rects[i].left),top=std::max(0L,rects[i].top),right=std::min((LONG)width,rects[i].right),bottom=std::min((LONG)height,rects[i].bottom);
   if(right>left && bottom>top)areas.push_back({{{left,top},{(UINT)(right-left),(UINT)(bottom-top)}},0,1});
 }
 if(areas.empty() || !view_create(d,v) || !begin(d))return;
 if(!v->image){error(d,DXGI_ERROR_UNSUPPORTED);return;}
 std::vector<VkImageView> views{v->image};std::array<VkFormat,9> formats{};formats[0]=format(v->format);
 VkRenderPass pass=render_pass(d,formats,1);if(!pass)return;
 if(d->pass_views!=views || d->pass_formats!=formats || d->pass_targets!=1)end_pass(d);
 access(d,r->storage,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,true,v->level,1,v->layer,v->layers);
 if(!begin_pass(d,pass,views,formats,width,height,1))return;
 VkClearAttachment clear{};clear.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
 for(UINT i=0;i<4;i++)clear.clearValue.color.float32[i]=(BYTE)std::clamp(color[i]*255+0.5f,0.0f,255.0f)/255.0f;
 d->vk.CmdClearAttachments(d->cmd,1,&clear,(UINT)areas.size(),areas.data());
}
void d3d11_functions(D3D11_1DDI_DEVICEFUNCS *f) {
 f->pfnResourceConvert=ResourceCopy;f->pfnResourceConvertRegion=ResourceCopyRegion;
 f->pfnVsSetShaderWithIfaces=VsSetShaderWithIfaces;f->pfnPsSetShaderWithIfaces=PsSetShaderWithIfaces;f->pfnGsSetShaderWithIfaces=GsSetShaderWithIfaces;
 f->pfnSetResourceMinLOD=SetResourceMinLOD;f->pfnDiscard=Discard;f->pfnAssignDebugBinary=nullptr;
 f->pfnDynamicConstantBufferMapNoOverwrite=ResourceMap;f->pfnCheckDirectFlipSupport=CheckDirectFlipSupport;f->pfnClearView=ClearView;
}
