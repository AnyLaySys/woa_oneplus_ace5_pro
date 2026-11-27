#include "transport.h"
#include "vendor/dspqueue_shared.h"
#include "vendor/AEEStdErr.h"
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <iostream>

#define NPU_RPC_EXPORT extern "C"

struct dspqueue {
    uint32_t Handle = 0;
    uint64_t Id = 0;
    int StateFd = -1, QueueFd = -1;
    dspqueue_process_queue_state* Process = nullptr;
    dspqueue_header* Header = nullptr;
    uint32_t WriteCount = 0, ReadCount = 0, Seq = 0;
    std::mutex ReadMutex, WriteMutex;
};

static int Call(dspqueue& queue, uint32_t method, void* input = nullptr, size_t inBytes = 0, void* output = nullptr, size_t outBytes = 0) {
    remote_arg args[2]{}; unsigned count = 0;
    if (inBytes) args[count++].buf = {input, inBytes};
    if (outBytes) args[count++].buf = {output, outBytes};
    return NpuRpcTransport().Get().Invoke(queue.Handle,
        (method << 24) | (inBytes ? 0x10000 : 0) | (outBytes ? 0x100 : 0), count ? args : nullptr);
}
static uint8_t* At(dspqueue& q, uint32_t offset) { return reinterpret_cast<uint8_t*>(q.Header) + offset; }
static dspqueue_packet_queue_state& State(dspqueue& q, uint32_t offset) {
    return *reinterpret_cast<dspqueue_packet_queue_state*>(At(q, offset));
}
static uint32_t Aligned(uint32_t bytes, uint32_t alignment) { return (bytes + alignment - 1) & ~(alignment - 1); }

