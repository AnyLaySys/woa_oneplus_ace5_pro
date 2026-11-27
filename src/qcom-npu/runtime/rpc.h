#pragma once
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <map>
#include <mutex>
#include <memory>
class NpuScheduler;
#include "vendor/remote.h"
#include "../uapi.h"

class NpuRuntime {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    HANDLE power_ = INVALID_HANDLE_VALUE;
    std::thread listener_;
    std::atomic<bool> stop_{false};
    std::map<uint64_t, QCOM_NPU_ALLOCATION> allocations_;
    std::map<int, QCOM_NPU_ALLOCATION> fileAllocations_;
    std::mutex allocationMutex_;
    uint32_t control_ = 8;
    std::unique_ptr<NpuScheduler> scheduler_;
    void Listener();
public:
    explicit NpuRuntime(const std::filesystem::path& directory);
    ~NpuRuntime();
    NpuRuntime(const NpuRuntime&) = delete;
    NpuRuntime& operator=(const NpuRuntime&) = delete;
    int Invoke(uint32_t handle, uint32_t scalar, remote_arg* args, bool listener = false);
    int Open(const char* uri, uint32_t* handle);
    int Close(uint32_t handle);
    void EnableScheduler();
    QCOM_NPU_STATUS Status();
    int Allocate(uint32_t flags, int64_t size, uint64_t* address);
    int Free(uint64_t address, int64_t size);
    int MapFile(const std::filesystem::path& path, int* fd, int* size);
    int CloseFile(int fd);
    int FileAddress(int fd, int size, uint64_t* address);
    int MapBuffer(uint32_t size, uint32_t flags, QCOM_NPU_ALLOCATION* allocation);
    int MapShared(uint32_t size, QCOM_NPU_SHARED_ALLOCATION* allocation, uint32_t flags = QCOM_NPU_SHARED, bool deferred = false);
    int MapSharedFile(int fd, bool map, bool pin = false);
    int DispatchQueue(const QCOM_NPU_QUEUE_DISPATCH& dispatch);
    int WriteBuffer(uint32_t id, uint32_t offset, const uint8_t* data, uint32_t size);
    int ReadBuffer(uint32_t id, uint32_t offset, uint8_t* data, uint32_t size);
    uint32_t Capabilities[256]{};
    uint32_t SocId = 0;
    bool PagedMemory = false;
};
void NpuFilesStart(const std::filesystem::path& root, NpuRuntime* runtime);
void NpuFilesStop();
int NpuHostInvoke(uint32_t handle, uint32_t scalar, remote_arg* args);
