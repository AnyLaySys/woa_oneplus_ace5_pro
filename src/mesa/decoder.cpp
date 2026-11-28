#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl.h>
#include <mutex>
#include <deque>
#include <map>
#include <atomic>
#include "../qcom-gpu/uapi.h"
#include "decode-stream.h"
#include "decode-surfaces.h"

using namespace Microsoft::WRL;
static const CLSID DecoderClass={0x21c12dbb,0x5d71,0x4c39,{0x8a,0xe9,0x8f,0x38,0x41,0x2a,0x83,0x01}};
static std::atomic<long> Objects,Locks;
static const GUID Codecs[]={MFVideoFormat_H264,MFVideoFormat_HEVC,MFVideoFormat_VP90,MFVideoFormat_AV1};
static HRESULT Io(HANDLE handle,DWORD code,const void *input=nullptr,DWORD size=0,void *output=nullptr,DWORD capacity=0) {
 DWORD used=0;return DeviceIoControl(handle,code,const_cast<void *>(input),size,output,capacity,&used,nullptr)?S_OK:HRESULT_FROM_WIN32(GetLastError());
}
static unsigned Codec(IMFMediaType *type) {
 GUID subtype{};if(!type || FAILED(type->GetGUID(MF_MT_SUBTYPE,&subtype)))return 0;
 for(unsigned i=0;i<4;i++)if(subtype==Codecs[i])return i*2+1;return 0;
}
static HRESULT FrameSize(IMFMediaType *type,UINT32 *width,UINT32 *height) {
 HRESULT hr=MFGetAttributeSize(type,MF_MT_FRAME_SIZE,width,height);if(FAILED(hr))return hr;
 for(const GUID *key:{&MF_MT_MINIMUM_DISPLAY_APERTURE,&MF_MT_GEOMETRIC_APERTURE}) {
  MFVideoArea area{};UINT32 size=0;
  if(SUCCEEDED(type->GetBlob(*key,(BYTE *)&area,sizeof(area),&size)) && size==sizeof(area) &&
     !area.OffsetX.value && !area.OffsetX.fract && !area.OffsetY.value && !area.OffsetY.fract &&
     area.Area.cx>0 && area.Area.cy>0 && (UINT32)area.Area.cx<=*width && (UINT32)area.Area.cy<=*height) {
   *width=area.Area.cx;*height=area.Area.cy;break;
  }
 }
 return S_OK;
}
static unsigned BitDepth(IMFMediaType *type) {
 unsigned codec=Codec(type),profile=MFGetAttributeUINT32(type,MF_MT_VIDEO_PROFILE,0);
 if((codec==3 || codec==5) && profile==2)return 10;
 UINT32 size=0;
 if(type && SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER,&size)) && size>=4 && size<=1048576) {
  BYTE *header=nullptr;
  if(SUCCEEDED(type->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER,&header,&size))) {
   unsigned depth=8;
   if(codec==3 && size>=23 && header[0]==1)depth=8+(header[17]&7);
   if(codec==7 && size>=4 && header[0]==0x81)depth=header[2]&0x20?12:header[2]&0x40?10:8;
   CoTaskMemFree(header);return depth;
  }
 }
 return 8;
}
class Decoder final:public RuntimeClass<RuntimeClassFlags<ClassicCom>,IMFTransform,IMFMediaEventGenerator,IMFShutdown,IMFAsyncCallback> {
 std::mutex mutex_;
 ComPtr<IMFAttributes> attributes_,inputAttributes_,outputAttributes_;
 ComPtr<IMFMediaEventQueue> events_;
 ComPtr<IMFMediaBuffer> receive_;
 ComPtr<IMFMediaType> input_,output_;
 ComPtr<IMFDXGIDeviceManager> manager_;
 DecodeSurfaces surfaces_;
 struct Timing{LONGLONG Time,Duration;};
 std::map<ULONGLONG,Timing> times_;
 std::deque<ComPtr<IMFSample>> samples_;
 std::multimap<LONGLONG,ComPtr<IMFSample>> presentation_;
 LONGLONG nextPresentation_=0;
 bool presentationStarted_=false;
 std::vector<ULONG_PTR> markers_;
 DecodeStream stream_;
 GPU_DECODE_CONFIG config_{};
 HANDLE gpu_=INVALID_HANDLE_VALUE,done_=nullptr;
 DWORD queue_=0;
 ULONGLONG token_=0;
 bool streaming_=false,accept_=false,queued_=false,requested_=false,draining_=false,drained_=false,shutdown_=false,first_=true,discontinuity_=true;
 HRESULT Event(MediaEventType type,HRESULT status=S_OK) {
  PROPVARIANT value{};if(type==METransformNeedInput){value.vt=VT_UI4;requested_=true;}
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
 HRESULT Validate(IMFMediaType *type,bool output) {
  if(!type)return S_OK;
  GUID major{},sub{};UINT32 w=0,h=0;
  bool hevc=Codec(output?input_.Get():type)==3;
  UINT32 dimension=hevc?8192:4096,pixels=hevc?8192*4320:4096*2160;
  if(FAILED(type->GetGUID(MF_MT_MAJOR_TYPE,&major)) || major!=MFMediaType_Video || FAILED(type->GetGUID(MF_MT_SUBTYPE,&sub)) ||
     (output?(sub!=MFVideoFormat_NV12 && sub!=MFVideoFormat_P010):!Codec(type)) ||
     FAILED(MFGetAttributeSize(type,MF_MT_FRAME_SIZE,&w,&h)) || w<64 || h<64 || w>dimension || h>dimension ||
     w*h>pixels || ((w|h)&1))return MF_E_INVALIDMEDIATYPE;
  if(output && input_) {
   UINT32 iw=0,ih=0;FrameSize(input_.Get(),&iw,&ih);
   if(iw!=w || ih!=h || sub!=(BitDepth(input_.Get())==10?MFVideoFormat_P010:MFVideoFormat_NV12))return MF_E_INVALIDMEDIATYPE;
  }
  if(!output && BitDepth(type)>10)return MF_E_INVALIDMEDIATYPE;
  return S_OK;
 }
 HRESULT Start() {
  if(!input_ || !output_)return MF_E_TRANSFORM_TYPE_NOT_SET;
  GUID subtype{};output_->GetGUID(MF_MT_SUBTYPE,&subtype);
  config_={};config_.Codec=Codec(input_.Get());config_.BitDepth=subtype==MFVideoFormat_P010?10:8;
  MFGetAttributeSize(output_.Get(),MF_MT_FRAME_SIZE,&config_.Width,&config_.Height);
  UINT32 count=0;std::vector<BYTE> header;
  if(SUCCEEDED(input_->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER,&count))) {
   if(count>1048576)return MF_E_INVALIDMEDIATYPE;header.resize(count);
   HRESULT hr=input_->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,header.data(),count,&count);if(FAILED(hr))return hr;
  }
  if(!stream_.Configure(config_.Codec,header.data(),header.size()))return MF_E_INVALIDMEDIATYPE;
  gpu_=CreateFileW(L"\\\\.\\QcomGpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
  if(gpu_==INVALID_HANDLE_VALUE)return HRESULT_FROM_WIN32(GetLastError());
  if(manager_) {
   HRESULT hr=surfaces_.Open(manager_.Get(),gpu_,config_.Width,config_.Height,config_.BitDepth);
   if(FAILED(hr)){surfaces_.Close();CloseHandle(gpu_);gpu_=INVALID_HANDLE_VALUE;return hr;}
   config_.Flags|=GPU_DECODE_SURFACES|GPU_DECODE_QUEUED_SURFACES;
   if(config_.Codec!=7)config_.Flags|=GPU_DECODE_ORDER;
  }
  HRESULT hr=Io(gpu_,GPU_IOCTL_DECODE_OPEN,&config_,sizeof(config_));
  if(FAILED(hr)){surfaces_.Close();CloseHandle(gpu_);gpu_=INVALID_HANDLE_VALUE;return hr;}
  streaming_=true;first_=true;discontinuity_=true;return S_OK;
 }
 HRESULT Stop(std::unique_lock<std::mutex> &lock) {
  streaming_=false;accept_=false;requested_=false;
  if(queued_){lock.unlock();WaitForSingleObject(done_,INFINITE);lock.lock();}
  HRESULT hr=gpu_==INVALID_HANDLE_VALUE?S_OK:Io(gpu_,GPU_IOCTL_VIDEO_CLOSE);
  surfaces_.Close();
  if(gpu_!=INVALID_HANDLE_VALUE){CloseHandle(gpu_);gpu_=INVALID_HANDLE_VALUE;}
  samples_.clear();presentation_.clear();presentationStarted_=false;times_.clear();markers_.clear();receive_.Reset();draining_=false;drained_=false;return hr;
 }
 HRESULT Schedule(IMFSample *sample) {
  queued_=true;requested_=false;ResetEvent(done_);HRESULT hr=MFPutWorkItem(queue_,this,sample);
  if(FAILED(hr)){queued_=false;SetEvent(done_);}return hr;
 }
 HRESULT Collect(std::deque<ComPtr<IMFSample>> &frames,bool &end) {
  unsigned capacity=config_.Width*config_.Height*3/2*(config_.BitDepth==10?2:1);
  for(;;) {
   if(surfaces_.Enabled()) {
    GPU_DECODE_COMPLETION completion{};HRESULT hr=Io(gpu_,GPU_IOCTL_DECODE_DEQUEUE,nullptr,0,&completion,sizeof(completion));
    if(hr==HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS))return S_OK;if(FAILED(hr))return hr;
    const auto &out=completion.Output;
    if(!out.Bytes){end=true;return surfaces_.Release(completion.SurfaceAddress);}
    if(out.Bytes!=capacity || out.Width!=config_.Width || out.Height!=config_.Height || out.BitDepth!=config_.BitDepth)return E_FAIL;
    auto timing=times_.find(out.Time);if(timing==times_.end())return E_FAIL;
    ComPtr<IMFSample> sample;hr=surfaces_.Sample(completion.SurfaceAddress,&sample);if(FAILED(hr))return hr;
    sample->SetSampleTime(timing->second.Time);sample->SetSampleDuration(timing->second.Duration);
    if(discontinuity_){sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);discontinuity_=false;}
    if(!(config_.Flags&GPU_DECODE_ORDER)){frames.push_back(sample);continue;}
    LONGLONG time=0;sample->GetSampleTime(&time);
    if(!presentationStarted_){nextPresentation_=time;presentationStarted_=true;}
    presentation_.emplace(time,sample);
    while(!presentation_.empty()) {
     auto first=presentation_.begin();LONGLONG duration=0;first->second->GetSampleDuration(&duration);
     if(first->first>nextPresentation_+duration/2 && presentation_.size()<3)break;
     nextPresentation_=first->first+duration;frames.push_back(first->second);presentation_.erase(first);
    }
    continue;
   }
   HRESULT hr=receive_?S_OK:MFCreateMemoryBuffer(sizeof(GPU_DECODE_OUTPUT)+capacity,&receive_);
   if(FAILED(hr))return hr;
   BYTE *p=nullptr;hr=receive_->Lock(&p,nullptr,nullptr);if(FAILED(hr))return hr;
   hr=Io(gpu_,GPU_IOCTL_DECODE_OUTPUT,nullptr,0,p,sizeof(GPU_DECODE_OUTPUT)+capacity);
   GPU_DECODE_OUTPUT out{};if(SUCCEEDED(hr))memcpy(&out,p,sizeof(out));receive_->Unlock();
   if(hr==HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS))return S_OK;if(FAILED(hr))return hr;
   if(!out.Bytes){end=true;return S_OK;}
   if(out.Bytes!=capacity || out.Width!=config_.Width || out.Height!=config_.Height || out.BitDepth!=config_.BitDepth)return E_FAIL;
   auto timing=times_.find(out.Time);if(timing==times_.end())return E_FAIL;
   ComPtr<IMFSample> sample;ComPtr<IMFMediaBuffer> buffer;
   hr=MFCreateSample(&sample);if(FAILED(hr))return hr;
   hr=receive_->SetCurrentLength(sizeof(out)+out.Bytes);if(FAILED(hr))return hr;
   hr=MFCreateMediaBufferWrapper(receive_.Get(),sizeof(out),out.Bytes,&buffer);if(FAILED(hr))return hr;
   receive_.Reset();
   hr=sample->AddBuffer(buffer.Get());if(FAILED(hr))return hr;
   sample->SetSampleTime(timing->second.Time);sample->SetSampleDuration(timing->second.Duration);
   if(discontinuity_){sample->SetUINT32(MFSampleExtension_Discontinuity,TRUE);discontinuity_=false;}
   frames.push_back(sample);
  }
 }
 HRESULT Decode(IMFSample *sample,std::deque<ComPtr<IMFSample>> &frames) {
  while(times_.size()>=256)times_.erase(times_.begin());
  ComPtr<IMFMediaBuffer> buffer;HRESULT hr=sample->ConvertToContiguousBuffer(&buffer);if(FAILED(hr))return hr;
  DWORD length=0;BYTE *p=nullptr;hr=buffer->Lock(&p,nullptr,&length);if(FAILED(hr))return hr;
  std::vector<BYTE> data(sizeof(GPU_DECODE_INPUT)),frame;
  bool valid=false;try{valid=stream_.Frame(p,length,first_,frame);}catch(...){buffer->Unlock();throw;}
  buffer->Unlock();if(!valid)return MF_E_INVALID_STREAM_DATA;
  first_=false;data.insert(data.end(),frame.begin(),frame.end());
  auto *input=(GPU_DECODE_INPUT *)data.data();input->Time=++token_;input->Bytes=(DWORD)frame.size();
  Timing timing{};sample->GetSampleTime(&timing.Time);sample->GetSampleDuration(&timing.Duration);
  if(timing.Duration<=0){UINT32 n=30,d=1;MFGetAttributeRatio(input_.Get(),MF_MT_FRAME_RATE,&n,&d);timing.Duration=n&&d?10000000LL*d/n:333333;}
  times_.emplace(token_,timing);
  if(surfaces_.Enabled()) {
   ULONGLONG deadline=GetTickCount64()+10000;
   do {bool end=false;hr=Collect(frames,end);if(FAILED(hr))return hr;
    hr=surfaces_.Queue();if(hr!=E_PENDING)break;Sleep(1);
   }while(GetTickCount64()<deadline);
   if(hr==E_PENDING)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);if(FAILED(hr))return hr;
  }
  hr=Io(gpu_,GPU_IOCTL_DECODE_INPUT,data.data(),(DWORD)data.size());if(FAILED(hr))return hr;
  bool end=false;return Collect(frames,end);
 }
 HRESULT Drain(std::deque<ComPtr<IMFSample>> &frames) {
  if(first_)return S_OK;
  HRESULT hr=Io(gpu_,GPU_IOCTL_DECODE_DRAIN);if(FAILED(hr))return hr;
  ULONGLONG deadline=GetTickCount64()+10000;bool end=false;
  while(!end && GetTickCount64()<deadline){hr=Collect(frames,end);if(FAILED(hr))return hr;
   if(!end && surfaces_.Enabled()){hr=surfaces_.Queue();if(FAILED(hr) && hr!=E_PENDING)return hr;}
   if(!end)Sleep(1);
  }
  if(end && surfaces_.Enabled()){for(auto &entry:presentation_)frames.push_back(entry.second);presentation_.clear();}
  return end?S_OK:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
 }
 HRESULT Available(DWORD id,DWORD index,bool output,IMFMediaType **out) {
  if(id)return MF_E_INVALIDSTREAMNUMBER;if(!out)return E_POINTER;*out=nullptr;std::lock_guard lock(mutex_);
  if(index>=(output?(input_?1u:2u):4u))return MF_E_NO_MORE_TYPES;
  bool p010=output && (input_?BitDepth(input_.Get())==10:index!=0);
  ComPtr<IMFMediaType> type;HRESULT hr=MFCreateMediaType(&type);if(FAILED(hr))return hr;
  type->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);type->SetGUID(MF_MT_SUBTYPE,output?(p010?MFVideoFormat_P010:MFVideoFormat_NV12):Codecs[index]);
  if(output && input_) {
   UINT32 w=0,h=0;FrameSize(input_.Get(),&w,&h);MFSetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,w,h);
   type->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);type->SetUINT32(MF_MT_DEFAULT_STRIDE,w*(p010?2:1));
   type->SetUINT32(MF_MT_SAMPLE_SIZE,w*h*3/2*(p010?2:1));
   for(const GUID *key:{&MF_MT_FRAME_RATE,&MF_MT_PIXEL_ASPECT_RATIO,&MF_MT_VIDEO_PRIMARIES,&MF_MT_TRANSFER_FUNCTION,&MF_MT_YUV_MATRIX,&MF_MT_VIDEO_NOMINAL_RANGE}) {
    PROPVARIANT value{};if(SUCCEEDED(input_->GetItem(*key,&value))){type->SetItem(*key,value);PropVariantClear(&value);}
   }
  }
  return type.CopyTo(out);
 }
 HRESULT SetType(DWORD id,IMFMediaType *type,DWORD flags,bool output) {
  if(id)return MF_E_INVALIDSTREAMNUMBER;if(flags&~MFT_SET_TYPE_TEST_ONLY)return E_INVALIDARG;
  std::lock_guard lock(mutex_);if(shutdown_)return MF_E_SHUTDOWN;if(streaming_||queued_)return MF_E_TRANSFORM_CANNOT_CHANGE_MEDIATYPE_WHILE_PROCESSING;
  HRESULT hr;try{hr=Validate(type,output);}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}if(FAILED(hr) || (flags&MFT_SET_TYPE_TEST_ONLY))return hr;
  if(output)output_=type;else{input_=type;output_.Reset();}return S_OK;
 }