NPU_RPC_EXPORT AEEResult dspqueue_create(int domain, uint32_t flags, uint32_t requestBytes, uint32_t responseBytes,
    dspqueue_callback_t callback, dspqueue_callback_t errorCallback, void*, dspqueue_t* result) {
    if (!result || domain != CDSP_DOMAIN_ID || flags || callback || errorCallback ||
        requestBytes > DSPQUEUE_MAX_QUEUE_SIZE || responseBytes > DSPQUEUE_MAX_QUEUE_SIZE) return AEE_EBADPARM;
    *result = nullptr;
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    if (owner.Queues) return AEE_EUNSUPPORTED;
    auto queue = std::make_unique<dspqueue>();
    try {
        auto& runtime = owner.Get();
        int error = runtime.Open("file:///libdspqueue_rpc_skel.so?dspqueue_rpc_skel_handle_invoke&_modver=1.0", &queue->Handle);
        if (error) throw std::runtime_error("DSP packet queues unavailable: " + std::to_string(error));
        owner.Handles.insert(queue->Handle);
        requestBytes = requestBytes ? Aligned(requestBytes, 8) : DSPQUEUE_DEFAULT_REQ_SIZE;
        responseBytes = responseBytes ? Aligned(responseBytes, 8) : DSPQUEUE_DEFAULT_RESP_SIZE;
        if (requestBytes < 64 || responseBytes < 64) throw std::runtime_error("DSP packet queues are too small");
        QCOM_NPU_SHARED_ALLOCATION state{}, memory{};
        error = runtime.MapShared(sizeof(dspqueue_process_queue_state), &state);
        if (error) throw std::runtime_error("Cannot allocate queue state");
        queue->StateFd = static_cast<int>(state.Allocation.Id);
        queue->Process = reinterpret_cast<dspqueue_process_queue_state*>(state.UserAddress);
        owner.Buffers.emplace(queue->StateFd, NpuSharedBuffer{state, sizeof(dspqueue_process_queue_state), 0});
        uint32_t bytes = 256 + Aligned(requestBytes, 256) + Aligned(responseBytes, 256) + 4 * 256;
        error = runtime.MapShared(bytes, &memory);
        if (error) throw std::runtime_error("Cannot allocate packet queue");
        queue->QueueFd = static_cast<int>(memory.Allocation.Id);
        owner.Buffers.emplace(queue->QueueFd, NpuSharedBuffer{memory, bytes, 0});
        queue->Header = reinterpret_cast<dspqueue_header*>(memory.UserAddress);
        auto& header = *queue->Header;
        header.version = DSPQUEUE_HEADER_CURRENT_VERSION;
        header.flags = DSPQUEUE_HEADER_FLAG_WAIT_COUNTS;
        header.queue_count = 1;
        uint32_t offset = 256;
        header.req_queue.queue_offset = offset; header.req_queue.queue_length = requestBytes;
        offset += Aligned(requestBytes, 256);
        header.req_queue.read_state_offset = offset; offset += 256;
        header.req_queue.write_state_offset = offset; offset += 256;
        header.resp_queue.queue_offset = offset; header.resp_queue.queue_length = responseBytes;
        offset += Aligned(responseBytes, 256);
        header.resp_queue.read_state_offset = offset; offset += 256;
        header.resp_queue.write_state_offset = offset;
        MemoryBarrier();
        error = Call(*queue, 2, &queue->StateFd, sizeof(queue->StateFd));
        if (error) throw std::runtime_error("DSP queue state registration: " + std::to_string(error));
        uint32_t input[] = {0, static_cast<uint32_t>(queue->QueueFd), 1};
        error = Call(*queue, 3, input, sizeof(input), &queue->Id, sizeof(queue->Id));
        if (error) throw std::runtime_error("DSP queue creation: " + std::to_string(error));
        if (!queue->Id) throw std::runtime_error("DSP returned an empty queue ID");
        owner.Queues++; *result = queue.release(); return 0;
    } catch (const std::exception& error) {
        std::cerr << "NPU queue: " << error.what() << '\n';
        // Partial mappings remain owned until the CDSP process has stopped.
        if (queue->Handle) { owner.Get().Close(queue->Handle); owner.Handles.erase(queue->Handle); }
        return AEE_ERPC;
    }
}
NPU_RPC_EXPORT AEEResult dspqueue_export(dspqueue_t queue, uint64_t* id) {
    if (!queue || !id) return AEE_EBADPARM;
    *id = queue->Id; return 0;
}
NPU_RPC_EXPORT AEEResult dspqueue_close(dspqueue_t queue) {
    if (!queue) return AEE_EBADPARM;
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    try {
        int error = Call(*queue, 4, &queue->Id, sizeof(queue->Id));
        if (error) return error;
        error = owner.Get().Close(queue->Handle);
        if (error) return error;
        owner.Handles.erase(queue->Handle);
        for (int fd : {queue->QueueFd, queue->StateFd}) {
            error = owner.Get().CloseFile(fd);
            if (error) return error;
            owner.Buffers.erase(fd);
        }
        owner.Queues--; delete queue; owner.ReleaseIfIdle(); return 0;
    } catch (...) { return AEE_ERPC; }
}

