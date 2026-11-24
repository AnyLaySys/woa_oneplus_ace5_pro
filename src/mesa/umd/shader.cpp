#include "umd.h"

#include "../signature.h"

bool shader_create(Device *d,Shader *s,const UINT *code,const D3D10DDIARG_STAGE_IO_SIGNATURES *io,unsigned stage) {
 if(!code || code[1]<2 || code[1]>1048576){error(d,E_INVALIDARG);return false;}
 auto input=A830Signature(io?io->pInputSignature:nullptr,io?io->NumInputSignatureEntries:0,stage==1,false);
 auto output=A830Signature(io?io->pOutputSignature:nullptr,io?io->NumOutputSignatureEntries:0,stage==1,true);
 std::string key((const char *)code,(size_t)code[1]*4);key.append((const char *)&stage,sizeof(stage));
 for(const auto *signature:{&input,&output}){UINT size=(UINT)signature->size();key.append((const char *)&size,sizeof(size));key.append((const char *)signature->data(),signature->size()*4);}
 bool dual=stage==1 && io && io->NumOutputSignatureEntries==2 && std::any_of(io->pOutputSignature,io->pOutputSignature+2,[](const auto &e){return e.Register==1 && (!e.SystemValue || e.SystemValue==64);});
 key.append((const char *)&dual,sizeof(dual));
 auto cached=d->shader_cache.find(key);if(cached!=d->shader_cache.end()){cached->second.references++;*s=cached->second;s->owner=&cached->second;return true;}
 vkd3d_shader_dxbc_section_desc sections[3]={{0x4e475349,{input.data(),input.size()*4}},{0x4e47534f,{output.data(),output.size()*4}},{0x52444853,{code,(size_t)code[1]*4}}};
 vkd3d_shader_code dxbc{},spirv{};
 int result=vkd3d_shader_serialize_dxbc(3,sections,&dxbc,nullptr);
 if(result<0){error(d,E_FAIL);return false;}
 vkd3d_shader_scan_descriptor_info scan{};scan.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;
 vkd3d_shader_compile_info ci{};ci.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;ci.next=&scan;ci.source=dxbc;ci.source_type=VKD3D_SHADER_SOURCE_DXBC_TPF;ci.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;ci.log_level=VKD3D_SHADER_LOG_NONE;
 result=vkd3d_shader_scan(&ci,nullptr);
 std::vector<vkd3d_shader_resource_binding> bindings;
 if(result>=0)for(UINT i=0;i<scan.descriptor_count;i++) {
   const auto &r=scan.descriptors[i];
   for(UINT n=0;n<r.count;n++) {
     UINT reg=r.register_index+n,binding=0;bool image=r.resource_type!=VKD3D_SHADER_RESOURCE_BUFFER;
     if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV && reg<16){s->cb.push_back(reg);binding=reg;}
     else if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER && reg<16){s->samplers.push_back(reg);binding=16+reg;}
     else if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV && reg<128){s->textures.push_back(reg);s->texel[reg]=!image;binding=(image?32:160)+reg;}
     else {error(d,DXGI_ERROR_UNSUPPORTED);result=-1;break;}
     vkd3d_shader_resource_binding b{};b.type=r.type;b.register_space=r.register_space;b.register_index=reg;b.shader_visibility=VKD3D_SHADER_VISIBILITY_ALL;
     b.flags=r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER?(VKD3D_SHADER_BINDING_FLAG_IMAGE|VKD3D_SHADER_BINDING_FLAG_BUFFER):image?VKD3D_SHADER_BINDING_FLAG_IMAGE:VKD3D_SHADER_BINDING_FLAG_BUFFER;
     if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV)b.flags=VKD3D_SHADER_BINDING_FLAG_BUFFER;
     b.binding={stage,binding,1};bindings.push_back(b);
   }
 }
 vkd3d_shader_interface_info binding_info{};binding_info.type=VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;binding_info.bindings=bindings.data();binding_info.binding_count=(UINT)bindings.size();
 vkd3d_shader_spirv_target_info target{};target.type=VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;target.environment=VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;binding_info.next=&target;ci.next=&binding_info;
 if(result>=0)result=vkd3d_shader_compile(&ci,&spirv,nullptr);
 vkd3d_shader_free_scan_descriptor_info(&scan);
 if(result<0){vkd3d_shader_free_shader_code(&dxbc);error(d,DXGI_ERROR_UNSUPPORTED);return false;}
 VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=spirv.size;mi.pCode=(const UINT *)spirv.code;
 bool okay=check(d,d->vk.CreateShaderModule(d->device,&mi,nullptr,&s->module));vkd3d_shader_free_shader_code(&spirv);
 if(okay && dual) {
   target.dual_source_blending=true;spirv={};
   result=vkd3d_shader_compile(&ci,&spirv,nullptr);
   if(result>=0){mi.codeSize=spirv.size;mi.pCode=(const UINT *)spirv.code;okay=check(d,d->vk.CreateShaderModule(d->device,&mi,nullptr,&s->dual_module));}
   vkd3d_shader_free_shader_code(&spirv);
 }
 vkd3d_shader_free_shader_code(&dxbc);
 if(!okay){if(s->module)d->vk.DestroyShaderModule(d->device,s->module,nullptr);if(s->dual_module)d->vk.DestroyShaderModule(d->device,s->dual_module,nullptr);s->module=s->dual_module=VK_NULL_HANDLE;return false;}
 for(auto *slots:{&s->cb,&s->samplers,&s->textures}){std::sort(slots->begin(),slots->end());slots->erase(std::unique(slots->begin(),slots->end()),slots->end());}
  if(d->shader_cache.size()>=512) {
   auto old=std::find_if(d->shader_cache.begin(),d->shader_cache.end(),[](const auto &entry){return !entry.second.references;});
   if(old!=d->shader_cache.end()){auto &shader=old->second;d->vk.DestroyShaderModule(d->device,shader.module,nullptr);if(shader.dual_module)d->vk.DestroyShaderModule(d->device,shader.dual_module,nullptr);d->shader_cache.erase(old);}
 }
 s->id=d->next_shader++;s->references=1;auto entry=d->shader_cache.emplace(std::move(key),*s);s->owner=&entry.first->second;return true;
}