public:
 Decoder(){Objects++;}
 ~Decoder(){std::unique_lock lock(mutex_);Stop(lock);if(queue_)MFUnlockWorkQueue(queue_);if(events_)events_->Shutdown();if(done_)CloseHandle(done_);Objects--;}
 HRESULT Initialize() {
  HRESULT hr=MFCreateAttributes(&attributes_,4);if(FAILED(hr))return hr;
  attributes_->SetUINT32(MF_TRANSFORM_ASYNC,TRUE);attributes_->SetUINT32(MF_SA_D3D11_AWARE,TRUE);attributes_->SetString(MFT_ENUM_HARDWARE_URL_Attribute,L"ACPI\\HMRG0830");
  hr=MFCreateAttributes(&inputAttributes_,1);if(FAILED(hr))return hr;hr=MFCreateAttributes(&outputAttributes_,1);if(FAILED(hr))return hr;
  hr=MFCreateEventQueue(&events_);if(FAILED(hr))return hr;hr=MFAllocateWorkQueueEx(MF_STANDARD_WORKQUEUE,&queue_);if(FAILED(hr))return hr;
  done_=CreateEventW(nullptr,TRUE,TRUE,nullptr);return done_?S_OK:HRESULT_FROM_WIN32(GetLastError());
 }
 STDMETHODIMP GetStreamLimits(DWORD *a,DWORD *b,DWORD *c,DWORD *d) override{if(!a||!b||!c||!d)return E_POINTER;*a=*b=*c=*d=1;return S_OK;}
 STDMETHODIMP GetStreamCount(DWORD *a,DWORD *b) override{if(!a||!b)return E_POINTER;*a=*b=1;return S_OK;}
 STDMETHODIMP GetStreamIDs(DWORD,DWORD *,DWORD,DWORD *) override{return E_NOTIMPL;}
 STDMETHODIMP GetInputStreamInfo(DWORD id,MFT_INPUT_STREAM_INFO *info) override{if(id)return MF_E_INVALIDSTREAMNUMBER;if(!info)return E_POINTER;*info={};info->dwFlags=MFT_INPUT_STREAM_WHOLE_SAMPLES|MFT_INPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER|MFT_INPUT_STREAM_HOLDS_BUFFERS;return S_OK;}
 STDMETHODIMP GetOutputStreamInfo(DWORD id,MFT_OUTPUT_STREAM_INFO *info) override{if(id)return MF_E_INVALIDSTREAMNUMBER;if(!info)return E_POINTER;*info={};info->dwFlags=MFT_OUTPUT_STREAM_WHOLE_SAMPLES|MFT_OUTPUT_STREAM_SINGLE_SAMPLE_PER_BUFFER|MFT_OUTPUT_STREAM_PROVIDES_SAMPLES;return S_OK;}
 STDMETHODIMP GetAttributes(IMFAttributes **out) override{return attributes_.CopyTo(out);}
 STDMETHODIMP GetInputStreamAttributes(DWORD id,IMFAttributes **out) override{return id?MF_E_INVALIDSTREAMNUMBER:inputAttributes_.CopyTo(out);}
 STDMETHODIMP GetOutputStreamAttributes(DWORD id,IMFAttributes **out) override{return id?MF_E_INVALIDSTREAMNUMBER:outputAttributes_.CopyTo(out);}
 STDMETHODIMP DeleteInputStream(DWORD) override{return E_NOTIMPL;}
 STDMETHODIMP AddInputStreams(DWORD,DWORD *) override{return E_NOTIMPL;}
 STDMETHODIMP GetInputAvailableType(DWORD id,DWORD index,IMFMediaType **out) override{return Available(id,index,false,out);}
 STDMETHODIMP GetOutputAvailableType(DWORD id,DWORD index,IMFMediaType **out) override{return Available(id,index,true,out);}
 STDMETHODIMP SetInputType(DWORD id,IMFMediaType *type,DWORD flags) override{return SetType(id,type,flags,false);}
 STDMETHODIMP SetOutputType(DWORD id,IMFMediaType *type,DWORD flags) override{return SetType(id,type,flags,true);}
 STDMETHODIMP GetInputCurrentType(DWORD id,IMFMediaType **out) override{std::lock_guard lock(mutex_);return id?MF_E_INVALIDSTREAMNUMBER:input_?input_.CopyTo(out):MF_E_TRANSFORM_TYPE_NOT_SET;}
 STDMETHODIMP GetOutputCurrentType(DWORD id,IMFMediaType **out) override{std::lock_guard lock(mutex_);return id?MF_E_INVALIDSTREAMNUMBER:output_?output_.CopyTo(out):MF_E_TRANSFORM_TYPE_NOT_SET;}
 STDMETHODIMP GetInputStatus(DWORD id,DWORD *flags) override{if(id)return MF_E_INVALIDSTREAMNUMBER;if(!flags)return E_POINTER;std::lock_guard lock(mutex_);*flags=streaming_&&accept_&&requested_&&!queued_&&!draining_?MFT_INPUT_STATUS_ACCEPT_DATA:0;return S_OK;}
 STDMETHODIMP GetOutputStatus(DWORD *flags) override{if(!flags)return E_POINTER;std::lock_guard lock(mutex_);*flags=samples_.empty()?0:MFT_OUTPUT_STATUS_SAMPLE_READY;return S_OK;}
 STDMETHODIMP SetOutputBounds(LONGLONG,LONGLONG) override{return E_NOTIMPL;}
 STDMETHODIMP ProcessEvent(DWORD,IMFMediaEvent *) override{return E_NOTIMPL;}
 STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE message,ULONG_PTR value) override {
  std::unique_lock lock(mutex_);if(shutdown_)return MF_E_SHUTDOWN;
  if(message==MFT_MESSAGE_SET_D3D_MANAGER){HRESULT hr=Stop(lock);manager_.Reset();if(SUCCEEDED(hr) && value)hr=((IUnknown *)value)->QueryInterface(IID_PPV_ARGS(&manager_));return hr;}
  if(message==MFT_MESSAGE_NOTIFY_BEGIN_STREAMING){if(streaming_)return S_OK;try{return Start();}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}}
  if(message==MFT_MESSAGE_NOTIFY_END_STREAMING || message==MFT_MESSAGE_NOTIFY_RELEASE_RESOURCES)return Stop(lock);
  if(message==MFT_MESSAGE_NOTIFY_END_OF_STREAM){accept_=false;requested_=false;return S_OK;}
  if(message==MFT_MESSAGE_COMMAND_FLUSH) {
   HRESULT hr=Stop(lock);ComPtr<IMFMediaEvent> event;while(events_->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event)==S_OK)event.Reset();return hr;
  }
  if(message==MFT_MESSAGE_COMMAND_DRAIN){draining_=true;requested_=false;return queued_?S_OK:Schedule(nullptr);}
  if(message==MFT_MESSAGE_COMMAND_MARKER){if(queued_){try{markers_.push_back(value);}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}return S_OK;}return Marker(value);}
  if(message==MFT_MESSAGE_NOTIFY_START_OF_STREAM){if(!streaming_){try{HRESULT hr=Start();if(FAILED(hr))return hr;}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}}accept_=true;draining_=false;return !queued_&&!requested_?Event(METransformNeedInput):S_OK;}
  return S_OK;
 }
 STDMETHODIMP ProcessInput(DWORD id,IMFSample *sample,DWORD flags) override {
  if(id)return MF_E_INVALIDSTREAMNUMBER;if(!sample)return E_POINTER;if(flags)return E_INVALIDARG;std::lock_guard lock(mutex_);
  HRESULT hr=Ready();if(FAILED(hr))return hr;if(!streaming_||!accept_||!requested_||queued_||draining_)return MF_E_NOTACCEPTING;return Schedule(sample);
 }
 STDMETHODIMP ProcessOutput(DWORD flags,DWORD count,MFT_OUTPUT_DATA_BUFFER *buffers,DWORD *status) override {
  if(flags || count!=1)return E_INVALIDARG;if(!buffers||!status)return E_POINTER;std::lock_guard lock(mutex_);
  HRESULT hr=Ready();if(FAILED(hr))return hr;if(samples_.empty())return MF_E_TRANSFORM_NEED_MORE_INPUT;if(buffers[0].pSample)return E_INVALIDARG;
  *status=0;buffers[0].dwStatus=0;buffers[0].pEvents=nullptr;buffers[0].pSample=samples_.front().Detach();samples_.pop_front();
  if(samples_.empty()&&drained_){drained_=false;Event(METransformDrainComplete);}
  if(streaming_&&accept_&&!draining_&&!queued_&&!requested_&&samples_.size()<8)Event(METransformNeedInput);
  return S_OK;
 }
 STDMETHODIMP GetParameters(DWORD *,DWORD *) override{return E_NOTIMPL;}
 STDMETHODIMP Invoke(IMFAsyncResult *result) override {
  ComPtr<IMFSample> input;IUnknown *state=result->GetStateNoAddRef();HRESULT hr=state?state->QueryInterface(IID_PPV_ARGS(&input)):S_OK;
  std::deque<ComPtr<IMFSample>> frames;bool drain=false;
  try {
   if(SUCCEEDED(hr)&&input)hr=Decode(input.Get(),frames);
   {std::lock_guard lock(mutex_);drain=draining_&&streaming_&&!input;}
   if(SUCCEEDED(hr)&&drain)hr=Drain(frames);
  }catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
  std::lock_guard lock(mutex_);
  if(streaming_&&!shutdown_) {
   if(SUCCEEDED(hr))try{for(auto &sample:frames){samples_.push_back(sample);Event(METransformHaveOutput);}}catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
   if(FAILED(hr)){accept_=false;Event(MEError,hr);}
   for(auto marker:markers_)Marker(marker);markers_.clear();
   if(SUCCEEDED(hr)&&drain){drained_=!samples_.empty();if(!drained_)Event(METransformDrainComplete);}
   if(SUCCEEDED(hr)&&accept_&&!draining_&&samples_.size()<8&&!requested_)Event(METransformNeedInput);
  }
  queued_=false;SetEvent(done_);
  if(SUCCEEDED(hr)&&streaming_&&draining_&&!drain)Schedule(nullptr);
  return S_OK;
 }
 STDMETHODIMP GetEvent(DWORD flags,IMFMediaEvent **event) override{return events_->GetEvent(flags,event);}
 STDMETHODIMP BeginGetEvent(IMFAsyncCallback *callback,IUnknown *state) override{return events_->BeginGetEvent(callback,state);}
 STDMETHODIMP EndGetEvent(IMFAsyncResult *result,IMFMediaEvent **event) override{return events_->EndGetEvent(result,event);}
 STDMETHODIMP QueueEvent(MediaEventType type,REFGUID extended,HRESULT status,const PROPVARIANT *value) override{return events_->QueueEventParamVar(type,extended,status,value);}
 STDMETHODIMP Shutdown() override{std::unique_lock lock(mutex_);if(shutdown_)return MF_E_SHUTDOWN;shutdown_=true;HRESULT hr=Stop(lock);events_->Shutdown();return hr;}
 STDMETHODIMP GetShutdownStatus(MFSHUTDOWN_STATUS *status) override{if(!status)return E_POINTER;std::lock_guard lock(mutex_);if(!shutdown_)return MF_E_INVALIDREQUEST;*status=MFSHUTDOWN_COMPLETED;return S_OK;}
};
class Factory final:public RuntimeClass<RuntimeClassFlags<ClassicCom>,IClassFactory> {
public:
 Factory(){Objects++;}~Factory(){Objects--;}
 STDMETHODIMP CreateInstance(IUnknown *outer,REFIID iid,void **out) override{if(outer)return CLASS_E_NOAGGREGATION;if(!out)return E_POINTER;*out=nullptr;auto decoder=Make<Decoder>();if(!decoder)return E_OUTOFMEMORY;HRESULT hr=decoder->Initialize();return FAILED(hr)?hr:decoder->QueryInterface(iid,out);}
 STDMETHODIMP LockServer(BOOL lock) override{if(lock)Locks++;else Locks--;return S_OK;}
};
extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID clsid,REFIID iid,void **out){if(clsid!=DecoderClass)return CLASS_E_CLASSNOTAVAILABLE;auto factory=Make<Factory>();return factory?factory->QueryInterface(iid,out):E_OUTOFMEMORY;}
extern "C" HRESULT WINAPI DllCanUnloadNow(){return Objects==0&&Locks==0?S_OK:S_FALSE;}
