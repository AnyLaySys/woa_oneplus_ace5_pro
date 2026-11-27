#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <stdexcept>
#pragma comment(lib, "advapi32.lib")

namespace qcom_npu {
inline std::filesystem::path InstalledDirectory() {
    wchar_t image[32768]{}; DWORD bytes = sizeof(image);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\qcom_npu", L"ImagePath",
        RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, image, &bytes) != ERROR_SUCCESS)
        throw std::runtime_error("The Qualcomm NPU driver is not installed");
    std::wstring path(image);
    if (path.compare(0, 12, L"\\SystemRoot\\") == 0) {
        wchar_t windows[32768]{}; UINT count = GetWindowsDirectoryW(windows, 32768);
        if (!count || count >= 32768) throw std::runtime_error("Cannot locate Windows");
        path = std::wstring(windows) + path.substr(11);
    }
    if (path.compare(0, 4, L"\\??\\") == 0) path.erase(0, 4);
    auto directory = std::filesystem::path(path).parent_path();
    if (!directory.is_absolute()) throw std::runtime_error("Invalid NPU driver location");
    return directory;
}
}
