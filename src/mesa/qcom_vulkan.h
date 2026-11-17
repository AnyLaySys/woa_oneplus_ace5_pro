#include <windows.h>
#include <wchar.h>

static HMODULE qcom_load_vulkan(void)
{
    WCHAR key_name[128], paths[2 * MAX_PATH], candidate[MAX_PATH], *path, *file;
    HKEY key;
    DWORD index, size;
    HMODULE module;
#ifdef _M_ARM64
    const WCHAR *json = L"freedreno_icd.json", *dll = L"vulkan_freedreno.dll";
#else
    const WCHAR *json = L"freedreno_icd_x64.json", *dll = L"vulkan_freedreno_x64.dll";
#endif
    for (index = 0; index < 64; ++index) {
        swprintf(key_name, sizeof(key_name) / sizeof(WCHAR),
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04lu", index);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_name, 0, KEY_READ | KEY_WOW64_64KEY, &key))
            continue;
        size = sizeof(paths) - 2 * sizeof(WCHAR);
        ZeroMemory(paths, sizeof(paths));
        if (RegQueryValueExW(key, L"VulkanDriverName", NULL, NULL, (BYTE *)paths, &size))
            paths[0] = 0;
        RegCloseKey(key);
        for (path = paths; *path; path += wcslen(path) + 1) {
            file = wcsrchr(path, L'\\');
            if (!file || wcscmp(file + 1, json))
                continue;
            if ((size_t)(file + 1 - path) + wcslen(dll) >= MAX_PATH)
                continue;
            wcscpy(candidate, path);
            wcscpy(candidate + (file + 1 - path), dll);
            module = LoadLibraryExW(candidate, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (module)
                return module;
        }
    }
    return NULL;
}
