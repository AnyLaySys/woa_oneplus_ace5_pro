#pragma once
#include <onnxruntime_c_api.h>

struct NpuComputeInfo : OrtNodeComputeInfo {
    NpuComputeInfo() : OrtNodeComputeInfo{} { ort_version_supported = ORT_API_VERSION; }
    virtual ~NpuComputeInfo() = default;
};
bool NpuSupportsNode(const OrtApi& api, const OrtNode* node);
NpuComputeInfo* NpuCompileNode(const OrtApi& api, const OrtGraph* graph);
