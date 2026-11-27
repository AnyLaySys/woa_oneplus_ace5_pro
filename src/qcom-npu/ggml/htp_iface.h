#pragma once
#include <remote.h>
#include <cstring>

struct htp_iface_pmu_conf { uint32_t events[8]; };

inline int htp_iface_open(const char* uri, remote_handle64* handle) { return remote_handle64_open(uri, handle); }
inline int htp_iface_close(remote_handle64 handle) { return remote_handle64_close(handle); }
inline int htp_iface_call(remote_handle64 handle, uint32_t method, void* input, size_t size, void* output = nullptr, size_t bytes = 0) {
    remote_arg arguments[2]{}; unsigned count = 0;
    if (size) arguments[count++].buf = {input, size};
    if (bytes) arguments[count++].buf = {output, bytes};
    return remote_handle64_invoke(handle, (method << 24) | (size ? 0x10000 : 0) | (bytes ? 0x100 : 0), count ? arguments : nullptr);
}
inline int htp_iface_start(remote_handle64 handle, uint32_t session, uint64_t queue, uint32_t hvx, uint32_t hmx,
    uint64_t vtcm, uint64_t vmem, uint32_t power) {
    uint8_t arguments[44]{};
    std::memcpy(arguments, &session, 4); std::memcpy(arguments + 8, &queue, 8);
    std::memcpy(arguments + 16, &hvx, 4); std::memcpy(arguments + 20, &hmx, 4);
    std::memcpy(arguments + 24, &vtcm, 8); std::memcpy(arguments + 32, &vmem, 8); std::memcpy(arguments + 40, &power, 4);
    return htp_iface_call(handle, 2, arguments, sizeof(arguments));
}
inline int htp_iface_stop(remote_handle64 handle) { return htp_iface_call(handle, 3, nullptr, 0); }
inline int htp_iface_mmap(remote_handle64 handle, uint32_t fd, uint64_t size) {
    uint8_t arguments[16]{}; std::memcpy(arguments, &fd, 4); std::memcpy(arguments + 8, &size, 8);
    return htp_iface_call(handle, 4, arguments, sizeof(arguments));
}
inline int htp_iface_munmap(remote_handle64 handle, uint32_t fd) { return htp_iface_call(handle, 5, &fd, sizeof(fd)); }
inline int htp_iface_profiler(remote_handle64 handle, uint32_t mode, htp_iface_pmu_conf* pmu) {
    uint32_t arguments[9]{}; arguments[0] = mode;
    if (pmu) std::memcpy(arguments + 1, pmu, sizeof(*pmu));
    return htp_iface_call(handle, 6, arguments, sizeof(arguments));
}
inline int htp_iface_etm(remote_handle64 handle, uint32_t enable) { return htp_iface_call(handle, 7, &enable, sizeof(enable)); }
inline int htp_iface_hwinfo(remote_handle64 handle, uint32_t* threads, uint32_t* hvx, uint32_t* hmx, uint64_t* vtcm) {
    uint8_t result[24]{};
    int error = htp_iface_call(handle, 8, nullptr, 0, result, sizeof(result));
    if (!error) { std::memcpy(threads, result, 4); std::memcpy(hvx, result + 4, 4); std::memcpy(hmx, result + 8, 4); std::memcpy(vtcm, result + 16, 8); }
    return error;
}
