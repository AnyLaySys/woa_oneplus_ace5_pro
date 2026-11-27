#include "rpc.h"
#include "scheduler.h"
#include "vendor/AEEStdErr.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <future>

static std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    auto size = f.tellg();
    if (!f || size < 0 || size > QCOM_NPU_MAX_DATA) throw std::runtime_error("Invalid DSP shell file");
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.seekg(0); f.read(reinterpret_cast<char*>(data.data()), size);
    if (!f) throw std::runtime_error("Cannot read DSP shell");
    return data;
}
static bool Io(HANDLE f, DWORD code, void* in, DWORD size, void* out, DWORD capacity, DWORD& used) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    for (;;) {
        if (DeviceIoControl(f, code, in, size, out, capacity, &used, nullptr)) return true;
        if (GetLastError() != ERROR_BUSY || code == QCOM_NPU_COLLECT || code == QCOM_NPU_EXECUTION_RESULT ||
            code == QCOM_NPU_DISPATCH_RESULT || std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(1);
    }
}
NpuRuntime::NpuRuntime(const std::filesystem::path& directory) {
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION; reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString = const_cast<wchar_t*>(L"NPU library loading and inference");
    power_ = PowerCreateRequest(&reason);
    if (power_ != INVALID_HANDLE_VALUE) {
        PowerSetRequest(power_, PowerRequestSystemRequired);
        PowerSetRequest(power_, PowerRequestExecutionRequired);
    }
    file_ = CreateFileW(L"\\\\.\\QcomNpu", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (power_ != INVALID_HANDLE_VALUE) CloseHandle(power_);
        throw std::runtime_error("Cannot open QcomNpu: " + std::to_string(error));
    }
    try {
        NpuFilesStart(directory, this);
        SocId = Status().SocId;
        PagedMemory = (Status().Flags & QCOM_NPU_PAGED_MEMORY) != 0;
        std::vector<uint8_t> query(152 + 255 * 4);
        auto* capabilities = reinterpret_cast<QCOM_NPU_PACKET*>(query.data());
        capabilities->Abi = QCOM_NPU_ABI; capabilities->Handle = 2; capabilities->Scalar = 0x10100; capabilities->Count = 2;
        capabilities->Arg[0] = { 148, 4 }; capabilities->Arg[1] = { 152, 255 * 4 };
        uint32_t count = 255; std::memcpy(query.data() + 148, &count, 4);
        DWORD capacityUsed = 0;
        if (!Io(file_, QCOM_NPU_INVOKE, query.data(), 152, query.data(), static_cast<DWORD>(query.size()), capacityUsed) ||
            capacityUsed != query.size() || capabilities->Result) throw std::runtime_error("DSP capability query failed");
        Capabilities[0] = (Status().Flags & 7) == 7;
        std::memcpy(Capabilities + 1, query.data() + 152, 255 * 4);
        auto shell = ReadFile(directory / L"fastrpc_shell_unsigned_3");
        std::vector<uint8_t> data(16 + shell.size());
        auto* p = reinterpret_cast<QCOM_NPU_PROCESS*>(data.data());
        p->Abi = QCOM_NPU_ABI; p->Attributes = 8;
        p->ShellBytes = static_cast<ULONG>(shell.size()); p->HeapBytes = 0x800000;
        std::memcpy(data.data() + 16, shell.data(), shell.size());
        DWORD used = 0;
        if (!Io(file_, QCOM_NPU_CREATE, data.data(), static_cast<DWORD>(data.size()), nullptr, 0, used))
            throw std::runtime_error("CDSP process creation failed: " + std::to_string(GetLastError()));
        auto s = Status();
        if ((s.Flags & 95) != 95) throw std::runtime_error("CDSP user process is not ready");
        std::promise<int> initialized;
        auto ready = initialized.get_future();
        listener_ = std::thread([this, result = std::move(initialized)]() mutable {
            int error = Invoke(3, 0x03000000, nullptr);
            result.set_value(error);
            if (!error) Listener();
        });
        int result = ready.get();
        if (result) throw std::runtime_error("CDSP listener initialization failed: " + std::to_string(result));
    } catch (...) {
        stop_ = true;
        if (listener_.joinable()) listener_.join();
        NpuFilesStop(); CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
        if (power_ != INVALID_HANDLE_VALUE) {
            PowerClearRequest(power_, PowerRequestExecutionRequired); PowerClearRequest(power_, PowerRequestSystemRequired);
            CloseHandle(power_); power_ = INVALID_HANDLE_VALUE;
        }
        throw;
    }
}
NpuRuntime::~NpuRuntime() {
    stop_ = true;
    if (listener_.joinable()) listener_.join();
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    NpuFilesStop();
    if (power_ != INVALID_HANDLE_VALUE) {
        PowerClearRequest(power_, PowerRequestExecutionRequired); PowerClearRequest(power_, PowerRequestSystemRequired);
        CloseHandle(power_);
    }
}
void NpuRuntime::EnableScheduler() { if (!scheduler_) scheduler_ = std::make_unique<NpuScheduler>(); }
QCOM_NPU_STATUS NpuRuntime::Status() {
    QCOM_NPU_STATUS s{}; DWORD used = 0;
    if (!Io(file_, QCOM_NPU_QUERY, nullptr, 0, &s, sizeof(s), used) || used != sizeof(s) || s.Abi != QCOM_NPU_ABI)
        throw std::runtime_error("NPU query failed");
    return s;
}
int NpuRuntime::Invoke(uint32_t handle, uint32_t scalar, remote_arg* args, bool listener) {
    unsigned inputs = (scalar >> 16) & 255, outputs = (scalar >> 8) & 255, count = inputs + outputs;
    if ((scalar & 255) || count > QCOM_NPU_MAX_ARGUMENTS || (count && !args)) return AEE_EBADPARM;
    size_t size = 148;
    for (unsigned i = 0; i < count; i++) {
        if (args[i].buf.nLen > QCOM_NPU_MAX_DATA || (args[i].buf.nLen && !args[i].buf.pv)) return AEE_EBADPARM;
        size += args[i].buf.nLen;
        if (size > QCOM_NPU_MAX_DATA + 148) return AEE_EBADPARM;
    }
    std::vector<uint8_t> input(size), output(size);
    auto* q = reinterpret_cast<QCOM_NPU_PACKET*>(input.data());
    q->Abi = QCOM_NPU_ABI; q->Handle = handle; q->Scalar = scalar; q->Count = count;
    ULONG at = 148;
    for (unsigned i = 0; i < count; i++) {
        q->Arg[i] = { at, static_cast<ULONG>(args[i].buf.nLen) };
        if (i < inputs && args[i].buf.nLen) std::memcpy(input.data() + at, args[i].buf.pv, args[i].buf.nLen);
        at += static_cast<ULONG>(args[i].buf.nLen);
    }
    bool scheduled = scheduler_ && handle >= 256 && scalar == 0x07020100;
    DWORD submit = scheduled ? QCOM_NPU_QUEUE_EXECUTION : QCOM_NPU_SUBMIT;
    DWORD collect = scheduled ? QCOM_NPU_EXECUTION_RESULT : QCOM_NPU_COLLECT;
    QCOM_NPU_TICKET ticket{}; DWORD used = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (!Io(file_, submit, input.data(), static_cast<DWORD>(input.size()), &ticket, sizeof(ticket), used)) {
        DWORD error = GetLastError();
        if (error != ERROR_BUSY || std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "RPC submit failed handle=" << handle << " scalar=" << std::hex << scalar << std::dec << " Windows error=" << error << '\n';
            return AEE_ERPC;
        }
        if (stop_) return AEE_ECONNRESET;
        Sleep(1);
    }
    if (used != sizeof(ticket) || ticket.Abi != QCOM_NPU_ABI) return AEE_ERPC;
    if (scheduled) scheduler_->Submit(ticket);
    while (!Io(file_, collect, &ticket, sizeof(ticket), output.data(), static_cast<DWORD>(output.size()), used)) {
        DWORD error = GetLastError();
        if (error != ERROR_BUSY) {
            std::cerr << "RPC collect failed handle=" << handle << " scalar=" << std::hex << scalar << std::dec << " Windows error=" << error << '\n';
            return AEE_ERPC;
        }
        if (stop_) return AEE_ECONNRESET;
        if (!listener && std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "RPC response timeout handle=" << handle << " scalar=" << std::hex << scalar << std::dec << '\n';
            return AEE_ERPC;
        }
        Sleep(1);
    }
    if (used < 148) return AEE_ERPC;
    auto* response = reinterpret_cast<QCOM_NPU_PACKET*>(output.data());
    for (unsigned i = inputs; i < count; i++) {
        if (q->Arg[i].Offset > used || q->Arg[i].Length > used - q->Arg[i].Offset) return AEE_ERPC;
        if (args[i].buf.nLen) std::memcpy(args[i].buf.pv, output.data() + q->Arg[i].Offset, args[i].buf.nLen);
    }
    return static_cast<int>(response->Result);
}
int NpuRuntime::Open(const char* uri, uint32_t* handle) {
    uint32_t in[] = { static_cast<uint32_t>(std::strlen(uri) + 1), 1024 }, out[2]{};
    char error[1024]{};
    remote_arg args[4]{};
    args[0].buf = { in, sizeof(in) }; args[1].buf = { const_cast<char*>(uri), in[0] };
    args[2].buf = { out, sizeof(out) }; args[3].buf = { error, sizeof(error) };
    int result = Invoke(control_, control_ == 8 ? 0x02020200 : 0x20200, args);
    if (control_ == 8 && (static_cast<uint32_t>(result) == 0x80000414u ||
        static_cast<uint32_t>(result) == 0x8000042cu || static_cast<uint32_t>(result) == 0x8000043eu)) {
        control_ = 0;
        result = Invoke(0, 0x20200, args);
    }
    if (!result && out[1]) result = static_cast<int>(out[1]);
    if (result) std::cerr << "CDSP open " << uri << ": " << result << ' ' << error << '\n';
    else *handle = out[0];
    return result;
}
int NpuRuntime::Close(uint32_t handle) {
    uint32_t in[] = { handle, 1024 }, out{}; char error[1024]{};
    remote_arg args[3]{};
    args[0].buf = { in, sizeof(in) }; args[1].buf = { &out, sizeof(out) }; args[2].buf = { error, sizeof(error) };
    int result = Invoke(control_, control_ == 8 ? 0x03010200 : 0x1010200, args);
    return result ? result : static_cast<int>(out);
}
int NpuRuntime::Allocate(uint32_t flags, int64_t size, uint64_t* address) {
    if (flags != 0x1000 || size <= 0 || size > QCOM_NPU_MAX_HEAP || !address) return AEE_EBADPARM;
    QCOM_NPU_ALLOCATION input{ QCOM_NPU_ABI, flags, (static_cast<ULONG>(size) + 4095) & ~4095u, 0, 0 }, output{};
    DWORD used = 0;
    if (!Io(file_, QCOM_NPU_ALLOCATE, &input, sizeof(input), &output, sizeof(output), used) || used != sizeof(output)) {
        std::cerr << "CDSP heap expansion failed: bytes=" << size << " Windows error=" << GetLastError() << '\n'; return AEE_ERPC;
    }
    std::lock_guard<std::mutex> guard(allocationMutex_);
    allocations_[output.DspAddress] = output; *address = output.DspAddress;
    return 0;
}
int NpuRuntime::Free(uint64_t address, int64_t size) {
    std::lock_guard<std::mutex> guard(allocationMutex_);
    auto it = allocations_.find(address);
    if (it == allocations_.end() || size <= 0 || size > it->second.Bytes) return AEE_EBADPARM;
    DWORD used = 0;
    if (!Io(file_, QCOM_NPU_FREE, &it->second, sizeof(it->second), nullptr, 0, used)) return AEE_ERPC;
    allocations_.erase(it); return 0;
}
int NpuRuntime::MapFile(const std::filesystem::path& path, int* fd, int* size) {
    if (!fd || !size) return AEE_EBADPARM;
    std::ifstream source(path, std::ios::binary | std::ios::ate);
    auto length = source.tellg();
    if (!source || length <= 0 || length > QCOM_NPU_MAX_HEAP) return AEE_ENOSUCH;
    source.seekg(0);
    QCOM_NPU_ALLOCATION in{ QCOM_NPU_ABI, 2, (static_cast<ULONG>(length) + 4095) & ~4095u, 0, 0 }, out{};
    DWORD used = 0;
    if (!Io(file_, QCOM_NPU_ALLOCATE, &in, sizeof(in), &out, sizeof(out), used) || used != sizeof(out)) {
        std::cerr << "DMA file map failed: " << GetLastError() << '\n'; return AEE_ERPC;
    }
    for (ULONG offset = 0; offset < static_cast<ULONG>(length);) {
        ULONG bytes = (std::min)(ULONG(0x10000), static_cast<ULONG>(length) - offset);
        std::vector<uint8_t> buffer(16 + bytes);
        auto* q = reinterpret_cast<QCOM_NPU_MEMORY_WRITE*>(buffer.data());
        q->Abi = QCOM_NPU_ABI; q->Id = out.Id; q->Offset = offset; q->Bytes = bytes;
        source.read(reinterpret_cast<char*>(q->Data), bytes);
        if (!source || !Io(file_, QCOM_NPU_WRITE_MEMORY, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, used))
            return AEE_ERPC; // The driver retains the owned file map until process release.
        offset += bytes;
    }
    std::lock_guard<std::mutex> guard(allocationMutex_);
    fileAllocations_[static_cast<int>(out.Id)] = out;
    *fd = static_cast<int>(out.Id); *size = static_cast<int>(length);
    return 0;
}
int NpuRuntime::MapBuffer(uint32_t size, uint32_t flags, QCOM_NPU_ALLOCATION* allocation) {
    if (!allocation || !size || size > QCOM_NPU_MAX_HEAP || flags != 3) return AEE_EBADPARM;
    QCOM_NPU_SHARED_ALLOCATION shared{};
    int error = MapShared(size, &shared, flags);
    if (!error) *allocation = shared.Allocation;
    return error;
}
int NpuRuntime::MapShared(uint32_t size, QCOM_NPU_SHARED_ALLOCATION* allocation, uint32_t flags, bool deferred) {
    if (!allocation || !size || size > (flags == 3 ? QCOM_NPU_MAX_HEAP : QCOM_NPU_MAX_SHARED) ||
        (flags != 3 && flags != QCOM_NPU_SHARED)) return AEE_EBADPARM;
    QCOM_NPU_SHARED_ALLOCATION in{{QCOM_NPU_ABI, flags, (size + 4095) & ~4095u, 0, 0}, 0}, out{};
    DWORD used = 0;
    if (!Io(file_, QCOM_NPU_SHARED_ALLOCATE, &in, sizeof(in), &out, sizeof(out), used) || used != sizeof(out) || !out.UserAddress) {
        DWORD error = GetLastError();
        try { auto state = Status(); std::cerr << "Shared NPU allocation failed: bytes=" << size << " Windows=" << error
            << " status=0x" << std::hex << state.LastStatus << " DSP=0x" << state.LastResult << std::dec << '\n'; }
        catch (...) { std::cerr << "Shared NPU allocation failed: " << error << '\n'; }
        return AEE_ERPC;
    }
    std::lock_guard<std::mutex> guard(allocationMutex_);
    fileAllocations_[static_cast<int>(out.Allocation.Id)] = out.Allocation;
    if (PagedMemory && flags == QCOM_NPU_SHARED && !deferred) {
        QCOM_NPU_SHARED_MAPPING mapping{QCOM_NPU_ABI, out.Allocation.Id, 1, 1};
        if (!Io(file_, QCOM_NPU_MAP_SHARED, &mapping, sizeof(mapping), nullptr, 0, used)) return AEE_ERPC;
    }
    if (!scheduler_) EnableScheduler();
    scheduler_->RegisterMemory(out);
    *allocation = out; return 0;
}
int NpuRuntime::MapSharedFile(int fd, bool map, bool pin) {
    if (!PagedMemory) return 0;
    QCOM_NPU_SHARED_MAPPING mapping{QCOM_NPU_ABI, static_cast<uint32_t>(fd), map ? 1u : 0u, pin ? 1u : 0u};
    DWORD used = 0;
    return Io(file_, QCOM_NPU_MAP_SHARED, &mapping, sizeof(mapping), nullptr, 0, used) ? 0 : static_cast<int>(GetLastError());
}
int NpuRuntime::DispatchQueue(const QCOM_NPU_QUEUE_DISPATCH& dispatch) {
    if (!scheduler_) EnableScheduler();
    QCOM_NPU_TICKET ticket{}; DWORD used = 0;
    auto request = dispatch;
    if (!Io(file_, QCOM_NPU_DISPATCH_QUEUE, &request, sizeof(request), &ticket, sizeof(ticket), used) || used != sizeof(ticket)) {
        std::cerr << "NPU queue submission: " << GetLastError() << '\n'; return AEE_ERPC;
    }
    scheduler_->Submit(ticket);
    if (!Io(file_, QCOM_NPU_DISPATCH_RESULT, &ticket, sizeof(ticket), nullptr, 0, used)) {
        std::cerr << "NPU queue completion: " << GetLastError() << '\n'; return AEE_ERPC;
    }
    return 0;
}
int NpuRuntime::WriteBuffer(uint32_t id, uint32_t offset, const uint8_t* data, uint32_t size) {
    if (!data || !size || size > 0x10000) return AEE_EBADPARM;
    std::vector<uint8_t> buffer(16 + size);
    auto* q = reinterpret_cast<QCOM_NPU_MEMORY_WRITE*>(buffer.data());
    q->Abi = QCOM_NPU_ABI; q->Id = id; q->Offset = offset; q->Bytes = size;
    std::memcpy(buffer.data() + 16, data, size); DWORD used = 0;
    return Io(file_, QCOM_NPU_WRITE_MEMORY, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, used) ? 0 : AEE_ERPC;
}
int NpuRuntime::ReadBuffer(uint32_t id, uint32_t offset, uint8_t* data, uint32_t size) {
    if (!data || !size || size > 0x10000) return AEE_EBADPARM;
    QCOM_NPU_MEMORY_READ q{QCOM_NPU_ABI, id, offset, size}; DWORD used = 0;
    return Io(file_, QCOM_NPU_READ_MEMORY, &q, sizeof(q), data, size, used) && used == size ? 0 : AEE_ERPC;
}
int NpuRuntime::CloseFile(int fd) {
    std::lock_guard<std::mutex> guard(allocationMutex_);
    auto it = fileAllocations_.find(fd);
    if (it == fileAllocations_.end()) return AEE_EBADPARM;
    DWORD used = 0;
    if (!Io(file_, QCOM_NPU_FREE, &it->second, sizeof(it->second), nullptr, 0, used)) return AEE_ERPC;
    if (scheduler_) scheduler_->UnregisterMemory(it->second.Id);
    if (!Io(file_, QCOM_NPU_FREE, &it->second, sizeof(it->second), nullptr, 0, used) && GetLastError() != ERROR_INVALID_PARAMETER) return AEE_ERPC;
    fileAllocations_.erase(it); return 0;
}
int NpuRuntime::FileAddress(int fd, int size, uint64_t* address) {
    std::lock_guard<std::mutex> guard(allocationMutex_);
    auto it = fileAllocations_.find(fd);
    if (!address || it == fileAllocations_.end() || size <= 0 || static_cast<ULONG>(size) > it->second.Bytes || !it->second.DspAddress)
        return AEE_EBADPARM;
    *address = it->second.DspAddress; return 0;
}
static uint32_t Get32(const std::vector<uint8_t>& b, size_t at) {
    if (at > b.size() || b.size() - at < 4) throw std::runtime_error("Truncated reverse RPC packet");
    uint32_t value; std::memcpy(&value, b.data() + at, 4); return value;
}
static void Put32(std::vector<uint8_t>& b, uint32_t value) {
    size_t at = b.size(); b.resize(at + 4); std::memcpy(b.data() + at, &value, 4);
}
void NpuRuntime::Listener() {
    uint32_t context = 0; int result = 0;
    std::vector<uint8_t> previous;
    try {
        while (!stop_) {
            uint32_t in[] = { context, static_cast<uint32_t>(result), static_cast<uint32_t>(previous.size()), 0x10000 }, out[4]{};
            std::vector<uint8_t> incoming(0x10000);
            remote_arg args[4]{};
            args[0].buf = { in, sizeof(in) }; args[1].buf = { previous.data(), previous.size() };
            args[2].buf = { out, sizeof(out) }; args[3].buf = { incoming.data(), incoming.size() };
            int callResult = Invoke(3, 0x04020200, args, true);
            if (stop_) break;
            if (callResult) throw std::runtime_error("CDSP listener failed: " + std::to_string(callResult));
            context = out[0]; uint32_t handle = out[1], scalar = out[2], required = out[3];
            if (required > QCOM_NPU_MAX_DATA - 4096) throw std::runtime_error("Reverse RPC buffer is too large");
            if (required > incoming.size()) {
                size_t old = incoming.size(); incoming.resize(required);
                uint32_t request[] = { context, static_cast<uint32_t>(old), static_cast<uint32_t>(required - old) }, needed = 0;
                remote_arg more[3]{};
                more[0].buf = { request, sizeof(request) }; more[1].buf = { &needed, sizeof(needed) };
                more[2].buf = { incoming.data() + old, required - old };
                if (Invoke(3, 0x05010200, more, true)) throw std::runtime_error("Cannot fetch reverse RPC arguments");
            }
            incoming.resize(required);
            unsigned ni = (scalar >> 16) & 255, no = (scalar >> 8) & 255;
            if ((scalar & 255) || ni + no > 16) throw std::runtime_error("Invalid reverse RPC scalar");
            remote_arg reverse[16]{}; size_t at = 0;
            for (unsigned i = 0; i < ni; i++) {
                uint32_t length = Get32(incoming, at); at += 4;
                if (length) at = (at + 7) & ~size_t(7);
                if (at > incoming.size() || length > incoming.size() - at) throw std::runtime_error("Invalid reverse RPC input length");
                reverse[i].buf = { incoming.data() + at, length }; at += length;
            }
            previous.clear(); size_t total = 0;
            for (unsigned i = 0; i < no; i++) {
                uint32_t length = Get32(incoming, at); at += 4;
                Put32(previous, length);
                if (length) previous.resize((previous.size() + 7) & ~size_t(7));
                total += length;
                if (total > QCOM_NPU_MAX_DATA - 4096) throw std::runtime_error("Invalid reverse RPC output length");
                size_t offset = previous.size(); previous.resize(offset + length);
                reverse[ni + i].buf.nLen = length;
                // Store offsets until all reallocations have finished.
                reverse[ni + i].buf.pv = reinterpret_cast<void*>(offset);
            }
            for (unsigned i = 0; i < no; i++) reverse[ni + i].buf.pv = previous.data() + reinterpret_cast<size_t>(reverse[ni + i].buf.pv);
            result = NpuHostInvoke(handle, scalar, reverse);
        }
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; stop_ = true; }
}
