#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <strmif.h>
#include <wrl.h>
#include <mutex>
#include <vector>
#include <atomic>
#include "../qcom-gpu/wddm/abi.h"
#include "../qcom-gpu/video-config.h"

using namespace Microsoft::WRL;
static const CLSID EncoderClass={0xa9d65645,0x996b,0x4af7,{0x8e,0x31,0x6e,0x1d,0xcd,0x74,0xa5,0x29}};
static const wchar_t EncoderName[]=L"Qualcomm Iris H.264 Encoder";
static std::atomic<long> Objects,Locks;
static HMODULE EncoderModule;
static HRESULT Kernel(NTSTATUS status){return status<0?HRESULT_FROM_NT(status):S_OK;}
static HRESULT Io(HANDLE handle,DWORD code,const void *input,DWORD size,void *output=nullptr,DWORD capacity=0) {
 DWORD used=0;return DeviceIoControl(handle,code,const_cast<void *>(input),size,output,capacity,&used,nullptr)?S_OK:HRESULT_FROM_WIN32(GetLastError());
}
static GPU_VIDEO_CONFIG Config(IMFMediaType *type) {
 GPU_VIDEO_CONFIG c{};UINT32 n=0,d=0;
 MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&c.Width,&c.Height);MFGetAttributeRatio(type,MF_MT_FRAME_RATE,&n,&d);c.Rate=d?(n+d/2)/d:0;
 c.Bitrate=MFGetAttributeUINT32(type,MF_MT_AVG_BITRATE,3000000);
 UINT32 profile=MFGetAttributeUINT32(type,MF_MT_MPEG2_PROFILE,eAVEncH264VProfile_Base),level=MFGetAttributeUINT32(type,MF_MT_MPEG2_LEVEL,42);
 c.Profile=profile==eAVEncH264VProfile_Base?0:profile==eAVEncH264VProfile_ConstrainedBase?1:profile==eAVEncH264VProfile_Main?2:profile==eAVEncH264VProfile_High?4:3;
 const UINT32 levels[]={30,31,32,40,41,42};for(UINT32 i=0;i<6;i++)if(levels[i]==level)c.Level=i+8;
 return c;
}
static HRESULT Type(IMFMediaType *type,bool output) {
 if(!type)return S_OK;
 GUID major{},subtype{};UINT32 w=0,h=0,n=0,d=0;
 if(FAILED(type->GetGUID(MF_MT_MAJOR_TYPE,&major)) || major!=MFMediaType_Video ||
    FAILED(type->GetGUID(MF_MT_SUBTYPE,&subtype)) ||
    (output?subtype!=MFVideoFormat_H264:subtype!=MFVideoFormat_NV12 && subtype!=MFVideoFormat_ARGB32))return MF_E_INVALIDMEDIATYPE;
 if(FAILED(MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&w,&h)) || !w || !h || ((w|h)&1) || w>4096 || h>4096)return MF_E_INVALIDMEDIATYPE;
 if(output && (w<128 || h<128 || w>1920 || h>1920 || w*h>1920*1080))return MF_E_INVALIDMEDIATYPE;
 if(FAILED(MFGetAttributeRatio(type,MF_MT_FRAME_RATE,&n,&d)) || !n || !d || (UINT64)n>60ULL*d)return MF_E_INVALIDMEDIATYPE;
 if(output){auto config=Config(type);if(!GpuVideoConfigValid(&config))return MF_E_INVALIDMEDIATYPE;}
 return S_OK;
}
class Encoder final:public RuntimeClass<RuntimeClassFlags<ClassicCom>,IMFTransform,IMFMediaEventGenerator,ICodecAPI,IMFShutdown,IMFAsyncCallback> {
 std::mutex mutex_;
 ComPtr<IMFAttributes> attributes_;
 ComPtr<IMFAttributes> inputAttributes_,outputAttributes_;
 ComPtr<IMFMediaEventQueue> events_;
 ComPtr<IMFMediaType> input_,output_;
 ComPtr<IMFDXGIDeviceManager> manager_;
 ComPtr<IMFSample> encoded_;
 ComPtr<ID3D11Device> device_;
 ComPtr<ID3D11DeviceContext> context_;
 ComPtr<IDXGIDevice2> dxgi_;
 ComPtr<ID3D11VideoDevice> video_;
 ComPtr<ID3D11VideoContext> videoContext_;
 ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
 ComPtr<ID3D11VideoProcessor> processor_;
 ComPtr<ID3D11Texture2D> texture_,source_;
 ComPtr<ID3D11VideoProcessorOutputView> target_;
 ComPtr<ID3D11VideoProcessorInputView> sourceView_;
 HANDLE gpu_=INVALID_HANDLE_VALUE,deviceHandle_=nullptr,done_=nullptr,converted_=nullptr;
 D3DKMT_HANDLE adapter_=0,kernelDevice_=0,resource_=0;
 GPU_VIDEO_CONFIG config_{};
 GPU_U64 address_=0;
 std::vector<BYTE> data_;
 std::vector<ULONG_PTR> markers_;
 ULONG import_=0,sourceIndex_=~0u;
 DWORD queue_=0;
 ULONG bitrate_=3000000,gop_=60;
 bool queued_=false,streaming_=false,accept_=false,requested_=false,draining_=false,shutdown_=false,key_=true,discontinuity_=true;
 HRESULT Event(MediaEventType type,HRESULT status=S_OK) {
  PROPVARIANT value{};if(type==METransformNeedInput){value.vt=VT_UI4;value.ulVal=0;requested_=true;}
  return events_->QueueEventParamVar(type,GUID_NULL,status,type==METransformNeedInput?&value:nullptr);
 }
 HRESULT Marker(ULONG_PTR value) {
  ComPtr<IMFMediaEvent> event;HRESULT hr=MFCreateMediaEvent(METransformMarker,GUID_NULL,S_OK,nullptr,&event);
  if(SUCCEEDED(hr))hr=event->SetUINT64(MF_EVENT_MFT_CONTEXT,value);
  return FAILED(hr)?hr:events_->QueueEvent(event.Get());
 }
 HRESULT Ready() {
  if(shutdown_)return MF_E_SHUTDOWN;
  return MFGetAttributeUINT32(attributes_.Get(),MF_TRANSFORM_ASYNC_UNLOCK,0)?S_OK:MF_E_TRANSFORM_ASYNC_LOCKED;
 }
 ULONG BitrateLimit() {
  if(!output_)return 40000000;
  auto config=Config(output_.Get());return GpuVideoMaxBitrate(&config);
 }
 HRESULT Stop(std::unique_lock<std::mutex> &lock) {
  streaming_=false;accept_=false;requested_=false;
  if(queued_){lock.unlock();WaitForSingleObject(done_,INFINITE);lock.lock();}
  HRESULT hr=gpu_==INVALID_HANDLE_VALUE?S_OK:Io(gpu_,GPU_IOCTL_VIDEO_CLOSE,nullptr,0);
  if(gpu_!=INVALID_HANDLE_VALUE) {
   if(import_)Io(gpu_,GPU_IOCTL_SHARED_RELEASE,&import_,sizeof(import_));
   CloseHandle(gpu_);gpu_=INVALID_HANDLE_VALUE;
  }
  if(resource_){D3DKMT_DESTROYALLOCATION args{};args.hDevice=kernelDevice_;args.hResource=resource_;D3DKMTDestroyAllocation(&args);resource_=0;}
  if(kernelDevice_){D3DKMT_DESTROYDEVICE args{};args.hDevice=kernelDevice_;D3DKMTDestroyDevice(&args);kernelDevice_=0;}
  if(adapter_){D3DKMT_CLOSEADAPTER args{};args.hAdapter=adapter_;D3DKMTCloseAdapter(&args);adapter_=0;}
  sourceView_.Reset();source_.Reset();target_.Reset();texture_.Reset();processor_.Reset();enumerator_.Reset();videoContext_.Reset();video_.Reset();dxgi_.Reset();context_.Reset();device_.Reset();
  if(deviceHandle_ && manager_){manager_->CloseDeviceHandle(deviceHandle_);deviceHandle_=nullptr;}
  import_=0;address_=0;sourceIndex_=~0u;encoded_.Reset();markers_.clear();key_=true;draining_=false;discontinuity_=true;
  return hr;
 }
 HRESULT Share() {
  ComPtr<IDXGIDevice> dxgiDevice;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
  HRESULT hr=device_.As(&dxgiDevice);if(FAILED(hr))return hr;
  hr=dxgiDevice->GetAdapter(&adapter);if(FAILED(hr))return hr;
  hr=adapter->GetDesc(&desc);if(FAILED(hr))return hr;
  attributes_->SetBlob(MFT_ENUM_ADAPTER_LUID,(const BYTE *)&desc.AdapterLuid,sizeof(desc.AdapterLuid));
  D3DKMT_OPENADAPTERFROMLUID open{};open.AdapterLuid=desc.AdapterLuid;
  hr=Kernel(D3DKMTOpenAdapterFromLuid(&open));if(FAILED(hr))return hr;adapter_=open.hAdapter;
  D3DKMT_CREATEDEVICE create{};create.hAdapter=adapter_;
  hr=Kernel(D3DKMTCreateDevice(&create));if(FAILED(hr))return hr;kernelDevice_=create.hDevice;
  ComPtr<IDXGIResource> shared;HANDLE handle=nullptr;
  hr=texture_.As(&shared);if(FAILED(hr))return hr;
  hr=shared->GetSharedHandle(&handle);if(FAILED(hr))return hr;
  D3DKMT_QUERYRESOURCEINFO info{};info.hDevice=kernelDevice_;info.hGlobalShare=(D3DKMT_HANDLE)(ULONG_PTR)handle;
  hr=Kernel(D3DKMTQueryResourceInfo(&info));if(FAILED(hr))return hr;
  if(info.NumAllocations!=1 || info.ResourcePrivateDriverDataSize>65536 || info.PrivateRuntimeDataSize>65536 || info.TotalPrivateDriverDataSize>65536)return E_FAIL;
  std::vector<BYTE> runtime(info.PrivateRuntimeDataSize),resource(info.ResourcePrivateDriverDataSize),allocation(info.TotalPrivateDriverDataSize);
  D3DDDI_OPENALLOCATIONINFO allocationInfo{};D3DKMT_OPENRESOURCE opened{};opened.hDevice=kernelDevice_;opened.hGlobalShare=info.hGlobalShare;
  opened.NumAllocations=1;opened.pOpenAllocationInfo=&allocationInfo;opened.pPrivateRuntimeData=runtime.data();opened.PrivateRuntimeDataSize=info.PrivateRuntimeDataSize;
  opened.pResourcePrivateDriverData=resource.data();opened.ResourcePrivateDriverDataSize=info.ResourcePrivateDriverDataSize;
  opened.pTotalPrivateDriverDataBuffer=allocation.data();opened.TotalPrivateDriverDataBufferSize=info.TotalPrivateDriverDataSize;
  hr=Kernel(D3DKMTOpenResource(&opened));if(FAILED(hr))return hr;resource_=opened.hResource;
  A830_ALLOCATION_ADDRESS address{A830_ABI_VERSION,allocationInfo.hAllocation,0,0,0,0,0};
  D3DKMT_ESCAPE escape{};escape.hAdapter=adapter_;escape.hDevice=kernelDevice_;escape.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;escape.pPrivateDriverData=&address;escape.PrivateDriverDataSize=sizeof(address);
  hr=Kernel(D3DKMTEscape(&escape));if(FAILED(hr))return hr;
  if(address.Format!=A830_FORMAT_NV12 || address.Pitch!=((config_.Width+127)&~127u))return MF_E_UNSUPPORTED_D3D_TYPE;
  GPU_SHARED_IMPORT imported{address.Address,address.Size};hr=Io(gpu_,GPU_IOCTL_SHARED_RETAIN,&imported,sizeof(imported),&import_,sizeof(import_));
  if(SUCCEEDED(hr))address_=address.Address;return hr;
 }
 HRESULT Start() {
  if(!input_ || !output_)return MF_E_TRANSFORM_TYPE_NOT_SET;
  if(!manager_)return MF_E_UNSUPPORTED_D3D_TYPE;
  UINT32 n=0,d=0,iw=0,ih=0;
  MFGetAttributeRatio(output_.Get(),MF_MT_FRAME_RATE,&n,&d);config_=Config(output_.Get());config_.Bitrate=bitrate_;config_.Gop=gop_;
  if(!GpuVideoConfigValid(&config_))return MF_E_INVALIDMEDIATYPE;
  try{data_.resize(4*1024*1024);}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
  MFGetAttributeSize(input_.Get(),MF_MT_FRAME_SIZE,&iw,&ih);
  HRESULT hr=manager_->OpenDeviceHandle(&deviceHandle_);if(FAILED(hr))return hr;
  hr=manager_->LockDevice(deviceHandle_,IID_PPV_ARGS(&device_),TRUE);if(FAILED(hr))return hr;
  manager_->UnlockDevice(deviceHandle_,FALSE);device_->GetImmediateContext(&context_);
  hr=device_.As(&dxgi_);if(FAILED(hr))return hr;
  hr=device_.As(&video_);if(FAILED(hr))return hr;
  hr=context_.As(&videoContext_);if(FAILED(hr))return hr;
  ComPtr<ID3D10Multithread> multithread;if(SUCCEEDED(context_.As(&multithread)))multithread->SetMultithreadProtected(TRUE);
  D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};content.InputFrameFormat=D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
  content.InputWidth=iw;content.InputHeight=ih;content.OutputWidth=config_.Width;content.OutputHeight=config_.Height;content.InputFrameRate={n,d};content.OutputFrameRate={n,d};content.Usage=D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
  hr=video_->CreateVideoProcessorEnumerator(&content,&enumerator_);if(FAILED(hr))return hr;
  hr=video_->CreateVideoProcessor(enumerator_.Get(),0,&processor_);if(FAILED(hr))return hr;
  D3D11_TEXTURE2D_DESC desc{};desc.Width=config_.Width;desc.Height=config_.Height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=DXGI_FORMAT_NV12;
  desc.BindFlags=D3D11_BIND_RENDER_TARGET;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
  hr=device_->CreateTexture2D(&desc,nullptr,&texture_);if(FAILED(hr))return hr;
  D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC view{};view.ViewDimension=D3D11_VPOV_DIMENSION_TEXTURE2D;
  hr=video_->CreateVideoProcessorOutputView(texture_.Get(),enumerator_.Get(),&view,&target_);if(FAILED(hr))return hr;
  D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};color.YCbCr_Matrix=1;color.Nominal_Range=D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
  videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(),&color);videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(),0,FALSE);
  gpu_=CreateFileW(L"\\\\.\\QcomGpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
  if(gpu_==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
  try{hr=Share();}catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}if(FAILED(hr))return hr;
  hr=Io(gpu_,GPU_IOCTL_VIDEO_OPEN,&config_,sizeof(config_));if(FAILED(hr))return hr;
  streaming_=true;return S_OK;
 }
 HRESULT Encode(IMFSample *sample,IMFSample **result,ULONG bitrate,bool key) {
  ComPtr<IMFMediaBuffer> buffer;ComPtr<IMFDXGIBuffer> dxgi;ComPtr<ID3D11Texture2D> source;
  HRESULT hr=sample->GetBufferByIndex(0,&buffer);if(FAILED(hr))return hr;
  hr=buffer.As(&dxgi);if(FAILED(hr))return MF_E_UNSUPPORTED_D3D_TYPE;
  UINT subresource=0;hr=dxgi->GetResource(IID_PPV_ARGS(&source));if(FAILED(hr))return hr;
  hr=dxgi->GetSubresourceIndex(&subresource);if(FAILED(hr))return hr;
  D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
  if(desc.SampleDesc.Count!=1 || desc.MipLevels!=1 || subresource>=desc.ArraySize)return MF_E_UNSUPPORTED_D3D_TYPE;
  ComPtr<ID3D11Device> owner;source->GetDevice(&owner);if(owner.Get()!=device_.Get())return MF_E_UNSUPPORTED_D3D_TYPE;
  if(desc.Format==DXGI_FORMAT_NV12 && desc.Width==config_.Width && desc.Height==config_.Height && desc.ArraySize==1)context_->CopyResource(texture_.Get(),source.Get());
  else {
   if(source_.Get()!=source.Get() || sourceIndex_!=subresource) {
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC view{};view.ViewDimension=D3D11_VPIV_DIMENSION_TEXTURE2D;view.Texture2D.ArraySlice=subresource;
    sourceView_.Reset();hr=video_->CreateVideoProcessorInputView(source.Get(),enumerator_.Get(),&view,&sourceView_);if(FAILED(hr))return hr;source_=source;sourceIndex_=subresource;
   }
   D3D11_VIDEO_PROCESSOR_COLOR_SPACE color{};color.YCbCr_Matrix=1;color.Nominal_Range=desc.Format==DXGI_FORMAT_NV12?D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235:D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
   videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(),0,&color);
   D3D11_VIDEO_PROCESSOR_STREAM stream{};stream.Enable=TRUE;stream.pInputSurface=sourceView_.Get();
   hr=videoContext_->VideoProcessorBlt(processor_.Get(),target_.Get(),0,1,&stream);if(FAILED(hr))return hr;
  }
  context_->Flush();ResetEvent(converted_);hr=dxgi_->EnqueueSetEvent(converted_);if(FAILED(hr))return hr;
  if(WaitForSingleObject(converted_,1000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  hr=device_->GetDeviceRemovedReason();if(FAILED(hr))return hr;
  LONGLONG time=0,duration=0;sample->GetSampleTime(&time);sample->GetSampleDuration(&duration);
  GPU_VIDEO_INPUT frame{address_,time<0?0:(GPU_U64)(time/10),key,bitrate};
  hr=Io(gpu_,GPU_IOCTL_VIDEO_ENCODE,&frame,sizeof(frame),data_.data(),(DWORD)data_.size());if(FAILED(hr))return hr;
  const GPU_VIDEO_OUTPUT *encoded=(const GPU_VIDEO_OUTPUT *)data_.data();if(!encoded->Bytes || encoded->Bytes>data_.size()-sizeof(*encoded))return E_FAIL;
  ComPtr<IMFSample> output;ComPtr<IMFMediaBuffer> compressed;
  hr=MFCreateSample(&output);if(FAILED(hr))return hr;
  hr=MFCreateMemoryBuffer(encoded->Bytes,&compressed);if(FAILED(hr))return hr;
  BYTE *bytes=nullptr;hr=compressed->Lock(&bytes,nullptr,nullptr);if(FAILED(hr))return hr;
  memcpy(bytes,encoded+1,encoded->Bytes);compressed->Unlock();compressed->SetCurrentLength(encoded->Bytes);
  hr=output->AddBuffer(compressed.Get());if(FAILED(hr))return hr;
  output->SetSampleTime(time);output->SetSampleDuration(duration>0?duration:10000000/config_.Rate);output->SetUINT32(MFSampleExtension_CleanPoint,encoded->Keyframe);
  *result=output.Detach();return S_OK;
 }
 static bool Supported(const GUID &key) {return key==CODECAPI_AVEncCommonMeanBitRate || key==CODECAPI_AVEncCommonRateControlMode || key==CODECAPI_AVEncVideoForceKeyFrame || key==CODECAPI_AVEncMPVDefaultBPictureCount || key==CODECAPI_AVEncMPVGOPSize || key==CODECAPI_AVLowLatencyMode;}
public:
 Encoder(){Objects++;}
 ~Encoder(){std::unique_lock lock(mutex_);Stop(lock);if(queue_)MFUnlockWorkQueue(queue_);if(events_)events_->Shutdown();if(done_)CloseHandle(done_);if(converted_)CloseHandle(converted_);Objects--;}
 HRESULT Initialize() {
  HRESULT hr=MFCreateAttributes(&attributes_,8);if(FAILED(hr))return hr;
  attributes_->SetUINT32(MF_TRANSFORM_ASYNC,TRUE);attributes_->SetUINT32(MF_SA_D3D11_AWARE,TRUE);attributes_->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE,TRUE);attributes_->SetString(MFT_ENUM_HARDWARE_URL_Attribute,L"ACPI\\HMRG0830");
  hr=MFCreateAttributes(&inputAttributes_,4);if(FAILED(hr))return hr;
  hr=MFCreateAttributes(&outputAttributes_,4);if(FAILED(hr))return hr;
  hr=MFCreateEventQueue(&events_);if(FAILED(hr))return hr;
  done_=CreateEventW(nullptr,TRUE,TRUE,nullptr);converted_=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!done_ || !converted_)return HRESULT_FROM_WIN32(GetLastError());
  return MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED,&queue_);
 }
 STDMETHODIMP GetStreamLimits(DWORD *a,DWORD *b,DWORD *c,DWORD *d) override {if(!a||!b||!c||!d)return E_POINTER;*a=*b=*c=*d=1;return S_OK;}
 STDMETHODIMP GetStreamCount(DWORD *a,DWORD *b) override {if(!a||!b)return E_POINTER;*a=*b=1;return S_OK;}
 STDMETHODIMP GetStreamIDs(DWORD,DWORD *,DWORD,DWORD *) override {return E_NOTIMPL;}
 STDMETHODIMP GetInputStreamInfo(DWORD id,MFT_INPUT_STREAM_INFO *info) override {if(id)return MF_E_INVALIDSTREAMNUMBER;if(!info)return E_POINTER;*info={};info->dwFlags=MFT_INPUT_STREAM_WHOLE_SAMPLES|MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER|MFT_INPUT_STREAM_HOLDS_BUFFERS;return S_OK;}
 STDMETHODIMP GetOutputStreamInfo(DWORD id,MFT_OUTPUT_STREAM_INFO *info) override {if(id)return MF_E_INVALIDSTREAMNUMBER;if(!info)return E_POINTER;*info={};info->dwFlags=MFT_OUTPUT_STREAM_WHOLE_SAMPLES|MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER|MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;return S_OK;}
 STDMETHODIMP GetAttributes(IMFAttributes **out) override {return attributes_.CopyTo(out);}
 STDMETHODIMP GetInputStreamAttributes(DWORD id,IMFAttributes **out) override {return id?MF_E_INVALIDSTREAMNUMBER:inputAttributes_.CopyTo(out);}
 STDMETHODIMP GetOutputStreamAttributes(DWORD id,IMFAttributes **out) override {return id?MF_E_INVALIDSTREAMNUMBER:outputAttributes_.CopyTo(out);}
 STDMETHODIMP DeleteInputStream(DWORD) override {return E_NOTIMPL;}
 STDMETHODIMP AddInputStreams(DWORD,DWORD *) override {return E_NOTIMPL;}
 STDMETHODIMP GetInputAvailableType(DWORD id,DWORD index,IMFMediaType **out) override {return Available(id,index,false,out);}
 STDMETHODIMP GetOutputAvailableType(DWORD id,DWORD index,IMFMediaType **out) override {return Available(id,index,true,out);}
 HRESULT Available(DWORD id,DWORD index,bool output,IMFMediaType **out) {
  if(id)return MF_E_INVALIDSTREAMNUMBER;if(!out)return E_POINTER;if(index>=(output?1u:2u))return MF_E_NO_MORE_TYPES;
  ComPtr<IMFMediaType> type;HRESULT hr=MFCreateMediaType(&type);if(FAILED(hr))return hr;
  type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,output?MFVideoFormat_H264:index?MFVideoFormat_ARGB32:MFVideoFormat_NV12);
  type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
  if(output){type->SetUINT32(MF_MT_MPEG2_PROFILE,eAVEncH264VProfile_Base);type->SetUINT32(MF_MT_MPEG2_LEVEL,42);type->SetUINT32(MF_MT_AVG_BITRATE,bitrate_);}
  *out=type.Detach();return S_OK;
 }
 STDMETHODIMP SetInputType(DWORD id,IMFMediaType *type,DWORD flags) override {return SetType(id,type,flags,false);}
 STDMETHODIMP SetOutputType(DWORD id,IMFMediaType *type,DWORD flags) override {return SetType(id,type,flags,true);}
 HRESULT SetType(DWORD id,IMFMediaType *type,DWORD flags,bool output) {
  if(id)return MF_E_INVALIDSTREAMNUMBER;if(flags&~MFT_SET_TYPE_TEST_ONLY)return E_INVALIDARG;std::unique_lock lock(mutex_);
  if(shutdown_)return MF_E_SHUTDOWN;
  HRESULT hr=Type(type,output);if(FAILED(hr))return hr;
  if(flags&MFT_SET_TYPE_TEST_ONLY)return S_OK;
  if(queued_||encoded_)return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
  bool active=streaming_,accept=accept_,requested=requested_;
  if(active){hr=Stop(lock);if(FAILED(hr))return hr;}
  if(output){output_=type;if(type)bitrate_=MFGetAttributeUINT32(type,MF_MT_AVG_BITRATE,bitrate_);}else input_=type;
  if(active && input_ && output_) {
   hr=Start();if(FAILED(hr)){Stop(lock);return hr;}
   accept_=accept;requested_=requested;if(accept_&&!requested_)return Event(METransformNeedInput);
  }
  return S_OK;
 }
 STDMETHODIMP GetInputCurrentType(DWORD id,IMFMediaType **out) override {std::lock_guard lock(mutex_);return id?MF_E_INVALIDSTREAMNUMBER:input_?input_.CopyTo(out):MF_E_TRANSFORM_TYPE_NOT_SET;}
 STDMETHODIMP GetOutputCurrentType(DWORD id,IMFMediaType **out) override {std::lock_guard lock(mutex_);return id?MF_E_INVALIDSTREAMNUMBER:output_?output_.CopyTo(out):MF_E_TRANSFORM_TYPE_NOT_SET;}
 STDMETHODIMP GetInputStatus(DWORD id,DWORD *flags) override {if(id)return MF_E_INVALIDSTREAMNUMBER;if(!flags)return E_POINTER;std::lock_guard lock(mutex_);*flags=streaming_&&accept_&&requested_&&!queued_&&!encoded_&&!draining_?MFT_INPUT_STATUS_ACCEPT_DATA:0;return S_OK;}
 STDMETHODIMP GetOutputStatus(DWORD *flags) override {if(!flags)return E_POINTER;std::lock_guard lock(mutex_);*flags=encoded_?MFT_OUTPUT_STATUS_SAMPLE_READY:0;return S_OK;}
 STDMETHODIMP SetOutputBounds(LONGLONG,LONGLONG) override {return E_NOTIMPL;}
 STDMETHODIMP ProcessEvent(DWORD,IMFMediaEvent *) override {return E_NOTIMPL;}
 STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE message,ULONG_PTR value) override {
  std::unique_lock lock(mutex_);if(shutdown_)return MF_E_SHUTDOWN;
  if(message==MFT_MESSAGE_SET_D3D_MANAGER){HRESULT hr=Stop(lock);manager_.Reset();if(value)hr=((IUnknown *)value)->QueryInterface(IID_PPV_ARGS(&manager_));return hr;}
  if(message==MFT_MESSAGE_NOTIFY_BEGIN_STREAMING){if(streaming_)return S_OK;HRESULT hr=Start();if(FAILED(hr))Stop(lock);return hr;}
  if(message==MFT_MESSAGE_NOTIFY_END_STREAMING || message==MFT_MESSAGE_NOTIFY_RELEASE_RESOURCES)return Stop(lock);
  if(message==MFT_MESSAGE_NOTIFY_END_OF_STREAM){accept_=false;requested_=false;return S_OK;}
  if(message==MFT_MESSAGE_COMMAND_FLUSH) {
   if(queued_){lock.unlock();WaitForSingleObject(done_,INFINITE);lock.lock();}
   encoded_.Reset();markers_.clear();accept_=false;requested_=false;draining_=false;key_=true;discontinuity_=true;ComPtr<IMFMediaEvent> event;while(events_->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event)==S_OK)event.Reset();return S_OK;
  }
  if(message==MFT_MESSAGE_COMMAND_DRAIN){draining_=true;requested_=false;return queued_?S_OK:Event(METransformDrainComplete);}
  if(message==MFT_MESSAGE_COMMAND_MARKER){if(queued_){try{markers_.push_back(value);}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}return S_OK;}return Marker(value);}
  if(message==MFT_MESSAGE_NOTIFY_START_OF_STREAM){if(!streaming_){HRESULT hr=Start();if(FAILED(hr)){Stop(lock);return hr;}}accept_=true;draining_=false;key_=true;discontinuity_=true;return !queued_&&!encoded_&&!requested_?Event(METransformNeedInput):S_OK;}
  return S_OK;
 }
 STDMETHODIMP ProcessInput(DWORD id,IMFSample *sample,DWORD flags) override {
  if(id)return MF_E_INVALIDSTREAMNUMBER;if(!sample)return E_POINTER;if(flags)return E_INVALIDARG;std::lock_guard lock(mutex_);
  HRESULT hr=Ready();if(FAILED(hr))return hr;if(!streaming_||!accept_||!requested_||queued_||encoded_||draining_)return MF_E_NOTACCEPTING;
  queued_=true;requested_=false;ResetEvent(done_);hr=MFPutWorkItem(queue_,this,sample);if(FAILED(hr)){queued_=false;requested_=true;SetEvent(done_);}return hr;
 }
 STDMETHODIMP ProcessOutput(DWORD flags,DWORD count,MFT_OUTPUT_DATA_BUFFER *buffers,DWORD *status) override {
  if(flags||count!=1)return E_INVALIDARG;if(!buffers||!status)return E_POINTER;if(buffers[0].dwStreamID)return MF_E_INVALIDSTREAMNUMBER;
  std::lock_guard lock(mutex_);HRESULT hr=Ready();if(FAILED(hr))return hr;if(!encoded_)return E_UNEXPECTED;
  if(buffers[0].pSample)return E_INVALIDARG;*status=0;buffers[0].dwStatus=0;buffers[0].pEvents=nullptr;buffers[0].pSample=encoded_.Detach();return streaming_&&accept_&&!draining_?Event(METransformNeedInput):S_OK;
 }
 STDMETHODIMP GetParameters(DWORD *,DWORD *) override {return E_NOTIMPL;}
 STDMETHODIMP Invoke(IMFAsyncResult *result) override {
  ComPtr<IUnknown> state;ComPtr<IMFSample> input,output;HRESULT hr=result->GetState(&state);if(SUCCEEDED(hr))hr=state.As(&input);
  ULONG bitrate;bool key;{std::lock_guard lock(mutex_);bitrate=bitrate_;key=key_;key_=false;}
  try {if(SUCCEEDED(hr))hr=Encode(input.Get(),&output,bitrate,key);}catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
  std::lock_guard lock(mutex_);if(streaming_&&!shutdown_){if(SUCCEEDED(hr)){if(discontinuity_){output->SetUINT32(MFSampleExtension_Discontinuity,TRUE);discontinuity_=false;}encoded_=output;Event(METransformHaveOutput);}else Event(MEError,hr);for(auto marker:markers_)Marker(marker);markers_.clear();if(draining_)Event(METransformDrainComplete);}
  queued_=false;SetEvent(done_);return S_OK;
 }
 STDMETHODIMP GetEvent(DWORD flags,IMFMediaEvent **event) override {return events_->GetEvent(flags,event);}
 STDMETHODIMP BeginGetEvent(IMFAsyncCallback *callback,IUnknown *state) override {return events_->BeginGetEvent(callback,state);}
 STDMETHODIMP EndGetEvent(IMFAsyncResult *result,IMFMediaEvent **event) override {return events_->EndGetEvent(result,event);}
 STDMETHODIMP QueueEvent(MediaEventType type,REFGUID extended,HRESULT status,const PROPVARIANT *value) override {return events_->QueueEventParamVar(type,extended,status,value);}
 STDMETHODIMP IsSupported(const GUID *key) override {return key&&Supported(*key)?S_OK:S_FALSE;}
 STDMETHODIMP IsModifiable(const GUID *key) override {return key&&(*key==CODECAPI_AVEncCommonMeanBitRate||*key==CODECAPI_AVEncVideoForceKeyFrame)?S_OK:S_FALSE;}
 STDMETHODIMP GetParameterRange(const GUID *key,VARIANT *low,VARIANT *high,VARIANT *step) override {if(!key||!low||!high||!step)return E_POINTER;if(*key!=CODECAPI_AVEncCommonMeanBitRate)return E_NOTIMPL;std::lock_guard lock(mutex_);VariantInit(low);VariantInit(high);VariantInit(step);low->vt=high->vt=step->vt=VT_UI4;low->ulVal=64000;high->ulVal=BitrateLimit();step->ulVal=1;return S_OK;}
 STDMETHODIMP GetParameterValues(const GUID *,VARIANT **,ULONG *) override {return E_NOTIMPL;}
 STDMETHODIMP GetDefaultValue(const GUID *key,VARIANT *value) override {if(!key||!value)return E_POINTER;VariantInit(value);if(!Supported(*key))return E_NOTIMPL;if(*key==CODECAPI_AVLowLatencyMode){value->vt=VT_BOOL;value->boolVal=VARIANT_TRUE;}else{value->vt=VT_UI4;value->ulVal=*key==CODECAPI_AVEncCommonMeanBitRate?3000000:*key==CODECAPI_AVEncMPVGOPSize?60:0;}return S_OK;}
 STDMETHODIMP GetValue(const GUID *key,VARIANT *value) override {if(!key||!value)return E_POINTER;std::lock_guard lock(mutex_);HRESULT hr=GetDefaultValue(key,value);if(SUCCEEDED(hr)){if(*key==CODECAPI_AVEncCommonMeanBitRate)value->ulVal=bitrate_;else if(*key==CODECAPI_AVEncMPVGOPSize)value->ulVal=gop_;else if(*key==CODECAPI_AVEncVideoForceKeyFrame){value->vt=VT_BOOL;value->boolVal=key_?VARIANT_TRUE:VARIANT_FALSE;}}return hr;}
 STDMETHODIMP SetValue(const GUID *key,VARIANT *value) override {
  if(!key||!value)return E_POINTER;std::lock_guard lock(mutex_);if(shutdown_)return MF_E_SHUTDOWN;
  if(*key==CODECAPI_AVLowLatencyMode)return value->vt==VT_BOOL&&value->boolVal!=VARIANT_FALSE?S_OK:E_INVALIDARG;
  if(*key==CODECAPI_AVEncVideoForceKeyFrame){if(value->vt!=VT_BOOL&&value->vt!=VT_UI4)return E_INVALIDARG;key_=value->vt==VT_BOOL?value->boolVal!=VARIANT_FALSE:value->ulVal!=0;return S_OK;}
  if(value->vt!=VT_UI4)return E_INVALIDARG;
  if(*key==CODECAPI_AVEncCommonMeanBitRate){if(value->ulVal<64000||value->ulVal>BitrateLimit())return E_INVALIDARG;bitrate_=value->ulVal;return S_OK;}
  if(*key==CODECAPI_AVEncCommonRateControlMode)return value->ulVal==eAVEncCommonRateControlMode_CBR?S_OK:E_INVALIDARG;
  if(*key==CODECAPI_AVEncMPVDefaultBPictureCount)return value->ulVal?E_INVALIDARG:S_OK;
  if(*key==CODECAPI_AVEncMPVGOPSize){if(!value->ulVal||value->ulVal>1800)return E_INVALIDARG;if(streaming_&&value->ulVal!=gop_)return MF_E_INVALIDREQUEST;gop_=value->ulVal;return S_OK;}
  return E_NOTIMPL;
 }
 STDMETHODIMP RegisterForEvent(const GUID *,LONG_PTR) override {return E_NOTIMPL;}
 STDMETHODIMP UnregisterForEvent(const GUID *) override {return E_NOTIMPL;}
 STDMETHODIMP SetAllDefaults() override {std::lock_guard lock(mutex_);if(streaming_&&gop_!=60)return MF_E_INVALIDREQUEST;bitrate_=3000000;gop_=60;key_=true;return S_OK;}
 STDMETHODIMP SetValueWithNotify(const GUID *key,VARIANT *value,GUID **changed,ULONG *count) override {if(!changed||!count)return E_POINTER;*changed=nullptr;*count=0;return SetValue(key,value);}
 STDMETHODIMP SetAllDefaultsWithNotify(GUID **changed,ULONG *count) override {if(!changed||!count)return E_POINTER;*changed=nullptr;*count=0;return SetAllDefaults();}
 STDMETHODIMP GetAllSettings(IStream *) override {return E_NOTIMPL;}
 STDMETHODIMP SetAllSettings(IStream *) override {return E_NOTIMPL;}
 STDMETHODIMP SetAllSettingsWithNotify(IStream *,GUID **,ULONG *) override {return E_NOTIMPL;}
 STDMETHODIMP Shutdown() override {std::unique_lock lock(mutex_);if(shutdown_)return MF_E_SHUTDOWN;shutdown_=true;HRESULT hr=Stop(lock);events_->Shutdown();return hr;}
 STDMETHODIMP GetShutdownStatus(MFSHUTDOWN_STATUS *status) override {if(!status)return E_POINTER;std::lock_guard lock(mutex_);if(!shutdown_)return MF_E_INVALIDREQUEST;*status=MFSHUTDOWN_COMPLETED;return S_OK;}
};
class Factory final:public RuntimeClass<RuntimeClassFlags<ClassicCom>,IClassFactory> {
public:
 Factory(){Objects++;}~Factory(){Objects--;}
 STDMETHODIMP CreateInstance(IUnknown *outer,REFIID iid,void **out) override {if(outer)return CLASS_E_NOAGGREGATION;if(!out)return E_POINTER;*out=nullptr;auto encoder=Make<Encoder>();if(!encoder)return E_OUTOFMEMORY;HRESULT hr=encoder->Initialize();return FAILED(hr)?hr:encoder->QueryInterface(iid,out);}
 STDMETHODIMP LockServer(BOOL lock) override {if(lock)Locks++;else Locks--;return S_OK;}
};
extern "C" BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,void *){if(reason==DLL_PROCESS_ATTACH){EncoderModule=module;DisableThreadLibraryCalls(module);}return TRUE;}
extern "C" HRESULT WINAPI DllCanUnloadNow(){return Objects==0&&Locks==0?S_OK:S_FALSE;}
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid,REFIID iid,void **out){if(clsid!=EncoderClass)return CLASS_E_CLASSNOTAVAILABLE;auto factory=Make<Factory>();return factory?factory->QueryInterface(iid,out):E_OUTOFMEMORY;}
static HRESULT Registration(bool add) {
 wchar_t clsid[40];StringFromGUID2(EncoderClass,clsid,40);std::wstring key=L"Software\\Classes\\CLSID\\";key+=clsid;
 if(!add){MFTUnregister(EncoderClass);LSTATUS status=RegDeleteTreeW(HKEY_LOCAL_MACHINE,key.c_str());return status==ERROR_FILE_NOT_FOUND?S_OK:HRESULT_FROM_WIN32(status);}
 wchar_t path[MAX_PATH];DWORD length=GetModuleFileNameW(EncoderModule,path,MAX_PATH);if(!length||length>=MAX_PATH)return E_FAIL;
 std::wstring file(path);file.resize(file.find_last_of(L"\\")+1);file+=L"a830_encoder.dll";
 key+=L"\\InprocServer32";HKEY registry=nullptr;LSTATUS status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,key.c_str(),0,nullptr,0,KEY_WRITE,nullptr,&registry,nullptr);
 if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
 status=RegSetValueExW(registry,nullptr,0,REG_SZ,(const BYTE *)file.c_str(),(DWORD)((file.size()+1)*sizeof(wchar_t)));
 if(status==ERROR_SUCCESS)status=RegSetValueExW(registry,L"ThreadingModel",0,REG_SZ,(const BYTE *)L"Both",10);RegCloseKey(registry);if(status!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(status);
 HRESULT hr=MFStartup(MF_VERSION);if(FAILED(hr))return hr;
 ComPtr<IMFAttributes> attributes;hr=MFCreateAttributes(&attributes,4);
 if(SUCCEEDED(hr)) {
  ComPtr<IDXGIFactory1> factory;hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  if(SUCCEEDED(hr)) {
   hr=MF_E_NOT_FOUND;
   for(UINT i=0;;i++) {
    ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
    DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc)) || (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) || !wcsstr(desc.Description,L"Adreno"))continue;
    wchar_t vendor[16];swprintf_s(vendor,L"VEN_%04X",desc.VendorId);
    attributes->SetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute,vendor);
    hr=S_OK;break;
   }
  }
  attributes->SetString(MFT_ENUM_HARDWARE_URL_Attribute,L"ACPI\\HMRG0830");attributes->SetUINT32(MF_TRANSFORM_ASYNC,TRUE);attributes->SetUINT32(MF_SA_D3D11_AWARE,TRUE);attributes->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE,TRUE);
  MFT_REGISTER_TYPE_INFO input[]={{MFMediaType_Video,MFVideoFormat_NV12},{MFMediaType_Video,MFVideoFormat_ARGB32}},output={MFMediaType_Video,MFVideoFormat_H264};
  if(SUCCEEDED(hr))hr=MFTRegister(EncoderClass,MFT_CATEGORY_VIDEO_ENCODER,(LPWSTR)EncoderName,MFT_ENUM_FLAG_ASYNCMFT|MFT_ENUM_FLAG_HARDWARE,2,input,1,&output,attributes.Get());
 }
 MFShutdown();return hr;
}
extern "C" HRESULT WINAPI DllRegisterServer(){return Registration(true);}
extern "C" HRESULT WINAPI DllUnregisterServer(){return Registration(false);}
