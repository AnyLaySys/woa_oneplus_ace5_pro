#include "transport.h"
#include "vendor/AEEStdErr.h"
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include "../ggml/htp_iface.h"

#define NPU_RPC_EXPORT extern "C"
static HMODULE module;

NpuTransport& NpuRpcTransport() {
    // Explicit handle/queue release shuts the runtime down outside loader lock.
    static auto* transport = new NpuTransport;
    return *transport;
}
NpuRuntime& NpuTransport::Get() {
    if (!Runtime) {
        wchar_t path[32768]{};
        DWORD count = GetModuleFileNameW(module, path, 32768);
        if (!count || count >= 32768) throw std::runtime_error("Cannot locate the NPU transport");
        Runtime = std::make_shared<NpuRuntime>(std::filesystem::path(path).parent_path() / L"DSP250");
    }
    return *Runtime;
}
NpuSharedBuffer& NpuTransport::Buffer(int fd) {
    auto it = Buffers.find(fd);
    if (it == Buffers.end()) throw std::runtime_error("Unknown shared NPU buffer");
    return it->second;
}
void NpuTransport::ReleaseIfIdle() {
    if (!Queues && Handles.empty() && Buffers.empty()) Runtime.reset();
}

NPU_RPC_EXPORT void* rpcmem_alloc2(int heap, uint32_t, size_t size) {
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    try {
        if (heap != 25 || !size || size > QCOM_NPU_MAX_SHARED) return nullptr;
        NpuSharedBuffer buffer{};
        int error = owner.Get().MapShared(static_cast<uint32_t>(size), &buffer.Mapping, QCOM_NPU_SHARED, true);
        if (error) return nullptr;
        buffer.Size = size;
        owner.Buffers.emplace(static_cast<int>(buffer.Mapping.Allocation.Id), buffer);
        return reinterpret_cast<void*>(buffer.Mapping.UserAddress);
    } catch (const std::exception& error) { std::cerr << "NPU shared memory: " << error.what() << '\n'; return nullptr; }
}
NPU_RPC_EXPORT void* rpcmem_alloc(int heap, uint32_t flags, int size) {
    return size > 0 ? rpcmem_alloc2(heap, flags, static_cast<size_t>(size)) : nullptr;
}
NPU_RPC_EXPORT int rpcmem_to_fd(void* pointer) {
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    for (const auto& entry : owner.Buffers)
        if (entry.second.Mapping.UserAddress == reinterpret_cast<uint64_t>(pointer)) return entry.first;
    return -1;
}
NPU_RPC_EXPORT void rpcmem_free(void* pointer) {
    if (!pointer) return;
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    for (auto it = owner.Buffers.begin(); it != owner.Buffers.end(); ++it) {
        if (it->second.Mapping.UserAddress != reinterpret_cast<uint64_t>(pointer)) continue;
        if (it->second.References || !owner.Runtime || owner.Runtime->CloseFile(it->first)) {
            std::cerr << "NPU retains a buffer still owned by DSP\n"; return;
        }
        owner.Buffers.erase(it); owner.ReleaseIfIdle(); return;
    }
}
NPU_RPC_EXPORT int fastrpc_mmap(int domain, int fd, void* pointer, int offset, size_t size, fastrpc_map_flags flags) {
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    try {
        auto& buffer = owner.Buffer(fd);
        if (domain != CDSP_DOMAIN_ID || offset || !size || size > buffer.Size ||
            reinterpret_cast<uint64_t>(pointer) != buffer.Mapping.UserAddress ||
            (flags != FASTRPC_MAP_FD && flags != FASTRPC_MAP_FD_DELAYED)) return AEE_EBADPARM;
        if (flags == FASTRPC_MAP_FD) {
            int error = owner.Get().MapSharedFile(fd, true, true);
            if (error) return AEE_ERPC;
        }
        return 0;
    } catch (...) { return AEE_EBADPARM; }
}
NPU_RPC_EXPORT int fastrpc_munmap(int domain, int fd, void* pointer, size_t size) {
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    try {
        auto& buffer = owner.Buffer(fd);
        if (domain != CDSP_DOMAIN_ID || size != buffer.Size || reinterpret_cast<uint64_t>(pointer) != buffer.Mapping.UserAddress)
            return AEE_EBADPARM;
        if (owner.Get().MapSharedFile(fd, false)) return AEE_ERPC;
        return 0;
    } catch (...) { return AEE_EBADPARM; }
}
NPU_RPC_EXPORT int remote_handle64_open(const char* uri, remote_handle64* handle) {
    if (!uri || !handle) return AEE_EBADPARM;
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    try {
        std::string name(uri);
        auto at = name.find("&_dom="); if (at != std::string::npos) name.erase(at);
        at = name.find("&_session="); if (at != std::string::npos) name.erase(at);
        uint32_t h = 0; int error = owner.Get().Open(name.c_str(), &h);
        if (!error) {
            owner.Handles.insert(h); *handle = h;
            if (name.find("libggml-htp-") != std::string::npos) owner.ComputeHandle = h;
        }
        return error;
    } catch (const std::exception& error) { std::cerr << "NPU module load: " << error.what() << '\n'; return AEE_ERPC; }
}
NPU_RPC_EXPORT int remote_handle64_invoke(remote_handle64 handle, uint32_t scalar, remote_arg* args) {
    auto& owner = NpuRpcTransport(); std::shared_ptr<NpuRuntime> runtime;
    {
        std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
        if (handle > UINT32_MAX || !owner.Handles.count(static_cast<uint32_t>(handle)) || !owner.Runtime) return AEE_EBADPARM;
        runtime = owner.Runtime;
    }
    try { return runtime->Invoke(static_cast<uint32_t>(handle), scalar, args); }
    catch (const std::exception& error) { std::cerr << "NPU RPC: " << error.what() << '\n'; return AEE_ERPC; }
}
NPU_RPC_EXPORT int remote_handle64_close(remote_handle64 handle) {
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    if (handle > UINT32_MAX || !owner.Handles.count(static_cast<uint32_t>(handle)) || !owner.Runtime) return AEE_EBADPARM;
    try {
        int error = owner.Runtime->Close(static_cast<uint32_t>(handle));
        if (!error) {
            owner.Handles.erase(static_cast<uint32_t>(handle));
            if (owner.ComputeHandle == handle) owner.ComputeHandle = 0;
            owner.ReleaseIfIdle();
        }
        return error;
    } catch (...) { return AEE_ERPC; }
}
NPU_RPC_EXPORT int remote_handle_control(uint32_t request, void* data, uint32_t size) {
    if (request != DSPRPC_GET_DSP_INFO || !data || size != sizeof(remote_dsp_capability)) return AEE_EUNSUPPORTED;
    auto& capability = *static_cast<remote_dsp_capability*>(data);
    if (capability.domain != CDSP_DOMAIN_ID || capability.attribute_ID >= 256) return AEE_EBADPARM;
    auto& owner = NpuRpcTransport(); std::lock_guard<std::recursive_mutex> guard(owner.Mutex);
    try { capability.capability = owner.Get().Capabilities[capability.attribute_ID]; return 0; }
    catch (...) { return AEE_ERPC; }
}
NPU_RPC_EXPORT int remote_handle64_control(remote_handle64, uint32_t request, void*, uint32_t) {
    return request == DSPRPC_CONTROL_LATENCY ? 0 : AEE_EUNSUPPORTED;
}
NPU_RPC_EXPORT int remote_session_control(uint32_t request, void* data, uint32_t size) {
    if (!data) return AEE_EBADPARM;
    if (request == DSPRPC_CONTROL_UNSIGNED_MODULE && size == sizeof(remote_rpc_control_unsigned_module)) {
        auto& control = *static_cast<remote_rpc_control_unsigned_module*>(data);
        return control.domain == CDSP_DOMAIN_ID && control.enable == 1 ? 0 : AEE_EBADPARM;
    }
    if (request == FASTRPC_GET_EFFECTIVE_DOMAIN_ID && size == sizeof(remote_rpc_effective_domain_id)) {
        auto& control = *static_cast<remote_rpc_effective_domain_id*>(data);
        if (!control.domain_name || control.session_id || control.domain_name_len != 4 || std::memcmp(control.domain_name, "cdsp", 4)) return AEE_EUNSUPPORTED;
        control.effective_domain_id = CDSP_DOMAIN_ID; return 0;
    }
    if (request == FASTRPC_GET_URI && size == sizeof(remote_rpc_get_uri)) {
        auto& control = *static_cast<remote_rpc_get_uri*>(data);
        if (!control.module_uri || !control.uri || !control.domain_name || control.session_id ||
            control.domain_name_len != 4 || std::memcmp(control.domain_name, "cdsp", 4) ||
            !control.module_uri_len || control.module_uri_len >= control.uri_len) return AEE_EBADPARM;
        std::memcpy(control.uri, control.module_uri, control.module_uri_len);
        control.uri[control.module_uri_len] = 0; return 0;
    }
    return AEE_EUNSUPPORTED;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { module = instance; DisableThreadLibraryCalls(instance); }
    return TRUE;
}
