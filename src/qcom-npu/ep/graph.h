#pragma once
#include <onnxruntime_c_api.h>
#include <stdexcept>
#include <string>

inline void Check(const OrtApi& api, OrtStatus* status) {
    if (!status) return;
    std::string message = api.GetErrorMessage(status); api.ReleaseStatus(status);
    throw std::runtime_error(message);
}

struct NpuComputeInfo : OrtNodeComputeInfo {
    NpuComputeInfo() : OrtNodeComputeInfo{} { ort_version_supported = ORT_API_VERSION; }
    virtual ~NpuComputeInfo() = default;
};
bool NpuSupportsNode(const OrtApi& api, const OrtNode* node);
NpuComputeInfo* NpuCompileNode(const OrtApi& api, const OrtGraph* graph);
