#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include "../wddm/abi.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { HRESULT hr=(x); if(FAILED(hr)){printf("{\"failure\":\"%s\",\"hr\":\"%08lx\",\"line\":%d}\n",#x,(unsigned long)hr,__LINE__);return 1;} } while(0)
#include "gpu-bench.h"
#include "d3d-dynamic-test.h"
#include "d3d-shared-sampler-test.h"
#include "d3d-image-reuse-test.h"
static int PresentTest(ID3D11Device *device, ID3D11DeviceContext *context,bool fullscreen) {
    SetProcessDPIAware();
    unsigned width=fullscreen?GetSystemMetrics(SM_CXSCREEN):512,height=fullscreen?GetSystemMetrics(SM_CYSCREEN):512;
    WNDCLASSW windowClass={};windowClass.lpfnWndProc=DefWindowProcW;
    windowClass.hInstance=GetModuleHandleW(NULL);windowClass.lpszClassName=L"A830D3DTest";
    if(!RegisterClassW(&windowClass))return 1;
    HWND window=CreateWindowExW(WS_EX_TOPMOST,windowClass.lpszClassName,L"Adreno 830 Direct3D GPU test",
        (fullscreen?WS_POPUP:WS_OVERLAPPEDWINDOW)|WS_VISIBLE,fullscreen?0:80,fullscreen?0:160,
        fullscreen?width:640,fullscreen?height:720,NULL,NULL,windowClass.hInstance,NULL);
    if(!window)return 1;
    STARTUPINFOW startup={};startup.cb=sizeof(startup);GetStartupInfoW(&startup);
    // The first ShowWindow can inherit the launcher's hidden startup state.
    // The second call explicitly shows this visual diagnostic window.
    ShowWindow(window,SW_SHOW);ShowWindow(window,SW_SHOW);UpdateWindow(window);
    SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    printf("{\"window_visible\":%s,\"window_iconic\":%s,\"startup_flags\":%lu,\"startup_show\":%u}\n",
        IsWindowVisible(window)?"true":"false",IsIconic(window)?"true":"false",startup.dwFlags,startup.wShowWindow);
    IDXGIDevice *dxgiDevice=NULL;IDXGIAdapter *adapter=NULL;IDXGIFactory *factory=NULL;
    CHECK(device->QueryInterface(__uuidof(IDXGIDevice),(void **)&dxgiDevice));
    CHECK(dxgiDevice->GetAdapter(&adapter));CHECK(adapter->GetParent(__uuidof(IDXGIFactory),(void **)&factory));
    DXGI_SWAP_CHAIN_DESC desc={};desc.BufferDesc.Width=width;desc.BufferDesc.Height=height;
    desc.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=1;desc.OutputWindow=window;
    desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *swapchain=NULL;CHECK(factory->CreateSwapChain(device,&desc,&swapchain));
    BOOL isFullscreen=FALSE;CHECK(swapchain->GetFullscreenState(&isFullscreen,NULL));printf("{\"swapchain_fullscreen\":%s}\n",isFullscreen?"true":"false");
    ID3D11Texture2D *backbuffer=NULL;CHECK(swapchain->GetBuffer(0,__uuidof(ID3D11Texture2D),(void **)&backbuffer));
    ID3D11RenderTargetView *target=NULL;CHECK(device->CreateRenderTargetView(backbuffer,NULL,&target));
    context->OMSetRenderTargets(1,&target,NULL);
    D3D11_VIEWPORT viewport={0,0,(float)width,(float)height,0,1};context->RSSetViewports(1,&viewport);
    unsigned frames=0;ULONGLONG deadline=GetTickCount64()+15000;
    while(GetTickCount64()<deadline && IsWindow(window)) {
        MSG message;while(PeekMessageW(&message,NULL,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        const float clear[]={0.02f,0.06f,0.15f,1};context->ClearRenderTargetView(target,clear);
        context->Draw(3,0);ULONGLONG presentBegin=GetTickCount64();HRESULT presented=swapchain->Present(0,0);
        if(frames<4 || FAILED(presented))printf("{\"present_index\":%u,\"present_ms\":%llu,\"present_hr\":\"%08lx\",\"visible\":%s,\"iconic\":%s}\n",frames,GetTickCount64()-presentBegin,(unsigned long)presented,IsWindowVisible(window)?"true":"false",IsIconic(window)?"true":"false");
        if(FAILED(presented)){printf("{\"failure\":\"swapchain->Present\",\"hr\":\"%08lx\",\"removed_reason\":\"%08lx\",\"frames\":%u}\n",(unsigned long)presented,(unsigned long)device->GetDeviceRemovedReason(),frames);return 1;}
        frames++;Sleep(33);
    }
    printf("{\"presented_frames\":%u,\"width\":%u,\"height\":%u,\"window_present\":\"PASS\"}\n",frames,width,height);
    context->OMSetRenderTargets(0,NULL,NULL);target->Release();backbuffer->Release();swapchain->Release();
    factory->Release();adapter->Release();dxgiDevice->Release();if(IsWindow(window))DestroyWindow(window);
    return frames?0:1;
}
static unsigned ReadShared(ID3D11Device *device,ID3D11DeviceContext *context,ID3D11Texture2D *image) {
    D3D11_TEXTURE2D_DESC desc={};image->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;
    desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    ID3D11Texture2D *staging=NULL;if(FAILED(device->CreateTexture2D(&desc,NULL,&staging)))return 0;
    context->CopyResource(staging,image);D3D11_MAPPED_SUBRESOURCE map={};
    if(FAILED(context->Map(staging,0,D3D11_MAP_READ,0,&map))){staging->Release();return 0;}
    unsigned value=*(unsigned *)((BYTE *)map.pData+map.RowPitch*32+32*4);context->Unmap(staging,0);staging->Release();return value;
}
static int SharedChild(ID3D11Device *device,ID3D11DeviceContext *context,HANDLE handle) {
    ID3D11Texture2D *opened=NULL;CHECK(device->OpenSharedResource(handle,__uuidof(ID3D11Texture2D),(void **)&opened));
    D3D11_TEXTURE2D_DESC desc={};opened->GetDesc(&desc);unsigned expected=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM?0xffff0000:0xff0000ff;
    unsigned value=ReadShared(device,context,opened);
    ID3D11RenderTargetView *view=NULL;CHECK(device->CreateRenderTargetView(opened,NULL,&view));
    float green[]={0,1,0,1};context->ClearRenderTargetView(view,green);context->Flush();CHECK(device->GetDeviceRemovedReason());
    view->Release();opened->Release();
    printf("{\"child_shared_center\":\"%08x\"}\n",value);return value==expected?0:1;
}
static int SharedTest(ID3D11Device *producer,ID3D11DeviceContext *producerContext,ID3D11Texture2D *image) {
    IDXGIResource *resource=NULL;CHECK(image->QueryInterface(__uuidof(IDXGIResource),(void **)&resource));
    HANDLE handle=NULL;producerContext->Flush();CHECK(producer->GetDeviceRemovedReason());CHECK(resource->GetSharedHandle(&handle));resource->Release();
    IDXGIDevice *dxgi=NULL;IDXGIAdapter *adapter=NULL;CHECK(producer->QueryInterface(__uuidof(IDXGIDevice),(void **)&dxgi));
    CHECK(dxgi->GetAdapter(&adapter));dxgi->Release();
    ID3D11Device *consumer=NULL;ID3D11DeviceContext *context=NULL;D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_10_0,actual;
    CHECK(D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,NULL,0,&requested,1,D3D11_SDK_VERSION,&consumer,&actual,&context));adapter->Release();
    ID3D11Texture2D *opened=NULL;CHECK(consumer->OpenSharedResource(handle,__uuidof(ID3D11Texture2D),(void **)&opened));
    D3D11_TEXTURE2D_DESC desc={};opened->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.BindFlags=desc.MiscFlags=0;
    unsigned expected=desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM?0xffff0000:0xff0000ff;
    ID3D11Texture2D *staging=NULL;CHECK(consumer->CreateTexture2D(&desc,NULL,&staging));
    context->CopyResource(staging,opened);D3D11_MAPPED_SUBRESOURCE map={};CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&map));
    unsigned center=*(unsigned *)((BYTE *)map.pData+map.RowPitch*32+32*4);context->Unmap(staging,0);
    printf("{\"shared_center\":\"%08x\",\"shared_texture\":\"%s\"}\n",center,center==expected?"PASS":"FAIL");
    staging->Release();opened->Release();context->Release();consumer->Release();
    if(center!=expected)return 1;
    WCHAR executable[MAX_PATH],command[MAX_PATH+80];if(!GetModuleFileNameW(NULL,executable,MAX_PATH))return 1;
    if(swprintf_s(command,L"\"%s\" shared-child %llu",executable,(unsigned long long)(uintptr_t)handle)<0)return 1;
    STARTUPINFOW startup={};startup.cb=sizeof(startup);PROCESS_INFORMATION process={};
    if(!CreateProcessW(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process))return 1;
    DWORD exitCode=1;if(WaitForSingleObject(process.hProcess,15000)==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&exitCode);
    else TerminateProcess(process.hProcess,1);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
    unsigned updated=ReadShared(producer,producerContext,image);
    printf("{\"shared_child_exit\":%lu,\"shared_updated\":\"%08x\",\"cross_process\":\"%s\"}\n",exitCode,updated,!exitCode && updated==0xff00ff00?"PASS":"FAIL");
    return !exitCode && updated==0xff00ff00?0:1;
}
int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    SetEnvironmentVariableA("A830_NATIVE_EXPERIMENTAL","true");
    bool samplerChild=argc>2 && (!strcmp(argv[1],"shared-sampler-child") || !strcmp(argv[1],"shared-sampler-child-wait"));
    bool child=samplerChild || (argc>2 && !strcmp(argv[1],"shared-child"));
