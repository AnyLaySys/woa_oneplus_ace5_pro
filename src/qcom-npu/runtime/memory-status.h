#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../uapi.h"

namespace qcom_npu {
inline bool MemoryStatus(QCOM_NPU_MEMORY_STATUS& status) {
    HANDLE file = CreateFileW(L"\\\\.\\QcomNpu", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD used = 0;
    BOOL ok = DeviceIoControl(file, QCOM_NPU_QUERY_MEMORY, nullptr, 0, &status, sizeof(status), &used, nullptr);
    CloseHandle(file);
    return ok && used == sizeof(status) && status.Abi == QCOM_NPU_ABI &&
        (!status.WindowBytes || status.WindowBytes == 0x70000000u);
}
}
