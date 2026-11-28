#include "rpc.h"
#include "vendor/apps_std.h"
#include "vendor/apps_mem.h"
#include "vendor/AEEStdErr.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <iostream>
#include <climits>

extern "C" int apps_std_skel_invoke(uint32_t scalar, remote_arg* args);
extern "C" int apps_mem_skel_invoke(uint32_t scalar, remote_arg* args);
static std::filesystem::path directory;
static NpuRuntime* runtime;
static std::map<int, FILE*> files;
static int nextFile = 10;
void NpuFilesStart(const std::filesystem::path& root, NpuRuntime* owner) {
    directory = std::filesystem::absolute(root); runtime = owner;
}
void NpuFilesStop() {
    for (auto& f : files) std::fclose(f.second);
    files.clear(); runtime = nullptr;
}
static FILE* Find(int id) {
    auto it = files.find(id); return it == files.end() ? nullptr : it->second;
}
static std::filesystem::path Path(const char* name) {
    std::string value(name);
    auto at = value.find_last_of("/\\");
    auto base = at == std::string::npos ? value : value.substr(at + 1);
    if (base.empty() || base == "." || base == ".." || base.find(':') != std::string::npos)
        throw std::runtime_error("Invalid DSP filename");
    return directory / std::filesystem::u8path(base);
}
extern "C" {
int apps_std_fopen(const char* name, const char* mode, apps_std_FILE* out) {
    if (!name || !mode || !out || (std::strcmp(mode, "r") && std::strcmp(mode, "rb"))) return AEE_EBADPARM;
    FILE* file = nullptr;
    try { file = _wfopen(Path(name).c_str(), L"rb"); } catch (...) { return AEE_EBADPARM; }
    if (!file) { std::cerr << "DSP file unavailable: " << name << '\n'; return AEE_ENOSUCH; }
    *out = nextFile++; files[*out] = file;
    return 0;
}
int apps_std_fopen_with_env(const char*, const char*, const char* name, const char* mode, apps_std_FILE* out) {
    return apps_std_fopen(name, mode, out);
}
int apps_std_fclose(apps_std_FILE id) {
    auto it = files.find(id); if (it == files.end()) return AEE_EBADPARM;
    int result = std::fclose(it->second); files.erase(it); return result ? AEE_EFILE : 0;
}
int apps_std_fflush(apps_std_FILE id) { return Find(id) ? 0 : AEE_EBADPARM; }
int apps_std_fread(apps_std_FILE id, unsigned char* data, int size, int* bytes, int* eof) {
    auto f = Find(id); if (!f || size < 0 || !bytes || !eof || (size && !data)) return AEE_EBADPARM;
    *bytes = static_cast<int>(std::fread(data, 1, static_cast<size_t>(size), f)); *eof = std::feof(f) != 0;
    return std::ferror(f) ? AEE_EFILE : 0;
}
int apps_std_fseek(apps_std_FILE id, int offset, apps_std_SEEK whence) {
    auto f = Find(id); if (!f || whence < 0 || whence > 2) return AEE_EBADPARM;
    return _fseeki64(f, offset, static_cast<int>(whence)) ? AEE_EFILE : 0;
}
int apps_std_ftell(apps_std_FILE id, int* pos) {
    auto f = Find(id); if (!f || !pos) return AEE_EBADPARM;
    auto offset = _ftelli64(f); if (offset < 0 || offset > INT_MAX) return AEE_EFILE;
    *pos = static_cast<int>(offset); return 0;
}
int apps_std_flen(apps_std_FILE id, uint64_t* length) {
    auto f = Find(id); if (!f || !length) return AEE_EBADPARM;
    auto offset = _ftelli64(f);
    if (offset < 0 || _fseeki64(f, 0, SEEK_END)) return AEE_EFILE;
    auto size = _ftelli64(f);
    if (_fseeki64(f, offset, SEEK_SET) || size < 0) return AEE_EFILE;
    *length = static_cast<uint64_t>(size); return 0;
}
int apps_std_rewind(apps_std_FILE id) { auto f = Find(id); if (!f) return AEE_EBADPARM; std::rewind(f); return 0; }
int apps_std_feof(apps_std_FILE id, int* out) { auto f = Find(id); if (!f || !out) return AEE_EBADPARM; *out = std::feof(f); return 0; }
int apps_std_ferror(apps_std_FILE id, int* out) { auto f = Find(id); if (!f || !out) return AEE_EBADPARM; *out = std::ferror(f); return 0; }
int apps_std_clearerr(apps_std_FILE id) { auto f = Find(id); if (!f) return AEE_EBADPARM; std::clearerr(f); return 0; }
int apps_std_fgetpos(apps_std_FILE id, unsigned char* pos, int size, int* needed) {
    auto f = Find(id); if (!f || size < 0 || !needed) return AEE_EBADPARM;
    auto at = _ftelli64(f); if (at < 0) return AEE_EFILE;
    *needed = sizeof(at); if (size && pos) std::memcpy(pos, &at, (std::min)(static_cast<size_t>(size), sizeof(at)));
    return 0;
}
int apps_std_fsetpos(apps_std_FILE id, const unsigned char* pos, int size) {
    auto f = Find(id); if (!f || !pos || size != sizeof(int64_t)) return AEE_EBADPARM;
    int64_t offset; std::memcpy(&offset, pos, sizeof(offset)); return _fseeki64(f, offset, SEEK_SET) ? AEE_EFILE : 0;
}
int apps_std_getenv(const char* name, char* value, int size, int* needed) {
    if (!name || size < 0 || !needed) return AEE_EBADPARM;
    const char* result = "";
    if (!std::strcmp(name, "ADSP_LIBRARY_PATH") || !std::strcmp(name, "DSP_LIBRARY_PATH")) result = "/npu";
    else return AEE_ENOSUCH;
    *needed = static_cast<int>(std::strlen(result) + 1);
    if (size && value) { std::strncpy(value, result, static_cast<size_t>(size)); value[size - 1] = 0; }
    return 0;
}
int apps_std_get_search_paths_with_env(const char*, const char*, _cstring1_t* paths, int count, uint32_t* number, uint16_t* maximum) {
    if (!number || !maximum || count < 0) return AEE_EBADPARM;
    *number = 1; *maximum = 5;
    if (count && paths && paths[0].data && paths[0].dataLen >= 5) std::memcpy(paths[0].data, "/npu", 5);
    return 0;
}
int apps_std_fileExists(const char* name, bool* exists) {
    if (!name || !exists) return AEE_EBADPARM;
    try { *exists = std::filesystem::is_regular_file(Path(name)); return 0; } catch (...) { return AEE_EBADPARM; }
}
int apps_std_stat(const char* name, apps_std_STAT* stat) {
    if (!name || !stat) return AEE_EBADPARM;
    try {
        auto p = Path(name); if (!std::filesystem::is_regular_file(p)) return AEE_ENOSUCH;
        *stat = {}; stat->tsz = sizeof(*stat); stat->size = std::filesystem::file_size(p); stat->mode = 0100444; stat->nlink = 1;
        return 0;
    } catch (...) { return AEE_EBADPARM; }
}
int apps_std_print_string(const char* text) { if (!text) return AEE_EBADPARM; std::cerr << "DSP: " << text; return 0; }
int apps_std_fopen_fd(const char* name, const char* mode, int* fd, int* length) {
    if (!runtime || !name || !mode || (std::strcmp(mode, "r") && std::strcmp(mode, "rb"))) return AEE_EBADPARM;
    try { return runtime->MapFile(Path(name), fd, length); } catch (...) { return AEE_EFILE; }
}
int apps_std_fopen_with_env_fd(const char*, const char*, const char* name, const char* mode, int* fd, int* length) {
    return apps_std_fopen_fd(name, mode, fd, length);
}
int apps_std_fclose_fd(int fd) { return runtime ? runtime->CloseFile(fd) : AEE_EBADPARM; }
int apps_mem_request_map64(int, uint32_t, uint32_t flags, uint64_t vin, int64_t size, uint64_t* host, uint64_t* dsp) {
    if (!runtime || vin || !host || !dsp) return AEE_EBADPARM;
    *host = 0; *dsp = 0; return runtime->Allocate(flags, size, dsp);
}
int apps_mem_request_unmap64(uint64_t dsp, int64_t size) { return runtime ? runtime->Free(dsp, size) : AEE_EBADPARM; }
int apps_mem_request_map(int heap, uint32_t ion, uint32_t flags, uint32_t vin, int32_t size, uint32_t* host, uint32_t* dsp) {
    if (!host || !dsp) return AEE_EBADPARM;
    uint64_t h = 0, d = 0; int result = apps_mem_request_map64(heap, ion, flags, vin, size, &h, &d);
    if (d > UINT32_MAX) return AEE_EBADSIZE;
    *host = static_cast<uint32_t>(h); *dsp = static_cast<uint32_t>(d); return result;
}
int apps_mem_request_unmap(uint32_t dsp, int32_t size) { return apps_mem_request_unmap64(dsp, size); }
int apps_mem_share_map(int fd, int size, uint64_t* host, uint64_t* dsp) {
    if (!runtime || !host || !dsp) return AEE_EBADPARM;
    *host = 0; return runtime->FileAddress(fd, size, dsp);
}
int apps_std_freopen(apps_std_FILE, const char*, const char*, apps_std_FILE*) { return AEE_EUNSUPPORTED; }
int apps_std_fwrite(apps_std_FILE, const unsigned char*, int, int*, int*) { return AEE_EUNSUPPORTED; }
int apps_std_setenv(const char*, const char*, int) { return AEE_EUNSUPPORTED; }
int apps_std_unsetenv(const char*) { return AEE_EUNSUPPORTED; }
int apps_std_fgets(apps_std_FILE, unsigned char*, int, int*) { return AEE_EUNSUPPORTED; }
int apps_std_fsync(apps_std_FILE) { return AEE_EUNSUPPORTED; }
int apps_std_fremove(const char*) { return AEE_EUNSUPPORTED; }
int apps_std_fdopen_decrypt(apps_std_FILE, apps_std_FILE*) { return AEE_EUNSUPPORTED; }
int apps_std_opendir(const char*, apps_std_DIR*) { return AEE_EUNSUPPORTED; }
int apps_std_closedir(const apps_std_DIR*) { return AEE_EUNSUPPORTED; }
int apps_std_readdir(const apps_std_DIR*, apps_std_DIRENT*, int*) { return AEE_EUNSUPPORTED; }
int apps_std_mkdir(const char*, int) { return AEE_EUNSUPPORTED; }
int apps_std_rmdir(const char*) { return AEE_EUNSUPPORTED; }
int apps_std_ftrunc(apps_std_FILE, int) { return AEE_EUNSUPPORTED; }
int apps_std_frename(const char*, const char*) { return AEE_EUNSUPPORTED; }
int apps_mem_share_unmap(uint64_t, int) { return AEE_EUNSUPPORTED; }
int apps_mem_dma_handle_map(int, int, int) { return AEE_EUNSUPPORTED; }
int apps_mem_dma_handle_unmap(int, int) { return AEE_EUNSUPPORTED; }
}
int NpuHostInvoke(uint32_t handle, uint32_t scalar, remote_arg* args) {
    if (handle == 100) return apps_std_skel_invoke(scalar, args);
    if (handle == 101) return apps_mem_skel_invoke(scalar, args);
    if (handle == 0 && scalar == 0x20200 && args[0].buf.nLen >= 8 && args[2].buf.nLen >= 8) {
        uint32_t length; std::memcpy(&length, args[0].buf.pv, 4);
        if (!length || length > args[1].buf.nLen) return AEE_EBADPARM;
        const char* name = static_cast<const char*>(args[1].buf.pv);
        if (name[length - 1]) return AEE_EBADPARM;
        uint32_t result[2] = { 100, 0 };
        if (!std::strcmp(name, "apps_mem")) result[0] = 101;
        else if (std::strcmp(name, "apps_std")) result[1] = AEE_EUNSUPPORTED;
        std::memcpy(args[2].buf.pv, result, sizeof(result));
        if (args[3].buf.nLen) std::memset(args[3].buf.pv, 0, args[3].buf.nLen);
        return 0;
    }
    if (handle == 0 && scalar == 0x1010200 && args[0].buf.nLen >= 8 && args[1].buf.nLen >= 4) {
        uint32_t closed; std::memcpy(&closed, args[0].buf.pv, 4);
        uint32_t error = closed == 100 || closed == 101 ? 0 : AEE_EBADPARM;
        std::memcpy(args[1].buf.pv, &error, 4);
        if (args[2].buf.nLen) std::memset(args[2].buf.pv, 0, args[2].buf.nLen);
        return 0;
    }
    return AEE_EUNSUPPORTED;
}
