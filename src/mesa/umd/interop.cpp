#include "umd.h"
#include <d3d11.h>

struct PresentImage {
 Device *device;
 ID3D11Texture2D *texture;
 Resource *resource;
 unsigned references;
};
struct ImageCapture { Device *device;Resource *resource; };
static thread_local ImageCapture *current_capture;

/* A one-pixel read during swapchain initialization supplies the DDI source
   identity. The retained native texture owns that resource until CloseImage. */
void capture_present_image(Device *d,Resource *r) {
 if(current_capture && current_capture->device==d && r && r->allocation && r->storage.external)
    current_capture->resource=r;
}
HRESULT WINAPI query_present_image(void *user,A830_IMAGE *out) {
 Device *d=(Device *)user;auto image=(PresentImage *)out->Token;
 if(!image || image->device!=d)return E_INVALIDARG;
 EnterCriticalSection(&d->lock);Resource *r=image->resource;
 HRESULT hr=r->storage.external && r->storage.address && r->storage.size<=UINT_MAX?S_OK:E_INVALIDARG;
 if(SUCCEEDED(hr)){out->Address=r->storage.address;out->Size=(unsigned)r->storage.size;out->Pitch=r->pitch;}
 LeaveCriticalSection(&d->lock);return hr;
}
HRESULT WINAPI open_present_image(void *user,void *resource,A830_IMAGE *out) {
 Device *d=(Device *)user;if(!resource || !out)return E_INVALIDARG;*out={};
 ID3D11Texture2D *texture=nullptr;
 HRESULT hr=((IUnknown *)resource)->QueryInterface(IID_PPV_ARGS(&texture));if(FAILED(hr))return hr;
 EnterCriticalSection(&d->lock);auto it=d->present_images.find(texture);
 if(it!=d->present_images.end()) {
   PresentImage *image=it->second;image->references++;out->Token=image;LeaveCriticalSection(&d->lock);
   texture->Release();hr=query_present_image(user,out);if(FAILED(hr))close_present_image(user,out);return hr;
 }
 LeaveCriticalSection(&d->lock);
 auto image=new(std::nothrow) PresentImage{d,texture,nullptr,1};
 if(!image){texture->Release();return E_OUTOFMEMORY;}
 ID3D11Device *device=nullptr;texture->GetDevice(&device);
 D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
 desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=1;
 desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
 ID3D11Texture2D *probe=nullptr;hr=device->CreateTexture2D(&desc,nullptr,&probe);
 ImageCapture capture{d,nullptr};
 if(SUCCEEDED(hr)) {
   ID3D11DeviceContext *context=nullptr;device->GetImmediateContext(&context);
   /* Nonzero backbuffer interfaces permit reads but reject RTV creation.
      Flush older commands before capturing this one initialization read. */
   context->Flush();ImageCapture *saved=current_capture;current_capture=&capture;
   const D3D11_BOX pixel={0,0,0,1,1,1};
   context->CopySubresourceRegion(probe,0,0,0,0,texture,0,&pixel);context->Flush();
   current_capture=saved;hr=device->GetDeviceRemovedReason();context->Release();probe->Release();
 }
 device->Release();image->resource=capture.resource;
 if(FAILED(hr) || !image->resource) {
   texture->Release();delete image;
   return FAILED(hr)?hr:E_NOINTERFACE;
 }
 EnterCriticalSection(&d->lock);d->present_images.emplace(texture,image);LeaveCriticalSection(&d->lock);
 out->Token=image;hr=query_present_image(user,out);
 if(FAILED(hr))close_present_image(user,out);return hr;
}
void WINAPI close_present_image(void *user,A830_IMAGE *out) {
 Device *d=(Device *)user;auto image=(PresentImage *)out->Token;if(!image || image->device!=d)return;
 EnterCriticalSection(&d->lock);bool last=!--image->references;
 if(last)d->present_images.erase(image->texture);LeaveCriticalSection(&d->lock);*out={};
 if(last){image->texture->Release();delete image;}
}