NPU_RPC_EXPORT AEEResult dspqueue_write(dspqueue_t queue, uint32_t flags, uint32_t bufferCount,
    dspqueue_buffer* buffers, uint32_t messageBytes, const uint8_t* message, uint32_t timeoutUs) {
    if (!queue || flags & DSPQUEUE_PACKET_FLAG_RESERVED_ZERO || bufferCount > DSPQUEUE_MAX_BUFFERS ||
        messageBytes > DSPQUEUE_MAX_MESSAGE_SIZE || (bufferCount && !buffers) || (messageBytes && !message)) return AEE_EBADPARM;
    std::lock_guard<std::mutex> writer(queue->WriteMutex);
    auto& owner = NpuRpcTransport();
    const auto& header = queue->Header->req_queue;
    auto& read = State(*queue, header.read_state_offset);
    auto& write = State(*queue, header.write_state_offset);
    uint32_t length = 8 + bufferCount * sizeof(dspqueue_buffer) + messageBytes;
    uint32_t padded = Aligned(length, 8);
    if (padded > header.queue_length - 8) return AEE_EBADSIZE;
    auto start = std::chrono::steady_clock::now();
    try {
        uint32_t position = 0; bool wrap = false;
        for (;;) {
            MemoryBarrier();
            if (queue->Header->error) return queue->Header->error;
            uint32_t r = read.position, w = write.position;
            if (r >= header.queue_length || w >= header.queue_length) return AEE_ERPC;
            uint32_t free = r > w ? r - w - 8 : header.queue_length - (w - r) - 8;
            wrap = header.queue_length - w < padded;
            uint32_t required = padded + (wrap ? header.queue_length - w : 0);
            if (free >= required) { position = w; break; }
            if (!timeoutUs) return AEE_EWOULDBLOCK;
            if (timeoutUs != DSPQUEUE_TIMEOUT_NONE && std::chrono::steady_clock::now() - start >= std::chrono::microseconds(timeoutUs)) return AEE_EEXPIRED;
            Sleep(1);
        }
        std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
        std::vector<dspqueue_buffer> wire;
        if (bufferCount) wire.assign(buffers, buffers + bufferCount);
        for (auto& descriptor : wire) {
            auto& buffer = owner.Buffer(static_cast<int>(descriptor.fd));
            if (descriptor.flags & DSPQUEUE_BUFFER_FLAG_RESERVED_ZERO ||
                (descriptor.flags & (DSPQUEUE_BUFFER_FLAG_REF | DSPQUEUE_BUFFER_FLAG_DEREF)) == (DSPQUEUE_BUFFER_FLAG_REF | DSPQUEUE_BUFFER_FLAG_DEREF) ||
                descriptor.offset > buffer.Size || descriptor.size > buffer.Size - descriptor.offset ||
                (!descriptor.size && descriptor.offset) ||
                ((descriptor.flags & DSPQUEUE_BUFFER_FLAG_DEREF) && buffer.References <= 0)) return AEE_EBADPARM;
        }
        flags = (flags & ~(DSPQUEUE_PACKET_FLAG_BUFFERS | DSPQUEUE_PACKET_FLAG_MESSAGE)) | DSPQUEUE_PACKET_FLAG_USER_READY;
        if (bufferCount) flags |= DSPQUEUE_PACKET_FLAG_BUFFERS;
        if (messageBytes) flags |= DSPQUEUE_PACKET_FLAG_MESSAGE;
        uint64_t packet = length | (static_cast<uint64_t>(flags) << 32) | (static_cast<uint64_t>(bufferCount) << 48) |
            (static_cast<uint64_t>(queue->Seq & 255) << 56);
        auto* memory = At(*queue, header.queue_offset);
        std::memcpy(memory + position, &packet, 8);
        if (wrap) { position = 0; std::memcpy(memory, &packet, 8); }
        position += 8;
        if (bufferCount) { std::memcpy(memory + position, wire.data(), bufferCount * sizeof(dspqueue_buffer)); position += bufferCount * sizeof(dspqueue_buffer); }
        if (messageBytes) std::memcpy(memory + position, message, messageBytes);
        position = Aligned(position + messageBytes, 8);
        if (position == header.queue_length) position = 0;
        for (const auto& descriptor : wire) {
            auto& buffer = owner.Buffer(static_cast<int>(descriptor.fd));
            if (descriptor.flags & DSPQUEUE_BUFFER_FLAG_REF) buffer.References++;
            if (descriptor.flags & DSPQUEUE_BUFFER_FLAG_DEREF) buffer.References--;
        }
        MemoryBarrier();
        QCOM_NPU_QUEUE_DISPATCH dispatch{2, static_cast<uint32_t>(queue->QueueFd), static_cast<uint32_t>(queue->StateFd), queue->Handle,
            position, queue->WriteCount + 1, queue->Header->queue_count, owner.ComputeHandle};
        int error = owner.Get().DispatchQueue(dispatch);
        if (!error) { queue->WriteCount++; queue->Seq++; }
        return error;
    } catch (const std::exception& error) {
        std::cerr << "NPU queue submission failed: " << error.what() << '\n';
        return AEE_ERPC;
    }
}

