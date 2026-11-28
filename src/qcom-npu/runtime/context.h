#pragma once
#include "rpc.h"
#include <istream>

// Compiled HTP program. Model compilation happens before deployment; all
// tensor evaluation happens on the connected V79 DSP through owned DMA maps.
class NpuContext {
    struct Block {
        uint32_t Kind, Bytes;
        uint64_t Original, Address;
        QCOM_NPU_ALLOCATION Allocation{};
    };
    NpuRuntime& runtime_;
    std::mutex mutex_;
    uint32_t handle_ = 0;
    uint32_t header_[16]{};
    std::vector<Block> blocks_;
    std::vector<uint8_t> execute_;
    std::vector<uint8_t> Relocate(const std::vector<uint8_t>& source) const;
    void Generic(const std::vector<uint8_t>& request, const std::vector<uint8_t>& input, std::vector<uint8_t>& output);
    void Upload(const Block& block, const std::vector<uint8_t>& data);
    void Initialize(std::istream& file, std::streamoff length);
public:
    NpuContext(NpuRuntime& runtime, const std::vector<uint8_t>& binary);
    NpuContext(const NpuContext&) = delete;
    NpuContext& operator=(const NpuContext&) = delete;
    void Execute(const uint8_t* input, uint32_t bytes, std::vector<uint8_t>& output);
};
