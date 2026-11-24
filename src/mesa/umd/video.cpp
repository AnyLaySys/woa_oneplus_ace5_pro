#include "umd.h"

template<class T> struct Unsupported;
template<class R,class... A> struct Unsupported<R (APIENTRY *)(D3D10DDI_HDEVICE,A...)> {
 static R APIENTRY call(D3D10DDI_HDEVICE h,A...) {
   if constexpr(std::is_same_v<R,HRESULT>)return DXGI_ERROR_UNSUPPORTED;
   else {error(dev(h),DXGI_ERROR_UNSUPPORTED);if constexpr(!std::is_void_v<R>)return 0;}
 }
};
struct VideoProcessor {
 RECT source{},destination{},target{};
 D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE input{},output{};
 D3D11_1DDI_VIDEO_COLOR background{};
 D3D11_1DDI_VIDEO_PROCESSOR_ROTATION rotation=D3D11_1DDI_VIDEO_PROCESSOR_ROTATION_IDENTITY;
 bool background_yuv=false;
};
struct VideoInput { View image,chroma; };
static bool rgb(DXGI_FORMAT f){return f==DXGI_FORMAT_B8G8R8A8_UNORM || f==DXGI_FORMAT_R8G8B8A8_UNORM;}
static bool rect_valid(const RECT &r,UINT width,UINT height){return r.left>=0 && r.top>=0 && r.right>r.left && r.bottom>r.top && (UINT)r.right<=width && (UINT)r.bottom<=height;}
static RECT full(Resource *r,UINT level){return {0,0,(LONG)std::max(1u,r->width>>level),(LONG)std::max(1u,r->height>>level)};}
static bool video_shader(Device *d) {
 if(d->video_pixel.module)return true;
 static const char source[]=R"(
cbuffer Params : register(b0) { float4 row0,row1,destination,source,flags,input,output,background; };
Texture2D<float4> image : register(t0);
Texture2D<float2> chroma : register(t1);
float3 yuv(float3 color,float2 space) {
 float y=space.x?(color.x-16.0/255)*255/219:color.x;
 float2 uv=(color.yz-128.0/255)*(space.x?255.0/224:1);
 return space.y?float3(y+1.5748*uv.y,y-0.187324*uv.x-0.468124*uv.y,y+1.8556*uv.x):
                float3(y+1.402*uv.y,y-0.344136*uv.x-0.714136*uv.y,y+1.772*uv.x);
}
float4 main(float4 position : SV_Position) : SV_Target {
 float3 color=flags.z?yuv(background.xyz,output.xy):background.xyz;
 float2 uv=(position.xy-destination.xy)/destination.zw;
 if(flags.y && all(uv>=0) && all(uv<1)) {
   float2 rotated=float2(dot(float3(uv,1),row0.xyz),dot(float3(uv,1),row1.xyz));
   int2 pixel=int2(source.xy+min(source.zw-1,floor(rotated*source.zw)));
   float4 texel=image.Load(int3(pixel,0));
   if(flags.x)color=yuv(float3(texel.x,chroma.Load(int3(pixel>>1,0))),input.xy);
   else color=input.z?(texel.xyz-16.0/255)*255/219:texel.xyz;
 }
 if(flags.w)color=color*219/255+16.0/255;
 return float4(floor(saturate(color)*255+0.5)/255,1);
}
)";
 vkd3d_shader_hlsl_source_info hlsl{};hlsl.type=VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;hlsl.profile="ps_4_0";
 vkd3d_shader_compile_info ci{};ci.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;ci.next=&hlsl;ci.source={source,sizeof(source)-1};ci.source_type=VKD3D_SHADER_SOURCE_HLSL;ci.target_type=VKD3D_SHADER_TARGET_DXBC_TPF;ci.log_level=VKD3D_SHADER_LOG_NONE;
 vkd3d_shader_code code{};vkd3d_shader_dxbc_desc desc{};bool okay=false;
 D3D10DDIARG_SIGNATURE_ENTRY in{D3D10_SB_NAME_POSITION,0,15},out{(D3D10_SB_NAME)64,0,15};D3D10DDIARG_STAGE_IO_SIGNATURES io{&in,1,&out,1};
 if(vkd3d_shader_compile(&ci,&code,nullptr)>=0 && vkd3d_shader_parse_dxbc(&code,0,&desc,nullptr)>=0)
   for(size_t i=0;i<desc.section_count;i++)if(desc.sections[i].tag==0x52444853 || desc.sections[i].tag==0x58454853){okay=shader_create(d,&d->video_pixel,(const UINT *)desc.sections[i].data.code,&io,1);break;}
 vkd3d_shader_free_dxbc(&desc);vkd3d_shader_free_shader_code(&code);if(!okay && SUCCEEDED(d->failure))error(d,E_FAIL);return okay;
}
static void APIENTRY GetVideoDecoderProfileCount(D3D10DDI_HDEVICE,UINT *count){*count=0;}
static void APIENTRY CheckVideoDecoderFormat(D3D10DDI_HDEVICE,const GUID *,DXGI_FORMAT,BOOL *supported){*supported=FALSE;}
static void APIENTRY GetVideoDecoderConfigCount(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *,UINT *count){*count=0;}
static SIZE_T APIENTRY CalcPrivateVideoProcessorEnumSize(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *){return sizeof(D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC);}
static HRESULT APIENTRY CreateVideoProcessorEnum(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM *p,D3D11_1DDI_HVIDEOPROCESSORENUM h,D3D11_1DDI_HRTVIDEOPROCESSORENUM) {
 if(p->Desc.InputFrameFormat!=D3D11_1DDI_VIDEO_FRAME_FORMAT_PROGRESSIVE)return DXGI_ERROR_UNSUPPORTED;
 if(!p->Desc.InputWidth || !p->Desc.InputHeight || !p->Desc.OutputWidth || !p->Desc.OutputHeight)return E_INVALIDARG;
 new(h.pDrvPrivate) D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC(p->Desc);return S_OK;
}
static void APIENTRY DestroyVideoProcessorEnum(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM){}
static void APIENTRY CheckVideoProcessorFormat(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM,DXGI_FORMAT f,UINT *caps){*caps=rgb(f)?3:f==DXGI_FORMAT_NV12?1:0;}
static void APIENTRY GetVideoProcessorCaps(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSORENUM,D3D11_1DDI_VIDEO_PROCESSOR_CAPS *p) {
 *p={};p->DeviceCaps=D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_YCbCr_MATRIX_CONVERSION|D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_NOMINAL_RANGE|D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_RGB_RANGE_CONVERSION;
 p->FeatureCaps=D3D11_1DDI_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION;p->RateConversionCapsCount=p->MaxInputStreams=p->MaxStreamStates=1;
}
static void APIENTRY GetVideoProcessorRateConversionCaps(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSORENUM,UINT index,D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS *p){*p={};if(index)error(dev(h),E_INVALIDARG);}
static SIZE_T APIENTRY CalcPrivateVideoProcessorSize(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *){return sizeof(VideoProcessor);}
static HRESULT APIENTRY CreateVideoProcessor(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSOR *p,D3D11_1DDI_HVIDEOPROCESSOR h,D3D11_1DDI_HRTVIDEOPROCESSOR) {
 if(p->RateConversionCapsIndex)return E_INVALIDARG;new(h.pDrvPrivate) VideoProcessor{};return S_OK;
}
static void APIENTRY DestroyVideoProcessor(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR h){object<VideoProcessor>(h)->~VideoProcessor();}
static void APIENTRY VideoProcessorSetOutputTargetRect(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR h,BOOL enabled,const RECT *r){object<VideoProcessor>(h)->target=enabled?*r:RECT{};}
static void APIENTRY VideoProcessorSetOutputBackgroundColor(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR h,BOOL is_yuv,const D3D11_1DDI_VIDEO_COLOR *color){auto *p=object<VideoProcessor>(h);p->background=*color;p->background_yuv=is_yuv!=FALSE;}
static void APIENTRY VideoProcessorSetOutputColorSpace(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEOPROCESSOR h,const D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE *s){object<VideoProcessor>(h)->output=*s;}
static void APIENTRY VideoProcessorSetOutputAlphaFillMode(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,D3D11_1DDI_VIDEO_PROCESSOR_ALPHA_FILL_MODE mode,UINT){if(mode!=D3D11_1DDI_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetOutputConstriction(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,BOOL enabled,SIZE){if(enabled)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetOutputStereoMode(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,BOOL enabled){if(enabled)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamFrameFormat(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,D3D11_1DDI_VIDEO_FRAME_FORMAT format){if(stream || format!=D3D11_1DDI_VIDEO_FRAME_FORMAT_PROGRESSIVE)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamColorSpace(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,const D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE *s){if(stream){error(dev(h),E_INVALIDARG);return;}object<VideoProcessor>(p)->input=*s;}
static void APIENTRY VideoProcessorSetStreamOutputRate(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,D3D11_1DDI_VIDEO_PROCESSOR_OUTPUT_RATE rate,BOOL,const DXGI_RATIONAL *){if(stream || rate!=D3D11_1DDI_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamSourceRect(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enabled,const RECT *r){if(stream){error(dev(h),E_INVALIDARG);return;}object<VideoProcessor>(p)->source=enabled?*r:RECT{};}
static void APIENTRY VideoProcessorSetStreamDestRect(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enabled,const RECT *r){if(stream){error(dev(h),E_INVALIDARG);return;}object<VideoProcessor>(p)->destination=enabled?*r:RECT{};}
static void APIENTRY VideoProcessorSetStreamAlpha(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,BOOL enabled,FLOAT alpha){if(stream || (enabled && alpha!=1))error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamPixelAspectRatio(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,BOOL enabled,const DXGI_RATIONAL *,const DXGI_RATIONAL *){if(stream || enabled)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamLumaKey(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,BOOL enabled,FLOAT,FLOAT){if(stream || enabled)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamStereoFormat(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,BOOL enabled,D3D11_1DDI_VIDEO_PROCESSOR_STEREO_FORMAT,BOOL,BOOL,D3D11_1DDI_VIDEO_PROCESSOR_STEREO_FLIP_MODE,int){if(stream || enabled)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamAutoProcessingMode(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,BOOL){if(stream)error(dev(h),E_INVALIDARG);}
static void APIENTRY VideoProcessorSetStreamFilter(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR,UINT stream,D3D11_1DDI_VIDEO_PROCESSOR_FILTER,BOOL enabled,int){if(stream || enabled)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
static void APIENTRY VideoProcessorSetStreamRotation(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR p,UINT stream,BOOL enabled,D3D11_1DDI_VIDEO_PROCESSOR_ROTATION rotation){if(stream || rotation>D3D11_1DDI_VIDEO_PROCESSOR_ROTATION_270){error(dev(h),E_INVALIDARG);return;}object<VideoProcessor>(p)->rotation=enabled?rotation:D3D11_1DDI_VIDEO_PROCESSOR_ROTATION_IDENTITY;}
template<class P,class H> static HRESULT create_view(Device *d,const P *p,H h,bool output) {
 Resource *r=res(p->hDrvResource);if(!r || p->MipSlice>=r->levels || p->FirstArraySlice>=r->layers || p->ArraySize!=1 || (output?!rgb(r->format):!rgb(r->format) && r->format!=DXGI_FORMAT_NV12))return DXGI_ERROR_UNSUPPORTED;
 auto *v=new(h.pDrvPrivate) View{};v->resource=r;v->format=r->format;v->level=p->MipSlice;v->layer=p->FirstArraySlice;
 return FAILED(d->failure)?d->failure:S_OK;
}
static SIZE_T APIENTRY CalcPrivateVideoProcessorInputViewSize(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW *){return sizeof(VideoInput);}
static HRESULT APIENTRY CreateVideoProcessorInputView(D3D10DDI_HDEVICE h,const D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW *p,D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW v,D3D11_1DDI_HRTVIDEOPROCESSORINPUTVIEW) {
 if(p->FourCC)return DXGI_ERROR_UNSUPPORTED;auto *input=new(v.pDrvPrivate) VideoInput{};HRESULT hr=create_view(dev(h),p,v,false);
 if(SUCCEEDED(hr) && input->image.format==DXGI_FORMAT_NV12){input->image.format=DXGI_FORMAT_R8_UNORM;input->chroma=input->image;input->chroma.format=DXGI_FORMAT_R8G8_UNORM;}return hr;
}
static void APIENTRY DestroyVideoProcessorInputView(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW v){auto *input=object<VideoInput>(v);view_destroy(dev(h),&input->image);view_destroy(dev(h),&input->chroma);input->~VideoInput();}
static SIZE_T APIENTRY CalcPrivateVideoProcessorOutputViewSize(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW *){return sizeof(View);}
static HRESULT APIENTRY CreateVideoProcessorOutputView(D3D10DDI_HDEVICE h,const D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW *p,D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW v,D3D11_1DDI_HRTVIDEOPROCESSOROUTPUTVIEW){return create_view(dev(h),p,v,true);}
static void APIENTRY DestroyVideoProcessorOutputView(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW v){view_destroy(dev(h),object<View>(v));object<View>(v)->~View();}
static void APIENTRY VideoProcessorInputViewReadAfterWriteHazard(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSORINPUTVIEW hv,D3D10DDI_HRESOURCE r){auto *v=object<VideoInput>(hv);access(dev(h),res(r)->storage,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,false,v->image.level,1,v->image.layer,1);}
static HRESULT APIENTRY GetContentProtectionCaps(D3D10DDI_HDEVICE,const GUID *,const GUID *,D3D11_1DDI_VIDEO_CONTENT_PROTECTION_CAPS *p){*p={};return S_OK;}
static HRESULT APIENTRY VideoProcessorBlt(D3D10DDI_HDEVICE h,D3D11_1DDI_HVIDEOPROCESSOR processor,D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW output,UINT,UINT count,const D3D11_1DDI_VIDEO_PROCESSOR_STREAM *streams) {
 Device *d=dev(h);auto *p=object<VideoProcessor>(processor);auto *dst=object<View>(output);
 if(count>1 || !dst || !rgb(dst->format))return DXGI_ERROR_UNSUPPORTED;
 const auto *stream=count && streams[0].Enable?streams:nullptr;auto *input=stream?object<VideoInput>(stream->hInputSurface):nullptr;View *src=input?&input->image:nullptr;
 if(stream && (!src || stream->PastFrames || stream->FutureFrames || src->resource==dst->resource || (!rgb(src->resource->format) && src->resource->format!=DXGI_FORMAT_NV12)))return DXGI_ERROR_UNSUPPORTED;
 RECT target=p->target.right?p->target:full(dst->resource,dst->level),destination=p->destination.right?p->destination:full(dst->resource,dst->level);
 RECT source=src?(p->source.right?p->source:full(src->resource,src->level)):RECT{};
 RECT bounds=full(dst->resource,dst->level);
 if(!rect_valid(target,bounds.right,bounds.bottom) || !rect_valid(destination,bounds.right,bounds.bottom) || (src && !rect_valid(source,std::max(1u,src->resource->width>>src->level),std::max(1u,src->resource->height>>src->level))))return E_INVALIDARG;
 if(!video_shader(d))return d->failure;
 bool nv12=src && src->resource->format==DXGI_FORMAT_NV12;
 if(src && (!view_create(d,src,true,true) || (nv12 && !view_create(d,&input->chroma,true,true))))return d->failure;
 float values[32]{};memcpy(values,blit_matrices[p->rotation],32);
 values[8]=(float)destination.left;values[9]=(float)destination.top;values[10]=(float)(destination.right-destination.left);values[11]=(float)(destination.bottom-destination.top);
 values[12]=(float)source.left;values[13]=(float)source.top;values[14]=(float)(source.right-source.left);values[15]=(float)(source.bottom-source.top);
 values[16]=(float)nv12;values[17]=(float)(src!=nullptr);values[18]=(float)p->background_yuv;values[19]=(float)p->output.RGB_Range;
 values[20]=(float)(p->input.Nominal_Range!=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255);values[21]=(float)p->input.YCbCr_Matrix;values[22]=(float)p->input.RGB_Range;
 values[24]=(float)(p->output.Nominal_Range!=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255);values[25]=(float)p->output.YCbCr_Matrix;memcpy(values+28,&p->background,16);
 return blit(d,dst,{src,nv12?&input->chroma:nullptr},&d->video_pixel,values,sizeof(values),target);
}
HRESULT APIENTRY retrieve_video(D3D10DDI_HDEVICE,UINT32 id,SIZE_T size,void *input,SIZE_T output_size,void *output) {
 if(id!=D3D11_1DDI_VIDEO_FUNCTIONS)return E_NOINTERFACE;
 if(size!=sizeof(D3D11_1DDI_VIDEO_INPUT) || !input || output_size!=sizeof(D3D11_1DDI_VIDEO_OUTPUT) || !output)return E_INVALIDARG;
 auto *f=((D3D11_1DDI_VIDEO_INPUT *)input)->p11VideoDeviceFuncs;if(!f)return E_INVALIDARG;
 f->pfnGetVideoDecoderProfileCount=GetVideoDecoderProfileCount;
 f->pfnGetVideoDecoderProfile=Unsupported<decltype(f->pfnGetVideoDecoderProfile)>::call;
 f->pfnCheckVideoDecoderFormat=CheckVideoDecoderFormat;
 f->pfnGetVideoDecoderConfigCount=GetVideoDecoderConfigCount;
 f->pfnGetVideoDecoderConfig=Unsupported<decltype(f->pfnGetVideoDecoderConfig)>::call;
 f->pfnGetVideoDecoderBufferTypeCount=Unsupported<decltype(f->pfnGetVideoDecoderBufferTypeCount)>::call;
 f->pfnGetVideoDecoderBufferInfo=Unsupported<decltype(f->pfnGetVideoDecoderBufferInfo)>::call;
 f->pfnCalcPrivateVideoDecoderSize=Unsupported<decltype(f->pfnCalcPrivateVideoDecoderSize)>::call;
 f->pfnCreateVideoDecoder=Unsupported<decltype(f->pfnCreateVideoDecoder)>::call;
 f->pfnDestroyVideoDecoder=Unsupported<decltype(f->pfnDestroyVideoDecoder)>::call;
 f->pfnVideoDecoderExtension=Unsupported<decltype(f->pfnVideoDecoderExtension)>::call;
 f->pfnVideoDecoderBeginFrame=Unsupported<decltype(f->pfnVideoDecoderBeginFrame)>::call;
 f->pfnVideoDecoderEndFrame=Unsupported<decltype(f->pfnVideoDecoderEndFrame)>::call;
 f->pfnVideoDecoderSubmitBuffers=Unsupported<decltype(f->pfnVideoDecoderSubmitBuffers)>::call;
 f->pfnCalcPrivateVideoProcessorEnumSize=CalcPrivateVideoProcessorEnumSize;
 f->pfnCreateVideoProcessorEnum=CreateVideoProcessorEnum;
 f->pfnDestroyVideoProcessorEnum=DestroyVideoProcessorEnum;
 f->pfnCheckVideoProcessorFormat=CheckVideoProcessorFormat;
 f->pfnGetVideoProcessorCaps=GetVideoProcessorCaps;
 f->pfnGetVideoProcessorRateConversionCaps=GetVideoProcessorRateConversionCaps;
 f->pfnGetVideoProcessorCustomRate=Unsupported<decltype(f->pfnGetVideoProcessorCustomRate)>::call;
 f->pfnGetVideoProcessorFilterRange=Unsupported<decltype(f->pfnGetVideoProcessorFilterRange)>::call;
 f->pfnCalcPrivateVideoProcessorSize=CalcPrivateVideoProcessorSize;
 f->pfnCreateVideoProcessor=CreateVideoProcessor;
 f->pfnDestroyVideoProcessor=DestroyVideoProcessor;
 f->pfnVideoProcessorSetOutputTargetRect=VideoProcessorSetOutputTargetRect;
 f->pfnVideoProcessorSetOutputBackgroundColor=VideoProcessorSetOutputBackgroundColor;
 f->pfnVideoProcessorSetOutputColorSpace=VideoProcessorSetOutputColorSpace;
 f->pfnVideoProcessorSetOutputAlphaFillMode=VideoProcessorSetOutputAlphaFillMode;
 f->pfnVideoProcessorSetOutputConstriction=VideoProcessorSetOutputConstriction;
 f->pfnVideoProcessorSetOutputStereoMode=VideoProcessorSetOutputStereoMode;
 f->pfnVideoProcessorSetOutputExtension=Unsupported<decltype(f->pfnVideoProcessorSetOutputExtension)>::call;
 f->pfnVideoProcessorGetOutputExtension=Unsupported<decltype(f->pfnVideoProcessorGetOutputExtension)>::call;
 f->pfnVideoProcessorSetStreamFrameFormat=VideoProcessorSetStreamFrameFormat;
 f->pfnVideoProcessorSetStreamColorSpace=VideoProcessorSetStreamColorSpace;
 f->pfnVideoProcessorSetStreamOutputRate=VideoProcessorSetStreamOutputRate;
 f->pfnVideoProcessorSetStreamSourceRect=VideoProcessorSetStreamSourceRect;
 f->pfnVideoProcessorSetStreamDestRect=VideoProcessorSetStreamDestRect;
 f->pfnVideoProcessorSetStreamAlpha=VideoProcessorSetStreamAlpha;
 f->pfnVideoProcessorSetStreamPalette=Unsupported<decltype(f->pfnVideoProcessorSetStreamPalette)>::call;
 f->pfnVideoProcessorSetStreamPixelAspectRatio=VideoProcessorSetStreamPixelAspectRatio;
 f->pfnVideoProcessorSetStreamLumaKey=VideoProcessorSetStreamLumaKey;
 f->pfnVideoProcessorSetStreamStereoFormat=VideoProcessorSetStreamStereoFormat;
 f->pfnVideoProcessorSetStreamAutoProcessingMode=VideoProcessorSetStreamAutoProcessingMode;
 f->pfnVideoProcessorSetStreamFilter=VideoProcessorSetStreamFilter;
 f->pfnVideoProcessorSetStreamExtension=Unsupported<decltype(f->pfnVideoProcessorSetStreamExtension)>::call;
 f->pfnVideoProcessorGetStreamExtension=Unsupported<decltype(f->pfnVideoProcessorGetStreamExtension)>::call;
 f->pfnVideoProcessorBlt=VideoProcessorBlt;
 f->pfnCalcPrivateVideoDecoderOutputViewSize=Unsupported<decltype(f->pfnCalcPrivateVideoDecoderOutputViewSize)>::call;
 f->pfnCreateVideoDecoderOutputView=Unsupported<decltype(f->pfnCreateVideoDecoderOutputView)>::call;
 f->pfnDestroyVideoDecoderOutputView=Unsupported<decltype(f->pfnDestroyVideoDecoderOutputView)>::call;
 f->pfnCalcPrivateVideoProcessorInputViewSize=CalcPrivateVideoProcessorInputViewSize;
 f->pfnCreateVideoProcessorInputView=CreateVideoProcessorInputView;
 f->pfnDestroyVideoProcessorInputView=DestroyVideoProcessorInputView;
 f->pfnCalcPrivateVideoProcessorOutputViewSize=CalcPrivateVideoProcessorOutputViewSize;
 f->pfnCreateVideoProcessorOutputView=CreateVideoProcessorOutputView;
 f->pfnDestroyVideoProcessorOutputView=DestroyVideoProcessorOutputView;
 f->pfnVideoProcessorInputViewReadAfterWriteHazard=VideoProcessorInputViewReadAfterWriteHazard;
 f->pfnGetContentProtectionCaps=GetContentProtectionCaps;
 f->pfnGetCryptoKeyExchangeType=Unsupported<decltype(f->pfnGetCryptoKeyExchangeType)>::call;
 f->pfnCalcPrivateCryptoSessionSize=Unsupported<decltype(f->pfnCalcPrivateCryptoSessionSize)>::call;
 f->pfnCreateCryptoSession=Unsupported<decltype(f->pfnCreateCryptoSession)>::call;
 f->pfnDestroyCryptoSession=Unsupported<decltype(f->pfnDestroyCryptoSession)>::call;
 f->pfnGetCertificateSize=Unsupported<decltype(f->pfnGetCertificateSize)>::call;
 f->pfnGetCertificate=Unsupported<decltype(f->pfnGetCertificate)>::call;
 f->pfnNegotiateCryptoSessionKeyExchange=Unsupported<decltype(f->pfnNegotiateCryptoSessionKeyExchange)>::call;
 f->pfnEncryptionBlt=Unsupported<decltype(f->pfnEncryptionBlt)>::call;
 f->pfnDecryptionBlt=Unsupported<decltype(f->pfnDecryptionBlt)>::call;
 f->pfnStartSessionKeyRefresh=Unsupported<decltype(f->pfnStartSessionKeyRefresh)>::call;
 f->pfnFinishSessionKeyRefresh=Unsupported<decltype(f->pfnFinishSessionKeyRefresh)>::call;
 f->pfnGetEncryptionBltKey=Unsupported<decltype(f->pfnGetEncryptionBltKey)>::call;
 f->pfnCalcPrivateAuthenticatedChannelSize=Unsupported<decltype(f->pfnCalcPrivateAuthenticatedChannelSize)>::call;
 f->pfnCreateAuthenticatedChannel=Unsupported<decltype(f->pfnCreateAuthenticatedChannel)>::call;
 f->pfnDestroyAuthenticatedChannel=Unsupported<decltype(f->pfnDestroyAuthenticatedChannel)>::call;
 f->pfnNegotiateAuthenticatedChannelKeyExchange=Unsupported<decltype(f->pfnNegotiateAuthenticatedChannelKeyExchange)>::call;
 f->pfnQueryAuthenticatedChannel=Unsupported<decltype(f->pfnQueryAuthenticatedChannel)>::call;
 f->pfnConfigureAuthenticatedChannel=Unsupported<decltype(f->pfnConfigureAuthenticatedChannel)>::call;
 f->pfnVideoDecoderGetHandle=Unsupported<decltype(f->pfnVideoDecoderGetHandle)>::call;
 f->pfnCryptoSessionGetHandle=Unsupported<decltype(f->pfnCryptoSessionGetHandle)>::call;
 f->pfnVideoProcessorSetStreamRotation=VideoProcessorSetStreamRotation;
 f->pfnGetCaptureHandle=Unsupported<decltype(f->pfnGetCaptureHandle)>::call;
 *(D3D11_1DDI_VIDEO_OUTPUT *)output={};return S_OK;
}
