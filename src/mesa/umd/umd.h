#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define D3D10DDI_MINOR_HEADER_VERSION 2
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <winternl.h>
#include <d3d10_1.h>
#include <d3d10umddi.h>
#include <d3dkmthk.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <unordered_map>
#include <new>
#include <cstring>
#include <cstdint>
#include <type_traits>
#include "a830_host.h"
#include "abi.h"
#include "vkd3d_shader.h"

#define VK_FUNCTIONS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceQueueFamilyProperties) \
 X(GetPhysicalDeviceFormatProperties) X(CreateDevice) X(GetDeviceProcAddr) \
 X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(QueueWaitIdle) X(QueueSubmit) \
 X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool) X(AllocateCommandBuffers) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences) \
 X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
 X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(GetImageSubresourceLayout) \
 X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
 X(CreateImageView) X(DestroyImageView) X(CreateBufferView) X(DestroyBufferView) \
 X(CreateSampler) X(DestroySampler) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
 X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
 X(CreateGraphicsPipelines) X(DestroyPipeline) X(CreatePipelineCache) X(DestroyPipelineCache) \
 X(CmdPipelineBarrier) X(CmdCopyBuffer) X(CmdUpdateBuffer) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdCopyImage) X(CmdBlitImage) \
 X(CmdClearColorImage) X(CmdClearDepthStencilImage) X(CmdBeginRenderPass) X(CmdEndRenderPass) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) \
 X(CmdSetViewport) X(CmdSetScissor) X(CmdSetBlendConstants) X(CmdSetStencilReference) \
 X(CmdDraw) X(CmdDrawIndexed) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) \
 X(CmdBeginQuery) X(CmdEndQuery) X(CmdWriteTimestamp) X(GetQueryPoolResults)

struct Dispatch {
#define DECLARE(name) PFN_vk##name name = nullptr;
 VK_FUNCTIONS(DECLARE)
#undef DECLARE
};

struct Adapter { HANDLE runtime; unsigned flags; };
struct Device;
struct Storage {
 VkBuffer buffer = VK_NULL_HANDLE;
 VkImage image = VK_NULL_HANDLE;
 VkDeviceMemory memory = VK_NULL_HANDLE;
 VkDeviceSize size = 0;
 VkDeviceSize capacity = 0;
 void *map = nullptr;
 bool coherent = false;
 bool initialized = false;
 uint64_t used = 0;
 VkBufferUsageFlags usage = 0;
 UINT memory_type = ~0u;
 bool external = false;
};
struct Mapping { Storage staging; UINT sub = 0, mode = 0, row = 0, slice = 0; };
struct Resource {
 Storage storage;
 DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
 VkFormat vkformat = VK_FORMAT_UNDEFINED;
 UINT width = 0, height = 1, depth = 1, levels = 1, layers = 1;
 UINT bind = 0, dimension = 0, pitch = 0;
 D3DKMT_HANDLE allocation = 0, kmresource = 0;
 HANDLE runtime = nullptr;
 uint64_t generation = 1;
 bool imported = false;
 Mapping mapping;
};
struct View {
 Resource *resource = nullptr;
 DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
 UINT level = 0, levels = 1, layer = 0, layers = 1, first = 0, count = 0;
 VkImageView image = VK_NULL_HANDLE;
 VkBufferView buffer = VK_NULL_HANDLE;
 uint64_t generation = 0;
};
struct Shader {
 VkShaderModule module = VK_NULL_HANDLE;
 VkShaderModule dual_module = VK_NULL_HANDLE;
 uint64_t id = 0;
 unsigned stage = 0;
 std::array<bool,16> cb{};
 std::array<bool,16> samplers{};
 std::array<bool,128> textures{};
 std::array<bool,128> texel{};
};
struct Layout { uint64_t id = 0; std::vector<D3D10DDIARG_INPUT_ELEMENT_DESC> elements; };
struct Blend { uint64_t id = 0; D3D10_DDI_BLEND_DESC desc{}; };
struct Depth { uint64_t id = 0; D3D10_DDI_DEPTH_STENCIL_DESC desc{}; };
struct Raster { uint64_t id = 0; D3D10_DDI_RASTERIZER_DESC desc{}; };
struct Sampler { VkSampler handle = VK_NULL_HANDLE; };
struct Query { D3D10DDI_QUERY type{}; VkQueryPool pool = VK_NULL_HANDLE; bool ended = false, active = false; UINT count = 0; };
struct VertexBinding { Resource *resource = nullptr; UINT stride = 0, offset = 0; };
struct Pipeline { VkPipeline handle; VkRenderPass pass; };
struct Framebuffer { VkFramebuffer handle; };
struct RetiredView { VkImageView image; VkBufferView buffer; };
struct Batch {
 VkCommandPool commands = VK_NULL_HANDLE;
 VkCommandBuffer cmd = VK_NULL_HANDLE;
 VkFence fence = VK_NULL_HANDLE;
 VkDescriptorPool descriptors = VK_NULL_HANDLE;
 uint64_t submitted = 0;
 std::vector<Storage> temporary,renamed;
 std::vector<RetiredView> retired_views;
 std::vector<VkSampler> retired_samplers;
 std::vector<VkQueryPool> retired_queries;
 std::vector<Framebuffer> framebuffers;
 std::unordered_map<std::string,VkDescriptorSet> descriptor_cache[3];
};
struct Device : Batch {
 Batch pending;
 uint64_t next_submit = 1,completed = 0;
 VkPipeline bound_pipeline = VK_NULL_HANDLE;
 std::array<VkDescriptorSet,3> bound_sets{};
 std::string pipeline_key,descriptor_key;
 HANDLE runtime = nullptr, adapter = nullptr, context = nullptr;
 D3D10DDI_HRTCORELAYER core{};
 D3DDDI_DEVICECALLBACKS callbacks{};
 D3D10DDI_CORELAYER_DEVICECALLBACKS errors{};
 DXGI_DDI_BASE_CALLBACKS *present = nullptr;
 CRITICAL_SECTION lock{};
 void *command_buffer = nullptr;
 UINT command_buffer_size = 0;
 HMODULE module = nullptr;
 Dispatch vk;
 VkInstance instance = VK_NULL_HANDLE;
 VkPhysicalDevice physical = VK_NULL_HANDLE;
 VkDevice device = VK_NULL_HANDLE;
 VkQueue queue = VK_NULL_HANDLE;
 VkPhysicalDeviceMemoryProperties memory{};
 VkPhysicalDeviceProperties properties{};
 VkDescriptorSetLayout sets[3]{};
 VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
 VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
 std::vector<Storage> buffer_cache;
 VkDeviceSize buffer_cache_bytes = 0;
 std::vector<Storage> image_cache;
 VkDeviceSize image_cache_bytes = 0;
 std::vector<VkImageView> pass_views;
 std::array<VkFormat,9> pass_formats{};
 bool render_pass = false;
 std::unordered_map<std::string,Pipeline> pipelines;
 std::array<Shader *,3> shaders{};
 std::array<std::array<Resource *,16>,3> cb{};
 std::array<std::array<Sampler *,16>,3> samplers{};
 std::array<std::array<View *,128>,3> textures{};
 std::array<VertexBinding,32> vertices{};
 Resource *indices = nullptr;
 UINT index_offset = 0, index_size = 0;
 Layout *layout = nullptr;
 Blend *blend = nullptr;
 Depth *depth = nullptr;
 Raster *raster = nullptr;
 std::array<View *,8> targets{};
 UINT target_count = 0;
 View *depth_view = nullptr;
 D3D10_DDI_PRIMITIVE_TOPOLOGY topology = D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
 D3D10_DDI_VIEWPORT viewport{};
 D3D10_DDI_RECT scissor{};
 float blend_color[4]{};
 UINT sample_mask = ~0u, stencil_ref = 0;
 bool recording = false;
 HRESULT failure = S_OK;
 Storage dummy_buffer;
 Resource dummy_image;
 View dummy_view;
 Sampler dummy_sampler;
 std::vector<Query *> queries;
 Shader rotation_vertex,rotation_pixel;
 Resource rotation_constants;
 Sampler rotation_sampler;
};

