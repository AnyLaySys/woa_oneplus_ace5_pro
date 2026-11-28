#include "context.h"
#include <cstring>
#include <stdexcept>
#include <sstream>

static void CheckRpc(int error, const char* operation) {
    if (error) throw std::runtime_error(std::string(operation) + ": " + std::to_string(error));
}
static uint32_t Get(const std::vector<uint8_t>& data, size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) throw std::runtime_error("Invalid HTP context field");
    uint32_t value; std::memcpy(&value, data.data() + offset, 4); return value;
}
std::vector<uint8_t> NpuContext::Relocate(const std::vector<uint8_t>& source) const {
    auto data = source;
    for (size_t offset = 0; offset + 8 <= data.size(); offset += 8) {
        uint64_t address; std::memcpy(&address, data.data() + offset, 8);
        for (const auto& block : blocks_) {
            if (address >= block.Original && address - block.Original < block.Bytes) {
                address = block.Address + address - block.Original;
                std::memcpy(data.data() + offset, &address, 8); break;
            }
        }
    }
    return data;
}
void NpuContext::Upload(const Block& block, const std::vector<uint8_t>& data) {
    if (data.size() > block.Bytes) throw std::runtime_error("HTP context image exceeds allocation");
    for (uint32_t offset = 0; offset < data.size();) {
        uint32_t bytes = (std::min)(uint32_t(0x10000), static_cast<uint32_t>(data.size()) - offset);
        CheckRpc(runtime_.WriteBuffer(block.Allocation.Id, offset, data.data() + offset, bytes), "Upload HTP context DMA");
        offset += bytes;
    }
}
void NpuContext::Generic(const std::vector<uint8_t>& request, const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    uint32_t lengths[] = {static_cast<uint32_t>(request.size()), static_cast<uint32_t>(input.size()), static_cast<uint32_t>(input.size()),
        static_cast<uint32_t>(output.size()), static_cast<uint32_t>(output.size()), 0};
    uint64_t result = 0;
    auto in = input;
    remote_arg args[7]{};
    args[0].buf = {lengths, sizeof(lengths)}; args[1].buf = {const_cast<uint8_t*>(request.data()), request.size()};
    args[2].buf = {in.data(), in.size()}; args[3].buf = {output.data(), output.size()};
    args[4].buf = {&result, sizeof(result)}; args[5].buf = {in.data(), in.size()}; args[6].buf = {output.data(), output.size()};
    CheckRpc(runtime_.Invoke(handle_, 0x04040300, args), "HTP control RPC");
    CheckRpc(static_cast<int>(result), "HTP control result");
}
NpuContext::NpuContext(NpuRuntime& runtime, const std::vector<uint8_t>& binary) : runtime_(runtime) {
    if (binary.size() < 64 || binary.size() > QCOM_NPU_MAX_DATA) throw std::runtime_error("Invalid native HTP context data");
    std::istringstream file(std::string(reinterpret_cast<const char*>(binary.data()), binary.size()), std::ios::binary);
    Initialize(file, static_cast<std::streamoff>(binary.size()));
}
void NpuContext::Initialize(std::istream& file, std::streamoff length) {
    std::vector<std::vector<uint8_t>> images;
    file.read(reinterpret_cast<char*>(header_), sizeof(header_));
    if (header_[0] != 0x3156504e || header_[1] != 1 || header_[2] != runtime_.SocId || !header_[3] || header_[3] > 16 ||
        header_[4] > 0x10000 || header_[5] != 72 || header_[6] >= header_[3] || header_[9] >= header_[3] || header_[12] >= header_[3] ||
        !header_[8] || !header_[11] || header_[13] || header_[14] || header_[15]) throw std::runtime_error("Incompatible native HTP context");
    for (uint32_t i = 0; i < header_[3]; i++) {
        struct Descriptor { uint32_t Kind, Bytes; uint64_t Address; uint32_t DataBytes, Reserved; } d{};
        file.read(reinterpret_cast<char*>(&d), sizeof(d));
        if (!file || (d.Kind != 2 && d.Kind != 7) || !d.Bytes || d.Bytes > 0x200000 || !d.Address || d.DataBytes != d.Bytes || d.Reserved)
            throw std::runtime_error("Invalid native HTP DMA descriptor");
        images.emplace_back(d.DataBytes);
        file.read(reinterpret_cast<char*>(images.back().data()), images.back().size());
        if (!file) throw std::runtime_error("Truncated native HTP DMA image");
        blocks_.push_back({d.Kind, d.Bytes, d.Address, 0, {}});
    }
    std::vector<uint8_t> initialize(header_[4]); execute_.resize(header_[5]);
    file.read(reinterpret_cast<char*>(initialize.data()), initialize.size()); file.read(reinterpret_cast<char*>(execute_.data()), execute_.size());
    if (!file || file.tellg() != length) throw std::runtime_error("Invalid native HTP context size");
    auto validRange = [this](uint32_t index, uint32_t offset, uint32_t bytes) {
        return offset <= blocks_[index].Bytes && bytes <= blocks_[index].Bytes - offset;
    };
    if (!validRange(header_[6], header_[7], header_[8]) || !validRange(header_[9], header_[10], header_[11]) || blocks_[header_[12]].Bytes < 64)
        throw std::runtime_error("Invalid native HTP tensor range");
    CheckRpc(runtime_.Open("file:///libQnnHtpV79Skel.so?qnn_2_50_0_skel_handle_invoke&_modver=1.0", &handle_), "Open V79 inference library");
    uint32_t setup[] = {5, 0, 0x9a029163}; uint8_t settings[] = {1, 0, 0, 0, 0}; uint64_t answer = 0;
    remote_arg arguments[4]{};
    arguments[0].buf = {setup, sizeof(setup)}; arguments[1].buf = {settings, sizeof(settings)};
    arguments[2].buf = {&answer, sizeof(answer)};
    CheckRpc(runtime_.Invoke(handle_, 0x02020200, arguments), "Initialize HTP transport");
    CheckRpc(static_cast<int>(answer), "Initialize HTP transport result");
    uint32_t op[] = {24, 17, 3, 0, 1, 0}; std::vector<uint8_t> config(sizeof(op)), none;
    std::memcpy(config.data(), op, sizeof(op)); Generic(config, {}, none);
    for (auto& block : blocks_) {
        QCOM_NPU_SHARED_ALLOCATION allocation{};
        CheckRpc(runtime_.MapShared(block.Bytes, &allocation, 3), "Create native HTP DMA object");
        block.Allocation = allocation.Allocation;
        struct Map { uint32_t Kind, Id; uint64_t Offset, Bytes, Reserved; } request{block.Kind, block.Allocation.Id, 0, block.Bytes, 0};
        uint64_t result[2]{}; remote_arg args[2]{};
        args[0].buf = {&request, sizeof(request)}; args[1].buf = {result, sizeof(result)};
        CheckRpc(runtime_.Invoke(handle_, 0x09010100, args), "Map native HTP DMA object");
        if (!result[0] || result[1]) throw std::runtime_error("DSP did not map native HTP memory");
        block.Address = result[0];
    }
    for (size_t i = 0; i < blocks_.size(); i++) Upload(blocks_[i], Relocate(images[i]));
    // Validate the compiler-produced deserialization packet. Only buffers,
    // never handles or host/physical pointers, pass through this packet.
    if (Get(initialize, 4) != 0x04040300 || Get(initialize, 8) != 7) throw std::runtime_error("Invalid context initialization scalar");
    uint32_t bytes = Get(initialize, 16), inputBytes = Get(initialize, 20), inoutBytes = Get(initialize, 24);
    size_t offset = 12 + 7 * 4 + 24;
    if (bytes > initialize.size() - offset || inputBytes != 8 || inoutBytes || initialize.size() != offset + bytes + inputBytes ||
        Get(initialize, offset) != bytes || Get(initialize, offset + 4) != 15) throw std::runtime_error("Invalid compiled HTP import request");
    std::vector<uint8_t> request(initialize.begin() + offset, initialize.begin() + offset + bytes);
    std::vector<uint8_t> in(initialize.end() - inputBytes, initialize.end());
    Generic(Relocate(request), in, none);
}
void NpuContext::Execute(const uint8_t* input, uint32_t bytes, std::vector<uint8_t>& output) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!input || bytes != header_[8]) throw std::runtime_error("Native HTP input size mismatch");
    const auto& source = blocks_[header_[6]];
    CheckRpc(runtime_.WriteBuffer(source.Allocation.Id, header_[7], input, bytes), "Write native HTP input");
    auto request = Relocate(execute_); uint64_t flags = 9, result = 0;
    remote_arg args[3]{}; args[0].buf = {&flags, sizeof(flags)}; args[1].buf = {request.data(), request.size()}; args[2].buf = {&result, sizeof(result)};
    CheckRpc(runtime_.Invoke(handle_, 0x07020100, args), "Execute native HTP context");
    CheckRpc(static_cast<int>(result), "Native HTP execution result");
    const auto& destination = blocks_[header_[9]]; output.resize(header_[11]);
    CheckRpc(runtime_.ReadBuffer(destination.Allocation.Id, header_[10], output.data(), static_cast<uint32_t>(output.size())), "Read native HTP output");
    uint64_t profile[8]{};
    CheckRpc(runtime_.ReadBuffer(blocks_[header_[12]].Allocation.Id, 0, reinterpret_cast<uint8_t*>(profile), sizeof(profile)), "Read native HTP profiling");
    if (!profile[6]) throw std::runtime_error("No native HTP hardware cycles reported");
}
