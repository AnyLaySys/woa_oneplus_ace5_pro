#include "umd.h"
#include <sddl.h>

struct ProfileMapping {
 HANDLE file=nullptr;A830Profile *data=nullptr;
 ~ProfileMapping(){if(data)UnmapViewOfFile(data);if(file)CloseHandle(file);}
};
static A830Profile *profile_mapping() {
 static ProfileMapping mapping;
 static bool initialized=[](){
   PSECURITY_DESCRIPTOR descriptor=nullptr;
   if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;OW)(A;;GR;;;BA)S:(ML;;NW;;;LW)",SDDL_REVISION_1,&descriptor,nullptr)){fprintf(stderr,"A830 profile descriptor error=%lu\n",GetLastError());return false;}
   SECURITY_ATTRIBUTES security{sizeof(security),descriptor,FALSE};WCHAR name[96];
   swprintf(name,96,L"Local\\A830Profile-%lu",GetCurrentProcessId());
   mapping.file=CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,sizeof(A830Profile),name);LocalFree(descriptor);
   if(!mapping.file){fprintf(stderr,"A830 profile mapping error=%lu\n",GetLastError());return false;}
   auto data=(A830Profile *)MapViewOfFile(mapping.file,FILE_MAP_ALL_ACCESS,0,0,sizeof(A830Profile));
   if(data){LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);data->version=1;data->size=sizeof(*data);data->pid=GetCurrentProcessId();data->frequency=(uint64_t)frequency.QuadPart;}
   mapping.data=data;return data!=nullptr;
 }();
 (void)initialized;return mapping.data;
}
void profile_record(Device *d) {
 A830Profile *mapping=profile_mapping();
 if(mapping && !InterlockedCompareExchange(&mapping->writer,1,0)) {
   LONG64 serial=InterlockedIncrement64(&mapping->serial);
   A830FrameProfile *target=mapping->frames+(serial-1)%256;
   InterlockedExchange64(&target->sequence,0);MemoryBarrier();
   memcpy((BYTE *)target+sizeof(target->sequence),(const BYTE *)&d->profile+sizeof(d->profile.sequence),sizeof(*target)-sizeof(target->sequence));
   MemoryBarrier();InterlockedExchange64(&target->sequence,serial);InterlockedExchange(&mapping->writer,0);
 }
 uint64_t context=d->profile.context;d->profile={};d->profile.context=context;
}

int direct_log(FILE *,const char *format,...) {
 char text[8192];va_list ap;va_start(ap,format);int size=vsnprintf(text,sizeof(text),format,ap);va_end(ap);
 if(size<0)return size;DWORD length=(DWORD)std::min(size,(int)sizeof(text)-1),written=0;
 static HANDLE file=[](){CreateDirectoryW(L"C:\\ProgramData\\A830Direct",nullptr);WCHAR path[MAX_PATH];swprintf(path,MAX_PATH,L"C:\\ProgramData\\A830Direct\\direct-%lu.log",GetCurrentProcessId());return CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);}();
 if(file!=INVALID_HANDLE_VALUE)WriteFile(file,text,length,&written,nullptr);
 HANDLE output=GetStdHandle(STD_ERROR_HANDLE);if(output && output!=INVALID_HANDLE_VALUE)WriteFile(output,text,length,&written,nullptr);
 return size;
}

