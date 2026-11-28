#include <onnxruntime_c_api.h>
#include "../runtime/context.h"
#include "../runtime/installed.h"
#include "graph.h"
#include <stdexcept>
#include <cstring>

static constexpr const char* Name = "QcomHtpExecutionProvider";
static std::filesystem::path DspDirectory() {
    return qcom_npu::InstalledDirectory() / L"DSP250";
}
static uint32_t TargetSoc() {
    HANDLE file = CreateFileW(L"\\\\.\\QcomNpu", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("NPU driver unavailable: " + std::to_string(GetLastError()));
    QCOM_NPU_STATUS status{}; DWORD used = 0;
    BOOL ready = DeviceIoControl(file, QCOM_NPU_QUERY, nullptr, 0, &status, sizeof(status), &used, nullptr);
    uint32_t soc = ready && used == sizeof(status) && (status.Flags & 7) == 7 ? status.SocId : 0;
    CloseHandle(file);
    if (!soc) throw std::runtime_error("Cannot verify connected NPU hardware");
    return soc;
}
static std::vector<uint8_t> Attribute(const OrtApi& api, const OrtNode* node, const char* name) {
    const OrtOpAttr* attribute = nullptr;
    Check(api, api.Node_GetAttributeByName(node, name, &attribute));
    if (!attribute) throw std::runtime_error(std::string("Missing HTP attribute: ") + name);
    size_t size = 0; auto status = api.ReadOpAttr(attribute, ORT_OP_ATTR_STRING, nullptr, 0, &size);
    if (status) api.ReleaseStatus(status);
    if (!size || size > QCOM_NPU_MAX_DATA) throw std::runtime_error("Invalid HTP attribute size");
    std::vector<uint8_t> bytes(size);
    Check(api, api.ReadOpAttr(attribute, ORT_OP_ATTR_STRING, bytes.data(), bytes.size(), &size));
    bytes.resize(size); return bytes;
}
struct Shape {
    std::vector<int64_t> Dims;
    size_t Bytes = 2;
    static Shape Read(const OrtApi& api, const OrtValueInfo* value) {
        const OrtTypeInfo* type = nullptr; const OrtTensorTypeAndShapeInfo* info = nullptr;
        Check(api, api.GetValueInfoTypeInfo(value, &type)); Check(api, api.CastTypeInfoToTensorInfo(type, &info));
        ONNXTensorElementDataType dataType{}; Check(api, api.GetTensorElementType(info, &dataType));
        if (dataType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16) throw std::runtime_error("Native HTP context requires FP16 tensors");
        size_t rank = 0; Check(api, api.GetDimensionsCount(info, &rank));
        if (!rank || rank > 8) throw std::runtime_error("Invalid HTP tensor rank");
        Shape shape; shape.Dims.resize(rank); Check(api, api.GetDimensions(info, shape.Dims.data(), rank));
        for (auto dim : shape.Dims) {
            if (dim <= 0 || static_cast<uint64_t>(dim) > QCOM_NPU_MAX_DATA / shape.Bytes) throw std::runtime_error("HTP context needs bounded static tensor dimensions");
            shape.Bytes *= static_cast<size_t>(dim);
        }
        return shape;
    }
};
struct ComputeInfo final : NpuComputeInfo {
    const OrtApi& Api;
    Shape Input, Output;
    std::vector<uint8_t> Binary;
    struct State {
        NpuRuntime Runtime;
        NpuContext Context;
        State(const std::vector<uint8_t>& binary) : Runtime(DspDirectory()), Context(Runtime, binary) {
            Runtime.EnableScheduler();
        }
    };
    ComputeInfo(const OrtApi& api, const OrtNode* node) : Api(api) {
        ort_version_supported = ORT_API_VERSION;
        CreateState = [](OrtNodeComputeInfo* base, OrtNodeComputeContext*, void** state) noexcept -> OrtStatus* {
            auto& self = *static_cast<ComputeInfo*>(base);
            try { *state = new State(self.Binary); return nullptr; }
            catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
        ReleaseState = [](OrtNodeComputeInfo*, void* state) noexcept { delete static_cast<State*>(state); };
        Compute = [](OrtNodeComputeInfo* base, void* pointer, OrtKernelContext* context) noexcept -> OrtStatus* {
            auto& self = *static_cast<ComputeInfo*>(base);
            try {
                auto& state = *static_cast<State*>(pointer); const OrtValue* input = nullptr; const void* data = nullptr;
                Check(self.Api, self.Api.KernelContext_GetInput(context, 0, &input));
                Check(self.Api, self.Api.GetTensorData(input, &data));
                OrtTensorTypeAndShapeInfo* inputInfo = nullptr;
                Check(self.Api, self.Api.GetTensorTypeAndShape(input, &inputInfo));
                size_t count = 0; auto error = self.Api.GetTensorShapeElementCount(inputInfo, &count); self.Api.ReleaseTensorTypeAndShapeInfo(inputInfo); Check(self.Api, error);
                if (count != self.Input.Bytes / 2) throw std::runtime_error("HTP input shape does not match compiled context");
                std::vector<uint8_t> values; state.Context.Execute(static_cast<const uint8_t*>(data), static_cast<uint32_t>(self.Input.Bytes), values);
                if (values.size() != self.Output.Bytes) throw std::runtime_error("HTP output size does not match compiled context");
                OrtValue* output = nullptr; void* destination = nullptr;
                Check(self.Api, self.Api.KernelContext_GetOutput(context, 0, self.Output.Dims.data(), self.Output.Dims.size(), &output));
                Check(self.Api, self.Api.GetTensorMutableData(output, &destination));
                std::memcpy(destination, values.data(), values.size()); return nullptr;
            } catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
        size_t inputs = 0, outputs = 0;
        Check(Api, Api.Node_GetNumInputs(node, &inputs)); Check(Api, Api.Node_GetNumOutputs(node, &outputs));
        if (inputs != 1 || outputs != 1) throw std::runtime_error("Native HTP context currently needs one input and output");
        const OrtValueInfo* in = nullptr; const OrtValueInfo* out = nullptr;
        Check(Api, Api.Node_GetInputs(node, &in, 1)); Check(Api, Api.Node_GetOutputs(node, &out, 1));
        Input = Shape::Read(Api, in); Output = Shape::Read(Api, out); Binary = Attribute(Api, node, "ep_cache_context");
        if (Binary.size() < 64) throw std::runtime_error("Invalid compiled HTP context");
        uint32_t inputBytes = 0, outputBytes = 0;
        std::memcpy(&inputBytes, Binary.data() + 32, 4); std::memcpy(&outputBytes, Binary.data() + 44, 4);
        if (Input.Bytes != inputBytes || Output.Bytes != outputBytes) throw std::runtime_error("ONNX interface differs from compiled HTP context");
    }
};
struct Provider final : OrtEp {
    const OrtApi& Api;
    const OrtEpApi& EpApi;
    Provider(const OrtApi& api) : OrtEp{}, Api(api), EpApi(*api.GetEpApi()) {
        ort_version_supported = ORT_API_VERSION;
        GetName = [](const OrtEp*) noexcept { return Name; };
        GetCapability = [](OrtEp* base, const OrtGraph* graph, OrtEpGraphSupportInfo* support) noexcept -> OrtStatus* {
            auto& self = *static_cast<Provider*>(base);
            try {
                size_t count = 0; Check(self.Api, self.Api.Graph_GetNumNodes(graph, &count));
                std::vector<const OrtNode*> nodes(count); Check(self.Api, self.Api.Graph_GetNodes(graph, nodes.data(), count));
                for (auto* node : nodes) {
                    const char* type = nullptr; const char* domain = nullptr;
                    Check(self.Api, self.Api.Node_GetOperatorType(node, &type)); Check(self.Api, self.Api.Node_GetDomain(node, &domain));
                    if (std::strcmp(type, "EPContext") || std::strcmp(domain, "com.microsoft")) {
                        if (NpuSupportsNode(self.Api, node)) Check(self.Api, self.EpApi.EpGraphSupportInfo_AddNodesToFuse(support, &node, 1, nullptr));
                        continue;
                    }
                    auto source = Attribute(self.Api, node, "source"); source.push_back(0);
                    if (std::strcmp(reinterpret_cast<const char*>(source.data()), Name)) continue;
                    Check(self.Api, self.EpApi.EpGraphSupportInfo_AddNodesToFuse(support, &node, 1, nullptr));
                }
                return nullptr;
            } catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
        Compile = [](OrtEp* base, const OrtGraph** graphs, const OrtNode**, size_t count, OrtNodeComputeInfo** infos, OrtNode**) noexcept -> OrtStatus* {
            auto& self = *static_cast<Provider*>(base); size_t created = 0;
            try {
                for (; created < count; created++) {
                    size_t nodes = 0; Check(self.Api, self.Api.Graph_GetNumNodes(graphs[created], &nodes));
                    if (nodes != 1) throw std::runtime_error("Expected a compiled HTP context node");
                    const OrtNode* node = nullptr; Check(self.Api, self.Api.Graph_GetNodes(graphs[created], &node, 1));
                    const char* type = nullptr; Check(self.Api, self.Api.Node_GetOperatorType(node, &type));
                    infos[created] = !std::strcmp(type, "EPContext") ? static_cast<NpuComputeInfo*>(new ComputeInfo(self.Api, node)) : NpuCompileNode(self.Api, graphs[created]);
                }
                return nullptr;
            } catch (const std::exception& error) {
                for (size_t i = 0; i < created; i++) { delete static_cast<NpuComputeInfo*>(infos[i]); infos[i] = nullptr; }
                return self.Api.CreateStatus(ORT_EP_FAIL, error.what());
            }
        };
        ReleaseNodeComputeInfos = [](OrtEp*, OrtNodeComputeInfo** infos, size_t count) noexcept {
            for (size_t i = 0; i < count; i++) delete static_cast<NpuComputeInfo*>(infos[i]);
        };
    }
};
struct Factory final : OrtEpFactory {
    const OrtApi& Api;
    OrtHardwareDevice* Hardware = nullptr;
    Factory(const OrtApi& api) : OrtEpFactory{}, Api(api) {
        ort_version_supported = ORT_API_VERSION;
        GetName = [](const OrtEpFactory*) noexcept { return Name; };
        GetVendor = [](const OrtEpFactory*) noexcept { return "AnyLaySys"; };
        GetVendorId = [](const OrtEpFactory*) noexcept { return uint32_t(0x17cb); };
        GetVersion = [](const OrtEpFactory*) noexcept { return "1.0.0"; };
        // ORT tensors remain in host staging memory. NpuContext copies them
        // into real driver-owned DMA objects before executing on HTP.
        CreateAllocator = [](OrtEpFactory*, const OrtMemoryInfo*, const OrtKeyValuePairs*, OrtAllocator** allocator) noexcept -> OrtStatus* {
            *allocator = nullptr; return nullptr;
        };
        ReleaseAllocator = [](OrtEpFactory*, OrtAllocator*) noexcept {};
        CreateDataTransfer = [](OrtEpFactory*, OrtDataTransferImpl** transfer) noexcept -> OrtStatus* {
            *transfer = nullptr; return nullptr;
        };
        IsStreamAware = [](const OrtEpFactory*) noexcept { return false; };
        CreateSyncStreamForDevice = [](OrtEpFactory*, const OrtMemoryDevice*, const OrtKeyValuePairs*, OrtSyncStreamImpl** stream) noexcept -> OrtStatus* {
            *stream = nullptr; return nullptr;
        };
        GetSupportedDevices = [](OrtEpFactory* base, const OrtHardwareDevice* const*, size_t, OrtEpDevice** devices, size_t maximum, size_t* count) noexcept -> OrtStatus* {
            auto& self = *static_cast<Factory*>(base); *count = 0;
            try {
                uint32_t soc = TargetSoc(); if (!maximum) throw std::runtime_error("No HTP device capacity");
                if (!self.Hardware) Check(self.Api, self.Api.GetEpApi()->CreateHardwareDevice(OrtHardwareDeviceType_NPU, 0x17cb, soc, "Qualcomm", nullptr, &self.Hardware));
                Check(self.Api, self.Api.GetEpApi()->CreateEpDevice(&self, self.Hardware, nullptr, nullptr, &devices[0])); *count = 1;
                return nullptr;
            } catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
        CreateEp = [](OrtEpFactory* base, const OrtHardwareDevice* const*, const OrtKeyValuePairs* const*, size_t count, const OrtSessionOptions*, const OrtLogger*, OrtEp** provider) noexcept -> OrtStatus* {
            auto& self = *static_cast<Factory*>(base);
            try { if (count != 1) throw std::runtime_error("Select one physical HTP device"); *provider = new Provider(self.Api); return nullptr; }
            catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
        ReleaseEp = [](OrtEpFactory*, OrtEp* provider) noexcept { delete static_cast<Provider*>(provider); };
    }
    ~Factory() { if (Hardware) Api.GetEpApi()->ReleaseHardwareDevice(Hardware); }
};
extern "C" __declspec(dllexport) OrtStatus* CreateEpFactories(const char*, const OrtApiBase* base, const OrtLogger*, OrtEpFactory** factories, size_t maximum, size_t* count) {
    const OrtApi* api = base->GetApi(ORT_API_VERSION);
    if (!api) return base->GetApi(1)->CreateStatus(ORT_FAIL, "ONNX Runtime 1.24 or newer is required");
    try { if (!maximum || !factories || !count) throw std::runtime_error("Invalid HTP factory capacity"); factories[0] = new Factory(*api); *count = 1; return nullptr; }
    catch (const std::exception& error) { return api->CreateStatus(ORT_FAIL, error.what()); }
}
extern "C" __declspec(dllexport) OrtStatus* ReleaseEpFactory(OrtEpFactory* factory) { delete static_cast<Factory*>(factory); return nullptr; }
