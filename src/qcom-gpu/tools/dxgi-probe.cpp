#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>

static void Probe(IDXGIAdapter *adapter,D3D_DRIVER_TYPE type,const char *mode) {
    D3D_FEATURE_LEVEL request=D3D_FEATURE_LEVEL_10_0,actual={};
    ID3D11Device *device=NULL;
    HRESULT hr=D3D11CreateDevice(adapter,type,NULL,0,&request,1,D3D11_SDK_VERSION,&device,&actual,NULL);
    DXGI_ADAPTER_DESC desc={};
    if(device) {
        IDXGIDevice *dxgi=NULL;
        if(SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),(void **)&dxgi))) {
            IDXGIAdapter *selected=NULL;
            if(SUCCEEDED(dxgi->GetAdapter(&selected))){selected->GetDesc(&desc);selected->Release();}
            dxgi->Release();
        }
        device->Release();
    } else if(adapter)adapter->GetDesc(&desc);
    printf("{\"mode\":\"%s\",\"hr\":\"%08lx\",\"vendor\":\"%04x\",\"device\":\"%04x\","
        "\"luid\":\"%08lx:%08lx\",\"feature_level\":\"%x\"}\n",
        mode,(ULONG)hr,desc.VendorId,desc.DeviceId,(ULONG)desc.AdapterLuid.HighPart,desc.AdapterLuid.LowPart,actual);
}
int main() {
#ifdef _M_X64
    puts("{\"architecture\":\"x64\"}");
#elif defined(_M_ARM64)
    puts("{\"architecture\":\"arm64\"}");
#endif
    Probe(NULL,D3D_DRIVER_TYPE_HARDWARE,"default_hardware");
    IDXGIFactory1 *factory=NULL;
    if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void **)&factory)))return 1;
    for(UINT i=0;;i++) {
        IDXGIAdapter1 *adapter=NULL;
        if(FAILED(factory->EnumAdapters1(i,&adapter)))break;
        DXGI_ADAPTER_DESC1 desc={};
        if(SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))
            Probe(adapter,D3D_DRIVER_TYPE_UNKNOWN,"explicit_hardware");
        adapter->Release();
    }
    factory->Release();return 0;
}
