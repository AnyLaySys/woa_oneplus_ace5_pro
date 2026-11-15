#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static LONG ConfigRotation(UINT rotation,bool apply,bool transpose){
    UINT paths=0,modes=0;LONG status=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes);if(status!=ERROR_SUCCESS)return status;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    status=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,p.data(),&modes,m.data(),nullptr);if(status!=ERROR_SUCCESS || !paths)return status?status:ERROR_NOT_FOUND;
    UINT index=p[0].sourceInfo.modeInfoIdx;
    if(transpose && ((rotation+1)^p[0].targetInfo.rotation)&1 && index<modes){UINT width=m[index].sourceMode.width;m[index].sourceMode.width=m[index].sourceMode.height;m[index].sourceMode.height=width;}
    p[0].targetInfo.rotation=(DISPLAYCONFIG_ROTATION)(rotation+1);
    return SetDisplayConfig(paths,p.data(),modes,m.data(),SDC_USE_SUPPLIED_DISPLAY_CONFIG|SDC_ALLOW_CHANGES|(apply?SDC_APPLY:SDC_VALIDATE));
}
static void State(){
    DEVMODEW mode={};mode.dmSize=sizeof(mode);
    if(EnumDisplaySettingsExW(nullptr,ENUM_CURRENT_SETTINGS,&mode,0))
        printf("{\"orientation\":%lu,\"width\":%lu,\"height\":%lu,\"frequency\":%lu,\"desktop_width\":%d,\"desktop_height\":%d}\n",mode.dmDisplayOrientation,mode.dmPelsWidth,mode.dmPelsHeight,mode.dmDisplayFrequency,GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
    UINT paths=0,modes=0;LONG status=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes);
    if(status!=ERROR_SUCCESS)return;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(paths);std::vector<DISPLAYCONFIG_MODE_INFO> m(modes);
    status=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,p.data(),&modes,m.data(),nullptr);
    if(status!=ERROR_SUCCESS)return;
    for(UINT i=0;i<paths;i++)printf("{\"path\":%u,\"rotation\":%u,\"source\":%u,\"target\":%u,\"source_mode\":%u,\"target_mode\":%u}\n",i,p[i].targetInfo.rotation,p[i].sourceInfo.id,p[i].targetInfo.id,p[i].sourceInfo.modeInfoIdx,p[i].targetInfo.modeInfoIdx);
}
int main(int argc,char **argv){
    SetProcessDPIAware();setvbuf(stdout,nullptr,_IONBF,0);
    if(argc<2){State();return 0;}
    if(!strcmp(argv[1],"probe")){
        State();DEVMODEW current={};current.dmSize=sizeof(current);if(!EnumDisplaySettingsExW(nullptr,ENUM_CURRENT_SETTINGS,&current,0))return 1;
        for(UINT rotation=0;rotation<4;rotation++)for(UINT swap=0;swap<2;swap++){
            DEVMODEW candidate=current;candidate.dmDisplayOrientation=rotation;
            if(swap){DWORD width=candidate.dmPelsWidth;candidate.dmPelsWidth=candidate.dmPelsHeight;candidate.dmPelsHeight=width;}
            candidate.dmFields|=DM_DISPLAYORIENTATION|DM_PELSWIDTH|DM_PELSHEIGHT;
            LONG result=ChangeDisplaySettingsExW(nullptr,&candidate,nullptr,CDS_TEST,nullptr);
            printf("{\"probe_rotation\":%u,\"width\":%lu,\"height\":%lu,\"result\":%ld}\n",rotation,candidate.dmPelsWidth,candidate.dmPelsHeight,result);
        }
        for(UINT i=0;i<32;i++){DEVMODEW candidate={};candidate.dmSize=sizeof(candidate);if(!EnumDisplaySettingsExW(nullptr,i,&candidate,EDS_ROTATEDMODE))break;printf("{\"mode\":%u,\"rotation\":%lu,\"width\":%lu,\"height\":%lu,\"frequency\":%lu}\n",i,candidate.dmDisplayOrientation,candidate.dmPelsWidth,candidate.dmPelsHeight,candidate.dmDisplayFrequency);}
        for(UINT rotation=0;rotation<4;rotation++)for(UINT transpose=0;transpose<2;transpose++)printf("{\"config_rotation\":%u,\"transpose\":%u,\"result\":%ld}\n",rotation,transpose,ConfigRotation(rotation,false,transpose!=0));
        return 0;
    }
    UINT orientation=strtoul(argv[1],nullptr,10);if(orientation>3)return 2;
    DEVMODEW mode={};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsExW(nullptr,ENUM_CURRENT_SETTINGS,&mode,0))return 1;
    if((orientation^mode.dmDisplayOrientation)&1){DWORD width=mode.dmPelsWidth;mode.dmPelsWidth=mode.dmPelsHeight;mode.dmPelsHeight=width;}
    mode.dmDisplayOrientation=orientation;mode.dmFields=DM_DISPLAYORIENTATION|DM_PELSWIDTH|DM_PELSHEIGHT;
    LONG status=ChangeDisplaySettingsExW(nullptr,&mode,nullptr,CDS_TEST,nullptr);
    printf("{\"requested_orientation\":%u,\"test_status\":%ld}\n",orientation,status);
    if(status!=DISP_CHANGE_SUCCESSFUL){
        LONG test=ConfigRotation(orientation,false,true);printf("{\"config_test_status\":%ld}\n",test);if(test!=ERROR_SUCCESS)return 1;
        if(argc>2 && !strcmp(argv[2],"test")){State();return 0;}
        status=ConfigRotation(orientation,true,true);printf("{\"config_apply_status\":%ld}\n",status);Sleep(1200);State();return status==ERROR_SUCCESS?0:1;
    }
    if(argc>2 && !strcmp(argv[2],"test")){State();return 0;}
    status=ChangeDisplaySettingsExW(nullptr,&mode,nullptr,0,nullptr);
    printf("{\"apply_status\":%ld}\n",status);Sleep(1200);State();return status==DISP_CHANGE_SUCCESSFUL?0:1;
}
