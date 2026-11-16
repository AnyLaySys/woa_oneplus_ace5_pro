#include "umd.h"

static std::vector<uint32_t> signature(const D3D10DDIARG_SIGNATURE_ENTRY *entries,UINT count,unsigned stage,bool output) {
 std::vector<uint32_t> words(2+count*6);words[0]=count;words[1]=8;
 for(UINT i=0;i<count;i++) {
   UINT sys=entries[i].SystemValue,reg=entries[i].Register;
   std::string name="TEXCOORD";
   if(sys==D3D10_SB_NAME_POSITION)name="SV_Position";
   else if(sys==D3D10_SB_NAME_VERTEX_ID)name="SV_VertexID";
   else if(sys==D3D10_SB_NAME_INSTANCE_ID)name="SV_InstanceID";
   else if(sys==D3D10_SB_NAME_IS_FRONT_FACE)name="SV_IsFrontFace";
   else if(sys==D3D10_SB_NAME_PRIMITIVE_ID)name="SV_PrimitiveID";
   if(stage==1 && output && (!sys || sys==64)){name="SV_Target";sys=64;}
   UINT at=2+i*6;words[at]=(UINT)words.size()*4;words[at+1]=(name=="TEXCOORD" || name=="SV_Target")?reg:0;
   words[at+2]=sys;words[at+3]=(sys==D3D10_SB_NAME_VERTEX_ID || sys==D3D10_SB_NAME_INSTANCE_ID || sys==D3D10_SB_NAME_IS_FRONT_FACE || sys==D3D10_SB_NAME_PRIMITIVE_ID)?1:3;
   words[at+4]=reg;words[at+5]=entries[i].Mask|((output?0:entries[i].Mask)<<8);
   size_t offset=words.size()*4;words.resize(words.size()+(name.size()+4)/4);memcpy((char *)words.data()+offset,name.c_str(),name.size()+1);
 }
 return words;
}
bool shader_create(Device *d,Shader *s,const UINT *code,const D3D10DDIARG_STAGE_IO_SIGNATURES *io,unsigned stage) {

 if(!code || code[1]<2 || code[1]>1048576){error(d,E_INVALIDARG);return false;}
 auto input=signature(io?io->pInputSignature:nullptr,io?io->NumInputSignatureEntries:0,stage,false);
 auto output=signature(io?io->pOutputSignature:nullptr,io?io->NumOutputSignatureEntries:0,stage,true);
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
     if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV && reg<16){s->cb[reg]=true;binding=reg;}
     else if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER && reg<16){s->samplers[reg]=true;binding=16+reg;}
     else if(r.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV && reg<128){s->textures[reg]=true;s->texel[reg]=!image;binding=(image?32:160)+reg;}
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
 if(okay && stage==1 && io && io->NumOutputSignatureEntries==2 &&
    std::any_of(io->pOutputSignature,io->pOutputSignature+2,[](const auto &e){return e.Register==1 && (!e.SystemValue || e.SystemValue==64);})) {
   target.dual_source_blending=true;spirv={};
   result=vkd3d_shader_compile(&ci,&spirv,nullptr);

   if(result>=0){mi.codeSize=spirv.size;mi.pCode=(const UINT *)spirv.code;okay=check(d,d->vk.CreateShaderModule(d->device,&mi,nullptr,&s->dual_module));}
   vkd3d_shader_free_shader_code(&spirv);
 }
 vkd3d_shader_free_shader_code(&dxbc);
 uint64_t id=14695981039346656037ull;const auto *bytes=(const unsigned char *)code;
 for(size_t i=0;i<(size_t)code[1]*4;i++){id^=bytes[i];id*=1099511628211ull;}
 for(uint32_t value:input){id^=value;id*=1099511628211ull;}for(uint32_t value:output){id^=value;id*=1099511628211ull;}
 s->id=id;s->stage=stage;return okay;
}
