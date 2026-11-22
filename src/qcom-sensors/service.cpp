#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>
#include <array>
#include <string>
#include <filesystem>
#include <sys/stat.h>
#include <io.h>
#include "rpc-abi.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include "abi.h"
namespace fs = std::filesystem;
using Bytes = std::vector<UCHAR>;
struct Span {
    UCHAR *Data;
    ULONG Size;
};
static HANDLE Stop, Device = INVALID_HANDLE_VALUE;
static SERVICE_STATUS_HANDLE ServiceHandle;
static SERVICE_STATUS ServiceStatus = { SERVICE_WIN32_OWN_PROCESS };
static fs::path Root;
static std::array<FILE *, 64> Files;
struct Directory {
    HANDLE Find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW Entry{};
    bool Valid = false;
};
static std::array<Directory, 32> Directories;
static ULONG Get(Span s, ULONG offset = 0) {
    ULONG n = 0;
    if (offset <= s.Size && sizeof(n) <= s.Size - offset) memcpy(&n, s.Data + offset, sizeof(n));
    return n;
}
static void Copy(Span s, const void *data, ULONG size, ULONG offset = 0) {
    if (offset > s.Size || size > s.Size - offset) throw EINVAL;
    if (size) memcpy(s.Data + offset, data, size);
}
static void Put(Span s, ULONG value, ULONG offset = 0) { Copy(s, &value, sizeof(value), offset); }
static std::string Text(Span s) {
    if (!s.Size || s.Data[s.Size - 1]) throw EINVAL;
    return std::string((char *)s.Data, s.Size - 1);
}
static fs::path Path(const std::string &name) {
    fs::path p = fs::u8path(name).relative_path();
    for (const auto &part : p)
        if (part == "..") throw EINVAL;
    return Root / p;
}
static void Provision() {
    wchar_t path[MAX_PATH];
    if (!ExpandEnvironmentStringsW(L"%ProgramData%\\Qualcomm\\Sensors", path, MAX_PATH)) throw EINVAL;
    Root = path;
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(101), RT_RCDATA);
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const UCHAR *p = (const UCHAR *)LockResource(loaded);
    if (!p) throw EINVAL;
    const UCHAR *end = p + SizeofResource(nullptr, resource);
    while (end - p >= 8) {
        ULONG name, size;
        memcpy(&name, p, 4);
        memcpy(&size, p + 4, 4);
        p += 8;
        if (name > (ULONG)(end - p) || size > (ULONG)(end - p) - name) throw EINVAL;
        fs::path file = Path(std::string((char *)p, name));
        p += name;
        if (!fs::exists(file)) {
            fs::create_directories(file.parent_path());
            FILE *f = nullptr;
            if (_wfopen_s(&f, file.c_str(), L"wb")) throw EINVAL;
            bool ok = fwrite(p, 1, size, f) == size;
            fclose(f);
            if (!ok) throw EINVAL;
        }
        p += size;
    }
    fs::create_directories(Path("/mnt/vendor/persist/sensors/registry/registry"));
}
static LONG Invoke(HANDLE device, ULONG handle, ULONG scalar, std::initializer_list<Span> args) {
    ULONG input = (scalar >> 16) & 255, base = FIELD_OFFSET(QCOM_RPC_PACKET, Data), size = base;
    for (auto a : args) size += a.Size;
    Bytes packet(size);
    auto q = (QCOM_RPC_PACKET *)packet.data();
    q->Handle = handle;
    q->Scalar = scalar;
    q->Count = (ULONG)args.size();
    ULONG offset = base, i = 0;
    if (q->Count > 16) return EINVAL;
    for (auto a : args) {
        q->Arg[i] = { offset, a.Size };
        if (i < input && a.Size) memcpy(packet.data() + offset, a.Data, a.Size);
        offset += a.Size;
        ++i;
    }
    OVERLAPPED operation{};
    operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!operation.hEvent) return (LONG)GetLastError();
    DWORD returned = 0;
    bool ok = DeviceIoControl(device, QCOM_RPC_INVOKE, packet.data(), size, packet.data(), size, &returned,
                  &operation) != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(device, &operation, &returned, TRUE) != FALSE;
    LONG result = ok ? (LONG)q->Result : (LONG)GetLastError();
    CloseHandle(operation.hEvent);
    if (!result) {
        i = 0;
        for (auto a : args) {
            if (i >= input && a.Size) memcpy(a.Data, packet.data() + q->Arg[i].Offset, a.Size);
            ++i;
        }
    }
    return result;
}
static int OpenFile(const std::string &name, const std::string &mode, Span output) {
    if (output.Size < 4) return EINVAL;
    for (ULONG i = 1; i < Files.size(); i++)
        if (!Files[i]) {
            std::wstring m(mode.begin(), mode.end());
            if (m.find(L'b') == std::wstring::npos) m += L'b';
            int result = _wfopen_s(&Files[i], Path(name).c_str(), m.c_str());
            if (result) return result;
            Put(output, i);
            return 0;
        }
    return EMFILE;
}
static int FileRequest(ULONG method, const std::vector<Span> &input, const std::vector<Span> &output) {
    if (input.empty()) return EINVAL;
    Span p = input[0];
    ULONG id = Get(p);
    FILE *f = id < Files.size() ? Files[id] : nullptr;
    auto out = [&](size_t i) -> Span {
        if (i >= output.size()) throw EINVAL;
        return output[i];
    };
    auto text = [&](size_t i) {
        if (i >= input.size()) throw EINVAL;
        return Text(input[i]);
    };
    switch (method) {
    case 0:
        return OpenFile(text(1), text(2), out(0));
    case 1:
        if (!f) return EBADF;
        fclose(f);
        Files[id] = nullptr;
        return OpenFile(text(1), text(2), out(0));
    case 15:
        return 0;
    case 16: {
        std::string name = text(1),
                    value = name == "ADSP_LIBRARY_PATH" ? "/vendor/lib/rfsa/adsp;/vendor/etc/sensors" : "";
        ULONG n = (ULONG)value.size() + 1;
        Put(out(0), n);
        if (out(1).Size < n) return ERANGE;
        Copy(out(1), value.c_str(), n);
        return 0;
    }
    case 17:
    case 18:
        return 0;
    case 19: {
        std::string name = text(3), mode = text(4);
        int result = OpenFile(name, mode, out(0));
        if (result == ENOENT && fs::u8path(name).is_relative())
            result = OpenFile("/vendor/etc/sensors/" + name, mode, out(0));
        return result;
    }
    case 22: {
        UCHAR exists = fs::exists(Path(text(1))) ? 1 : 0;
        Copy(out(0), &exists, 1);
        return 0;
    }
    case 24:
        return _wremove(Path(text(1)).c_str()) ? errno : 0;
    case 26: {
        if (out(0).Size < 8) return EINVAL;
        fs::path dir = Path(text(1));
        if (!fs::is_directory(dir)) return ENOENT;
        for (ULONG i = 0; i < Directories.size(); i++)
            if (Directories[i].Find == INVALID_HANDLE_VALUE) {
                auto &d = Directories[i];
                d.Find = FindFirstFileW((dir / L"*").c_str(), &d.Entry);
                d.Valid = d.Find != INVALID_HANDLE_VALUE;
                ULONGLONG n = i + 1;
                Copy(out(0), &n, 8);
                return 0;
            }
        return EMFILE;
    }
    case 27:
    case 28: {
        ULONGLONG n = 0;
        if (p.Size < 8) return EINVAL;
        memcpy(&n, p.Data, 8);
        if (!n || n > Directories.size()) return EBADF;
        auto &d = Directories[n - 1];
        if (method == 27) {
            if (d.Find != INVALID_HANDLE_VALUE) FindClose(d.Find);
            d = {};
            return 0;
        }
        while (d.Valid && (!wcscmp(d.Entry.cFileName, L".") || !wcscmp(d.Entry.cFileName, L"..")))
            d.Valid = FindNextFileW(d.Find, &d.Entry) != FALSE;
        if (!d.Valid) {
            Put(out(0), 1, 260);
            return 0;
        }
        std::string name = fs::path(d.Entry.cFileName).u8string();
        Put(out(0), (ULONG)n);
        Copy(out(0), name.c_str(), (ULONG)name.size() + 1, 4);
        d.Valid = FindNextFileW(d.Find, &d.Entry) != FALSE;
        return 0;
    }
    case 29:
        fs::create_directories(Path(text(1)));
        return 0;
    case 30:
        fs::remove(Path(text(1)));
        return 0;
    case 31:
        if (id == 33) {
            fs::rename(Path(text(1)), Path(text(2)));
            return 0;
        }
        if (id == 31) {
            struct _stat64 st{};
            if (_wstat64(Path(text(1)).c_str(), &st)) return errno;
            struct Stat {
                ULONGLONG SizeOf, Device, Inode;
                ULONG Mode, Links;
                ULONGLONG Rdev, Size;
                LONGLONG Atime, AtimeNs, Mtime, MtimeNs, Ctime, CtimeNs;
            } value = { sizeof(Stat), (ULONGLONG)st.st_dev, st.st_ino, (ULONG)st.st_mode, (ULONG)st.st_nlink,
                (ULONGLONG)st.st_rdev, (ULONGLONG)st.st_size, st.st_atime, 0, st.st_mtime, 0, st.st_ctime,
                0 };
            Copy(out(0), &value, sizeof(value));
            return 0;
        }
        return ENOSYS;
    }
    if (!f) return EBADF;
    switch (method) {
    case 2:
    case 23:
        return fflush(f) ? errno : 0;
    case 3: {
        int result = fclose(f);
        Files[id] = nullptr;
        return result ? errno : 0;
    }
    case 4: {
        Span data = out(1);
        ULONG n = (ULONG)fread(data.Data, 1, data.Size, f);
        Put(out(0), n);
        Put(out(0), (ULONG)feof(f), 4);
        return ferror(f) ? errno : 0;
    }
    case 5: {
        if (input.size() < 2) return EINVAL;
        ULONG n = (ULONG)fwrite(input[1].Data, 1, input[1].Size, f);
        Put(out(0), n);
        Put(out(0), (ULONG)feof(f), 4);
        return ferror(f) ? errno : 0;
    }
    case 6: {
        LONGLONG position = _ftelli64(f);
        Put(out(0), 8);
        if (out(1).Size < 8) return ERANGE;
        Copy(out(1), &position, 8);
        return position < 0 ? errno : 0;
    }
    case 7: {
        if (input.size() < 2 || input[1].Size < 8) return EINVAL;
        LONGLONG position;
        memcpy(&position, input[1].Data, 8);
        return _fseeki64(f, position, SEEK_SET) ? errno : 0;
    }
    case 8: {
        LONGLONG n = _ftelli64(f);
        Put(out(0), (ULONG)n);
        return n < 0 ? errno : 0;
    }
    case 9:
        return _fseeki64(f, (LONG)Get(p, 4), (int)Get(p, 8)) ? errno : 0;
    case 10: {
        struct _stat64 st{};
        if (_fstat64(_fileno(f), &st)) return errno;
        ULONGLONG n = st.st_size;
        Copy(out(0), &n, 8);
        return 0;
    }
    case 11:
        rewind(f);
        return 0;
    case 12:
        Put(out(0), (ULONG)feof(f));
        return 0;
    case 13:
        Put(out(0), (ULONG)ferror(f));
        return 0;
    case 14:
        clearerr(f);
        return 0;
    case 20: {
        Span data = out(1);
        if (data.Size && !fgets((char *)data.Data, (int)data.Size, f) && ferror(f)) return errno;
        Put(out(0), (ULONG)feof(f));
        return 0;
    }
    default:
        return ENOSYS;
    }
}
static LONG Request(ULONG handle, ULONG scalar, const UCHAR *data, ULONG size, Bytes &response) {
    ULONG inputs = (scalar >> 16) & 255, outputs = (scalar >> 8) & 255;
    if ((scalar & 255) || inputs + outputs > 16) return EINVAL;
    std::vector<Span> in, out;
    ULONG at = 0;
    response.clear();
    try {
        for (ULONG i = 0; i < inputs; i++) {
            if (at > size || size - at < 4) return EINVAL;
            ULONG n;
            memcpy(&n, data + at, 4);
            at += 4;
            if (n) at = (at + 7) & ~7u;
            if (at > size || n > size - at) return EINVAL;
            in.push_back({ (PUCHAR)data + at, n });
            at += n;
        }
        std::array<ULONG, 16> lengths{}, offsets{};
        ULONG total = 0;
        for (ULONG i = 0; i < outputs; i++) {
            if (at > size || size - at < 4) return EINVAL;
            memcpy(&lengths[i], data + at, 4);
            at += 4;
            total += 4;
            if (lengths[i]) total = (total + 7) & ~7u;
            if (total > 0x8000 || lengths[i] > 0x8000 - total) return EINVAL;
            offsets[i] = total;
            total += lengths[i];
        }
        response.resize(total);
        ULONG cursor = 0;
        for (ULONG i = 0; i < outputs; i++) {
            memcpy(response.data() + cursor, &lengths[i], 4);
            out.push_back({ response.data() + offsets[i], lengths[i] });
            cursor = offsets[i] + lengths[i];
        }
        ULONG method = (scalar >> 24) & 31;
        if (handle == 1) return FileRequest(method, in, out);
        if (handle == 0 && method == 0 && in.size() == 2 && out.size() == 2) {
            std::string name = Text(in[1]);
            ULONG id = name.find("apps_std") != std::string::npos       ? 1
                       : name.find("sns_registry") != std::string::npos ? 4
                                                                        : 0;
            if (!id) return ENOENT;
            Put(out[0], id);
            Put(out[0], 0u, 4);
            return 0;
        }
        if (handle == 0 && method == 1) return 0;
        if (handle == 4 && method == 0 && in.size() == 2 && in[0].Size >= 8 && out.size() == 1) {
            Text(in[1]);
            return 0;
        }
        return ENOSYS;
    } catch (...) {
        return EINVAL;
    }
}
struct ListenerState {
    HANDLE Device, Ready;
    LONG Error;
};
static DWORD WINAPI Listener(PVOID context) {
    auto state = (ListenerState *)context;
    state->Error = Invoke(state->Device, 7, 0x05000000, {});
    SetEvent(state->Ready);
    if (state->Error) return 1;
    ULONG previous = 0;
    LONG result = -1;
    Bytes response;
    std::array<UCHAR, 0x4000> data{};
    while (WaitForSingleObject(Stop, 0) != WAIT_OBJECT_0) {
        ULONG input[] = { previous, (ULONG)result, (ULONG)response.size(), (ULONG)data.size() },
              output[4] = { 0 };
        LONG error = Invoke(state->Device, 7, 0x06020200,
            { { (PUCHAR)input, sizeof(input) }, { response.data(), (ULONG)response.size() },
                { (PUCHAR)output, sizeof(output) }, { data.data(), (ULONG)data.size() } });
        if (error) return 1;
        previous = output[0];
        result = output[3] <= data.size() ? Request(output[1], output[2], data.data(), output[3], response)
                                          : ERANGE;
        if (result) response.clear();
    }
    return 0;
}
static void Work() {
    Provision();
    while (WaitForSingleObject(Stop, 0) != WAIT_OBJECT_0) {
        HANDLE device = CreateFileW(L"\\\\.\\AlsCharge", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        Device = device;
        if (device == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(Stop, 1000);
            continue;
        }
        ULONG pid = GetCurrentProcessId();
        LONG error = Invoke(device, 1, 0x00010000, { { (PUCHAR)&pid, sizeof(pid) } });
        HANDLE thread = nullptr;
        ListenerState state{ device, CreateEventW(nullptr, TRUE, FALSE, nullptr), 0 };
        if (!error && state.Ready) {
            thread = CreateThread(nullptr, 0, Listener, &state, 0, nullptr);
            if (thread) {
                HANDLE events[] = { Stop, state.Ready };
                DWORD wait = WaitForMultipleObjects(2, events, FALSE, 30000);
                if (wait == WAIT_OBJECT_0 + 1 && !state.Error) {
                    static const char module[] = "file:///"
                                                 "libadsp_default_listener1_skel.so?adsp_default_listener1_"
                                                 "skel_handle_invoke&_modver=1.0";
                    ULONG input[] = { sizeof(module), 128 }, output[2] = { 0 };
                    UCHAR text[128] = { 0 };
                    error = Invoke(device, 0, 0x00020200,
                        { { (PUCHAR)input, sizeof(input) }, { (PUCHAR)module, sizeof(module) },
                            { (PUCHAR)output, sizeof(output) }, { text, sizeof(text) } });
                    if (!error && !output[1]) error = Invoke(device, output[0], 0x02000000, {});
                    if (!error && !output[1]) {
                        HANDLE live[] = { Stop, thread };
                        WaitForMultipleObjects(2, live, FALSE, INFINITE);
                    }
                }
            }
        }
        CancelIoEx(device, nullptr);
        if (thread) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
        if (state.Ready) CloseHandle(state.Ready);
        CloseHandle(device);
        Device = INVALID_HANDLE_VALUE;
        for (auto &f : Files)
            if (f) {
                fclose(f);
                f = nullptr;
            }
        for (auto &d : Directories)
            if (d.Find != INVALID_HANDLE_VALUE) {
                FindClose(d.Find);
                d = {};
            }
        WaitForSingleObject(Stop, 1000);
    }
}
#define SENSOR_SERVICE
#include "hall.inc"
#undef SENSOR_SERVICE
static void Status(DWORD state, DWORD error = 0) {
    ServiceStatus.dwCurrentState = state;
    ServiceStatus.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    ServiceStatus.dwWin32ExitCode = error;
    if (ServiceHandle) SetServiceStatus(ServiceHandle, &ServiceStatus);
}
static void WINAPI Control(DWORD code) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) {
        Status(SERVICE_STOP_PENDING);
        SetEvent(Stop);
        if (Device != INVALID_HANDLE_VALUE) CancelIoEx(Device, nullptr);
    }
}
static void WINAPI Main(DWORD argc, LPWSTR *argv) {
    (void)argc;
    (void)argv;
    ServiceHandle = RegisterServiceCtrlHandlerW(L"QcomSensors", Control);
    if (!ServiceHandle) return;
    Stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!Stop) {
        Status(SERVICE_STOPPED, GetLastError());
        return;
    }
    HANDLE hall = CreateThread(nullptr, 0, HallWorker, nullptr, 0, nullptr);
    Status(SERVICE_RUNNING);
    DWORD error = 0;
    try {
        Work();
    } catch (...) {
        error = ERROR_GEN_FAILURE;
    }
    SetEvent(Stop);
    if (hall) {
        WaitForSingleObject(hall, INFINITE);
        CloseHandle(hall);
    }
    Status(SERVICE_STOPPED, error);
    CloseHandle(Stop);
}
int wmain() {
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(L"QcomSensors"), Main },
        { nullptr, nullptr } };
    return StartServiceCtrlDispatcherW(table) ? 0 : (int)GetLastError();
}
