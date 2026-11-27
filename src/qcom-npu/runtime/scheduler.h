#pragma once
#include <windows.h>
#include <d3dkmthk.h>
#include <map>
#include <mutex>
#include "../scheduler-abi.h"
class NpuScheduler {
    D3DKMT_HANDLE adapter_ = 0, device_ = 0, context_ = 0;
    D3DKMT_HANDLE paging_ = 0, pagingFence_ = 0;
    HANDLE completion_ = nullptr;
    void* command_ = nullptr;
    D3DKMT_HANDLE commandAllocation_ = 0;
    uint64_t commandAddress_ = 0;
    uint64_t cookie_ = 0;
    struct Memory { D3DKMT_HANDLE Handle; uint64_t Address; uint32_t Bytes; };
    std::map<uint32_t, Memory> memory_;
    std::mutex mutex_;
    void WaitPaging(uint64_t fence);
    uint64_t MapMemory(D3DKMT_HANDLE allocation, uint32_t bytes);
    void FreeMemory(const Memory& memory);
    void Close();
public:
    NpuScheduler();
    ~NpuScheduler();
    void Submit(const QCOM_NPU_TICKET& ticket);
    void RegisterMemory(const QCOM_NPU_SHARED_ALLOCATION& allocation);
    void UnregisterMemory(uint32_t id);
};
