#define UNICODE
#define _UNICODE
#include <windows.h>
static VOID Pulse(UCHAR *data, ULONG *bits, ULONG duration, BOOLEAN mark) {
    ULONG count = (duration * 4u + 17u) / 35u;
    for (ULONG i = 0; i < count; i++, (*bits)++)
        if (mark && i % 3 == 0) data[*bits / 8] |= (UCHAR)(0x80u >> (*bits % 8));
}
static ULONG PowerWave(UCHAR *data) {
    ULONG bits = 0;
    Pulse(data, &bits, 4480, TRUE);
    Pulse(data, &bits, 4480, FALSE);
    for (ULONG i = 0; i < 32; i++) {
        Pulse(data, &bits, 560, TRUE);
        Pulse(data, &bits, 0xe0e040bfu & (0x80000000u >> i) ? 1680u : 560u, FALSE);
    }
    Pulse(data, &bits, 560, TRUE);
    Pulse(data, &bits, 47040, FALSE);
    return (bits + 7u) / 8u;
}
static LRESULT CALLBACK Window(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CREATE) {
        HWND button = CreateWindowW(L"BUTTON", L"Samsung M5 电源", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20,
            20, 240, 60, window, (HMENU)1, NULL, NULL);
        SendMessageW(button, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return 0;
    }
    if (message == WM_COMMAND && LOWORD(wparam) == 1 && HIWORD(wparam) == BN_CLICKED) {
        HWND button = GetDlgItem(window, 1);
        EnableWindow(button, FALSE);
        UCHAR data[2048] = { 0 };
        ULONG count = PowerWave(data);
        DWORD sent = 0;
        HANDLE device = CreateFileW(L"\\\\.\\QcomIr", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
            OPEN_EXISTING, 0, NULL);
        BOOL ok =
            device != INVALID_HANDLE_VALUE && WriteFile(device, data, count, &sent, NULL) && sent == count;
        if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
        if (!ok)
            MessageBoxW(window, L"红外发射失败，请确认红外设备可用。", L"红外遥控", MB_OK | MB_ICONERROR);
        EnableWindow(button, TRUE);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(command);
    WNDCLASSW cls = { 0 };
    cls.lpfnWndProc = Window;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cls.lpszClassName = L"QcomIrPower";
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowW(cls.lpszClassName, L"红外遥控", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 296, 145, NULL, NULL, instance, NULL);
    if (!window) return 1;
    ShowWindow(window, show);
    MSG message;
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