void error(Device *d, HRESULT hr, const char *where) {
 if (!d || SUCCEEDED(hr)) return;
 fprintf(stderr,"A830 DIRECT %s hr=%08lx\n",where,(unsigned long)hr);
 d->failure=hr;
 if(d->errors.pfnSetErrorCb)d->errors.pfnSetErrorCb(d->core,hr);
}
bool check(Device *d,VkResult result,const char *where) {
 if(result==VK_SUCCESS)return true;
 fprintf(stderr,"A830 DIRECT Vulkan %s result=%d\n",where,result);
 error(d,result==VK_ERROR_DEVICE_LOST?DXGI_ERROR_DEVICE_REMOVED:E_FAIL,where);return false;
}
VkFormat format(DXGI_FORMAT f) {
 switch(f) {
 case DXGI_FORMAT_R32G32B32A32_FLOAT:return VK_FORMAT_R32G32B32A32_SFLOAT;
 case DXGI_FORMAT_R32G32B32A32_UINT:return VK_FORMAT_R32G32B32A32_UINT;
 case DXGI_FORMAT_R32G32B32A32_SINT:return VK_FORMAT_R32G32B32A32_SINT;
 case DXGI_FORMAT_R32G32B32_FLOAT:return VK_FORMAT_R32G32B32_SFLOAT;
 case DXGI_FORMAT_R32G32B32_UINT:return VK_FORMAT_R32G32B32_UINT;
 case DXGI_FORMAT_R32G32B32_SINT:return VK_FORMAT_R32G32B32_SINT;
 case DXGI_FORMAT_R16G16B16A16_FLOAT:return VK_FORMAT_R16G16B16A16_SFLOAT;
 case DXGI_FORMAT_R16G16B16A16_UNORM:return VK_FORMAT_R16G16B16A16_UNORM;
 case DXGI_FORMAT_R16G16B16A16_SNORM:return VK_FORMAT_R16G16B16A16_SNORM;
 case DXGI_FORMAT_R16G16B16A16_UINT:return VK_FORMAT_R16G16B16A16_UINT;
 case DXGI_FORMAT_R16G16B16A16_SINT:return VK_FORMAT_R16G16B16A16_SINT;
 case DXGI_FORMAT_R32G32_FLOAT:return VK_FORMAT_R32G32_SFLOAT;
 case DXGI_FORMAT_R32G32_UINT:return VK_FORMAT_R32G32_UINT;
 case DXGI_FORMAT_R32G32_SINT:return VK_FORMAT_R32G32_SINT;
 case DXGI_FORMAT_R10G10B10A2_UNORM:return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
 case DXGI_FORMAT_R10G10B10A2_UINT:return VK_FORMAT_A2B10G10R10_UINT_PACK32;
 case DXGI_FORMAT_R11G11B10_FLOAT:return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
 case DXGI_FORMAT_R8G8B8A8_TYPELESS:
 case DXGI_FORMAT_R8G8B8A8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;
 case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:return VK_FORMAT_R8G8B8A8_SRGB;
 case DXGI_FORMAT_R8G8B8A8_UINT:return VK_FORMAT_R8G8B8A8_UINT;
 case DXGI_FORMAT_R8G8B8A8_SNORM:return VK_FORMAT_R8G8B8A8_SNORM;
 case DXGI_FORMAT_R8G8B8A8_SINT:return VK_FORMAT_R8G8B8A8_SINT;
 case DXGI_FORMAT_B8G8R8A8_TYPELESS:
 case DXGI_FORMAT_B8G8R8A8_UNORM:return VK_FORMAT_B8G8R8A8_UNORM;
 case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:return VK_FORMAT_B8G8R8A8_SRGB;
 case DXGI_FORMAT_B8G8R8X8_UNORM:return VK_FORMAT_B8G8R8A8_UNORM;
 case DXGI_FORMAT_R16G16_FLOAT:return VK_FORMAT_R16G16_SFLOAT;
 case DXGI_FORMAT_R16G16_UNORM:return VK_FORMAT_R16G16_UNORM;
 case DXGI_FORMAT_R16G16_UINT:return VK_FORMAT_R16G16_UINT;
 case DXGI_FORMAT_R16G16_SNORM:return VK_FORMAT_R16G16_SNORM;
 case DXGI_FORMAT_R16G16_SINT:return VK_FORMAT_R16G16_SINT;
 case DXGI_FORMAT_R32_TYPELESS:
 case DXGI_FORMAT_R32_FLOAT:return VK_FORMAT_R32_SFLOAT;
 case DXGI_FORMAT_R32_UINT:return VK_FORMAT_R32_UINT;
 case DXGI_FORMAT_R32_SINT:return VK_FORMAT_R32_SINT;
 case DXGI_FORMAT_R8G8_UNORM:return VK_FORMAT_R8G8_UNORM;
 case DXGI_FORMAT_R8G8_UINT:return VK_FORMAT_R8G8_UINT;
 case DXGI_FORMAT_R8G8_SNORM:return VK_FORMAT_R8G8_SNORM;
 case DXGI_FORMAT_R8G8_SINT:return VK_FORMAT_R8G8_SINT;
 case DXGI_FORMAT_R16_TYPELESS:
 case DXGI_FORMAT_R16_UNORM:return VK_FORMAT_R16_UNORM;
 case DXGI_FORMAT_R16_FLOAT:return VK_FORMAT_R16_SFLOAT;
 case DXGI_FORMAT_R16_UINT:return VK_FORMAT_R16_UINT;
 case DXGI_FORMAT_R16_SNORM:return VK_FORMAT_R16_SNORM;
 case DXGI_FORMAT_R16_SINT:return VK_FORMAT_R16_SINT;
 case DXGI_FORMAT_R8_TYPELESS:
 case DXGI_FORMAT_R8_UNORM:
 case DXGI_FORMAT_A8_UNORM:return VK_FORMAT_R8_UNORM;
 case DXGI_FORMAT_R8_UINT:return VK_FORMAT_R8_UINT;
 case DXGI_FORMAT_R8_SNORM:return VK_FORMAT_R8_SNORM;
 case DXGI_FORMAT_R8_SINT:return VK_FORMAT_R8_SINT;
 case DXGI_FORMAT_D16_UNORM:return VK_FORMAT_D16_UNORM;
 case DXGI_FORMAT_D24_UNORM_S8_UINT:return VK_FORMAT_D24_UNORM_S8_UINT;
 case DXGI_FORMAT_D32_FLOAT:return VK_FORMAT_D32_SFLOAT;
 case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:return VK_FORMAT_D32_SFLOAT_S8_UINT;
 case DXGI_FORMAT_BC1_UNORM:return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
 case DXGI_FORMAT_BC1_UNORM_SRGB:return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
 case DXGI_FORMAT_BC2_UNORM:return VK_FORMAT_BC2_UNORM_BLOCK;
 case DXGI_FORMAT_BC3_UNORM:return VK_FORMAT_BC3_UNORM_BLOCK;
 default:return VK_FORMAT_UNDEFINED;
 }
}
unsigned pixel_bytes(DXGI_FORMAT f) {
 if(f>=DXGI_FORMAT_R32G32B32A32_TYPELESS && f<=DXGI_FORMAT_R32G32B32A32_SINT)return 16;
 if(f>=DXGI_FORMAT_R32G32B32_TYPELESS && f<=DXGI_FORMAT_R32G32B32_SINT)return 12;
 if(f>=DXGI_FORMAT_R16G16B16A16_TYPELESS && f<=DXGI_FORMAT_R32G32_SINT)return 8;
 if(f>=DXGI_FORMAT_R8G8_TYPELESS && f<=DXGI_FORMAT_R16_SINT)return 2;
 if(f>=DXGI_FORMAT_R8_TYPELESS && f<=DXGI_FORMAT_A8_UNORM)return 1;
 return 4;
}
VkImageAspectFlags aspects(DXGI_FORMAT f) {
 if(f==DXGI_FORMAT_D16_UNORM || f==DXGI_FORMAT_D32_FLOAT)return VK_IMAGE_ASPECT_DEPTH_BIT;
 if(f==DXGI_FORMAT_D24_UNORM_S8_UINT || f==DXGI_FORMAT_D32_FLOAT_S8X24_UINT)return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
 return VK_IMAGE_ASPECT_COLOR_BIT;
}
static UINT memory_type(Device *d,UINT bits,VkMemoryPropertyFlags wanted) {
 for(UINT i=0;i<d->memory.memoryTypeCount;i++)if((bits&(1u<<i)) && (d->memory.memoryTypes[i].propertyFlags&wanted)==wanted)return i;
 return ~0u;
}
bool buffer_create(Device *d,Storage &s,VkDeviceSize size,VkBufferUsageFlags usage) {
 ProfileTimer timing(d->profile.buffer_ticks);
 VkBufferUsageFlags required=usage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 auto best=d->buffer_cache.end();
 for(auto it=d->buffer_cache.begin();it!=d->buffer_cache.end();++it) {
   if(it->capacity<std::max<VkDeviceSize>(size,256) || it->size>std::max<VkDeviceSize>(4096,size*2) || (it->usage&required)!=required)continue;
   if(best==d->buffer_cache.end() || it->size<best->size)best=it;
 }
 if(best!=d->buffer_cache.end()) {
   s=*best;d->buffer_cache_bytes-=s.size;d->buffer_cache.erase(best);d->profile.reused++;return true;
 }
 d->profile.buffers++;
 VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=std::max<VkDeviceSize>(size,256);ci.usage=usage|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
 if(!check(d,d->vk.CreateBuffer(d->device,&ci,nullptr,&s.buffer),"CreateBuffer"))return false;
 VkMemoryRequirements req;d->vk.GetBufferMemoryRequirements(d->device,s.buffer,&req);
 UINT type=memory_type(d,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 if(type==~0u)type=memory_type(d,req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
 if(type==~0u){error(d,E_OUTOFMEMORY,"buffer memory type");return false;}
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=type;
 s.size=req.size;s.capacity=ci.size;s.usage=ci.usage;s.memory_type=type;s.coherent=!!(d->memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
 return check(d,d->vk.AllocateMemory(d->device,&ai,nullptr,&s.memory),"AllocateBuffer") &&
   check(d,d->vk.BindBufferMemory(d->device,s.buffer,s.memory,0),"BindBuffer") &&
   check(d,d->vk.MapMemory(d->device,s.memory,0,VK_WHOLE_SIZE,0,&s.map),"MapBuffer");
}
bool buffer_rename(Device *d,Resource *r) {
 if(!r->storage.in_use)return true;
 Storage next;
 if(!buffer_create(d,next,r->width,r->storage.usage)){storage_destroy(d,next);return false;}
 d->renamed.push_back(r->storage);r->storage=next;r->generation++;return true;
}
void storage_destroy(Device *d,Storage &s) {
 if(s.map)d->vk.UnmapMemory(d->device,s.memory);
 if(s.buffer)d->vk.DestroyBuffer(d->device,s.buffer,nullptr);
 if(s.image)d->vk.DestroyImage(d->device,s.image,nullptr);
 if(s.memory)d->vk.FreeMemory(d->device,s.memory,nullptr);
 s={};
}
void buffer_recycle(Device *d,Storage &s) {
 if(s.buffer && s.map && s.size<=16777216 && d->buffer_cache_bytes+s.size<=67108864 && d->buffer_cache.size()<384) {
   s.in_use=false;d->buffer_cache_bytes+=s.size;d->buffer_cache.push_back(s);s={};
 } else storage_destroy(d,s);
}
void storage_recycle(Device *d,Storage &s) {
 if(s.image && !s.external && s.memory && s.size<=33554432 && d->image_cache_bytes+s.size<=100663296 && d->image_cache.size()<64) {
   d->vk.DestroyImage(d->device,s.image,nullptr);s.image=VK_NULL_HANDLE;s.initialized=false;s.in_use=false;
   d->image_cache_bytes+=s.size;d->image_cache.push_back(s);s={};
 } else buffer_recycle(d,s);
}
bool sync_memory(Device *d,Storage &s,bool read) {
 ProfileTimer timing(d->profile.sync_ticks);
 if(s.coherent)return true;
 VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};range.memory=s.memory;range.size=VK_WHOLE_SIZE;
 return check(d,read?d->vk.InvalidateMappedMemoryRanges(d->device,1,&range):d->vk.FlushMappedMemoryRanges(d->device,1,&range),read?"InvalidateMapped":"FlushMapped");
}
bool begin(Device *d) {
 if(FAILED(d->failure))return false;
 if(d->recording)return true;
 if(!d->profile.start)d->profile.start=profile_clock();
 VkCommandBufferBeginInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};ci.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
 d->recording=check(d,d->vk.BeginCommandBuffer(d->cmd,&ci),"BeginCommands");return d->recording;
}
void end_pass(Device *d) {
 if(!d->render_pass)return;
 ProfileTimer timing(d->profile.close_ticks);
 d->vk.CmdEndRenderPass(d->cmd);d->render_pass=false;d->pass_views.clear();
}
void barrier(Device *d) {
 ProfileTimer timing(d->profile.barrier_ticks);
 if(!begin(d))return;
 end_pass(d);
 VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};memory.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;memory.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT;
 d->vk.CmdPipelineBarrier(d->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&memory,0,nullptr,0,nullptr);
}
bool flush(Device *d) {
 if(FAILED(d->failure))return false;
 if(!d->recording)return true;
 barrier(d);
 if(!check(d,d->vk.EndCommandBuffer(d->cmd),"EndCommands"))return false;
 d->recording=false;
 if(!check(d,d->vk.ResetFences(d->device,1,&d->fence),"ResetFence"))return false;
 d->profile.prepared=profile_clock();
 VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&d->cmd;
 if(!check(d,d->vk.QueueSubmit(d->queue,1,&si,d->fence),"Submit"))return false;
 d->profile.submitted=profile_clock();
 if(!check(d,d->vk.WaitForFences(d->device,1,&d->fence,VK_TRUE,10000000000ull),"Wait"))return false;
 d->profile.waited=profile_clock();
 for(auto &f:d->framebuffers)d->vk.DestroyFramebuffer(d->device,f.handle,nullptr);d->framebuffers.clear();
 for(auto &v:d->retired_views){if(v.image)d->vk.DestroyImageView(d->device,v.image,nullptr);if(v.buffer)d->vk.DestroyBufferView(d->device,v.buffer,nullptr);}d->retired_views.clear();
 for(auto sampler:d->retired_samplers)d->vk.DestroySampler(d->device,sampler,nullptr);d->retired_samplers.clear();
 for(auto query:d->retired_queries)d->vk.DestroyQueryPool(d->device,query,nullptr);d->retired_queries.clear();
 for(auto &s:d->renamed)buffer_recycle(d,s);d->renamed.clear();
 for(auto &s:d->temporary)storage_recycle(d,s);d->temporary.clear();
 for(auto *r:d->resources)r->storage.in_use=false;
 bool okay=check(d,d->vk.ResetCommandPool(d->device,d->commands,0),"ResetCommands") &&
    check(d,d->vk.ResetDescriptorPool(d->device,d->descriptors,0),"ResetDescriptors");
 d->profile.finished=profile_clock();profile_record(d);return okay;
}
bool image_create(Device *d,Resource *r,uint64_t address,unsigned size) {
 ProfileTimer timing(d->profile.image_ticks);
 VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
 ci.imageType=r->dimension==D3D10DDIRESOURCE_TEXTURE1D?VK_IMAGE_TYPE_1D:r->dimension==D3D10DDIRESOURCE_TEXTURE3D?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;
 ci.format=r->vkformat;ci.extent={r->width,r->height,r->depth};ci.mipLevels=r->levels;ci.arrayLayers=r->layers;ci.samples=VK_SAMPLE_COUNT_1_BIT;
 ci.tiling=address?VK_IMAGE_TILING_LINEAR:VK_IMAGE_TILING_OPTIMAL;
 ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
 if(r->bind&D3D10_DDI_BIND_RENDER_TARGET)ci.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
 if(r->bind&D3D10_DDI_BIND_DEPTH_STENCIL)ci.usage|=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
 ci.initialLayout=address?VK_IMAGE_LAYOUT_PREINITIALIZED:VK_IMAGE_LAYOUT_UNDEFINED;
 if(r->dimension==D3D10DDIRESOURCE_TEXTURECUBE)ci.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
 if(!check(d,d->vk.CreateImage(d->device,&ci,nullptr,&r->storage.image),"CreateImage"))return false;
 VkMemoryRequirements req;d->vk.GetImageMemoryRequirements(d->device,r->storage.image,&req);
 UINT type=memory_type(d,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);if(type==~0u)type=memory_type(d,req.memoryTypeBits,0);
 if(type==~0u || (address && req.size>size)){error(d,E_OUTOFMEMORY,"image import size");return false;}
 VkMemoryOpaqueCaptureAddressAllocateInfo replay{VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO};replay.opaqueCaptureAddress=address;
 VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};dedicated.image=r->storage.image;dedicated.pNext=address?&replay:nullptr;
 VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.pNext=address?&dedicated:nullptr;ai.allocationSize=address?size:req.size;ai.memoryTypeIndex=type;
 r->storage.size=ai.allocationSize;r->storage.memory_type=type;r->storage.external=address!=0;
 if(!address) {
   auto best=d->image_cache.end();
   for(auto it=d->image_cache.begin();it!=d->image_cache.end();++it) {
     if(it->memory_type!=type || it->size<req.size || it->size>std::max<VkDeviceSize>(1048576,req.size*2))continue;
     if(best==d->image_cache.end() || it->size<best->size)best=it;
   }
   if(best!=d->image_cache.end()){r->storage.memory=best->memory;r->storage.size=best->size;d->image_cache_bytes-=best->size;d->image_cache.erase(best);d->profile.reused++;}
 }
 if(!r->storage.memory && !check(d,d->vk.AllocateMemory(d->device,&ai,nullptr,&r->storage.memory),"AllocateImage"))return false;
 if(!check(d,d->vk.BindImageMemory(d->device,r->storage.image,r->storage.memory,0),"BindImage"))return false;
 if(address) {
   VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};
   d->vk.GetImageSubresourceLayout(d->device,r->storage.image,&sub,&layout);
   if(layout.rowPitch!=r->pitch || layout.offset){error(d,E_INVALIDARG,"shared pitch mismatch");return false;}
 }
 if(!begin(d))return false;
 end_pass(d);
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=ci.initialLayout;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
 b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=r->storage.image;b.subresourceRange={aspects(r->format),0,r->levels,0,r->layers};
 b.srcAccessMask=address?VK_ACCESS_HOST_WRITE_BIT:0;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
 d->vk.CmdPipelineBarrier(d->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
 r->storage.initialized=true;return true;
}
void view_destroy(Device *d,View *v) {
 if(v->image || v->buffer) {
   if(d->recording)d->retired_views.push_back({v->image,v->buffer});
   else {if(v->image)d->vk.DestroyImageView(d->device,v->image,nullptr);if(v->buffer)d->vk.DestroyBufferView(d->device,v->buffer,nullptr);}
 }
 v->image=VK_NULL_HANDLE;v->buffer=VK_NULL_HANDLE;v->generation=0;
}
bool view_create(Device *d,View *v,bool sampled) {
 if(!v || !v->resource)return false;
 if(v->generation==v->resource->generation && (v->image||v->buffer))return true;
 if(v->image||v->buffer)view_destroy(d,v);
 Resource *r=v->resource;
 if(r->storage.buffer) {
   VkBufferViewCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};ci.buffer=r->storage.buffer;ci.format=format(v->format);ci.offset=(VkDeviceSize)v->first*pixel_bytes(v->format);ci.range=(VkDeviceSize)v->count*pixel_bytes(v->format);
   if(!ci.range)ci.range=VK_WHOLE_SIZE;
   if(!check(d,d->vk.CreateBufferView(d->device,&ci,nullptr,&v->buffer),"CreateBufferView"))return false;
 } else {
   VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};ci.image=r->storage.image;ci.format=format(v->format);
   ci.viewType=r->dimension==D3D10DDIRESOURCE_TEXTURE1D?(r->layers>1?VK_IMAGE_VIEW_TYPE_1D_ARRAY:VK_IMAGE_VIEW_TYPE_1D):r->dimension==D3D10DDIRESOURCE_TEXTURE3D?VK_IMAGE_VIEW_TYPE_3D:r->layers>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
   if(sampled && r->dimension==D3D10DDIRESOURCE_TEXTURECUBE)ci.viewType=VK_IMAGE_VIEW_TYPE_CUBE;
   ci.subresourceRange={aspects(v->format),v->level,v->levels,v->layer,v->layers};
   if(sampled && v->format==DXGI_FORMAT_A8_UNORM)ci.components={VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
   if(!check(d,d->vk.CreateImageView(d->device,&ci,nullptr,&v->image),"CreateImageView"))return false;
 }
 v->generation=r->generation;return true;
}
static HRESULT WINAPI host_create(void *user,HANDLE event,HANDLE *context,void **page) {
 Device *d=(Device *)user;A830_CONTEXT_DATA data={A830_ABI_VERSION,0,(GPU_U64)(uintptr_t)event,0};
 D3DDDICB_CREATECONTEXT c{};c.EngineAffinity=1;c.pPrivateDriverData=&data;c.PrivateDriverDataSize=sizeof(data);
 HRESULT hr=d->callbacks.pfnCreateContextCb(d->runtime,&c);if(FAILED(hr))return hr;
 d->context=c.hContext;*context=c.hContext;d->native_commands=c.pCommandBuffer;d->native_bytes=c.CommandBufferSize;
 if(!data.CompletionPage) {
   A830_CONTEXT_QUERY q={A830_ABI_VERSION,0,0};D3DDDICB_ESCAPE e{};e.hDevice=d->runtime;e.hContext=d->context;e.pPrivateDriverData=&q;e.PrivateDriverDataSize=sizeof(q);
   hr=d->callbacks.pfnEscapeCb(d->adapter,&e);if(FAILED(hr))return hr;data.CompletionPage=q.CompletionPage;
 }
 *page=(void *)(uintptr_t)data.CompletionPage;d->completion=(volatile A830_COMPLETION *)*page;d->generation=d->completion?d->completion->Generation:0;
 fprintf(stderr,"A830 DIRECT context=%p commands=%p bytes=%u generation=%u status=%x\n",d->context,d->native_commands,d->native_bytes,d->generation,d->completion?d->completion->Status:~0u);return *page?S_OK:E_FAIL;
}
static HRESULT WINAPI host_render(void *user,const void *commands,UINT size) {
 Device *d=(Device *)user;EnterCriticalSection(&d->lock);
 if(!d->native_commands || size>d->native_bytes){fprintf(stderr,"A830 DIRECT invalid runtime buffer ptr=%p requested=%u capacity=%u\n",d->native_commands,size,d->native_bytes);LeaveCriticalSection(&d->lock);return E_INVALIDARG;}
 memcpy(d->native_commands,commands,size);D3DDDICB_RENDER r{};r.hContext=d->context;r.CommandLength=size;
 HRESULT hr=d->callbacks.pfnRenderCb(d->runtime,&r);d->native_commands=r.pNewCommandBuffer;d->native_bytes=r.NewCommandBufferSize;
 if(FAILED(hr))fprintf(stderr,"A830 UMD render hr=%08lx bytes=%u\n",(unsigned long)hr,size);
 LeaveCriticalSection(&d->lock);return hr;
}
static void WINAPI host_destroy(void *user) {
 Device *d=(Device *)user;if(d->context){D3DDDICB_DESTROYCONTEXT c={d->context};d->callbacks.pfnDestroyContextCb(d->runtime,&c);d->context=nullptr;}
}
bool initialize(Device *d) {
 static volatile LONG64 next=0;d->profile.context=(uint64_t)InterlockedIncrement64(&next);profile_mapping();
 HMODULE self=nullptr;WCHAR path[MAX_PATH];
 if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCWSTR)(uintptr_t)initialize,&self) || !GetModuleFileNameW(self,path,MAX_PATH))return false;
 WCHAR *tail=wcsrchr(path,L'\\');if(!tail)return false;tail[1]=0;wcscat(path,L"vulkan_freedreno.dll");
 d->module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);if(!d->module){error(d,E_FAIL,"Turnip load");return false;}
 auto set=(A830_SET_HOST)GetProcAddress(d->module,"A830SetHost");
 auto gpa=(PFN_vkGetInstanceProcAddr)GetProcAddress(d->module,"vk_icdGetInstanceProcAddr");
 if(!gpa)gpa=(PFN_vkGetInstanceProcAddr)GetProcAddress(d->module,"vkGetInstanceProcAddr");
 if(!set||!gpa){error(d,E_FAIL,"Turnip exports");return false;}
 auto create=(PFN_vkCreateInstance)gpa(nullptr,"vkCreateInstance");
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="A830 Direct DDI";app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ici.pApplicationInfo=&app;
 if(!create || !check(d,create(&ici,nullptr,&d->instance),"CreateInstance"))return false;
