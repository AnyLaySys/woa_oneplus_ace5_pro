#pragma once
#include "rpc.h"
#include "vendor/dspqueue.h"
#include <set>

struct NpuSharedBuffer {
    QCOM_NPU_SHARED_ALLOCATION Mapping{};
    size_t Size = 0;
    int References = 0;
};

struct NpuTransport {
    std::recursive_mutex Mutex;
    std::shared_ptr<NpuRuntime> Runtime;
    std::map<int, NpuSharedBuffer> Buffers;
    std::set<uint32_t> Handles;
    size_t Queues = 0;
    uint32_t ComputeHandle = 0;
    NpuRuntime& Get();
    NpuSharedBuffer& Buffer(int fd);
    void ReleaseIfIdle();
};

NpuTransport& NpuRpcTransport();
