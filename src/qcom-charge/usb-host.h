#pragma once
#include <cfgmgr32.h>
#include <cfg.h>
// Only this board's USB root hub is paused; the xHCI controller and PMIC remain running.
static DEVINST UsbHub;
static HKEY UsbOwner;
static ULONGLONG UsbStable,UsbOffline,UsbRetry;
static BOOL UsbPaused;
static BOOL UsbRoot(DEVINST Node){
    DEVINST Parent;WCHAR Id[MAX_DEVICE_ID_LEN];
    return CM_Get_Parent(&Parent,Node,0)==CR_SUCCESS && CM_Get_Device_IDW(Parent,Id,ARRAYSIZE(Id),0)==CR_SUCCESS &&
        !_wcsicmp(Id,L"ACPI\\PNP0D10\\0") && CM_Get_Device_IDW(Node,Id,ARRAYSIZE(Id),0)==CR_SUCCESS && !wcsncmp(Id,L"USB\\ROOT_HUB30\\",15);
}
static void UsbInit(void){
    WCHAR Id[MAX_DEVICE_ID_LEN];DWORD Size=sizeof(Id),Type;ULONG State,Problem;DEVINST Controller;
    if(!UsbOwner && RegCreateKeyExW(HKEY_LOCAL_MACHINE,L"SYSTEM\\CurrentControlSet\\Services\\ALSCharge\\Runtime",0,0,REG_OPTION_VOLATILE,KEY_QUERY_VALUE|KEY_SET_VALUE,0,&UsbOwner,0)!=ERROR_SUCCESS)return;
    if(RegQueryValueExW(UsbOwner,L"PausedHub",0,&Type,(BYTE*)Id,&Size)==ERROR_SUCCESS && Type==REG_SZ && Size>=sizeof(WCHAR) && Size<=sizeof(Id) && !(Size%sizeof(WCHAR)) && Id[Size/sizeof(WCHAR)-1]==0 &&
       CM_Locate_DevNodeW(&UsbHub,Id,0)==CR_SUCCESS && UsbRoot(UsbHub) && CM_Get_DevNode_Status(&State,&Problem,UsbHub,0)==CR_SUCCESS && Problem==CM_PROB_DISABLED){UsbPaused=TRUE;return;}
    RegDeleteValueW(UsbOwner,L"PausedHub");UsbHub=0;
    if(CM_Locate_DevNodeW(&Controller,L"ACPI\\PNP0D10\\0",0)==CR_SUCCESS && CM_Get_Child(&UsbHub,Controller,0)==CR_SUCCESS && UsbRoot(UsbHub))return;
    UsbHub=0;
}
static void UsbResume(void){
    if(UsbPaused && CM_Enable_DevNode(UsbHub,0)==CR_SUCCESS){UsbPaused=FALSE;RegDeleteValueW(UsbOwner,L"PausedHub");}
}
static BOOL UsbOnlyFailedDevice(void){
    DEVINST Child,Next;WCHAR Id[MAX_DEVICE_ID_LEN];BOOL Found=FALSE;
    if(!UsbHub || CM_Get_Child(&Child,UsbHub,0)!=CR_SUCCESS)return FALSE;
    do{
        if(CM_Get_Device_IDW(Child,Id,ARRAYSIZE(Id),0)!=CR_SUCCESS || wcsncmp(Id,L"USB\\VID_0000&PID_0002\\",22))return FALSE;
        Found=TRUE;
        if(CM_Get_Sibling(&Next,Child,0)!=CR_SUCCESS)break;
        Child=Next;
    }while(TRUE);
    return Found;
}
static void UsbPoll(const ALS_CHARGE_STATUS_DATA *S,ULONGLONG Now){
    if(!UsbOwner||Now<UsbRetry)return;
    if(UsbPaused){
        // Fresh detach is required. Stale telemetry, cooling and normal charge stops never reconnect the host hub.
        BOOL Detached=S->Bridge==0?S->PortAge<=2500&&!S->PortOnline&&S->PortMv<1000:
            S->OnlineAge<=2500&&S->VoltageAge<=2500&&!S->Online&&S->UsbMv<1000;
        if(S->Bridge==2 && S->CableAge<=2500 && S->CableOrientation<=2)Detached=Detached&&!S->CableOrientation;
        else if(S->InputState)Detached=FALSE;
        if(!Detached){UsbOffline=0;return;}
        if(!UsbOffline)UsbOffline=Now;
        if(Now-UsbOffline>=500){UsbResume();UsbRetry=Now+1000;UsbStable=0;}
        return;
    }
    UsbOffline=0;
    BOOL Charger=S->Bridge==2 && S->OnlineAge<=2500 && S->VoltageAge<=2500 && S->RoleAge<=2500 && S->SourceAge<=2500 &&
        S->Online && S->Adapter==2 && !S->SourceEnabled && !S->SourceRole && S->UsbMv>=4500 && S->UsbMv<=11200;
    // Handle a hot/low-battery attachment before charging can acquire the PMIC channel.
    if(!Charger && S->Bridge==0 && S->PortAge<=2500 && S->PortOnline && !S->PortRole && S->PortMv>=4500 && S->PortMv<=5500)Charger=UsbOnlyFailedDevice();
    if(!Charger){UsbStable=0;return;}
    if(!UsbStable)UsbStable=Now;
    if(Now-UsbStable<500)return;
    if(!UsbHub){UsbInit();if(!UsbHub)return;}
    ULONG State,Problem;WCHAR Id[MAX_DEVICE_ID_LEN];
    if(!UsbRoot(UsbHub) || CM_Get_DevNode_Status(&State,&Problem,UsbHub,0)!=CR_SUCCESS || Problem || !(State&DN_DISABLEABLE))return;
    if(CM_Get_Device_IDW(UsbHub,Id,ARRAYSIZE(Id),0)!=CR_SUCCESS)return;
    // Record ownership before the nonpersistent pause, so a service crash can recover it.
    if(RegSetValueExW(UsbOwner,L"PausedHub",0,REG_SZ,(BYTE*)Id,(DWORD)((wcslen(Id)+1)*sizeof(WCHAR)))!=ERROR_SUCCESS)return;
    if(CM_Disable_DevNode(UsbHub,0)==CR_SUCCESS)UsbPaused=TRUE;
    else RegDeleteValueW(UsbOwner,L"PausedHub");
    UsbRetry=Now+1000;
}
static void UsbFinish(void){UsbResume();if(UsbOwner){RegCloseKey(UsbOwner);UsbOwner=0;}}
