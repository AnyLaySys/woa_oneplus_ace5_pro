"""Adapt the pinned upstream Hexagon backend to this DriverStore transport."""
from pathlib import Path
import shutil
import sys

source = Path(sys.argv[1])
npu = Path(__file__).resolve().parents[1]
backend = source / "ggml/src/ggml-hexagon"
shutil.copyfile(npu / "ggml/CMakeLists.txt", backend / "CMakeLists.txt")

path = backend / "htp-drv.cpp"
original = path.with_suffix(path.suffix + ".upstream")
if not original.exists(): shutil.copyfile(path, original)
text = original.read_text()
text = '#include "installed.h"\n' + text
text = text.replace("#include <domain.h>", "")
text = text.replace("typedef int (*remote_system_request_pfn_t)(system_req_payload * req);", "")
text = text.replace("remote_system_request_pfn_t   remote_system_request_pfn   = nullptr;", "")
start = text.index("HTPDRV_API int remote_system_request(")
end = text.index("\n#ifdef _WIN32", start)
text = text[:start] + text[end:]
start = text.index("static std::string get_driver_path()")
end = text.index("\n#endif", start)
text = text[:start] + '''static std::string get_driver_path() {
    try { return wstr_to_str(qcom_npu::InstalledDirectory().wstring()); }
    catch (...) { return {}; }
}
''' + text[end:]
text = text.replace('"libcdsprpc.dll"', '"qcom_npu_rpc.dll"')
text = "\n".join(line for line in text.split("\n") if "dlsym(handle.get(), remote_system_request_pfn_t" not in line)
start = text.index("domain * htpdrv_get_domain(")
end = text.index("int htpdrv_get_arch(", start)
text = text[:start] + text[end:]
start = text.index("std::string htp_lib_dir()")
end = text.index("int htpdrv_set_dsp_search_path(", start)
text = text[:start] + 'std::string htp_lib_dir() { return get_driver_path() + "\\\\DSP250\\\\"; }\n\n' + text[end:]
start = text.index("    fastrpc_manage_dsp_lib_path p {};")
end = text.index("\nvoid htpdrv_set_dsp_env_path(", start)
text = text[:start] + '    htpdrv_set_dsp_env_path(path, dev_name);\n    (void)domain_id;\n    return AEE_SUCCESS;\n}\n' + text[end:]
path.write_text(text)

path = backend / "htp-drv.h"
original = path.with_suffix(path.suffix + ".upstream")
if not original.exists(): shutil.copyfile(path, original)
text = original.read_text().replace("HTPDRV_API domain * htpdrv_get_domain(int domain_id);", "")
text = text.replace("HTPDRV_API int remote_system_request(system_req_payload * req);", "")
text = text.replace("__declspec(dllexport) extern", "extern").replace("__declspec(dllimport) extern", "extern")
path.write_text(text)

path = backend / "ggml-hexagon.cpp"
original = path.with_suffix(path.suffix + ".upstream")
if not original.exists(): shutil.copyfile(path, original)
text = '#include "memory-status.h"\n' + original.read_text()
text = text.replace("int    opt_power_mode = HTP_POWER_MODE_BURST;", "int    opt_power_mode = HTP_POWER_MODE_SUSTAINED_HIGH_PERFORMANCE;")
text = text.replace('{ "default",                    HTP_POWER_MODE_BURST }', '{ "default",                    HTP_POWER_MODE_SUSTAINED_HIGH_PERFORMANCE }')
text = text.replace("falling back to 'burst'", "falling back to 'sustained_high_performance'")
text = text.replace("bool   opt_hostbuf    = true;", "bool   opt_hostbuf    = false;")
text = text.replace("FASTRPC_MAP_FD_DELAYED_EXTENDED", "FASTRPC_MAP_FD_DELAYED")
text = text.replace("size_t opt_mbuf_dyn    = 512ul * 1024 * 1024;", "size_t opt_mbuf_dyn    = 64ul * 1024 * 1024;")
text = text.replace("size_t opt_mbuf_static = 1ul * 1024 * 1024 * 1024;", "size_t opt_mbuf_static = 256ul * 1024 * 1024;")
text = text.replace("n_bufs_max = HTP_OP_MAX_BUFS;", "n_bufs_max = std::min<unsigned int>(HTP_OP_MAX_BUFS, QCOM_NPU_MODEL_BUFFERS);")
# Repack in cached host memory, then copy once into the noncached DSP buffer.
text = text.replace("    htp_repack_tensor_tiled(tensor, raw.data(), raw.size());", '''    std::vector<uint8_t> packed(ggml_backend_buft_get_alloc_size(tensor->buffer->buft, tensor));
    ggml_tensor target = *tensor;
    target.data = packed.data();
    htp_repack_tensor_tiled(&target, raw.data(), raw.size());
    memcpy(tensor->data, packed.data(), packed.size());''')
