#pragma once
#include <winioctl.h>
#include <dxva.h>
#include "../hevc-stream.h"

namespace VideoDecode {
static const GUID Profiles[]={
 {0x5b11d51b,0x2f4c,0x4452,{0xbc,0xc3,0x09,0xf2,0xa1,0x16,0x0c,0xc0}},
 {0x107af0e0,0xef1a,0x4d19,{0xab,0xa8,0x67,0xa1,0x63,0x07,0x3d,0x13}}
};
static const GUID NoEncrypt={0x1b81bed0,0xa0c7,0x11d3,{0xb9,0x84,0x00,0xc0,0x4f,0x2e,0x73,0xc5}};
static bool Supported(const GUID &profile,DXGI_FORMAT format){return (profile==Profiles[0] && format==DXGI_FORMAT_NV12) || (profile==Profiles[1] && format==DXGI_FORMAT_P010);}
static bool Supported(const D3D11_1DDI_VIDEO_DECODER_DESC &p){return Supported(p.Guid,p.OutputFormat) && p.SampleWidth>=64 && p.SampleHeight>=64 && p.SampleWidth<=8192 && p.SampleHeight<=8192 && (uint64_t)p.SampleWidth*p.SampleHeight<=8192u*4320 && !((p.SampleWidth|p.SampleHeight)&1);}
static HRESULT Io(HANDLE gpu,DWORD code,const void *data=nullptr,DWORD bytes=0,void *output=nullptr,DWORD capacity=0){DWORD used=0;return DeviceIoControl(gpu,code,const_cast<void *>(data),bytes,output,capacity,&used,nullptr)?S_OK:HRESULT_FROM_WIN32(GetLastError());}
struct Output {Resource *resource;UINT sub;GUID profile;};
struct Decoder {
 D3D11_1DDI_VIDEO_DECODER_DESC desc{};
 GPU_DECODE_CONFIG config{};
 HANDLE gpu=INVALID_HANDLE_VALUE;
 Output *target=nullptr;
 DXVA_PicParams_HEVC picture{};
 DXVA_Qmatrix_HEVC matrix{};
 std::vector<uint8_t> bitstream,control,input;
 uint64_t sequence=0;
 bool opened=false,hasPicture=false,hasMatrix=false;
 ~Decoder(){if(gpu!=INVALID_HANDLE_VALUE){if(opened)Io(gpu,GPU_IOCTL_VIDEO_CLOSE);CloseHandle(gpu);}}
 HRESULT Frame(Device *d) {
  if(!target || !hasPicture || (!hasMatrix && picture.scaling_list_enabled_flag) || bitstream.empty() || control.empty() || control.size()%sizeof(DXVA_Slice_HEVC_Short))return E_INVALIDARG;
  if((picture.bit_depth_luma_minus8==2)!=(desc.OutputFormat==DXGI_FORMAT_P010) || picture.log2_min_luma_coding_block_size_minus3>3)return E_INVALIDARG;
  UINT cw=(UINT)picture.PicWidthInMinCbsY<<(picture.log2_min_luma_coding_block_size_minus3+3),ch=(UINT)picture.PicHeightInMinCbsY<<(picture.log2_min_luma_coding_block_size_minus3+3);
  UINT width=std::min(desc.SampleWidth,cw),height=std::min(desc.SampleHeight,ch);
  if(opened && (config.Width!=width || config.Height!=height))return DXGI_ERROR_UNSUPPORTED;
  input.resize(sizeof(GPU_DECODE_INPUT));
  for(size_t i=0;i<control.size()/sizeof(DXVA_Slice_HEVC_Short);i++){
   DXVA_Slice_HEVC_Short slice{};memcpy(&slice,control.data()+i*sizeof(slice),sizeof(slice));
   if(slice.wBadSliceChopping || slice.BSNALunitDataLocation>bitstream.size() || slice.SliceBytesInBuffer>bitstream.size()-slice.BSNALunitDataLocation)return E_INVALIDARG;
   const uint8_t *data=bitstream.data()+slice.BSNALunitDataLocation;size_t bytes=slice.SliceBytesInBuffer;
   if(bytes>=4 && !data[0] && !data[1] && !data[2] && data[3]==1){data+=4;bytes-=4;}
   else if(bytes>=3 && !data[0] && !data[1] && data[2]==1){data+=3;bytes-=3;}
   else return E_INVALIDARG;
   if(!HevcStream::Append(input,picture,matrix,data,bytes,width,height,i==0))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  if(!opened){config={};config.Width=width;config.Height=height;config.Codec=3;config.BitDepth=desc.OutputFormat==DXGI_FORMAT_P010?10:8;config.Flags=GPU_DECODE_ORDER|GPU_DECODE_SURFACES;
   HRESULT hr=Io(gpu,GPU_IOCTL_DECODE_OPEN,&config,sizeof(config));if(FAILED(hr))return hr;opened=true;
  }
  Resource *r=target->resource;Storage &storage=r->storage;
  if(!storage.address || !(r->bind&D3D11_DDI_BIND_DECODER))return DXGI_ERROR_UNSUPPORTED;
  VkImageSubresource sub{VK_IMAGE_ASPECT_PLANE_0_BIT,target->sub%r->levels,target->sub/r->levels};
  VkSubresourceLayout y{},uv{};d->vk.GetImageSubresourceLayout(d->device,storage.image,&sub,&y);
  sub.aspectMask=VK_IMAGE_ASPECT_PLANE_1_BIT;d->vk.GetImageSubresourceLayout(d->device,storage.image,&sub,&uv);
  if(!y.rowPitch || y.rowPitch!=uv.rowPitch || uv.size<y.rowPitch*((r->height+63)&~63u)/2 || y.rowPitch>UINT_MAX)return DXGI_ERROR_UNSUPPORTED;
  UINT lines=(r->height+63)&~63u;uint64_t size=y.rowPitch*(lines+lines/2);
  if(size>UINT_MAX)return E_OUTOFMEMORY;
  GPU_DECODE_SURFACE surface{storage.address,y.offset,uv.offset,(UINT)size,(UINT)y.rowPitch,lines,0};
  AccessState *state=storage.subresources.empty()?nullptr:&storage.subresources[target->sub];
  if(!(state?state->initialized:storage.initialized))access(d,storage,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,false,0,1,target->sub,1);
  if(!wait_for(d,state?state->used:storage.used,true))return d->failure;
  HRESULT hr=Io(gpu,GPU_IOCTL_DECODE_SURFACE,&surface,sizeof(surface));if(FAILED(hr))return hr;
  auto *header=(GPU_DECODE_INPUT *)input.data();header->Time=++sequence;header->Bytes=(UINT)input.size()-sizeof(*header);
  hr=Io(gpu,GPU_IOCTL_DECODE_INPUT,input.data(),(DWORD)input.size());if(FAILED(hr))return hr;
  UINT bytes=width*height*3/2*(config.BitDepth==10?2:1);GPU_DECODE_OUTPUT frame{};
  ULONGLONG deadline=GetTickCount64()+10000;
  do {hr=Io(gpu,GPU_IOCTL_DECODE_COMPLETE,nullptr,0,&frame,sizeof(frame));if(hr!=HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS))break;Sleep(1);}while(GetTickCount64()<deadline);
  if(hr==HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS))return HRESULT_FROM_WIN32(ERROR_TIMEOUT);if(FAILED(hr))return hr;
  if(frame.Time!=sequence || frame.Bytes!=bytes || frame.Width!=width || frame.Height!=height || frame.BitDepth!=config.BitDepth)return E_FAIL;
  if(state)*state={VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_WRITE_BIT,0,true};
  else {storage.stages=VK_PIPELINE_STAGE_HOST_BIT;storage.access=VK_ACCESS_HOST_WRITE_BIT;storage.used=0;storage.initialized=true;}
  return d->failure;
 }
};
static void APIENTRY ProfileCount(D3D10DDI_HDEVICE,UINT *count){*count=2;}
static void APIENTRY Profile(D3D10DDI_HDEVICE h,UINT index,GUID *guid){if(index>=2){*guid={};error(dev(h),E_INVALIDARG);return;}*guid=Profiles[index];}
static void APIENTRY Format(D3D10DDI_HDEVICE,const GUID *guid,DXGI_FORMAT format,BOOL *supported){*supported=guid && Supported(*guid,format);}
static void APIENTRY ConfigCount(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *p,UINT *count){*count=p && Supported(*p)?1:0;}
static void APIENTRY Config(D3D10DDI_HDEVICE h,const D3D11_1DDI_VIDEO_DECODER_DESC *p,UINT index,D3D11_1DDI_VIDEO_DECODER_CONFIG *out){*out={};if(!p || !Supported(*p) || index){error(dev(h),E_INVALIDARG);return;}out->guidConfigBitstreamEncryption=out->guidConfigMBcontrolEncryption=out->guidConfigResidDiffEncryption=NoEncrypt;out->ConfigBitstreamRaw=1;UINT area=p->SampleWidth*p->SampleHeight;out->ConfigMinRenderTargetBuffCount=(USHORT)(area<=35651584/4?16:area<=35651584/2?12:area<=35651584*3/4?8:6);}
static void APIENTRY BufferCount(D3D10DDI_HDEVICE,const D3D11_1DDI_VIDEO_DECODER_DESC *p,UINT *count){*count=p && Supported(*p)?4:0;}
static void APIENTRY BufferInfo(D3D10DDI_HDEVICE h,const D3D11_1DDI_VIDEO_DECODER_DESC *p,UINT index,D3D11_1DDI_VIDEO_DECODER_BUFFER_INFO *out){
 *out={};if(!p || !Supported(*p) || index>=4){error(dev(h),E_INVALIDARG);return;}
 static const D3D11_1DDI_VIDEO_DECODER_BUFFER_TYPE types[]={D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL,D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM};
 static const UINT sizes[]={sizeof(DXVA_PicParams_HEVC),sizeof(DXVA_Qmatrix_HEVC),1024*1024,32*1024*1024};
 out->Type=types[index];out->Size=sizes[index];out->Usage=D3D10_DDI_USAGE_DYNAMIC;
}
static SIZE_T APIENTRY Size(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODER *){return sizeof(Decoder);}
static HRESULT APIENTRY Create(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODER *p,D3D11_1DDI_HDECODE handle,D3D11_1DDI_HRTDECODE){
 if(!p || !Supported(p->Desc) || p->Config.ConfigBitstreamRaw!=1 || p->Config.guidConfigBitstreamEncryption!=NoEncrypt || p->Config.guidConfigMBcontrolEncryption!=NoEncrypt || p->Config.guidConfigResidDiffEncryption!=NoEncrypt)return E_INVALIDARG;
 auto *decoder=new(handle.pDrvPrivate) Decoder{};decoder->desc=p->Desc;
 decoder->gpu=CreateFileW(L"\\\\.\\QcomGpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
 if(decoder->gpu==INVALID_HANDLE_VALUE){HRESULT hr=HRESULT_FROM_WIN32(GetLastError());decoder->~Decoder();return hr;}return S_OK;
}
static void APIENTRY Destroy(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE h){object<Decoder>(h)->~Decoder();}
static SIZE_T APIENTRY ViewSize(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *){return sizeof(Output);}
static HRESULT APIENTRY ViewCreate(D3D10DDI_HDEVICE,const D3D11_1DDIARG_CREATEVIDEODECODEROUTPUTVIEW *p,D3D11_1DDI_HVIDEODECODEROUTPUTVIEW handle,D3D11_1DDI_HRTVIDEODECODEROUTPUTVIEW){
 Resource *r=res(p->hDrvResource);if(!r || !Supported(p->DecodeProfile,r->format) || p->MipSlice>=r->levels || p->FirstArraySlice>=r->layers || p->ArraySize!=1)return E_INVALIDARG;
 new(handle.pDrvPrivate) Output{r,p->MipSlice+p->FirstArraySlice*r->levels,p->DecodeProfile};return S_OK;
}
static void APIENTRY ViewDestroy(D3D10DDI_HDEVICE,D3D11_1DDI_HVIDEODECODEROUTPUTVIEW h){object<Output>(h)->~Output();}
void Clear(Device *d,void *handle,const FLOAT *color,const D3D10_DDI_RECT *rects,UINT count){
 auto *view=(Output *)handle;if(!view || !view->resource)return;Resource *r=view->resource;
 bool ten=r->format==DXGI_FORMAT_P010;UINT bytes=ten?2:1;uint16_t value[3];
 // Video view colors are unsigned integers, including NV12 and P010 (ClearView DDI).
 for(UINT i=0;i<3;i++)value[i]=(uint16_t)(color[i]>0?std::min(color[i],ten?65535.0f:255.0f):0);
 try{for(UINT i=0;i<(count?count:1);i++){
  RECT rect=count?rects[i]:RECT{0,0,(LONG)r->width,(LONG)r->height};
  LONG left=std::max(0L,rect.left),top=std::max(0L,rect.top),right=std::min((LONG)r->width,rect.right),bottom=std::min((LONG)r->height,rect.bottom);
  if(right<=left || bottom<=top)continue;
  if((left|top|right|bottom)&1){error(d,E_INVALIDARG);return;}
  UINT width=(UINT)(right-left),height=(UINT)(bottom-top),luma=width*height;
  std::vector<uint16_t> data((size_t)width*height*3/2*bytes/2);
  if(ten){std::fill_n(data.data(),luma,value[0]);for(UINT j=0;j<luma/2;j+=2){data[luma+j]=value[1];data[luma+j+1]=value[2];}}
  else {auto *p=(uint8_t *)data.data();memset(p,value[0],luma);for(UINT j=0;j<luma/2;j+=2){p[luma+j]=(uint8_t)value[1];p[luma+j+1]=(uint8_t)value[2];}}
  D3D10_DDI_BOX box={left,top,0,right,bottom,1};
  ResourceUpdateSubResourceUP(D3D10DDI_HDEVICE{d},D3D10DDI_HRESOURCE{r},view->sub,&box,data.data(),width*bytes,0);
  if(FAILED(d->failure))return;
 }}catch(const std::bad_alloc &){error(d,E_OUTOFMEMORY);}
}
static HRESULT APIENTRY Begin(D3D10DDI_HDEVICE,D3D11_1DDI_HDECODE h,const D3D11_1DDIARG_VIDEODECODERBEGINFRAME *p){
 auto *decoder=object<Decoder>(h);auto *target=p?object<Output>(p->hOutputView):nullptr;
 if(decoder->target || !target || p->ContentKeySize || target->profile!=decoder->desc.Guid || target->resource->width<decoder->desc.SampleWidth || target->resource->height<decoder->desc.SampleHeight)return E_INVALIDARG;
 decoder->target=target;decoder->hasPicture=false;decoder->bitstream.clear();decoder->control.clear();return S_OK;
}
static HRESULT APIENTRY Submit(D3D10DDI_HDEVICE h,D3D11_1DDI_HDECODE decoderHandle,UINT count,const D3D11_1DDI_VIDEO_DECODER_BUFFER_DESC *buffers){
 Device *d=dev(h);auto *decoder=object<Decoder>(decoderHandle);if(!decoder->target || !count || !buffers)return E_INVALIDARG;
 try{for(UINT i=0;i<count;i++){
  const auto &p=buffers[i];Resource *r=res(p.hResource);
  if(!r || !r->storage.buffer || !r->storage.map || p.DataOffset>r->width || !p.DataSize || p.DataSize>r->width-p.DataOffset || p.IVSize || p.PartialEncryption)return E_INVALIDARG;
  if(!wait_for(d,r->storage.used,true) || !sync_memory(d,r->storage,true,p.DataOffset,p.DataSize))return d->failure;
  const auto *data=(const uint8_t *)r->storage.map+p.DataOffset;
  if(p.BufferType==D3D11_1DDI_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS){if(p.DataSize!=sizeof(decoder->picture))return E_INVALIDARG;memcpy(&decoder->picture,data,p.DataSize);decoder->hasPicture=true;}
  else if(p.BufferType==D3D11_1DDI_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX){if(p.DataSize!=sizeof(decoder->matrix))return E_INVALIDARG;memcpy(&decoder->matrix,data,p.DataSize);decoder->hasMatrix=true;}
  else {auto *out=p.BufferType==D3D11_1DDI_VIDEO_DECODER_BUFFER_BITSTREAM?&decoder->bitstream:p.BufferType==D3D11_1DDI_VIDEO_DECODER_BUFFER_SLICE_CONTROL?&decoder->control:nullptr;if(!out || p.DataSize>33554432-out->size())return E_INVALIDARG;out->insert(out->end(),data,data+p.DataSize);}
 }return S_OK;}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
}
static void APIENTRY End(D3D10DDI_HDEVICE h,D3D11_1DDI_HDECODE handle){
 auto *decoder=object<Decoder>(handle);HRESULT hr=E_FAIL;try{hr=decoder->Frame(dev(h));}catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}decoder->target=nullptr;if(FAILED(hr))error(dev(h),hr);
}
static void Functions(D3D11_1DDI_VIDEODEVICEFUNCS *f){
 f->pfnGetVideoDecoderProfileCount=ProfileCount;f->pfnGetVideoDecoderProfile=Profile;f->pfnCheckVideoDecoderFormat=Format;f->pfnGetVideoDecoderConfigCount=ConfigCount;f->pfnGetVideoDecoderConfig=Config;
 f->pfnGetVideoDecoderBufferTypeCount=BufferCount;f->pfnGetVideoDecoderBufferInfo=BufferInfo;f->pfnCalcPrivateVideoDecoderSize=Size;f->pfnCreateVideoDecoder=Create;f->pfnDestroyVideoDecoder=Destroy;
 f->pfnVideoDecoderBeginFrame=Begin;f->pfnVideoDecoderSubmitBuffers=Submit;f->pfnVideoDecoderEndFrame=End;f->pfnCalcPrivateVideoDecoderOutputViewSize=ViewSize;f->pfnCreateVideoDecoderOutputView=ViewCreate;f->pfnDestroyVideoDecoderOutputView=ViewDestroy;
}
}
