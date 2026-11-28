#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dkmthk.h>
#include <memory>
#include "../qcom-gpu/wddm/abi.h"

/* Native MF output pool, shared by every codec. Tracked samples retain their
   texture and return a slot only when the downstream consumer releases it. */
class DecodeSurfaces {
 struct Slot {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> Texture;
  GPU_DECODE_SURFACE Surface{};
  D3DKMT_HANDLE Resource=0;
  ULONG Import=0;
  std::atomic<bool> Free{true};
  bool Pending=false;
 };
 class Released final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IMFAsyncCallback> {
  std::shared_ptr<Slot> slot_;
 public:
  explicit Released(std::shared_ptr<Slot> slot):slot_(std::move(slot)){}
  STDMETHODIMP GetParameters(DWORD *,DWORD *) override{return E_NOTIMPL;}
  STDMETHODIMP Invoke(IMFAsyncResult *) override{slot_->Free=true;return S_OK;}
 };
 Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
 Microsoft::WRL::ComPtr<ID3D11Device> device_;
 Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
 HANDLE deviceHandle_=nullptr;
 HANDLE gpu_=INVALID_HANDLE_VALUE;
 D3DKMT_HANDLE adapter_=0,deviceHandleKernel_=0;
 std::shared_ptr<Slot> slots_[4];
 static HRESULT Kernel(NTSTATUS status){return status<0?HRESULT_FROM_NT(status):S_OK;}
 HRESULT Share(Slot &slot,UINT height) {
  Microsoft::WRL::ComPtr<IDXGIResource> shared;HANDLE handle=nullptr;
  HRESULT hr=slot.Texture.As(&shared);if(FAILED(hr))return hr;
  hr=shared->GetSharedHandle(&handle);if(FAILED(hr))return hr;
  D3DKMT_QUERYRESOURCEINFO info{};info.hDevice=deviceHandleKernel_;info.hGlobalShare=(D3DKMT_HANDLE)(ULONG_PTR)handle;
  hr=Kernel(D3DKMTQueryResourceInfo(&info));if(FAILED(hr))return hr;
  if(info.NumAllocations!=1 || info.ResourcePrivateDriverDataSize>65536 || info.PrivateRuntimeDataSize>65536 || info.TotalPrivateDriverDataSize>65536)return E_FAIL;
  std::vector<BYTE> runtime(info.PrivateRuntimeDataSize),resource(info.ResourcePrivateDriverDataSize),allocation(info.TotalPrivateDriverDataSize);
  D3DDDI_OPENALLOCATIONINFO allocationInfo{};D3DKMT_OPENRESOURCE opened{};opened.hDevice=deviceHandleKernel_;opened.hGlobalShare=info.hGlobalShare;
  opened.NumAllocations=1;opened.pOpenAllocationInfo=&allocationInfo;opened.pPrivateRuntimeData=runtime.data();opened.PrivateRuntimeDataSize=info.PrivateRuntimeDataSize;
  opened.pResourcePrivateDriverData=resource.data();opened.ResourcePrivateDriverDataSize=info.ResourcePrivateDriverDataSize;
  opened.pTotalPrivateDriverDataBuffer=allocation.data();opened.TotalPrivateDriverDataBufferSize=info.TotalPrivateDriverDataSize;
  hr=Kernel(D3DKMTOpenResource(&opened));if(FAILED(hr))return hr;slot.Resource=opened.hResource;
  A830_ALLOCATION_ADDRESS address{A830_ABI_VERSION,allocationInfo.hAllocation,0,0,0,0,0};
  D3DKMT_ESCAPE escape{};escape.hAdapter=adapter_;escape.hDevice=deviceHandleKernel_;escape.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;escape.pPrivateDriverData=&address;escape.PrivateDriverDataSize=sizeof(address);
  hr=Kernel(D3DKMTEscape(&escape));if(FAILED(hr))return hr;
  UINT lines=(height+63)&~63u;uint64_t luma=(uint64_t)address.Pitch*lines,bytes=luma*3/2;
  if(bytes>address.Size || bytes>UINT_MAX || ((luma|bytes)&4095))return MF_E_UNSUPPORTED_D3D_TYPE;
  slot.Surface={address.Address,0,luma,(UINT)bytes,address.Pitch,lines,0};
  GPU_SHARED_IMPORT imported{address.Address,address.Size};DWORD used=0;
  return DeviceIoControl(gpu_,GPU_IOCTL_SHARED_RETAIN,&imported,sizeof(imported),&slot.Import,sizeof(slot.Import),&used,nullptr)?S_OK:HRESULT_FROM_WIN32(GetLastError());
 }
public:
 ~DecodeSurfaces(){Close();}
 bool Enabled() const{return device_!=nullptr;}
 void Close() {
  for(auto &slot:slots_)if(slot) {
   if(slot->Import && gpu_!=INVALID_HANDLE_VALUE){DWORD used=0;DeviceIoControl(gpu_,GPU_IOCTL_SHARED_RELEASE,&slot->Import,sizeof(slot->Import),nullptr,0,&used,nullptr);slot->Import=0;}
   if(slot->Resource){D3DKMT_DESTROYALLOCATION args{};args.hDevice=deviceHandleKernel_;args.hResource=slot->Resource;D3DKMTDestroyAllocation(&args);slot->Resource=0;}
   slot.reset();
  }
  if(deviceHandleKernel_){D3DKMT_DESTROYDEVICE args{};args.hDevice=deviceHandleKernel_;D3DKMTDestroyDevice(&args);deviceHandleKernel_=0;}
  if(adapter_){D3DKMT_CLOSEADAPTER args{};args.hAdapter=adapter_;D3DKMTCloseAdapter(&args);adapter_=0;}
  context_.Reset();device_.Reset();
  if(deviceHandle_ && manager_)manager_->CloseDeviceHandle(deviceHandle_);
  deviceHandle_=nullptr;gpu_=INVALID_HANDLE_VALUE;manager_.Reset();
 }
 HRESULT Open(IMFDXGIDeviceManager *manager,HANDLE gpu,UINT width,UINT height,UINT depth) {
  Close();manager_=manager;gpu_=gpu;
  HRESULT hr=manager_->OpenDeviceHandle(&deviceHandle_);if(FAILED(hr))return hr;
  hr=manager_->LockDevice(deviceHandle_,__uuidof(ID3D11Device),(void **)device_.GetAddressOf(),TRUE);if(FAILED(hr))return hr;
  manager_->UnlockDevice(deviceHandle_,FALSE);device_->GetImmediateContext(&context_);
  Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
  if(SUCCEEDED(context_.As(&multithread)))multithread->SetMultithreadProtected(TRUE);
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC desc{};
  hr=device_.As(&dxgi);if(FAILED(hr))return hr;hr=dxgi->GetAdapter(&adapter);if(FAILED(hr))return hr;
  hr=adapter->GetDesc(&desc);if(FAILED(hr))return hr;
  D3DKMT_OPENADAPTERFROMLUID open{};open.AdapterLuid=desc.AdapterLuid;
  hr=Kernel(D3DKMTOpenAdapterFromLuid(&open));if(FAILED(hr))return hr;adapter_=open.hAdapter;
  D3DKMT_CREATEDEVICE create{};create.hAdapter=adapter_;
  hr=Kernel(D3DKMTCreateDevice(&create));if(FAILED(hr))return hr;deviceHandleKernel_=create.hDevice;
  D3D11_TEXTURE2D_DESC td{};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
  td.Format=depth==10?DXGI_FORMAT_P010:DXGI_FORMAT_NV12;td.BindFlags=D3D11_BIND_DECODER|D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
  for(auto &slot:slots_) {
   slot=std::make_shared<Slot>();hr=device_->CreateTexture2D(&td,nullptr,&slot->Texture);if(FAILED(hr))return hr;
   hr=Share(*slot,height);if(FAILED(hr))return hr;
  }
  return S_OK;
 }
 HRESULT Queue() {
  bool pending=false;
  for(auto &slot:slots_)if(slot) {
   if(slot->Pending){pending=true;continue;}
   if(!slot->Free.exchange(false))continue;
   context_->Flush();HRESULT hr=device_->GetDeviceRemovedReason();DWORD used=0;
   if(SUCCEEDED(hr) && !DeviceIoControl(gpu_,GPU_IOCTL_DECODE_SURFACE,&slot->Surface,sizeof(slot->Surface),nullptr,0,&used,nullptr))hr=HRESULT_FROM_WIN32(GetLastError());
   if(FAILED(hr)){slot->Free=true;return hr;}
   slot->Pending=true;pending=true;
  }
  return pending?S_OK:E_PENDING;
 }
 HRESULT Release(ULONGLONG address) {
  for(auto &slot:slots_)if(slot && slot->Pending && slot->Surface.Address==address){slot->Pending=false;slot->Free=true;return S_OK;}
  return E_UNEXPECTED;
 }
 HRESULT Sample(ULONGLONG address,IMFSample **output) {
  std::shared_ptr<Slot> active;
  for(auto &slot:slots_)if(slot && slot->Pending && slot->Surface.Address==address){active=slot;break;}
  if(!active)return E_UNEXPECTED;
  Microsoft::WRL::ComPtr<IMFTrackedSample> tracked;
  HRESULT hr=MFCreateTrackedSample(&tracked);if(FAILED(hr))return hr;
  Microsoft::WRL::ComPtr<IMFSample> sample;hr=tracked.As(&sample);if(FAILED(hr))return hr;
  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  hr=MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),active->Texture.Get(),0,FALSE,&buffer);if(FAILED(hr))return hr;
  hr=sample->AddBuffer(buffer.Get());if(FAILED(hr))return hr;
  auto callback=Microsoft::WRL::Make<Released>(active);if(!callback)return E_OUTOFMEMORY;
  hr=tracked->SetAllocator(callback.Get(),nullptr);if(FAILED(hr))return hr;
  active->Pending=false;return sample.CopyTo(output);
 }
};
