static const GUID dxgi_vk_factory_id={0x4c5e1b0d,0xb0c8,0x4131,{0xbf,0xd8,0x9b,0x24,0x76,0xf7,0xf4,0x08}};
static const GUID dxgi_vk_swapchain_id={0xe7d6c3ca,0x23a0,0x4e08,{0x9f,0x2f,0xea,0x52,0x31,0xdf,0x66,0x33}};
static IDXGIFactory2 *dxgi_vk_factory;
static pthread_once_t dxgi_vk_once=PTHREAD_ONCE_INIT;
static HMODULE load_d3d12_sibling(const char *file)
{
    char path[MAX_PATH],*name;HMODULE owner;
    if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)load_d3d12_sibling,&owner))return NULL;
    if(!GetModuleFileNameA(owner,path,sizeof(path)))return NULL;
    name=strrchr(path,'\\');if(!name || strlen(file)>=sizeof(path)-(size_t)(name+1-path))return NULL;
    strcpy(name+1,file);
    return LoadLibraryExA(path,NULL,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
}
static void load_dxgi_vk(void)
{
    typedef HRESULT (WINAPI *CREATE)(UINT,REFIID,void **);
    HMODULE module;CREATE create;
#ifdef _M_ARM64
    module=load_d3d12_sibling("qcom_dxgi_arm64.dll");
#else
    module=load_d3d12_sibling("qcom_dxgi_x64.dll");
#endif
    if(!module)return;
    create=(CREATE)GetProcAddress(module,"CreateDXGIFactory2");
    if(create)create(0,&IID_IDXGIFactory2,(void **)&dxgi_vk_factory);
}
static pthread_once_t d3d12_system_once=PTHREAD_ONCE_INIT;
static HRESULT (WINAPI *d3d12_system_create)(IUnknown *,D3D_FEATURE_LEVEL,REFIID,void **);
static void load_d3d12_system(void)
{
    HMODULE module=load_d3d12_sibling("qcom_d3d12_system.dll");
    if(module)d3d12_system_create=(void *)GetProcAddress(module,"D3D12CreateDevice");
}
static bool d3d12_software_adapter(IUnknown *adapter)
{
    IDXGIAdapter1 *dxgi;DXGI_ADAPTER_DESC1 desc;HRESULT hr;
    if(!adapter || FAILED(IUnknown_QueryInterface(adapter,&IID_IDXGIAdapter1,(void **)&dxgi)))return false;
    hr=IDXGIAdapter1_GetDesc1(dxgi,&desc);IDXGIAdapter1_Release(dxgi);
    return SUCCEEDED(hr) && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
}
static IDXGIFactory2 *select_dxgi(IDXGIFactory2 *factory,IUnknown *device)
{
    IUnknown *interop;
    if(!device || FAILED(IUnknown_QueryInterface(device,&dxgi_vk_swapchain_id,(void **)&interop)))return factory;
    IUnknown_Release(interop);pthread_once(&dxgi_vk_once,load_dxgi_vk);return dxgi_vk_factory;
}
#define DXGI_HOOK(name,slot,params,args) \
    typedef HRESULT (WINAPI *name##_fn) params; \
    static name##_fn name##_native; \
    static HRESULT WINAPI name params { \
        IDXGIFactory2 *selected=select_dxgi(factory,device); \
        if(!selected)return E_FAIL; \
        if(selected==factory)return name##_native args; \
        factory=selected;return ((name##_fn)((void **)factory->lpVtbl)[slot]) args; \
    }
DXGI_HOOK(dxgi_swapchain,10,
    (IDXGIFactory2 *factory,IUnknown *device,DXGI_SWAP_CHAIN_DESC *desc,IDXGISwapChain **swapchain),
    (factory,device,desc,swapchain))
DXGI_HOOK(dxgi_hwnd,15,
    (IDXGIFactory2 *factory,IUnknown *device,HWND window,const DXGI_SWAP_CHAIN_DESC1 *desc,const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen,IDXGIOutput *output,IDXGISwapChain1 **swapchain),
    (factory,device,window,desc,fullscreen,output,swapchain))
DXGI_HOOK(dxgi_corewindow,16,
    (IDXGIFactory2 *factory,IUnknown *device,IUnknown *window,const DXGI_SWAP_CHAIN_DESC1 *desc,IDXGIOutput *output,IDXGISwapChain1 **swapchain),
    (factory,device,window,desc,output,swapchain))
DXGI_HOOK(dxgi_composition,24,
    (IDXGIFactory2 *factory,IUnknown *device,const DXGI_SWAP_CHAIN_DESC1 *desc,IDXGIOutput *output,IDXGISwapChain1 **swapchain),
    (factory,device,desc,output,swapchain))
#undef DXGI_HOOK
static void install_dxgi(void)
{
    IDXGIFactory2 *factory;IUnknown *interop;HMODULE owner;DWORD protection;void **table;
    if(FAILED(CreateDXGIFactory1(&IID_IDXGIFactory2,(void **)&factory)))return;
    if(SUCCEEDED(IDXGIFactory2_QueryInterface(factory,&dxgi_vk_factory_id,(void **)&interop))){IUnknown_Release(interop);IDXGIFactory2_Release(factory);return;}
    table=(void **)factory->lpVtbl;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCSTR)install_dxgi,&owner) &&
       VirtualProtect(table+10,15*sizeof(void *),PAGE_READWRITE,&protection)) {
        dxgi_swapchain_native=(dxgi_swapchain_fn)table[10];dxgi_hwnd_native=(dxgi_hwnd_fn)table[15];
        dxgi_corewindow_native=(dxgi_corewindow_fn)table[16];dxgi_composition_native=(dxgi_composition_fn)table[24];
        table[10]=dxgi_swapchain;table[15]=dxgi_hwnd;table[16]=dxgi_corewindow;table[24]=dxgi_composition;
        VirtualProtect(table+10,15*sizeof(void *),protection,&protection);
    }
    IDXGIFactory2_Release(factory);
}
