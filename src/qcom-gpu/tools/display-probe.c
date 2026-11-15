#include <windows.h>
#include <stdio.h>
#include <d3dkmthk.h>

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    D3DKMT_ADAPTERINFO adapters[16]={0};D3DKMT_ENUMADAPTERS2 list={16,adapters};
    NTSTATUS status=D3DKMTEnumAdapters2(&list);
    printf("enum_status=%08lx adapters=%u\n",status,list.NumAdapters);if(status<0)return 1;
    for(UINT i=0;i<list.NumAdapters;i++) {
        ULONG abi=0;D3DKMT_QUERYADAPTERINFO query={adapters[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&abi,sizeof(abi)};
        status=D3DKMTQueryAdapterInfo(&query);if(status<0 || abi!=1)continue;
        printf("adreno_luid=%08lx:%08lx\n",adapters[i].AdapterLuid.HighPart,adapters[i].AdapterLuid.LowPart);
        D3DKMT_CURRENTDISPLAYMODE mode={0};query.Type=KMTQAITYPE_CURRENTDISPLAYMODE;query.pPrivateDriverData=&mode;query.PrivateDriverDataSize=sizeof(mode);
        status=D3DKMTQueryAdapterInfo(&query);
        printf("current_mode_status=%08lx width=%u height=%u refresh=%u\n",status,mode.DisplayMode.Width,mode.DisplayMode.Height,mode.DisplayMode.IntegerRefreshRate);
        D3DKMT_DISPLAYMODE modes[32]={0};D3DKMT_GETDISPLAYMODELIST modesQuery={adapters[i].hAdapter,0,modes,32};
        status=D3DKMTGetDisplayModeList(&modesQuery);printf("mode_list_status=%08lx count=%u\n",status,modesQuery.ModeCount);
        if(status>=0)for(UINT j=0;j<modesQuery.ModeCount && j<32;j++)printf("mode=%ux%u pitch_format=%u refresh=%u\n",modes[j].Width,modes[j].Height,modes[j].Format,modes[j].IntegerRefreshRate);
        D3DKMT_UMDFILENAMEINFO umd={0};umd.Version=KMTUMDVERSION_DX11;
        query.Type=KMTQAITYPE_UMDRIVERNAME;query.pPrivateDriverData=&umd;query.PrivateDriverDataSize=sizeof(umd);
        status=D3DKMTQueryAdapterInfo(&query);printf("umd_name_status=%08lx\n",status);
        if(status>=0)wprintf(L"umd_name=%ls\n",umd.UmdFileName);
    }
    for(UINT i=0;i<list.NumAdapters;i++){D3DKMT_CLOSEADAPTER close={adapters[i].hAdapter};D3DKMTCloseAdapter(&close);}
    UINT paths=0,modes=0;LONG result=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes);
    printf("session_active_topology_status=%ld paths=%u modes=%u\n",result,paths,modes);
    return 0;
}
