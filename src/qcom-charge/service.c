#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#include "..\qcom-audio\charge-api.h"
#include "policy.h"
#include "usb-host.h"
static SERVICE_STATUS_HANDLE Service;
static SERVICE_STATUS Status;
static HANDLE Stop;
static void Report(DWORD State,DWORD Error){Status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;Status.dwCurrentState=State;Status.dwControlsAccepted=State==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;Status.dwWin32ExitCode=Error;Status.dwWaitHint=State==SERVICE_STOP_PENDING?10000:0;SetServiceStatus(Service,&Status);}
static void WINAPI Control(DWORD Code){if(Code==SERVICE_CONTROL_STOP||Code==SERVICE_CONTROL_SHUTDOWN){Report(SERVICE_STOP_PENDING,0);SetEvent(Stop);}else if(Code==SERVICE_CONTROL_INTERROGATE)SetServiceStatus(Service,&Status);}
static BOOL ReadStatus(HANDLE H,ALS_CHARGE_STATUS_DATA *S){DWORD N=0;ZeroMemory(S,sizeof(*S));return DeviceIoControl(H,ALS_CHARGE_STATUS,0,0,S,sizeof(*S),&N,0)&&N==sizeof(*S)&&S->Abi==ALS_CHARGE_ABI;}
static BOOL Command(HANDLE H,ULONG A){DWORD N=0;return DeviceIoControl(H,ALS_CHARGE_COMMAND,&A,sizeof(A),0,0,&N,0);}
static BOOL Key(HANDLE H){
    WCHAR Path[MAX_PATH];DWORD N=GetModuleFileNameW(0,Path,ARRAYSIZE(Path));
    if(!N||N>=ARRAYSIZE(Path))return FALSE;
    WCHAR *Name=wcsrchr(Path,L'\\');
    if(!Name||wcscpy_s(Name+1,ARRAYSIZE(Path)-(size_t)(Name+1-Path),L"credential.bin"))return FALSE;
    HANDLE F=CreateFileW(Path,GENERIC_READ,FILE_SHARE_READ,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if(F==INVALID_HANDLE_VALUE)return FALSE;
    BYTE Secret[16]={0};N=0;BOOL Ok=FALSE;
    if(GetFileSize(F,0)==sizeof(Secret)&&ReadFile(F,Secret,sizeof(Secret),&N,0)&&N==sizeof(Secret))
        Ok=DeviceIoControl(H,ALS_CHARGE_KEY,Secret,sizeof(Secret),0,0,&N,0);
    SecureZeroMemory(Secret,sizeof(Secret));CloseHandle(F);return Ok;
}
static void WINAPI Main(DWORD Count,LPWSTR *Args){
    (void)Count;(void)Args;Service=RegisterServiceCtrlHandlerW(L"ALSCharge",Control);if(!Service)return;
    Stop=CreateEventW(0,TRUE,FALSE,0);if(!Stop){Report(SERVICE_STOPPED,GetLastError());return;}
    CHARGE_POLICY P={0};UsbInit();Report(SERVICE_RUNNING,0);
    while(WaitForSingleObject(Stop,200)==WAIT_TIMEOUT){
        HANDLE H=CreateFileW(ALS_CHARGE_DEVICE,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0);
        if(H==INVALID_HANDLE_VALUE)continue;
        ALS_CHARGE_STATUS_DATA S;
        if(ReadStatus(H,&S)){
            UsbPoll(&S,GetTickCount64());
            int A=ChargeNext(&P,&S,GetTickCount64());BOOL Ok=TRUE;
            if(A==ChargeKey)Ok=Key(H);
            else if(A!=ChargeNone)Ok=Command(H,A==ChargeAbort?ChargeStop:(ULONG)A);
            if(!Ok){if(A==ChargeKey){P.Normal=1;P.Attempts=0;}if(A==ChargeStart||A==ChargeNormal){P.Active=0;if(P.Attempts)P.Attempts--;}P.RetryAt=GetTickCount64()+1000;}
        }
        CloseHandle(H);
    }
    // Stop/update/shutdown must not leave a fast-charge session unattended.
    ULONGLONG Until=GetTickCount64()+9000;
    while(GetTickCount64()<Until){
        HANDLE H=CreateFileW(ALS_CHARGE_DEVICE,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_EXISTING,0,0);if(H==INVALID_HANDLE_VALUE)break;
        ALS_CHARGE_STATUS_DATA S;BOOL Done=TRUE;
        if(ReadStatus(H,&S)&&S.Bridge==2){
            Done=FALSE;
            if(!S.Pending&&S.Running)Command(H,ChargeStop);
            else if(!S.Pending&&!S.Running&&S.Phase!=90&&S.Phase!=92&&!S.UfcsStarted&&!S.PeerRunning&&!S.Mos&&(!S.CpTouched||S.CpOff))Command(H,ChargeRelease);
        }
        CloseHandle(H);if(Done)break;Sleep(100);
    }
    UsbFinish();CloseHandle(Stop);Report(SERVICE_STOPPED,0);
}
int WINAPI wWinMain(HINSTANCE Instance,HINSTANCE Previous,LPWSTR Line,int Show){
    (void)Instance;(void)Previous;(void)Line;(void)Show;
    SERVICE_TABLE_ENTRYW Table[]={{L"ALSCharge",Main},{0,0}};
    return StartServiceCtrlDispatcherW(Table)?0:1;
}
