#include "umd.h"

static VkBlendFactor blend_factor(D3D10_DDI_BLEND b) {
 switch(b){
 case D3D10_DDI_BLEND_ZERO:return VK_BLEND_FACTOR_ZERO;case D3D10_DDI_BLEND_ONE:return VK_BLEND_FACTOR_ONE;
 case D3D10_DDI_BLEND_SRC_COLOR:return VK_BLEND_FACTOR_SRC_COLOR;case D3D10_DDI_BLEND_INV_SRC_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
 case D3D10_DDI_BLEND_SRC_ALPHA:return VK_BLEND_FACTOR_SRC_ALPHA;case D3D10_DDI_BLEND_INV_SRC_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
 case D3D10_DDI_BLEND_DEST_ALPHA:return VK_BLEND_FACTOR_DST_ALPHA;case D3D10_DDI_BLEND_INV_DEST_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
 case D3D10_DDI_BLEND_DEST_COLOR:return VK_BLEND_FACTOR_DST_COLOR;case D3D10_DDI_BLEND_INV_DEST_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
 case D3D10_DDI_BLEND_SRC_ALPHASAT:return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
 case D3D10_DDI_BLEND_BLEND_FACTOR:return VK_BLEND_FACTOR_CONSTANT_COLOR;case D3D10_DDI_BLEND_INVBLEND_FACTOR:return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
 case D3D10_DDI_BLEND_SRC1_COLOR:return VK_BLEND_FACTOR_SRC1_COLOR;case D3D10_DDI_BLEND_INV_SRC1_COLOR:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
 case D3D10_DDI_BLEND_SRC1_ALPHA:return VK_BLEND_FACTOR_SRC1_ALPHA;case D3D10_DDI_BLEND_INV_SRC1_ALPHA:return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
 default:return VK_BLEND_FACTOR_ONE;}
}
static VkStencilOp stencil_op(D3D10_DDI_STENCIL_OP o) {
 static const VkStencilOp ops[]={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_ZERO,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_INCREMENT_AND_CLAMP,VK_STENCIL_OP_DECREMENT_AND_CLAMP,VK_STENCIL_OP_INVERT,VK_STENCIL_OP_INCREMENT_AND_WRAP,VK_STENCIL_OP_DECREMENT_AND_WRAP};
 return (unsigned)o<9?ops[o]:VK_STENCIL_OP_KEEP;
}
static VkStencilOpState stencil(const D3D10_DDI_DEPTH_STENCILOP_DESC &s,UINT read,UINT write) {
 return {stencil_op(s.StencilFailOp),stencil_op(s.StencilPassOp),stencil_op(s.StencilDepthFailOp),(VkCompareOp)(s.StencilFunc-1),read,write,0};
}
static VkPrimitiveTopology topology(D3D10_DDI_PRIMITIVE_TOPOLOGY t) {
 switch(t){case D3D10_DDI_PRIMITIVE_TOPOLOGY_POINTLIST:return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
 case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINELIST:return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
 case D3D10_DDI_PRIMITIVE_TOPOLOGY_LINESTRIP:return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
 case D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
 default:return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;}
}
template<class T> static void key_append(std::string &key,const T &v){key.append((const char *)&v,sizeof(v));}
VkRenderPass render_pass(Device *d,const std::array<VkFormat,9> &formats,UINT targets,const std::array<VkImageAspectFlags,9> &discarded) {
 std::string key;key_append(key,formats);key_append(key,targets);key_append(key,discarded);
 auto found=d->render_passes.find(key);if(found!=d->render_passes.end())return found->second;
 std::vector<VkAttachmentDescription> attachments;std::vector<VkAttachmentReference> colors;
 for(UINT i=0;i<targets;i++) {
   VkAttachmentReference ref{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
   if(formats[i]!=VK_FORMAT_UNDEFINED){ref.attachment=(UINT)attachments.size();attachments.push_back({0,formats[i],VK_SAMPLE_COUNT_1_BIT,discarded[i]?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL});}
   colors.push_back(ref);
 }
 VkAttachmentReference depth{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};bool has_depth=formats[8]!=VK_FORMAT_UNDEFINED;
 if(has_depth){depth.attachment=(UINT)attachments.size();attachments.push_back({0,formats[8],VK_SAMPLE_COUNT_1_BIT,discarded[8]&VK_IMAGE_ASPECT_DEPTH_BIT?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,discarded[8]&VK_IMAGE_ASPECT_STENCIL_BIT?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL});}
 VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=targets;sub.pColorAttachments=colors.data();sub.pDepthStencilAttachment=has_depth?&depth:nullptr;
 VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ci.attachmentCount=(UINT)attachments.size();ci.pAttachments=attachments.data();ci.subpassCount=1;ci.pSubpasses=&sub;
 VkRenderPass pass=VK_NULL_HANDLE;if(!check(d,d->vk.CreateRenderPass(d->device,&ci,nullptr,&pass)))return VK_NULL_HANDLE;
 d->render_passes.emplace(std::move(key),pass);return pass;
}
bool begin_pass(Device *d,VkRenderPass pass,const std::vector<VkImageView> &views,const std::array<VkFormat,9> &formats,UINT width,UINT height,UINT targets) {
 if(d->pass_views!=views || d->pass_formats!=formats || d->pass_targets!=targets)end_pass(d);
 if(d->render_pass)return true;
 VkFramebuffer fb=VK_NULL_HANDLE;
 for(auto &f:d->framebuffers)if(f.views==views && f.formats==formats && f.width==width && f.height==height && f.targets==targets){fb=f.handle;break;}
 if(!fb) {
   VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fci.renderPass=pass;fci.attachmentCount=(UINT)views.size();fci.pAttachments=views.data();fci.width=width;fci.height=height;fci.layers=1;
   if(!check(d,d->vk.CreateFramebuffer(d->device,&fci,nullptr,&fb)))return false;
   if(d->framebuffers.size()>=256){d->retired_framebuffers.push_back(d->framebuffers.front().handle);d->framebuffers.erase(d->framebuffers.begin());}
   d->framebuffers.push_back({fb,views,formats,width,height,targets});
 }
 VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.framebuffer=fb;rp.renderArea.extent={width,height};
 d->vk.CmdBeginRenderPass(d->cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);d->render_pass=true;d->pass_views=views;d->pass_formats=formats;d->pass_targets=targets;return true;
}
bool draw(Device *d,UINT count,UINT first,INT bias,UINT instances,UINT first_instance,bool indexed) {
 if(!count || !instances)return true;
 if(!begin(d))return false;
 if(!d->shaders[0] || !d->shaders[1]){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
 UINT width=~0u,height=~0u;std::array<VkFormat,9> formats{};auto &views=d->draw_views;views.clear();
 for(UINT i=0;i<d->target_count;i++) {
   View *v=d->targets[i];if(!v){formats[i]=VK_FORMAT_UNDEFINED;continue;}
   if(!view_create(d,v))return false;formats[i]=format(v->format);views.push_back(v->image);
   UINT shift=v->level+(planar(v->resource->format) && (v->format==DXGI_FORMAT_R8G8_UNORM || v->format==DXGI_FORMAT_R16G16_UNORM));
   width=std::min(width,std::max(1u,v->resource->width>>shift));height=std::min(height,std::max(1u,v->resource->height>>shift));
 }
 if(d->depth_view) {
   if(!view_create(d,d->depth_view))return false;formats[8]=format(d->depth_view->format);views.push_back(d->depth_view->image);
   width=std::min(width,std::max(1u,d->depth_view->resource->width>>d->depth_view->level));height=std::min(height,std::max(1u,d->depth_view->resource->height>>d->depth_view->level));
 }
 if(width==~0u || !width || !height){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
 if(d->pass_views!=views || d->pass_formats!=formats || d->pass_targets!=d->target_count)end_pass(d);
 Pipeline pipeline=d->current_pipeline;
 if(d->pipeline_dirty || d->pipeline_indexed!=indexed || d->pipeline_formats!=formats) {
   auto &key=d->pipeline_key;key.clear();for(auto *s:d->shaders)key_append(key,s?s->id:0ull);
   UINT elements=d->layout?(UINT)d->layout->elements.size():0;key_append(key,elements);
   if(d->layout)for(const auto &e:d->layout->elements){key_append(key,e.InputSlot);key_append(key,e.AlignedByteOffset);key_append(key,e.Format);key_append(key,e.InputSlotClass);key_append(key,e.InstanceDataStepRate);key_append(key,e.InputRegister);}
   key_append(key,d->blend!=nullptr);if(d->blend)key_append(key,d->blend->desc);
   key_append(key,d->depth!=nullptr);if(d->depth)key_append(key,d->depth->desc);
   key_append(key,d->raster!=nullptr);if(d->raster)key_append(key,d->raster->desc);
   key_append(key,indexed);
   key_append(key,d->topology);key_append(key,d->sample_mask);key_append(key,d->target_count);key_append(key,formats);
   for(auto &v:d->vertices)key_append(key,v.stride);
   auto found=d->pipelines.find(key);
   bool dual=false;
   if(d->blend && d->blend->desc.BlendEnable[0]) {
     const auto &b=d->blend->desc;
     for(auto factor:{b.SrcBlend,b.DestBlend,b.SrcBlendAlpha,b.DestBlendAlpha})
       if(factor>=D3D10_DDI_BLEND_SRC1_COLOR && factor<=D3D10_DDI_BLEND_INV_SRC1_ALPHA)dual=true;
   }
   if(dual && (d->target_count!=1 || !d->shaders[1]->dual_module)){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
   if(found==d->pipelines.end()) {
     pipeline.pass=render_pass(d,formats,d->target_count);if(!pipeline.pass)return false;
     std::vector<VkPipelineShaderStageCreateInfo> stages;
     for(UINT i=0;i<3;i++)if(d->shaders[i]){VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};st.stage=i==0?VK_SHADER_STAGE_VERTEX_BIT:i==1?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_GEOMETRY_BIT;st.module=i==1 && dual?d->shaders[i]->dual_module:d->shaders[i]->module;st.pName="main";stages.push_back(st);}
     std::vector<VkVertexInputBindingDescription> bindings;std::vector<VkVertexInputAttributeDescription> attributes;
     std::array<bool,32> used{};
     if(d->layout)for(const auto &e:d->layout->elements){
       if(e.InputSlot>=32){error(d,E_INVALIDARG);return false;}
       if(!used[e.InputSlot]){bindings.push_back({e.InputSlot,d->vertices[e.InputSlot].stride,e.InputSlotClass==D3D10_DDI_INPUT_PER_INSTANCE_DATA?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX});used[e.InputSlot]=true;}
       if(e.InstanceDataStepRate>1){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
       attributes.push_back({e.InputRegister,e.InputSlot,format(e.Format),e.AlignedByteOffset});
     }
     VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vertex.vertexBindingDescriptionCount=(UINT)bindings.size();vertex.pVertexBindingDescriptions=bindings.data();vertex.vertexAttributeDescriptionCount=(UINT)attributes.size();vertex.pVertexAttributeDescriptions=attributes.data();
     VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=topology(d->topology);assembly.primitiveRestartEnable=indexed && (assembly.topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || assembly.topology==VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
     VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
     VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_BACK_BIT;raster.frontFace=VK_FRONT_FACE_CLOCKWISE;raster.lineWidth=1;
     if(d->raster){auto &r=d->raster->desc;raster.polygonMode=r.FillMode==D3D10_DDI_FILL_WIREFRAME?VK_POLYGON_MODE_LINE:VK_POLYGON_MODE_FILL;raster.cullMode=r.CullMode==D3D10_DDI_CULL_NONE?VK_CULL_MODE_NONE:r.CullMode==D3D10_DDI_CULL_FRONT?VK_CULL_MODE_FRONT_BIT:VK_CULL_MODE_BACK_BIT;raster.frontFace=r.FrontCounterClockwise?VK_FRONT_FACE_COUNTER_CLOCKWISE:VK_FRONT_FACE_CLOCKWISE;raster.depthClampEnable=!r.DepthClipEnable;raster.depthBiasEnable=r.DepthBias || r.SlopeScaledDepthBias;raster.depthBiasConstantFactor=(float)r.DepthBias;raster.depthBiasSlopeFactor=r.SlopeScaledDepthBias;raster.depthBiasClamp=r.DepthBiasClamp;}
     VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;ms.pSampleMask=&d->sample_mask;ms.alphaToCoverageEnable=d->blend && d->blend->desc.AlphaToCoverageEnable;
     VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};depth.depthCompareOp=VK_COMPARE_OP_LESS;
     if(d->depth_view){depth.depthTestEnable=true;depth.depthWriteEnable=true;if(d->depth){auto &s=d->depth->desc;depth.depthTestEnable=s.DepthEnable;depth.depthWriteEnable=s.DepthWriteMask==D3D10_DDI_DEPTH_WRITE_MASK_ALL;depth.depthCompareOp=(VkCompareOp)(s.DepthFunc-1);depth.stencilTestEnable=s.StencilEnable;depth.front=stencil(s.FrontFace,s.StencilReadMask,s.StencilWriteMask);depth.back=stencil(s.BackFace,s.StencilReadMask,s.StencilWriteMask);}}
     std::vector<VkPipelineColorBlendAttachmentState> blend;
     for(UINT i=0;i<d->target_count;i++){VkPipelineColorBlendAttachmentState b{};b.colorWriteMask=15;if(d->blend){auto &s=d->blend->desc;b.blendEnable=s.BlendEnable[i];b.srcColorBlendFactor=blend_factor(s.SrcBlend);b.dstColorBlendFactor=blend_factor(s.DestBlend);b.colorBlendOp=(VkBlendOp)(s.BlendOp-1);b.srcAlphaBlendFactor=blend_factor(s.SrcBlendAlpha);b.dstAlphaBlendFactor=blend_factor(s.DestBlendAlpha);b.alphaBlendOp=(VkBlendOp)(s.BlendOpAlpha-1);b.colorWriteMask=s.RenderTargetWriteMask[i];}blend.push_back(b);}
     VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};cb.attachmentCount=(UINT)blend.size();cb.pAttachments=blend.data();
     VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_REFERENCE};VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=4;dynamic.pDynamicStates=states;
     VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.stageCount=(UINT)stages.size();ci.pStages=stages.data();ci.pVertexInputState=&vertex;ci.pInputAssemblyState=&assembly;ci.pViewportState=&viewport;ci.pRasterizationState=&raster;ci.pMultisampleState=&ms;ci.pDepthStencilState=&depth;ci.pColorBlendState=&cb;ci.pDynamicState=&dynamic;ci.layout=d->pipeline_layout;ci.renderPass=pipeline.pass;
     if(!check(d,d->vk.CreateGraphicsPipelines(d->device,d->pipeline_cache,1,&ci,nullptr,&pipeline.handle)))return false;
     d->pipelines.emplace(key,pipeline);
   } else pipeline=found->second;
   d->current_pipeline=pipeline;d->pipeline_dirty=false;d->pipeline_indexed=indexed;d->pipeline_formats=formats;
 }
 std::array<VkDescriptorSet,3> sets{};std::array<UINT,3> offsets{};
 for(UINT stage=0;stage<3;stage++) {
   const VkPipelineStageFlags shader_stage=stage==0?VK_PIPELINE_STAGE_VERTEX_SHADER_BIT:stage==1?VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT:VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
   Shader *shader=d->shaders[stage];
   if(shader) {
     for(UINT i:shader->cb)access(d,d->cb[stage][i]?d->cb[stage][i]->storage:d->dummy_buffer,shader_stage,VK_ACCESS_UNIFORM_READ_BIT);
     for(UINT i:shader->textures) {
       View *v=d->textures[stage][i];if(!v)v=&d->dummy_view;if(!view_create(d,v,true))return false;
       access(d,v->resource->storage,shader_stage,VK_ACCESS_SHADER_READ_BIT,false,v->level,v->levels,v->layer,v->layers);
     }
     if(!shader->cb.empty() && shader->cb.front()==0 && d->cb[stage][0] && d->cb_first[stage][0]<d->cb[stage][0]->width)offsets[stage]=d->cb_first[stage][0];
   }
   if(d->bound_sets[stage] && !d->descriptor_dirty[stage] && d->descriptor_revision[stage]==d->resource_revision){sets[stage]=d->bound_sets[stage];continue;}
   auto &key=d->descriptor_key;key.clear();key_append(key,shader?shader->id:0ull);
   if(shader) {
     for(UINT i:shader->cb) {
       Resource *r=d->cb[stage][i];VkDeviceSize offset=r?d->cb_first[stage][i]:0,size=r?r->width:65536;
       if(offset>=size){r=nullptr;offset=0;size=65536;}else size-=offset;
       if(r && d->cb_count[stage][i])size=std::min(size,(VkDeviceSize)d->cb_count[stage][i]);
       key_append(key,r?r->storage.buffer:d->dummy_buffer.buffer);key_append(key,i?offset:0ull);key_append(key,size);
     }
     for(UINT i:shader->samplers)key_append(key,d->samplers[stage][i]?d->samplers[stage][i]->handle:d->dummy_sampler.handle);
     for(UINT i:shader->textures){View *v=d->textures[stage][i];if(!v)v=&d->dummy_view;key_append(key,v->image);key_append(key,v->buffer);}
   }
   d->descriptor_dirty[stage]=false;d->descriptor_revision[stage]=d->resource_revision;
   auto &cache=d->descriptor_cache[stage];auto found=cache.find(key);
   if(found!=cache.end()){sets[stage]=found->second;continue;}
   VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=d->descriptors;dai.descriptorSetCount=1;dai.pSetLayouts=d->sets+stage;
   if(!check(d,d->vk.AllocateDescriptorSets(d->device,&dai,&sets[stage])))return false;
   VkWriteDescriptorSet writes[160]{};VkDescriptorBufferInfo buffers[16]{};VkDescriptorImageInfo images[144]{};UINT count=0;
   if(shader) {
     for(UINT i:shader->cb) {
       Resource *r=d->cb[stage][i];VkDeviceSize offset=r?d->cb_first[stage][i]:0,size=r?r->width:65536;
       if(offset>=size){r=nullptr;offset=0;size=65536;}else size-=offset;
       if(r && d->cb_count[stage][i])size=std::min(size,(VkDeviceSize)d->cb_count[stage][i]);
       buffers[i]={r?r->storage.buffer:d->dummy_buffer.buffer,i?offset:0,size};
       auto &write=writes[count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=sets[stage];write.dstBinding=i;write.descriptorCount=1;write.descriptorType=i?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;write.pBufferInfo=buffers+i;
     }
     for(UINT i:shader->samplers) {
       images[i].sampler=d->samplers[stage][i]?d->samplers[stage][i]->handle:d->dummy_sampler.handle;
       auto &write=writes[count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=sets[stage];write.dstBinding=16+i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;write.pImageInfo=images+i;
     }
     for(UINT i:shader->textures) {
       View *v=d->textures[stage][i];if(!v)v=&d->dummy_view;images[16+i]={VK_NULL_HANDLE,v->image,VK_IMAGE_LAYOUT_GENERAL};
       auto &write=writes[count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=sets[stage];write.dstBinding=(shader->texel[i]?160:32)+i;write.descriptorCount=1;write.descriptorType=shader->texel[i]?VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=images+16+i;write.pTexelBufferView=&v->buffer;
     }
   }
   if(count)d->vk.UpdateDescriptorSets(d->device,count,writes,0,nullptr);
   cache.emplace(key,sets[stage]);
 }
 for(auto &v:d->vertices)if(v.resource)access(d,v.resource->storage,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
 if(indexed){if(!d->indices){end_pass(d);error(d,E_INVALIDARG);return false;}access(d,d->indices->storage,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,VK_ACCESS_INDEX_READ_BIT);}
 for(UINT i=0;i<d->target_count;i++)if(d->targets[i])access(d,d->targets[i]->resource->storage,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,true,d->targets[i]->level,1,d->targets[i]->layer,d->targets[i]->layers);
 if(d->depth_view)access(d,d->depth_view->resource->storage,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,true,d->depth_view->level,1,d->depth_view->layer,d->depth_view->layers);
 std::array<VkImageAspectFlags,9> discarded{};bool discard=false;
 for(UINT i=0;i<d->target_count;i++)if(d->targets[i]){discarded[i]=d->targets[i]->resource->storage.discarded&VK_IMAGE_ASPECT_COLOR_BIT;discard|=discarded[i]!=0;}
 if(d->depth_view){discarded[8]=d->depth_view->resource->storage.discarded;discard|=discarded[8]!=0;}
 VkRenderPass pass=pipeline.pass;
 if(discard){pass=render_pass(d,formats,d->target_count,discarded);if(!pass)return false;end_pass(d);}
 if(!begin_pass(d,pass,views,formats,width,height,d->target_count))return false;
 for(UINT i=0;i<d->target_count;i++)if(d->targets[i])d->targets[i]->resource->storage.discarded=0;
 if(d->depth_view)d->depth_view->resource->storage.discarded=0;
 if(d->bound_pipeline!=pipeline.handle){d->vk.CmdBindPipeline(d->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.handle);d->bound_pipeline=pipeline.handle;}
 if(d->bound_sets!=sets || d->bound_offsets!=offsets){d->vk.CmdBindDescriptorSets(d->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,d->pipeline_layout,0,3,sets.data(),3,offsets.data());d->bound_sets=sets;d->bound_offsets=offsets;}
 VkViewport viewport{d->viewport.TopLeftX,d->viewport.TopLeftY+d->viewport.Height,d->viewport.Width,-d->viewport.Height,d->viewport.MinDepth,d->viewport.MaxDepth};
 if(!viewport.width || !viewport.height)viewport={0,(float)height,(float)width,-(float)height,0,1};
 if(!d->bound.dynamic || memcmp(&d->bound.viewport,&viewport,sizeof(viewport))){d->vk.CmdSetViewport(d->cmd,0,1,&viewport);d->bound.viewport=viewport;}
 VkRect2D scissor{{0,0},{width,height}};
 if(d->raster && d->raster->desc.ScissorEnable){LONG x=std::max<LONG>(0,d->scissor.left),y=std::max<LONG>(0,d->scissor.top);LONG right=std::min<LONG>(width,d->scissor.right),bottom=std::min<LONG>(height,d->scissor.bottom);scissor={{x,y},{(UINT)std::max<LONG>(0,right-x),(UINT)std::max<LONG>(0,bottom-y)}};}
 if(!d->bound.dynamic || memcmp(&d->bound.scissor,&scissor,sizeof(scissor))){d->vk.CmdSetScissor(d->cmd,0,1,&scissor);d->bound.scissor=scissor;}
 if(!d->bound.dynamic || memcmp(d->bound.blend.data(),d->blend_color,16)){d->vk.CmdSetBlendConstants(d->cmd,d->blend_color);memcpy(d->bound.blend.data(),d->blend_color,16);}
 if(!d->bound.dynamic || d->bound.stencil!=d->stencil_ref){d->vk.CmdSetStencilReference(d->cmd,VK_STENCIL_FACE_FRONT_AND_BACK,d->stencil_ref);d->bound.stencil=d->stencil_ref;}
 d->bound.dynamic=true;
 for(UINT i=0;i<32;i++)if(d->vertices[i].resource){VkBuffer b=d->vertices[i].resource->storage.buffer;VkDeviceSize offset=d->vertices[i].offset;if(d->bound.vertices[i]!=b || d->bound.offsets[i]!=offset){d->vk.CmdBindVertexBuffers(d->cmd,i,1,&b,&offset);d->bound.vertices[i]=b;d->bound.offsets[i]=offset;}}
 for(auto *q:d->queries)if(q->active && q->pool && q->count<256)d->vk.CmdBeginQuery(d->cmd,q->pool,q->count,VK_QUERY_CONTROL_PRECISE_BIT);
 if(indexed){VkBuffer b=d->indices->storage.buffer;VkIndexType type=d->index_size==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;if(d->bound.index!=b || d->bound.index_offset!=d->index_offset || d->bound.index_type!=type){d->vk.CmdBindIndexBuffer(d->cmd,b,d->index_offset,type);d->bound.index=b;d->bound.index_offset=d->index_offset;d->bound.index_type=type;}d->vk.CmdDrawIndexed(d->cmd,count,instances,first,bias,first_instance);}
 else d->vk.CmdDraw(d->cmd,count,instances,first,first_instance);
 for(auto *q:d->queries)if(q->active && q->pool && q->count<256){d->vk.CmdEndQuery(d->cmd,q->pool,q->count);q->count++;}

 if(d->transient_bytes>=67108864 || d->retired_framebuffers.size()>=400 || d->descriptor_cache[0].size()+d->descriptor_cache[1].size()+d->descriptor_cache[2].size()>=1500)return flush(d);return true;
}
