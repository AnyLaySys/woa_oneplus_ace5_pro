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
bool draw(Device *d,UINT count,UINT first,INT bias,UINT instances,UINT first_instance,bool indexed) {

 if(!count || !instances)return true;
 if(!d->shaders[0] || !d->shaders[1]){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
 UINT width=~0u,height=~0u;std::array<VkFormat,9> formats{};std::vector<VkImageView> views;
 for(UINT i=0;i<d->target_count;i++) {
   View *v=d->targets[i];if(!v){formats[i]=VK_FORMAT_UNDEFINED;continue;}
   if(!view_create(d,v))return false;formats[i]=format(v->format);views.push_back(v->image);
   width=std::min(width,std::max(1u,v->resource->width>>v->level));height=std::min(height,std::max(1u,v->resource->height>>v->level));
 }
 if(d->depth_view) {
   if(!view_create(d,d->depth_view))return false;formats[8]=format(d->depth_view->format);views.push_back(d->depth_view->image);
   width=std::min(width,std::max(1u,d->depth_view->resource->width>>d->depth_view->level));height=std::min(height,std::max(1u,d->depth_view->resource->height>>d->depth_view->level));
 }
 if(width==~0u || !width || !height){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
 std::string key;for(auto *s:d->shaders)key_append(key,s?s->id:0ull);
 UINT elements=d->layout?(UINT)d->layout->elements.size():0;key_append(key,elements);
 if(d->layout)for(const auto &e:d->layout->elements){key_append(key,e.InputSlot);key_append(key,e.AlignedByteOffset);key_append(key,e.Format);key_append(key,e.InputSlotClass);key_append(key,e.InstanceDataStepRate);key_append(key,e.InputRegister);}
 key_append(key,d->blend!=nullptr);if(d->blend)key_append(key,d->blend->desc);
 key_append(key,d->depth!=nullptr);if(d->depth)key_append(key,d->depth->desc);
 key_append(key,d->raster!=nullptr);if(d->raster)key_append(key,d->raster->desc);
 key_append(key,indexed);
 key_append(key,d->topology);key_append(key,d->sample_mask);key_append(key,d->target_count);key_append(key,formats);
 for(auto &v:d->vertices)key_append(key,v.stride);
 auto found=d->pipelines.find(key);Pipeline pipeline{};
 bool dual=false;
 if(d->blend && d->blend->desc.BlendEnable[0]) {
   const auto &b=d->blend->desc;
   for(auto factor:{b.SrcBlend,b.DestBlend,b.SrcBlendAlpha,b.DestBlendAlpha})
     if(factor>=D3D10_DDI_BLEND_SRC1_COLOR && factor<=D3D10_DDI_BLEND_INV_SRC1_ALPHA)dual=true;
 }
 if(dual && (d->target_count!=1 || !d->shaders[1]->dual_module)){error(d,DXGI_ERROR_UNSUPPORTED);return false;}
 if(found==d->pipelines.end()) {

   std::vector<VkAttachmentDescription> attachments;std::vector<VkAttachmentReference> colors;
   for(UINT i=0;i<d->target_count;i++) {
     VkAttachmentReference ref{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
     if(formats[i]!=VK_FORMAT_UNDEFINED){ref.attachment=(UINT)attachments.size();attachments.push_back({0,formats[i],VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL});}
     colors.push_back(ref);
   }
   VkAttachmentReference depth_ref{VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
   if(formats[8]!=VK_FORMAT_UNDEFINED){depth_ref.attachment=(UINT)attachments.size();attachments.push_back({0,formats[8],VK_SAMPLE_COUNT_1_BIT,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_LOAD,VK_ATTACHMENT_STORE_OP_STORE,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL});}
   VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=(UINT)colors.size();sub.pColorAttachments=colors.data();sub.pDepthStencilAttachment=d->depth_view?&depth_ref:nullptr;
   VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};pass.attachmentCount=(UINT)attachments.size();pass.pAttachments=attachments.data();pass.subpassCount=1;pass.pSubpasses=&sub;
   if(!check(d,d->vk.CreateRenderPass(d->device,&pass,nullptr,&pipeline.pass)))return false;
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
   if(!check(d,d->vk.CreateGraphicsPipelines(d->device,d->pipeline_cache,1,&ci,nullptr,&pipeline.handle))){d->vk.DestroyRenderPass(d->device,pipeline.pass,nullptr);return false;}
   d->pipelines.emplace(key,pipeline);
 } else pipeline=found->second;
 VkDescriptorSet sets[3];VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};dai.descriptorPool=d->descriptors;dai.descriptorSetCount=3;dai.pSetLayouts=d->sets;
 if(!check(d,d->vk.AllocateDescriptorSets(d->device,&dai,sets)))return false;
 for(UINT stage=0;stage<3;stage++)if(d->shaders[stage]) {
   Shader *shader=d->shaders[stage];
   for(UINT i=0;i<16;i++)if(shader->cb[i]){
     Resource *r=d->cb[stage][i];VkDescriptorBufferInfo buffer{r?r->storage.buffer:d->dummy_buffer.buffer,0,r?(VkDeviceSize)r->width:65536};
     if(r)r->storage.in_use=true;
     VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[stage];write.dstBinding=i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&buffer;d->vk.UpdateDescriptorSets(d->device,1,&write,0,nullptr);
   }
   for(UINT i=0;i<16;i++)if(shader->samplers[i]){
     VkDescriptorImageInfo image{};image.sampler=d->samplers[stage][i]?d->samplers[stage][i]->handle:d->dummy_sampler.handle;
     VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[stage];write.dstBinding=16+i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;write.pImageInfo=&image;d->vk.UpdateDescriptorSets(d->device,1,&write,0,nullptr);
   }
   for(UINT i=0;i<128;i++)if(shader->textures[i]){
     View *v=d->textures[stage][i];if(!v)v=&d->dummy_view;if(!view_create(d,v,true))return false;
     v->resource->storage.in_use=true;
     VkDescriptorImageInfo image{VK_NULL_HANDLE,v->image,VK_IMAGE_LAYOUT_GENERAL};
     VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=sets[stage];write.dstBinding=(shader->texel[i]?160:32)+i;write.descriptorCount=1;write.descriptorType=shader->texel[i]?VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.pImageInfo=&image;write.pTexelBufferView=&v->buffer;d->vk.UpdateDescriptorSets(d->device,1,&write,0,nullptr);
   }
 }
 if(!d->render_pass || d->pass_views!=views || d->pass_formats!=formats) {
   barrier(d);
   VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fci.renderPass=pipeline.pass;fci.attachmentCount=(UINT)views.size();fci.pAttachments=views.data();fci.width=width;fci.height=height;fci.layers=1;VkFramebuffer fb;
   if(!check(d,d->vk.CreateFramebuffer(d->device,&fci,nullptr,&fb)))return false;d->framebuffers.push_back({fb});
   VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pipeline.pass;rp.framebuffer=fb;rp.renderArea.extent={width,height};
   d->vk.CmdBeginRenderPass(d->cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);d->render_pass=true;d->pass_views=views;d->pass_formats=formats;

 }
 d->vk.CmdBindPipeline(d->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.handle);d->vk.CmdBindDescriptorSets(d->cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,d->pipeline_layout,0,3,sets,0,nullptr);
 VkViewport viewport{d->viewport.TopLeftX,d->viewport.TopLeftY+d->viewport.Height,d->viewport.Width,-d->viewport.Height,d->viewport.MinDepth,d->viewport.MaxDepth};
 if(!viewport.width || !viewport.height)viewport={0,(float)height,(float)width,-(float)height,0,1};
 d->vk.CmdSetViewport(d->cmd,0,1,&viewport);VkRect2D scissor{{0,0},{width,height}};
 if(d->raster && d->raster->desc.ScissorEnable){LONG x=std::max<LONG>(0,d->scissor.left),y=std::max<LONG>(0,d->scissor.top);LONG right=std::min<LONG>(width,d->scissor.right),bottom=std::min<LONG>(height,d->scissor.bottom);scissor={{x,y},{(UINT)std::max<LONG>(0,right-x),(UINT)std::max<LONG>(0,bottom-y)}};}
 d->vk.CmdSetScissor(d->cmd,0,1,&scissor);d->vk.CmdSetBlendConstants(d->cmd,d->blend_color);d->vk.CmdSetStencilReference(d->cmd,VK_STENCIL_FACE_FRONT_AND_BACK,d->stencil_ref);
 for(UINT i=0;i<32;i++)if(d->vertices[i].resource){d->vertices[i].resource->storage.in_use=true;VkBuffer b=d->vertices[i].resource->storage.buffer;VkDeviceSize offset=d->vertices[i].offset;d->vk.CmdBindVertexBuffers(d->cmd,i,1,&b,&offset);}
 for(auto *q:d->queries)if(q->active && q->pool && q->count<256)d->vk.CmdBeginQuery(d->cmd,q->pool,q->count,VK_QUERY_CONTROL_PRECISE_BIT);
 if(indexed){if(!d->indices){end_pass(d);error(d,E_INVALIDARG);return false;}d->indices->storage.in_use=true;d->vk.CmdBindIndexBuffer(d->cmd,d->indices->storage.buffer,d->index_offset,d->index_size==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32);d->vk.CmdDrawIndexed(d->cmd,count,instances,first,bias,first_instance);}
 else d->vk.CmdDraw(d->cmd,count,instances,first,first_instance);
 for(auto *q:d->queries)if(q->active && q->pool && q->count<256){d->vk.CmdEndQuery(d->cmd,q->pool,q->count);q->count++;}
 d->draws++;

 if(d->framebuffers.size()>=400 || d->renamed.size()>=256 || d->draws%400==0)return flush(d);return true;
}