start = text.index("static void ggml_backend_hexagon_device_get_memory(")
end = text.index("static enum ggml_backend_dev_type", start)
text = text[:start] + '''static void ggml_backend_hexagon_device_get_memory(ggml_backend_dev_t, size_t * free, size_t * total) {
    QCOM_NPU_MEMORY_STATUS state{};
    *free = *total = 0;
    if (!qcom_npu::MemoryStatus(state)) return;
    *total = opt_mbuf_total ? std::min<uint64_t>(state.Budget, opt_mbuf_total) : state.Budget;
    *free = std::min<uint64_t>(state.Available, *total);
    MEMORYSTATUSEX system{}; system.dwLength = sizeof(system);
    if (GlobalMemoryStatusEx(&system)) {
        const uint64_t reserve = 256ul * 1024 * 1024;
        *free = std::min<uint64_t>(*free, system.ullAvailPhys > reserve ? system.ullAvailPhys - reserve : 0);
    }
}

''' + text[end:]
text = text.replace('return "Hexagon";', 'return "Qualcomm Hexagon V79 NPU";')
# A tensor and its guard page must fit one transport allocation.
start = text.index("static bool ggml_backend_hexagon_device_supports_op(")
end = text.index("static bool ggml_backend_hexagon_device_supports_buft(", start)
text = text[:start] + text[start:end].replace("sess->max_vmem", "std::min<size_t>(sess->max_vmem, QCOM_NPU_MAX_SHARED - 4096)") + text[end:]
text = text.replace("    this->op_batch = new ggml_hexagon_opbatch", '''    QCOM_NPU_MEMORY_STATUS memory{};
    if (qcom_npu::MemoryStatus(memory) && memory.WindowBytes) {
        this->max_vmem = std::min<size_t>(this->max_vmem, memory.WindowBytes - 64ul * 1024 * 1024);
    }
    this->op_batch = new ggml_hexagon_opbatch''')
start = text.index("static void ggml_hexagon_resolve_device_domain(")
end = text.index("static void ggml_hexagon_init(", start)
text = text[:start] + '''static void ggml_hexagon_discover_devices() {
    for (size_t i = 0; i < opt_ndev; ++i) {
        auto& cfg = opt_device_configs[i];
        cfg.domain_id = cfg.physical_idx == 0 && cfg.virtual_idx == 0 ? CDSP_DOMAIN_ID : -1;
        cfg.domain_name = cfg.domain_id == CDSP_DOMAIN_ID ? CDSP_DOMAIN_NAME : "";
    }
}

''' + text[end:]
path.write_text(text)

path = source / "src/llama-model.cpp"
original = path.with_suffix(path.suffix + ".upstream")
if not original.exists(): shutil.copyfile(path, original)
text = original.read_text().replace("    pimpl->dev_input = { cpu_dev, &pimpl->cpu_buft_list };", '''    pimpl->dev_input = { cpu_dev, &pimpl->cpu_buft_list };
    const char * strict = getenv("GGML_NPU_REQUIRE_FULL_OFFLOAD");
    if (strict && strcmp(strict, "1") == 0 && act_gpu_layers > 0) {
        auto input = get_layer_buft_list(0);
        if (strncmp(ggml_backend_dev_name(input.dev), "HTP", 3) == 0) pimpl->dev_input = input;
    }''')
path.write_text(text)

path = source / "ggml/src/ggml-backend.cpp"
original = path.with_suffix(path.suffix + ".upstream")
if not original.exists(): shutil.copyfile(path, original)
text = original.read_text()
text = text.replace("    return ggml_backend_sched_compute_splits(sched);", '''    const char * strict = getenv("GGML_NPU_REQUIRE_FULL_OFFLOAD");
    if (strict && strcmp(strict, "1") == 0) {
        for (int i = 0; i < ggml_graph_n_nodes(graph); i++) {
            ggml_tensor * node = ggml_graph_node(graph, i);
            if (ggml_op_is_empty(node->op) || ggml_is_empty(node)) continue;
            ggml_backend_t backend = ggml_backend_sched_get_tensor_backend(sched, node);
            if (!backend || strncmp(ggml_backend_dev_name(ggml_backend_get_device(backend)), "HTP", 3) != 0) {
                GGML_LOG_ERROR("NPU cannot execute %s (%s); CPU fallback is disabled\\n", node->name, ggml_op_desc(node));
                return GGML_STATUS_FAILED;
            }
        }
    }
    return ggml_backend_sched_compute_splits(sched);''')
path.write_text(text)

# Host layout calculations need the size of FP16 storage, not FP16 arithmetic.
for name in ["flash-attn-ops.h", "gated-delta-net-ops.h", "matmul-ops.h", "unary-ops.h", "cpy-ops.h", "upscale-ops.h"]:
    path = backend / "htp" / name
    original = path.with_suffix(path.suffix + ".upstream")
    if not original.exists(): shutil.copyfile(path, original)
    text = original.read_text().replace("sizeof(__fp16)", "sizeof(uint16_t)").replace("sizeof(_Float16)", "sizeof(uint16_t)")
    text = text.replace("__builtin_ctz(", "NpuCountTrailingZeros(")
    text = text.replace("struct __attribute__((aligned(128)))", "struct alignas(128)")
    path.write_text(text)

# An exported source tree lives inside the WoA repository; pin its own identity.
(source / "cmake/build-info.cmake").write_text('''set(BUILD_NUMBER 12359)
set(BUILD_COMMIT "b1b770d")
set(BUILD_COMPILER "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
set(BUILD_TARGET "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
''')