NPU_RPC_EXPORT AEEResult dspqueue_read(dspqueue_t queue, uint32_t* flags, uint32_t maxBuffers, uint32_t* bufferCount,
    dspqueue_buffer* buffers, uint32_t capacity, uint32_t* messageBytes, uint8_t* message, uint32_t timeoutUs) {
    if (!queue || !flags || !bufferCount || !messageBytes || (maxBuffers && !buffers) || (capacity && !message)) return AEE_EBADPARM;
    std::lock_guard<std::mutex> reader(queue->ReadMutex);
    const auto& header = queue->Header->resp_queue;
    auto& read = State(*queue, header.read_state_offset);
    auto& write = State(*queue, header.write_state_offset);
    auto start = std::chrono::steady_clock::now();
    try {
        for (;;) {
            MemoryBarrier();
            if (queue->Header->error) return queue->Header->error;
            if (write.packet_count != read.packet_count) break;
            if (!timeoutUs) return AEE_EWOULDBLOCK;
            if (timeoutUs != DSPQUEUE_TIMEOUT_NONE && std::chrono::steady_clock::now() - start >= std::chrono::microseconds(timeoutUs)) return AEE_EEXPIRED;
            Sleep(1);
        }
        auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
        uint32_t position = read.position;
        if (position >= header.queue_length) return AEE_ERPC;
        auto* memory = At(*queue, header.queue_offset);
        uint64_t packet = 0; std::memcpy(&packet, memory + position, 8);
        uint32_t length = static_cast<uint32_t>(packet);
        uint32_t count = static_cast<uint32_t>((packet >> 48) & 255);
        uint32_t packetFlags = static_cast<uint32_t>((packet >> 32) & 65535);
        if (count > DSPQUEUE_MAX_BUFFERS || length < 8 + count * sizeof(dspqueue_buffer) || Aligned(length, 8) > header.queue_length - 8 ||
            packetFlags & DSPQUEUE_PACKET_FLAG_RESERVED_ZERO || !(packetFlags & DSPQUEUE_PACKET_FLAG_USER_READY)) return AEE_ERPC;
        uint32_t bytes = length - 8 - count * sizeof(dspqueue_buffer);
        if (count > maxBuffers || bytes > capacity) return AEE_EBADSIZE;
        if (header.queue_length - position < Aligned(length, 8)) {
            position = 0; uint64_t repeated = 0; std::memcpy(&repeated, memory, 8);
            if (repeated != packet) return AEE_ERPC;
        }
        position += 8;
        std::vector<dspqueue_buffer> wire(count);
        if (count) std::memcpy(wire.data(), memory + position, count * sizeof(dspqueue_buffer));
        position += count * sizeof(dspqueue_buffer);
        for (auto& descriptor : wire) {
            auto& buffer = owner.Buffer(static_cast<int>(descriptor.fd));
            if (descriptor.offset > buffer.Size || descriptor.size > buffer.Size - descriptor.offset ||
                descriptor.flags & DSPQUEUE_BUFFER_FLAG_RESERVED_ZERO ||
                (descriptor.flags & (DSPQUEUE_BUFFER_FLAG_REF | DSPQUEUE_BUFFER_FLAG_DEREF)) == (DSPQUEUE_BUFFER_FLAG_REF | DSPQUEUE_BUFFER_FLAG_DEREF) ||
                ((descriptor.flags & DSPQUEUE_BUFFER_FLAG_DEREF) && buffer.References <= 0)) return AEE_ERPC;
            descriptor.ptr = reinterpret_cast<uint8_t*>(buffer.Mapping.UserAddress) + descriptor.offset;
        }
        if (bytes) std::memcpy(message, memory + position, bytes);
        if (count) std::memcpy(buffers, wire.data(), count * sizeof(dspqueue_buffer));
        for (auto& descriptor : wire) {
            auto& buffer = owner.Buffer(static_cast<int>(descriptor.fd));
            if (descriptor.flags & DSPQUEUE_BUFFER_FLAG_REF) buffer.References++;
            if (descriptor.flags & DSPQUEUE_BUFFER_FLAG_DEREF) buffer.References--;
        }
        *flags = packetFlags; *bufferCount = count; *messageBytes = bytes;
        position = Aligned(position + bytes, 8); if (position == header.queue_length) position = 0;
        MemoryBarrier(); read.position = position; read.packet_count = ++queue->ReadCount;
        queue->Process->resp_space_count[0] = queue->ReadCount;
        MemoryBarrier(); return Call(*queue, 8);
    } catch (...) { return AEE_ERPC; }
}
NPU_RPC_EXPORT AEEResult dspqueue_read_noblock(dspqueue_t queue, uint32_t* flags, uint32_t maxBuffers, uint32_t* bufferCount,
    dspqueue_buffer* buffers, uint32_t capacity, uint32_t* messageBytes, uint8_t* message) {
    return dspqueue_read(queue, flags, maxBuffers, bufferCount, buffers, capacity, messageBytes, message, 0);
}
