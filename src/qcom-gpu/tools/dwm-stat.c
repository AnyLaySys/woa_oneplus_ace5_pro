#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>

static LRESULT CALLBACK Window(HWND window,UINT message,WPARAM w,LPARAM l) {
    return DefWindowProcW(window,message,w,l);
}
static HRESULT Timing(DWM_TIMING_INFO *t) {
    ZeroMemory(t,sizeof(*t));t->cbSize=sizeof(*t);
    return DwmGetCompositionTimingInfo(NULL,t);
}
int main(void) {
    SetProcessDPIAware();
    WNDCLASSW wc={0};wc.lpfnWndProc=Window;wc.hInstance=GetModuleHandleW(NULL);
    wc.hbrBackground=(HBRUSH)(ULONG_PTR)(COLOR_WINDOW+1);wc.lpszClassName=L"A830DwmStat";
    if(!RegisterClassW(&wc))return 1;
    HWND window=CreateWindowExW(WS_EX_TOPMOST,wc.lpszClassName,L"DWM motion test",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        40,140,420,320,NULL,NULL,wc.hInstance,NULL);
    if(!window)return 1;
    DWM_TIMING_INFO before,after;HRESULT hr=Timing(&before);
    if(FAILED(hr)){printf("{\"dwm_error\":\"%08lx\"}\n",(ULONG)hr);DestroyWindow(window);return 1;}
    LARGE_INTEGER start,end,frequency;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    UINT moves=0;
    do {
        int t=(int)(moves%120);int x=40+3*(t<60?t:120-t);
        SetWindowPos(window,NULL,x,140,0,0,SWP_NOZORDER|SWP_NOSIZE|SWP_NOACTIVATE);
        MSG msg;while(PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        moves++;Sleep(8);QueryPerformanceCounter(&end);
    } while(end.QuadPart-start.QuadPart<10*frequency.QuadPart);
    hr=Timing(&after);DestroyWindow(window);
    if(FAILED(hr)){printf("{\"dwm_error\":\"%08lx\"}\n",(ULONG)hr);return 1;}
    if(after.cFrameDisplayed<before.cFrameDisplayed || after.cFramesDropped<before.cFramesDropped ||
        after.cFramesMissed<before.cFramesMissed || after.cFrame<before.cFrame){
        puts("{\"dwm_error\":\"counters reset during test\"}");return 1;
    }
    double seconds=(double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart;
    printf("{\"seconds\":%.3f,\"moves\":%u,\"refresh_num\":%u,\"refresh_den\":%u,"
        "\"displayed\":%llu,\"displayed_fps\":%.2f,\"composed\":%llu,\"dropped\":%llu,\"missed\":%llu}\n",
        seconds,moves,after.rateRefresh.uiNumerator,after.rateRefresh.uiDenominator,
        after.cFrameDisplayed-before.cFrameDisplayed,(double)(after.cFrameDisplayed-before.cFrameDisplayed)/seconds,
        after.cFrame-before.cFrame,after.cFramesDropped-before.cFramesDropped,after.cFramesMissed-before.cFramesMissed);
    return 0;
}
