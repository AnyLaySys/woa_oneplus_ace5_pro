#pragma once
#include <windows.h>
#include <onnxruntime_cxx_api.h>
#include "../runtime/installed.h"
#include <filesystem>
#include <string>
#include <cstring>
#include <stdexcept>
#pragma comment(lib, "advapi32.lib")

// ARM64 ONNX Runtime 1.24+. Register once per environment; keep that
// environment alive until every attached session has been destroyed.
namespace qcom_npu {
inline constexpr const char* LibraryName = "qcom-npu";
inline constexpr const char* ProviderName = "QcomHtpExecutionProvider";

inline std::filesystem::path InstalledLibrary() {
    auto library = InstalledDirectory() / L"qcom_npu_ep.dll";
    if (!std::filesystem::is_regular_file(library))
        throw std::runtime_error("The installed Qualcomm NPU execution provider is missing");
    return library;
}

inline const OrtEpDevice* Register(Ort::Env& environment, const wchar_t* library = nullptr) {
    const auto& api = Ort::GetApi();
    auto path = library ? std::filesystem::path(library) : InstalledLibrary();
    Ort::ThrowOnError(api.RegisterExecutionProviderLibrary(environment, LibraryName, path.c_str()));
    const OrtEpDevice* const* devices = nullptr;
    size_t count = 0;
    Ort::ThrowOnError(api.GetEpDevices(environment, &devices, &count));
    for (size_t i = 0; i < count; ++i) {
        if (std::strcmp(api.EpDevice_EpName(devices[i]), ProviderName)) continue;
        if (api.HardwareDevice_Type(api.EpDevice_Device(devices[i])) != OrtHardwareDeviceType_NPU)
            throw std::runtime_error("The Qualcomm execution provider did not discover NPU hardware");
        return devices[i];
    }
    throw std::runtime_error("The installed Qualcomm NPU is unavailable");
}

inline void Attach(Ort::Env& environment, Ort::SessionOptions& options, const OrtEpDevice* device, bool requireNpu = false) {
    if (!device) throw std::runtime_error("Select a registered Qualcomm NPU device");
    if (requireNpu) options.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    Ort::ThrowOnError(Ort::GetApi().SessionOptionsAppendExecutionProvider_V2(
        options, environment, &device, 1, nullptr, nullptr, 0));
}
}
