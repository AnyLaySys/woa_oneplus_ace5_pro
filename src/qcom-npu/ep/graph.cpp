#define NOMINMAX
#include "graph.h"
#include "../runtime/installed.h"
#include <ggml.h>
#include <ggml-backend.h>
#include <ggml-alloc.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

static void Check(const OrtApi& api, OrtStatus* status) {
    if (!status) return;
    std::string error = api.GetErrorMessage(status); api.ReleaseStatus(status);
    throw std::runtime_error(error);
}
struct Tensor {
    std::string Name;
    std::vector<int64_t> Dims;
    std::vector<uint8_t> Constant;
    size_t Bytes = sizeof(float);
    int Input = -1;
    static Tensor Read(const OrtApi& api, const OrtValueInfo* value) {
        Tensor t; const char* name = nullptr; Check(api, api.GetValueInfoName(value, &name)); t.Name = name;
        const OrtTypeInfo* type = nullptr; const OrtTensorTypeAndShapeInfo* shape = nullptr;
        Check(api, api.GetValueInfoTypeInfo(value, &type)); Check(api, api.CastTypeInfoToTensorInfo(type, &shape));
        if (!shape) throw std::runtime_error("NPU graph requires tensors");
        ONNXTensorElementDataType dtype{}; Check(api, api.GetTensorElementType(shape, &dtype));
        if (dtype != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) throw std::runtime_error("NPU graph requires FP32 tensors");
        size_t rank = 0; Check(api, api.GetDimensionsCount(shape, &rank));
        if (rank > 4) throw std::runtime_error("NPU graph tensor rank exceeds four");
        t.Dims.resize(rank); Check(api, api.GetDimensions(shape, t.Dims.data(), rank));
        for (auto dim : t.Dims) {
            if (dim <= 0 || static_cast<uint64_t>(dim) > QCOM_NPU_MAX_SHARED / t.Bytes)
                throw std::runtime_error("NPU graph needs bounded static dimensions");
            t.Bytes *= static_cast<size_t>(dim);
        }
        bool constant = false; Check(api, api.ValueInfo_IsConstantInitializer(value, &constant));
        if (constant) {
            const OrtValue* tensor = nullptr; const void* data = nullptr;
            Check(api, api.ValueInfo_GetInitializerValue(value, &tensor)); Check(api, api.GetTensorData(tensor, &data));
            t.Constant.resize(t.Bytes); std::memcpy(t.Constant.data(), data, t.Bytes);
        }
        return t;
    }
};
static std::vector<int64_t> Integers(const OrtApi& api, const OrtNode* node, const char* key, std::vector<int64_t> fallback) {
    const OrtOpAttr* attr = nullptr; Check(api, api.Node_GetAttributeByName(node, key, &attr));
    if (!attr) return fallback;
    size_t bytes = 0; auto status = api.ReadOpAttr(attr, ORT_OP_ATTR_INTS, nullptr, 0, &bytes);
    if (status) api.ReleaseStatus(status);
    if (!bytes || bytes % sizeof(int64_t) || bytes > 8 * sizeof(int64_t)) throw std::runtime_error("Invalid NPU graph attribute");
    std::vector<int64_t> result(bytes / sizeof(int64_t));
    Check(api, api.ReadOpAttr(attr, ORT_OP_ATTR_INTS, result.data(), bytes, &bytes)); return result;
}
static int64_t Integer(const OrtApi& api, const OrtNode* node, const char* key, int64_t fallback) {
    const OrtOpAttr* attr = nullptr; Check(api, api.Node_GetAttributeByName(node, key, &attr));
    if (!attr) return fallback;
    size_t bytes = sizeof(fallback); Check(api, api.ReadOpAttr(attr, ORT_OP_ATTR_INT, &fallback, bytes, &bytes)); return fallback;
}
static std::string String(const OrtApi& api, const OrtNode* node, const char* key, const char* fallback) {
    const OrtOpAttr* attr = nullptr; Check(api, api.Node_GetAttributeByName(node, key, &attr));
    if (!attr) return fallback;
    char text[32]{}; size_t bytes = sizeof(text); Check(api, api.ReadOpAttr(attr, ORT_OP_ATTR_STRING, text, sizeof(text), &bytes));
    if (bytes > sizeof(text)) throw std::runtime_error("Invalid NPU string attribute");
    return std::string(text, bytes);
}
struct Node {
    std::string Type;
    std::vector<Tensor> Inputs;
    Tensor Output;
    std::vector<int64_t> Stride, Pad, Dilation, Kernel;
    Node(const OrtApi& api, const OrtNode* node) {
        const char* type = nullptr; const char* domain = nullptr;
        Check(api, api.Node_GetOperatorType(node, &type)); Check(api, api.Node_GetDomain(node, &domain));
        if (*domain && std::strcmp(domain, "ai.onnx")) throw std::runtime_error("Unsupported NPU operator domain");
        Type = type;
        static const char* names[] = {"Add", "Sub", "Mul", "Div", "Relu", "Sigmoid", "Tanh", "Exp", "Sqrt", "Abs", "Neg", "Softmax", "MatMul", "Conv", "MaxPool", "Reshape"};
        if (std::none_of(std::begin(names), std::end(names), [&](const char* name) { return Type == name; }))
            throw std::runtime_error("Unsupported NPU operator");
        size_t ni = 0, no = 0; Check(api, api.Node_GetNumInputs(node, &ni)); Check(api, api.Node_GetNumOutputs(node, &no));
        if (!ni || ni > 3 || no != 1) throw std::runtime_error("Unsupported NPU operator arity");
        std::vector<const OrtValueInfo*> inputs(ni); const OrtValueInfo* output = nullptr;
        Check(api, api.Node_GetInputs(node, inputs.data(), ni)); Check(api, api.Node_GetOutputs(node, &output, 1));
        for (size_t i=0; i<ni; i++) if (!(Type == "Reshape" && i == 1)) Inputs.push_back(Tensor::Read(api, inputs[i]));
        Output = Tensor::Read(api, output);
        if (Type == "Reshape") {
            bool constant = false;
            if (ni == 2) Check(api, api.ValueInfo_IsConstantInitializer(inputs[1], &constant));
            if (!constant || Inputs[0].Bytes != Output.Bytes) throw std::runtime_error("NPU reshape needs a constant shape");
        } else if (Type == "MatMul") {
            if (ni != 2 || Inputs[0].Dims.size() != 2 || Inputs[1].Dims.size() != 2 ||
                Inputs[0].Dims[1] != Inputs[1].Dims[0] || Output.Dims != std::vector<int64_t>{Inputs[0].Dims[0], Inputs[1].Dims[1]})
                throw std::runtime_error("Unsupported NPU matrix shape");
        } else if (Type == "Conv") {
            if (ni < 2 || Inputs[0].Dims.size() != 4 || Inputs[1].Dims.size() != 4 || Output.Dims.size() != 4 ||
                Integer(api, node, "group", 1) != 1 || Inputs[0].Dims[1] != Inputs[1].Dims[1])
                throw std::runtime_error("Unsupported NPU convolution shape");
            Stride = Integers(api, node, "strides", {1, 1}); Pad = Integers(api, node, "pads", {0, 0, 0, 0});
            Dilation = Integers(api, node, "dilations", {1, 1});
            auto autoPad = String(api, node, "auto_pad", "NOTSET");
            if (autoPad == "VALID") Pad = {0, 0, 0, 0};
            else if (autoPad == "SAME_UPPER" || autoPad == "SAME_LOWER") {
                if (Stride.size() != 2 || Dilation.size() != 2) throw std::runtime_error("Invalid NPU convolution attributes");
                Pad.resize(4);
                for (size_t i=0; i<2; i++) {
                    int64_t padding = (Output.Dims[2+i]-1)*Stride[i] + Dilation[i]*(Inputs[1].Dims[2+i]-1)+1 - Inputs[0].Dims[2+i];
                    padding = (std::max)(int64_t(0), padding);
                    if (padding & 1) throw std::runtime_error("NPU convolution needs symmetric padding");
                    Pad[i] = Pad[i+2] = padding/2;
                }
            } else if (autoPad != "NOTSET") throw std::runtime_error("Invalid NPU convolution padding mode");
            if (Stride.size() != 2 || Pad.size() != 4 || Dilation.size() != 2 || Pad[0] != Pad[2] || Pad[1] != Pad[3])
                throw std::runtime_error("Unsupported NPU convolution padding");
            for (auto v : Stride) if (v <= 0 || v > 32) throw std::runtime_error("Invalid NPU convolution stride");
            for (auto v : Dilation) if (v <= 0 || v > 32) throw std::runtime_error("Invalid NPU convolution dilation");
            for (auto v : Pad) if (v < 0 || v > 4096) throw std::runtime_error("Invalid NPU convolution padding");
            if (Output.Dims[0] != Inputs[0].Dims[0] || Output.Dims[1] != Inputs[1].Dims[0]) throw std::runtime_error("Invalid NPU convolution output");
            for (size_t i = 0; i < 2; i++) {
                int64_t expected = (Inputs[0].Dims[2+i] + 2*Pad[i] - Dilation[i]*(Inputs[1].Dims[2+i]-1) - 1)/Stride[i] + 1;
                if (expected <= 0 || Output.Dims[2+i] != expected) throw std::runtime_error("Invalid NPU convolution output extent");
            }
            if (ni == 3 && Inputs[2].Dims != std::vector<int64_t>{Output.Dims[1]}) throw std::runtime_error("Invalid NPU convolution bias");
        } else if (Type == "MaxPool") {
            if (ni != 1 || Inputs[0].Dims.size() != 4 || Output.Dims.size() != 4 || Integer(api, node, "ceil_mode", 0) ||
                Integer(api, node, "storage_order", 0) || String(api, node, "auto_pad", "NOTSET") != "NOTSET")
                throw std::runtime_error("Unsupported NPU pooling shape");
            Kernel = Integers(api, node, "kernel_shape", {}); Stride = Integers(api, node, "strides", {1,1});
            Pad = Integers(api, node, "pads", {0,0,0,0});
            if (Kernel.size()!=2 || Stride.size()!=2 || Pad.size()!=4 || Pad[0]!=Pad[2] || Pad[1]!=Pad[3] ||
                Integers(api, node, "dilations", {1,1}) != std::vector<int64_t>{1,1}) throw std::runtime_error("Invalid NPU pooling attributes");
            for (size_t i=0; i<2; i++) {
                if (Kernel[i]<=0 || Kernel[i]>32 || Stride[i]<=0 || Stride[i]>32 || Pad[i]<0 || Pad[i]>=Kernel[i] ||
                    (Inputs[0].Dims[2+i]+2*Pad[i]-Kernel[i])/Stride[i]+1 != Output.Dims[2+i]) throw std::runtime_error("Invalid NPU pooling extent");
            }
            if (Inputs[0].Dims[0]!=Output.Dims[0] || Inputs[0].Dims[1]!=Output.Dims[1]) throw std::runtime_error("Invalid NPU pooling output");
        } else if (Type == "Add" || Type == "Sub" || Type == "Mul" || Type == "Div") {
            if (ni != 2 || Inputs[0].Dims != Output.Dims || Inputs[1].Dims.size() > Output.Dims.size()) throw std::runtime_error("Unsupported NPU broadcast shape");
            for (size_t i=0; i<Inputs[1].Dims.size(); i++) {
                auto a = Output.Dims[Output.Dims.size()-1-i], b = Inputs[1].Dims[Inputs[1].Dims.size()-1-i];
                if (b != 1 && b != a) throw std::runtime_error("Invalid NPU broadcast dimension");
            }
        } else if (ni != 1 || Inputs[0].Dims != Output.Dims) throw std::runtime_error("Invalid NPU unary shape");
        if (Type == "Softmax") {
            int64_t axis = Integer(api, node, "axis", -1);
            if (Output.Dims.empty() || (axis != -1 && axis != static_cast<int64_t>(Output.Dims.size()) - 1))
                throw std::runtime_error("Unsupported NPU softmax axis");
        }
    }
};
struct Backend {
    HMODULE Base = nullptr;
    HMODULE Module = nullptr;
    ggml_backend_dev_t Device = nullptr;
    ggml_backend_t Value = nullptr;
    std::mutex Mutex;
    Backend() {
        const auto path = qcom_npu::InstalledDirectory() / L"GGML";
        Base = LoadLibraryExW((path / L"ggml-base.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!Base)
            throw std::runtime_error("Cannot load the NPU graph ABI");
        Module = LoadLibraryExW((path / L"qcom_npu_ggml.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        auto init = Module ? reinterpret_cast<ggml_backend_reg_t(*)()>(GetProcAddress(Module, "ggml_backend_init")) : nullptr;
        if (!init) throw std::runtime_error("Cannot load the installed NPU graph backend");
        Device = ggml_backend_reg_dev_get(init(), 0); Value = ggml_backend_dev_init(Device, nullptr);
        if (!Value) throw std::runtime_error("Cannot create NPU graph execution context");
    }
    ~Backend() { if (Value) ggml_backend_free(Value); if (Module) FreeLibrary(Module); if (Base) FreeLibrary(Base); }
    static std::shared_ptr<Backend> Get() {
        static std::mutex mutex; static std::weak_ptr<Backend> weak;
        std::lock_guard<std::mutex> guard(mutex); auto value = weak.lock();
        if (!value) { value = std::make_shared<Backend>(); weak = value; } return value;
    }
};
struct State {
    std::shared_ptr<Backend> Owner;
    ggml_context* Context = nullptr;
    ggml_backend_buffer_t Buffer = nullptr;
    ggml_cgraph* Graph = nullptr;
    std::vector<ggml_tensor*> Inputs;
    ggml_tensor* Output = nullptr;
    ~State() { if (Buffer) ggml_backend_buffer_free(Buffer); if (Context) ggml_free(Context); }
    void Build(const Node& node) {
        Owner = Backend::Get(); std::lock_guard<std::mutex> guard(Owner->Mutex);
        Context = ggml_init({ggml_tensor_overhead()*64 + ggml_graph_overhead(), nullptr, true});
        if (!Context) throw std::runtime_error("NPU graph metadata allocation failed");
        for (const auto& input : node.Inputs) {
            int64_t dims[4] = {1,1,1,1}; std::reverse_copy(input.Dims.begin(), input.Dims.end(), dims);
            Inputs.push_back(ggml_new_tensor(Context, GGML_TYPE_F32, (std::max)(1, static_cast<int>(input.Dims.size())), dims));
        }
        auto* a = Inputs[0]; auto* b = Inputs.size() > 1 ? Inputs[1] : nullptr;
        const auto& t = node.Type;
        if (t == "Add") Output = ggml_add(Context, a, b);
        else if (t == "Sub") Output = ggml_sub(Context, a, b);
        else if (t == "Mul") Output = ggml_mul(Context, a, b);
        else if (t == "Div") Output = ggml_div(Context, a, b);
        else if (t == "Relu") Output = ggml_relu(Context, a);
        else if (t == "Sigmoid") Output = ggml_sigmoid(Context, a);
        else if (t == "Tanh") Output = ggml_tanh(Context, a);
        else if (t == "Exp") Output = ggml_exp(Context, a);
        else if (t == "Sqrt") Output = ggml_sqrt(Context, a);
        else if (t == "Abs") Output = ggml_abs(Context, a);
        else if (t == "Neg") Output = ggml_neg(Context, a);
        else if (t == "Softmax") Output = ggml_soft_max(Context, a);
        else if (t == "Reshape") {
            int64_t dims[4]={1,1,1,1}; std::reverse_copy(node.Output.Dims.begin(),node.Output.Dims.end(),dims);
            Output=ggml_reshape_4d(Context,a,dims[0],dims[1],dims[2],dims[3]);
        }
        else if (t == "MaxPool") Output = ggml_pool_2d(Context,a,GGML_OP_POOL_MAX,static_cast<int>(node.Kernel[1]),static_cast<int>(node.Kernel[0]),
            static_cast<int>(node.Stride[1]),static_cast<int>(node.Stride[0]),static_cast<float>(node.Pad[1]),static_cast<float>(node.Pad[0]));
        else if (t == "MatMul") Output = ggml_cont(Context, ggml_transpose(Context, ggml_mul_mat(Context, a, ggml_cont(Context, ggml_transpose(Context, b)))));
        else if (t == "Conv") {
            Output = ggml_conv_2d(Context, ggml_cast(Context, b, GGML_TYPE_F16), a, static_cast<int>(node.Stride[1]), static_cast<int>(node.Stride[0]),
                static_cast<int>(node.Pad[1]), static_cast<int>(node.Pad[0]), static_cast<int>(node.Dilation[1]), static_cast<int>(node.Dilation[0]));
            if (Inputs.size() == 3) Output = ggml_add(Context, Output, ggml_reshape_4d(Context, Inputs[2], 1, 1, Inputs[2]->ne[0], 1));
        }
        if (!Output || ggml_nbytes(Output) != node.Output.Bytes) throw std::runtime_error("NPU graph output layout mismatch");
        Graph = ggml_new_graph(Context); ggml_build_forward_expand(Graph, Output);
        for (int i=0; i<ggml_graph_n_nodes(Graph); i++)
            if (!ggml_backend_dev_supports_op(Owner->Device, ggml_graph_node(Graph, i))) throw std::runtime_error("NPU does not support this graph layout");
        Buffer = ggml_backend_alloc_ctx_tensors(Context, Owner->Value);
        if (!Buffer) throw std::runtime_error("NPU graph DMA allocation failed");
        for (size_t i=0; i<Inputs.size(); i++) if (!node.Inputs[i].Constant.empty())
            ggml_backend_tensor_set(Inputs[i], node.Inputs[i].Constant.data(), 0, node.Inputs[i].Bytes);
    }
};
struct Info final : NpuComputeInfo {
    const OrtApi& Api;
    Node Program;
    Info(const OrtApi& api, const OrtGraph* graph, const OrtNode* node) : Api(api), Program(api, node) {
        size_t count = 0; Check(api, api.Graph_GetNumInputs(graph, &count)); std::vector<const OrtValueInfo*> inputs(count);
        Check(api, api.Graph_GetInputs(graph, inputs.data(), count));
        for (size_t i=0; i<count; i++) {
            const char* name = nullptr; Check(api, api.GetValueInfoName(inputs[i], &name));
            for (auto& input : Program.Inputs) if (input.Constant.empty() && input.Name == name) input.Input = static_cast<int>(i);
        }
        for (const auto& input : Program.Inputs) if (input.Constant.empty() && input.Input < 0) throw std::runtime_error("Missing NPU graph input binding");
        CreateState = [](OrtNodeComputeInfo* base, OrtNodeComputeContext*, void** output) noexcept -> OrtStatus* {
            auto& self = *static_cast<Info*>(base);
            try { auto state = std::make_unique<State>(); state->Build(self.Program); *output = state.release(); return nullptr; }
            catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
        ReleaseState = [](OrtNodeComputeInfo*, void* state) noexcept { delete static_cast<State*>(state); };
        Compute = [](OrtNodeComputeInfo* base, void* pointer, OrtKernelContext* context) noexcept -> OrtStatus* {
            auto& self = *static_cast<Info*>(base); auto& state = *static_cast<State*>(pointer);
            try {
                std::lock_guard<std::mutex> guard(state.Owner->Mutex);
                for (size_t i=0; i<self.Program.Inputs.size(); i++) {
                    const auto& binding = self.Program.Inputs[i]; if (binding.Input < 0) continue;
                    const OrtValue* value = nullptr; const void* data = nullptr;
                    Check(self.Api, self.Api.KernelContext_GetInput(context, binding.Input, &value));
                    OrtTensorTypeAndShapeInfo* shape = nullptr; Check(self.Api, self.Api.GetTensorTypeAndShape(value, &shape));
                    size_t count = 0; auto error = self.Api.GetTensorShapeElementCount(shape, &count); self.Api.ReleaseTensorTypeAndShapeInfo(shape); Check(self.Api, error);
                    if (count != binding.Bytes/4) throw std::runtime_error("NPU graph input shape mismatch");
                    Check(self.Api, self.Api.GetTensorData(value, &data)); ggml_backend_tensor_set(state.Inputs[i], data, 0, binding.Bytes);
                }
                if (ggml_backend_graph_compute(state.Owner->Value, state.Graph) != GGML_STATUS_SUCCESS) throw std::runtime_error("NPU graph execution failed");
                OrtValue* output = nullptr; void* data = nullptr; const auto& shape = self.Program.Output;
                Check(self.Api, self.Api.KernelContext_GetOutput(context, 0, shape.Dims.data(), shape.Dims.size(), &output));
                Check(self.Api, self.Api.GetTensorMutableData(output, &data)); ggml_backend_tensor_get(state.Output, data, 0, shape.Bytes); return nullptr;
            } catch (const std::exception& error) { return self.Api.CreateStatus(ORT_EP_FAIL, error.what()); }
        };
    }
};
bool NpuSupportsNode(const OrtApi& api, const OrtNode* node) {
    try { Node program(api, node); return true; } catch (const std::exception&) { return false; }
}
NpuComputeInfo* NpuCompileNode(const OrtApi& api, const OrtGraph* graph) {
    const OrtNode* node = nullptr; Check(api, api.Graph_GetNodes(graph, &node, 1)); return new Info(api, graph, node);
}