#ifdef A830_BENCHMARK
    bool benchmark=true;
#else
    bool benchmark=argc>1 && !strcmp(argv[1],"native-bench");
#endif
    bool native=benchmark || child || (argc>1 && strstr(argv[1],"native")==argv[1]);
    HMODULE driver=native?NULL:LoadLibraryW(L"a830_umd.dll");
    if(!native && !driver){printf("{\"load_error\":%lu}\n",GetLastError());return 1;}
    IDXGIAdapter1 *selected=NULL;
    if(native) {
        IDXGIFactory1 *factory=NULL;CHECK(CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void **)&factory));
        for(UINT index=0;;index++) {
            IDXGIAdapter1 *adapter=NULL;if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
            DXGI_ADAPTER_DESC1 desc={};CHECK(adapter->GetDesc1(&desc));
            D3DKMT_OPENADAPTERFROMLUID open={};open.AdapterLuid=desc.AdapterLuid;
            if(D3DKMTOpenAdapterFromLuid(&open)>=0) {
                A830_ADAPTER_INFO info={};D3DKMT_QUERYADAPTERINFO query={open.hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&info,sizeof(info)};
                NTSTATUS status=D3DKMTQueryAdapterInfo(&query);
                D3DKMT_CLOSEADAPTER close={open.hAdapter};D3DKMTCloseAdapter(&close);
                if(status>=0 && info.Version==A830_ABI_VERSION && info.Chip==0x44050001 && (info.Flags&1) && !info.Status)selected=adapter;
            }
            if(selected)break;adapter->Release();
        }
        factory->Release();
        if(!selected){printf("{\"native_adapter\":\"NOT_FOUND\"}\n");return 1;}
    }
    ID3D11Device *device=NULL;ID3D11DeviceContext *context=NULL;
    D3D_FEATURE_LEVEL requested=D3D_FEATURE_LEVEL_10_0,actual;
    CHECK(D3D11CreateDevice(selected,native?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_SOFTWARE,driver,0,&requested,1,D3D11_SDK_VERSION,&device,&actual,&context));
    if(selected)selected->Release();
    printf("{\"feature_level\":\"%x\",\"native_adapter\":%s}\n",actual,native?"true":"false");
    if(samplerChild){int result=SharedSamplerChild(device,context,(HANDLE)(uintptr_t)_strtoui64(argv[2],NULL,10),strstr(argv[1],"wait")!=NULL);context->Release();device->Release();return result;}
    if(argc>2 && !strcmp(argv[1],"native-shared-sampler-mask")){unsigned mask=(unsigned)strtoul(argv[2],NULL,0);if(mask>63)return 2;int result=SharedSamplerTest(device,context,false,true,mask);context->Release();device->Release();return result;}
    if(argc>1 && (!strcmp(argv[1],"native-shared-sampler") || !strcmp(argv[1],"native-shared-sampler-wait") || !strcmp(argv[1],"native-shared-sampler-full") || !strcmp(argv[1],"native-shared-sampler-rebind") || !strcmp(argv[1],"native-shared-sampler-full-rebind"))){int result=SharedSamplerTest(device,context,strstr(argv[1],"wait")!=NULL,strstr(argv[1],"full")!=NULL,strstr(argv[1],"rebind")!=NULL?63u:0u);context->Release();device->Release();return result;}
    if(argc>1 && strstr(argv[1],"dynamic")){int result=DynamicUpdates(device,context);context->Release();device->Release();return result;}
    if(argc>1 && strstr(argv[1],"image-reuse")){int result=ImageReuse(device,context);context->Release();device->Release();return result;}
    if(benchmark){int result=Benchmark(device,context);context->Release();device->Release();return result;}
    if(child){int result=SharedChild(device,context,(HANDLE)(uintptr_t)_strtoui64(argv[2],NULL,10));context->Release();device->Release();return result;}
    D3D11_TEXTURE2D_DESC desc={};desc.Width=64;desc.Height=64;desc.MipLevels=1;desc.ArraySize=1;
    bool bgra=argc>1 && strstr(argv[1],"bgra");unsigned expectedRed=bgra?0xffff0000:0xff0000ff;
    desc.Format=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    bool shared=argc>1 && strstr(argv[1],"shared");if(shared)desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *image=NULL,*staging=NULL;CHECK(device->CreateTexture2D(&desc,NULL,&image));
    ID3D11RenderTargetView *view=NULL;CHECK(device->CreateRenderTargetView(image,NULL,&view));
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc,NULL,&staging));
    bool procedural=argc>1 && strstr(argv[1],"procedural");
    bool textured=argc>1 && strstr(argv[1],"texture");
    const char *source=textured?
        "Texture2D<float4> tex:register(t1);SamplerState samp:register(s3);float4 vs(float4 p:POSITION):SV_Position{return p;} float4 ps():SV_Target{return tex.Sample(samp,float2(.5,.5));}":procedural?
        "float4 vs(uint id:SV_VertexID):SV_Position {float2 p[3]={float2(-1,-1),float2(0,1),float2(1,-1)};return float4(p[id],0,1);} float4 ps():SV_Target {return float4(1,0,0,1);}":
        "float4 vs(float4 p:POSITION):SV_Position {return p;} float4 ps():SV_Target {return float4(1,0,0,1);}";
    ID3DBlob *vs=NULL,*ps=NULL,*errors=NULL;
    CHECK(D3DCompile(source,strlen(source),NULL,NULL,NULL,"vs","vs_4_0",0,0,&vs,&errors));
    CHECK(D3DCompile(source,strlen(source),NULL,NULL,NULL,"ps","ps_4_0",0,0,&ps,&errors));
    ID3D11VertexShader *vertex=NULL;ID3D11PixelShader *pixel=NULL;
    CHECK(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),NULL,&vertex));
    CHECK(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),NULL,&pixel));
    ID3D11Texture2D *sampleTexture=NULL;ID3D11ShaderResourceView *sampleView=NULL;ID3D11SamplerState *sampleState=NULL;
    if(textured) {
        D3D11_TEXTURE2D_DESC sampleDesc={};sampleDesc.Width=2;sampleDesc.Height=2;sampleDesc.MipLevels=1;sampleDesc.ArraySize=1;
        sampleDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sampleDesc.SampleDesc.Count=1;
        sampleDesc.Usage=D3D11_USAGE_IMMUTABLE;sampleDesc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const unsigned pixels[]={0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff};
        D3D11_SUBRESOURCE_DATA data={pixels,8,16};CHECK(device->CreateTexture2D(&sampleDesc,&data,&sampleTexture));
        CHECK(device->CreateShaderResourceView(sampleTexture,NULL,&sampleView));
        D3D11_SAMPLER_DESC sampler={};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;sampler.MaxLOD=D3D11_FLOAT32_MAX;
        CHECK(device->CreateSamplerState(&sampler,&sampleState));
        context->PSSetShaderResources(1,1,&sampleView);context->PSSetSamplers(3,1,&sampleState);
    }
    const float vertices[]={-1,-1,0,1,0,1,0,1,1,-1,0,1};
    D3D11_BUFFER_DESC bufferDesc={};bufferDesc.ByteWidth=sizeof(vertices);bufferDesc.Usage=D3D11_USAGE_IMMUTABLE;bufferDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial={};initial.pSysMem=vertices;
    ID3D11Buffer *buffer=NULL;CHECK(device->CreateBuffer(&bufferDesc,&initial,&buffer));
    D3D11_INPUT_ELEMENT_DESC element={"POSITION",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    ID3D11InputLayout *layout=NULL;if(!procedural)CHECK(device->CreateInputLayout(&element,1,vs->GetBufferPointer(),vs->GetBufferSize(),&layout));
    context->IASetInputLayout(layout);UINT stride=16,offset=0;context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
    D3D11_VIEWPORT viewport={0,0,64,64,0,1};context->RSSetViewports(1,&viewport);
    D3D11_RASTERIZER_DESC raster={};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    ID3D11RasterizerState *state=NULL;CHECK(device->CreateRasterizerState(&raster,&state));context->RSSetState(state);
    context->OMSetRenderTargets(1,&view,NULL);float clear[]={0,0,0,1};context->ClearRenderTargetView(view,clear);
    D3D11_BLEND_DESC blendDesc={};blendDesc.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *blend=NULL;CHECK(device->CreateBlendState(&blendDesc,&blend));context->OMSetBlendState(blend,NULL,0xffffffff);
    D3D11_DEPTH_STENCIL_DESC depthDesc={};depthDesc.DepthEnable=FALSE;depthDesc.DepthFunc=D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState *depth=NULL;CHECK(device->CreateDepthStencilState(&depthDesc,&depth));context->OMSetDepthStencilState(depth,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);context->VSSetShader(vertex,NULL,0);context->PSSetShader(pixel,NULL,0);
    D3D11_QUERY_DESC queryDesc={D3D11_QUERY_OCCLUSION,0};ID3D11Query *query=NULL;
    CHECK(device->CreateQuery(&queryDesc,&query));context->Begin(query);
    context->Draw(3,0);context->End(query);context->CopyResource(staging,image);
    D3D11_MAPPED_SUBRESOURCE map={};CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&map));
    unsigned center=*(unsigned *)((BYTE *)map.pData+map.RowPitch*32+32*4),corner=*(unsigned *)map.pData;
    unsigned red=0;for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)red+=*(unsigned *)((BYTE *)map.pData+map.RowPitch*y+x*4)==expectedRed;
    context->Unmap(staging,0);UINT64 samples=0;HRESULT queryResult=S_FALSE;ULONGLONG deadline=GetTickCount64()+10000;
    while(queryResult==S_FALSE && GetTickCount64()<deadline){queryResult=context->GetData(query,&samples,sizeof(samples),0);if(queryResult==S_FALSE)Sleep(1);}
    bool passed=center==expectedRed && corner==0xff000000 && red==2048 && samples==2048 && queryResult==S_OK;
    printf("{\"center\":\"%08x\",\"corner\":\"%08x\",\"red_pixels\":%u,\"samples\":%llu,\"query_hr\":\"%08lx\",\"triangle\":\"%s\"}\n",center,corner,red,samples,(unsigned long)queryResult,passed?"PASS":"FAIL");
    if(passed && shared)passed=SharedTest(device,context,image)==0;
    if(passed && argc>1 && strstr(argv[1],"stress")) {
        unsigned frames=0;ULONGLONG stop=GetTickCount64()+15000;
        while(GetTickCount64()<stop) {
            context->Draw(3,0);context->CopyResource(staging,image);
            CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&map));
            if(*(unsigned *)((BYTE *)map.pData+map.RowPitch*32+32*4)!=expectedRed)passed=false;
            context->Unmap(staging,0);frames++;if(!passed)break;
        }
        printf("{\"stress_frames\":%u,\"result\":\"%s\"}\n",frames,passed?"PASS":"FAIL");
    }
    if(passed && argc>1 && strstr(argv[1],"present"))passed=PresentTest(device,context,strstr(argv[1],"fullscreen")!=NULL)==0;
    context->ClearState();context->Flush();state->Release();vertex->Release();pixel->Release();vs->Release();ps->Release();
    if(sampleState)sampleState->Release();if(sampleView)sampleView->Release();if(sampleTexture)sampleTexture->Release();
    query->Release();blend->Release();depth->Release();buffer->Release();if(layout)layout->Release();view->Release();image->Release();staging->Release();context->Release();device->Release();if(driver)FreeLibrary(driver);
    return passed?0:1;
}