#define LOAD_INSTANCE(name) d->vk.name=(PFN_vk##name)gpa(d->instance,"vk" #name);
 VK_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
 UINT count=0;if(!check(d,d->vk.EnumeratePhysicalDevices(d->instance,&count,nullptr),"PhysicalCount")||!count)return false;
 std::vector<VkPhysicalDevice> devices(count);d->vk.EnumeratePhysicalDevices(d->instance,&count,devices.data());d->physical=devices[0];
 d->vk.GetPhysicalDeviceProperties(d->physical,&d->properties);d->vk.GetPhysicalDeviceMemoryProperties(d->physical,&d->memory);
 VkPhysicalDeviceFeatures supported{};d->vk.GetPhysicalDeviceFeatures(d->physical,&supported);
 VkPhysicalDeviceFeatures features{};features.robustBufferAccess=supported.robustBufferAccess;features.independentBlend=supported.independentBlend;features.geometryShader=supported.geometryShader;features.samplerAnisotropy=supported.samplerAnisotropy;features.depthClamp=supported.depthClamp;features.fillModeNonSolid=supported.fillModeNonSolid;features.occlusionQueryPrecise=supported.occlusionQueryPrecise;
 UINT family_count=0;d->vk.GetPhysicalDeviceQueueFamilyProperties(d->physical,&family_count,nullptr);std::vector<VkQueueFamilyProperties> families(family_count);d->vk.GetPhysicalDeviceQueueFamilyProperties(d->physical,&family_count,families.data());
 UINT family=0;while(family<family_count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))family++;if(family==family_count)return false;
 float priority=1;VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
 VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;dci.pEnabledFeatures=&features;
 A830_HOST host={2,sizeof(A830_HOST),d,host_create,host_render,host_destroy};set(&host);VkResult result=d->vk.CreateDevice(d->physical,&dci,nullptr,&d->device);set(nullptr);
 if(!check(d,result,"CreateDevice"))return false;