template<class T, class H> static inline T *object(H h) { return static_cast<T *>(h.pDrvPrivate); }
static inline Device *dev(D3D10DDI_HDEVICE h) { return object<Device>(h); }
static inline Device *dev(DXGI_DDI_HDEVICE h) { return reinterpret_cast<Device *>(h); }
static inline Resource *res(D3D10DDI_HRESOURCE h) { return object<Resource>(h); }
static inline Resource *res(DXGI_DDI_HRESOURCE h) { return reinterpret_cast<Resource *>(h); }
void error(Device *, HRESULT);
bool check(Device *, VkResult);
bool flush(Device *,bool = false);
bool begin(Device *);
void end_pass(Device *);
void barrier(Device *);
VkFormat format(DXGI_FORMAT);
unsigned pixel_bytes(DXGI_FORMAT);
VkImageAspectFlags aspects(DXGI_FORMAT);
bool buffer_create(Device *, Storage &, VkDeviceSize, VkBufferUsageFlags);
bool buffer_rename(Device *,Resource *);
void storage_destroy(Device *, Storage &);
void buffer_recycle(Device *,Storage &);
void storage_recycle(Device *,Storage &);
bool sync_memory(Device *,Storage &,bool);
bool image_create(Device *, Resource *, uint64_t = 0, unsigned = 0);
bool view_create(Device *, View *, bool = false);
void view_destroy(Device *, View *);
bool draw(Device *, UINT, UINT, INT, UINT, UINT, bool);
bool shader_create(Device *, Shader *, const UINT *, const D3D10DDIARG_STAGE_IO_SIGNATURES *, unsigned);
bool initialize(Device *);
void destroy(Device *);

#define DDI(slot,name) std::remove_pointer_t<decltype(D3D10DDI_DEVICEFUNCS::slot)> name;
#define DXGI(slot,name) std::remove_pointer_t<decltype(DXGI_DDI_BASE_FUNCTIONS::slot)> name;
#include "callbacks.inc"
#undef DXGI
#undef DDI
HRESULT APIENTRY _ResolveSharedResource(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *);