#define LOAD_DEVICE(name) {auto p=d->vk.GetDeviceProcAddr(d->device,"vk" #name);if(p)d->vk.name=(PFN_vk##name)p;}
 VK_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
 d->vk.GetDeviceQueue(d->device,family,0,&d->queue);
 VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pci.queueFamilyIndex=family;
 if(!check(d,d->vk.CreateCommandPool(d->device,&pci,nullptr,&d->commands),"CommandPool"))return false;
 VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=d->commands;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;
 if(!check(d,d->vk.AllocateCommandBuffers(d->device,&cai,&d->cmd),"CommandBuffer"))return false;
 VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};if(!check(d,d->vk.CreateFence(d->device,&fci,nullptr,&d->fence),"Fence"))return false;
 for(UINT stage=0;stage<3;stage++) {
   VkShaderStageFlags flags=stage==0?VK_SHADER_STAGE_VERTEX_BIT:stage==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_GEOMETRY_BIT;
   std::vector<VkDescriptorSetLayoutBinding> bindings;
   for(UINT i=0;i<16;i++)bindings.push_back({i,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,flags,nullptr});
   for(UINT i=0;i<16;i++)bindings.push_back({16+i,VK_DESCRIPTOR_TYPE_SAMPLER,1,flags,nullptr});
   for(UINT i=0;i<128;i++)bindings.push_back({32+i,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,flags,nullptr});
   for(UINT i=0;i<128;i++)bindings.push_back({160+i,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,flags,nullptr});
   VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=(UINT)bindings.size();ci.pBindings=bindings.data();
   if(!check(d,d->vk.CreateDescriptorSetLayout(d->device,&ci,nullptr,&d->sets[stage]),"DescriptorLayout"))return false;
 }
 VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};lci.setLayoutCount=3;lci.pSetLayouts=d->sets;
 if(!check(d,d->vk.CreatePipelineLayout(d->device,&lci,nullptr,&d->pipeline_layout),"PipelineLayout"))return false;
 VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,32768},{VK_DESCRIPTOR_TYPE_SAMPLER,32768},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,262144},{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,262144}};
 VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1536;pool.poolSizeCount=4;pool.pPoolSizes=sizes;
 if(!check(d,d->vk.CreateDescriptorPool(d->device,&pool,nullptr,&d->descriptors),"DescriptorPool"))return false;
 VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};d->vk.CreatePipelineCache(d->device,&cache,nullptr,&d->pipeline_cache);
 if(!buffer_create(d,d->dummy_buffer,65536,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))return false;
 memset(d->dummy_buffer.map,0,65536);
 d->dummy_image.width=d->dummy_image.height=1;d->dummy_image.dimension=D3D10DDIRESOURCE_TEXTURE2D;d->dummy_image.format=DXGI_FORMAT_R8G8B8A8_UNORM;d->dummy_image.vkformat=VK_FORMAT_R8G8B8A8_UNORM;
 if(!image_create(d,&d->dummy_image))return false;
 VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};d->vk.CmdClearColorImage(d->cmd,d->dummy_image.storage.image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);
 d->dummy_view.resource=&d->dummy_image;d->dummy_view.format=d->dummy_image.format;if(!view_create(d,&d->dummy_view,true))return false;
 VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;sampler.maxLod=1000;
 if(!check(d,d->vk.CreateSampler(d->device,&sampler,nullptr,&d->dummy_sampler.handle),"DummySampler"))return false;
 fprintf(stderr,"A830 DIRECT initialized %s context=%p; no Zink or Gallium renderer\n",d->properties.deviceName,d->context);
 return flush(d);
}
void destroy(Device *d) {
 if(d->device) {
   flush(d);d->vk.DeviceWaitIdle(d->device);
   for(auto &p:d->pipelines){d->vk.DestroyPipeline(d->device,p.second.handle,nullptr);d->vk.DestroyRenderPass(d->device,p.second.pass,nullptr);}
   for(auto &f:d->framebuffers)d->vk.DestroyFramebuffer(d->device,f.handle,nullptr);
   for(auto &s:d->temporary)storage_destroy(d,s);
   for(auto &s:d->renamed)storage_destroy(d,s);
   for(auto &s:d->buffer_cache)storage_destroy(d,s);
   for(auto &s:d->image_cache)storage_destroy(d,s);
   for(auto &v:d->retired_views){if(v.image)d->vk.DestroyImageView(d->device,v.image,nullptr);if(v.buffer)d->vk.DestroyBufferView(d->device,v.buffer,nullptr);}
   for(auto sampler:d->retired_samplers)d->vk.DestroySampler(d->device,sampler,nullptr);
   for(auto query:d->retired_queries)d->vk.DestroyQueryPool(d->device,query,nullptr);
   view_destroy(d,&d->dummy_view);storage_destroy(d,d->dummy_buffer);storage_destroy(d,d->dummy_image.storage);
   if(d->dummy_sampler.handle)d->vk.DestroySampler(d->device,d->dummy_sampler.handle,nullptr);
   if(d->rotation_sampler.handle)d->vk.DestroySampler(d->device,d->rotation_sampler.handle,nullptr);
   if(d->rotation_vertex.module)d->vk.DestroyShaderModule(d->device,d->rotation_vertex.module,nullptr);
   if(d->rotation_pixel.module)d->vk.DestroyShaderModule(d->device,d->rotation_pixel.module,nullptr);
   storage_destroy(d,d->rotation_constants.storage);
   if(d->pipeline_cache)d->vk.DestroyPipelineCache(d->device,d->pipeline_cache,nullptr);
   if(d->descriptors)d->vk.DestroyDescriptorPool(d->device,d->descriptors,nullptr);
   if(d->pipeline_layout)d->vk.DestroyPipelineLayout(d->device,d->pipeline_layout,nullptr);
   for(auto layout:d->sets)if(layout)d->vk.DestroyDescriptorSetLayout(d->device,layout,nullptr);
   if(d->fence)d->vk.DestroyFence(d->device,d->fence,nullptr);
   if(d->commands)d->vk.DestroyCommandPool(d->device,d->commands,nullptr);
   d->vk.DestroyDevice(d->device,nullptr);
 }
 if(d->instance)d->vk.DestroyInstance(d->instance,nullptr);
 if(d->module)FreeLibrary(d->module);
 fprintf(stderr,"A830 DIRECT destroyed draws=%llu clears=%llu copies=%llu\n",d->draws,d->clears,d->copies);
 DeleteCriticalSection(&d->lock);
}
