#include "a830_native.h"
#include "pipe/p_screen.h"
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "util/u_inlines.h"
#include "freedreno/freedreno_screen.h"
#include "freedreno/freedreno_context.h"
#include "freedreno/freedreno_resource.h"
#include "compiler/spirv/nir_spirv.h"
#include "spirv_info.h"
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/nir/nir_xfb_info.h"
#include "util/format/u_format.h"

static bool lower_descriptor(nir_builder *b,nir_intrinsic_instr *in,void *unused) {
    nir_def *value;
    b->cursor=nir_before_instr(&in->instr);
    switch(in->intrinsic) {
    case nir_intrinsic_vulkan_resource_index:{
        unsigned binding=nir_intrinsic_binding(in);
        if(nir_intrinsic_desc_type(in)==nir_descriptor_type_storage_buffer)binding-=160;
        value=nir_vec2(b,nir_iadd_imm(b,in->src[0].ssa,binding),nir_imm_int(b,0));break;
    }
    case nir_intrinsic_vulkan_resource_reindex:
        value=nir_vec2(b,nir_iadd(b,nir_channel(b,in->src[0].ssa,0),in->src[1].ssa),nir_channel(b,in->src[0].ssa,1));break;
    case nir_intrinsic_load_vulkan_descriptor:value=in->src[0].ssa;break;
    default:return false;
    }
    nir_def_replace(&in->def,value);return true;
}
static bool lower_texture(nir_builder *b,nir_tex_instr *tex,void *unused) {
    b->cursor=nir_before_instr(&tex->instr);
    const nir_tex_src_type offsets[]={nir_tex_src_texture_offset,nir_tex_src_sampler_offset};
    const nir_tex_src_type handles[]={nir_tex_src_texture_handle,nir_tex_src_sampler_handle};
    const unsigned bases[]={64,192},limits[]={128,16};
    for(unsigned i=0;i<2;i++) {
        if(i && !nir_tex_instr_need_sampler(tex))continue;
        unsigned index=i?tex->sampler_index:tex->texture_index;
        nir_def *value=nir_imm_int(b,index);int source=nir_tex_instr_src_index(tex,offsets[i]);
        if(source>=0){value=nir_iadd(b,value,tex->src[source].src.ssa);nir_tex_instr_remove_src(tex,source);}
        value=nir_bcsel(b,nir_ult_imm(b,value,limits[i]),nir_iadd_imm(b,value,bases[i]),nir_imm_int(b,255));
        value=nir_bindless_resource_ir3(b,32,value,b->shader->info.stage==MESA_SHADER_COMPUTE?0:b->shader->info.stage);
        nir_tex_instr_add_src(tex,handles[i],value);
    }
    return true;
}

struct pipe_screen *WINAPI A830GpuScreen(const A830_NATIVE_HOST *host) {
    glsl_type_singleton_init_or_ref();
    fd_windows_set_native_host(host);
    struct pipe_screen *screen=fd_screen_create(-1,NULL,NULL);
    fd_windows_set_native_host(NULL);if(!screen)glsl_type_singleton_decref();return screen;
}
void WINAPI A830GpuDestroyScreen(struct pipe_screen *screen){screen->destroy(screen);glsl_type_singleton_decref();}
struct pipe_context *WINAPI A830GpuContext(struct pipe_screen *screen,const A830_NATIVE_HOST *host) {
    fd_windows_set_native_host(host);
    struct pipe_context *context=screen->context_create(screen,NULL,0);
    fd_windows_set_native_host(NULL);return context;
}
void WINAPI A830GpuViews(struct pipe_context *context,unsigned stage,unsigned count,struct pipe_sampler_view **views,unsigned samplers,void **states) {
    if(stage>MESA_SHADER_COMPUTE || count>128 || samplers>16)return;
    struct fd_context *ctx=fd_context(context);struct fd_native_texture_state *native=&ctx->native_texture[stage];
    native->active=true;
    for(unsigned i=0;i<128;i++) {
        pipe_sampler_view_reference(&native->views[i],i<count?views[i]:NULL);
        if(native->views[i])fd_resource_set_usage(native->views[i]->texture,FD_DIRTY_TEX);
    }
    for(unsigned i=0;i<16;i++)native->samplers[i]=i<samplers?states[i]:NULL;
    fd_context_dirty_shader(ctx,(mesa_shader_stage)stage,FD_DIRTY_SHADER_TEX|FD_DIRTY_SHADER_IMAGE);
}
HRESULT WINAPI A830GpuClear(struct pipe_context *c,const struct pipe_image_view *v,const union pipe_color_union *color,BOOL integer,unsigned stride,unsigned count,const struct pipe_box *rects) {
    if(!v->resource)return S_OK;
    const struct util_format_description *fmt=util_format_description(v->format);
    if(!fmt || !fmt->block.bits || (fmt->block.bits&7) || fmt->block.width!=1 || fmt->block.height!=1 || fmt->block.bits>128)return DXGI_ERROR_UNSUPPORTED;
    unsigned size=fmt->block.bits/8;uint8_t data[16]={0};
    if(integer) {
        for(unsigned i=0;i<4;i++) {
            unsigned index=fmt->swizzle[i];if(index>=4)continue;
            unsigned bits=fmt->channel[index].size,shift=fmt->channel[index].shift;if(bits>32)return DXGI_ERROR_UNSUPPORTED;
            for(unsigned bit=0;bit<bits;bit++)data[(shift+bit)/8]|=((color->ui[i]>>bit)&1)<<((shift+bit)%8);
        }
    } else {
        const struct util_format_pack_description *pack=util_format_pack_description(v->format);
        if(!pack || !pack->pack_rgba_float)return DXGI_ERROR_UNSUPPORTED;
        pack->pack_rgba_float(data,size,color->f,16,1,1);
    }
    struct pipe_box whole={0};
    if(v->resource->target==PIPE_BUFFER){whole.width=v->u.buf.size/(stride?stride:size);whole.height=whole.depth=1;}
    else {whole.width=u_minify(v->resource->width0,v->u.tex.level);whole.height=u_minify(v->resource->height0,v->u.tex.level);whole.depth=v->u.tex.last_layer-v->u.tex.first_layer+1;}
    for(unsigned i=0;i<(count?count:1);i++) {
        struct pipe_box box=count?rects[i]:whole;
        if(box.x<0 || box.y<0 || box.width<0 || box.height<0 || box.x>whole.width || box.width>whole.width-box.x || box.y>whole.height || box.height>whole.height-box.y)return E_INVALIDARG;
        if(v->resource->target==PIPE_BUFFER)c->clear_buffer(c,v->resource,v->u.buf.offset+box.x*(stride?stride:size),box.width*(stride?stride:size),data,size);
        else {box.z=v->u.tex.first_layer;box.depth=whole.depth;c->clear_texture(c,v->resource,v->u.tex.level,&box,data);}
    }
    return S_OK;
}
struct pipe_resource *WINAPI A830GpuResource(struct pipe_screen *screen,const struct pipe_resource *tmpl,
    const GPU_ALLOCATION *memory,uint64_t offset,A830_NATIVE_LAYOUT *layout) {
    struct fd_bo *bo=memory?fd_windows_bo_import(fd_screen(screen)->dev,memory,offset):NULL;
    struct pipe_resource *resource=fd_native_resource(screen,tmpl,bo,layout);
    if(bo)fd_bo_del(bo);return resource;
}
struct pipe_stream_output_target *WINAPI A830GpuStreamTarget(struct pipe_context *context,struct pipe_resource *buffer,struct pipe_resource *counter) {
    struct fd_stream_output_target *target=calloc(1,sizeof(*target));if(!target)return NULL;
    pipe_reference_init(&target->base.reference,1);target->base.context=context;target->base.buffer_size=buffer->width0;
    pipe_resource_reference(&target->base.buffer,buffer);pipe_resource_reference(&target->offset_buf,counter);
    struct fd_resource *rsc=fd_resource(buffer);util_range_add(&rsc->b.b,&rsc->valid_buffer_range,0,buffer->width0);
    return &target->base;
}
static int compare_xfb(const void *a,const void *b) {
    const nir_xfb_output_info *x=a,*y=b;
    return x->buffer!=y->buffer?(int)x->buffer-(int)y->buffer:(int)x->offset-(int)y->offset;
}
static void *shader_create(struct pipe_context *context,unsigned stage,const uint32_t *words,size_t count,unsigned block[3],const A830_NATIVE_STREAM_OUTPUT *so) {
    if(stage>MESA_SHADER_COMPUTE || !words || count<5)return NULL;
    struct spirv_to_nir_options options={0};options.environment=NIR_SPIRV_VULKAN;
    const struct spirv_capabilities caps={.Shader=true,.Geometry=true,.Tessellation=true,.Float64=true,.Int64=true,
        .StorageImageReadWithoutFormat=true,.StorageImageWriteWithoutFormat=true,.ImageMSArray=true,.ImageQuery=true,
        .SampledBuffer=true,.ImageBuffer=true,.Sampled1D=true,.Image1D=true,.SampledCubeArray=true,
        .GeometryStreams=true,.TransformFeedback=true,.MultiViewport=true,.DerivativeControl=true,.DrawParameters=true,
        .UniformBufferArrayDynamicIndexing=true,.SampledImageArrayDynamicIndexing=true,.StorageBufferArrayDynamicIndexing=true,
        .StorageImageArrayDynamicIndexing=true,.UniformTexelBufferArrayDynamicIndexing=true,.StorageTexelBufferArrayDynamicIndexing=true,
        .ShaderNonUniform=true,.RuntimeDescriptorArray=true,.UniformBufferArrayNonUniformIndexing=true,.SampledImageArrayNonUniformIndexing=true,
        .StorageBufferArrayNonUniformIndexing=true,.StorageImageArrayNonUniformIndexing=true,
        .UniformTexelBufferArrayNonUniformIndexing=true,.StorageTexelBufferArrayNonUniformIndexing=true};
    options.capabilities=&caps;options.ubo_addr_format=nir_address_format_32bit_index_offset;
    options.ssbo_addr_format=nir_address_format_32bit_index_offset;options.shared_addr_format=nir_address_format_32bit_offset;
    options.temp_addr_format=nir_address_format_32bit_offset;options.constant_addr_format=nir_address_format_64bit_global;
    nir_shader *nir=spirv_to_nir(words,count,NULL,(mesa_shader_stage)stage,"main",&options,context->screen->nir_options[stage]);
    if(!nir)return NULL;
    nir_lower_variable_initializers(nir,nir_var_function_temp);
    nir_lower_returns(nir);nir_inline_functions(nir);nir_remove_non_entrypoints(nir);
    nir_lower_var_copies(nir);nir_lower_vars_to_ssa(nir);
    nir_lower_system_values(nir);
    if(stage==MESA_SHADER_COMPUTE) {
        const nir_lower_compute_system_values_options compute={.global_id_is_32bit=true};
        nir_lower_compute_system_values(nir,&compute);
    }
    nir_foreach_variable_with_modes(var,nir,nir_var_mem_ubo|nir_var_mem_ssbo) {
        unsigned count=glsl_type_is_array(var->type)?glsl_get_length(var->type):1;
        if(var->data.mode==nir_var_mem_ubo)nir->info.num_ubos=MAX2(nir->info.num_ubos,count?var->data.binding+count:16);
        else nir->info.num_ssbos=MAX2(nir->info.num_ssbos,count?var->data.binding-160+count:16);
    }
    nir_shader_intrinsics_pass(nir,lower_descriptor,nir_metadata_control_flow,NULL);
    nir_lower_explicit_io(nir,nir_var_mem_ubo|nir_var_mem_ssbo,nir_address_format_32bit_index_offset);
    nir_lower_vars_to_explicit_types(nir,nir_var_mem_shared,glsl_get_natural_size_align_bytes);
    nir_lower_explicit_io(nir,nir_var_mem_shared,nir_address_format_32bit_offset);
    nir_foreach_variable_with_modes(var,nir,nir_var_uniform|nir_var_image) {
        const struct glsl_type *type=glsl_without_array(var->type);
        if(glsl_type_is_sampler(type))var->data.binding-=glsl_get_sampler_result_type(type)==GLSL_TYPE_VOID?16:32;
        if(glsl_type_is_image(type))var->data.binding-=160;
        var->data.driver_location=var->data.binding;
    }
    nir_lower_samplers(nir);
    nir_assign_io_var_locations(nir,nir_var_shader_in);
    nir_assign_io_var_locations(nir,nir_var_shader_out);
    if(stage==MESA_SHADER_VERTEX)nir_foreach_shader_in_variable(var,nir) {
        if(var->data.location>=VERT_ATTRIB_GENERIC0) {
            var->data.driver_location=var->data.location-VERT_ATTRIB_GENERIC0;
            nir->num_inputs=MAX2(nir->num_inputs,var->data.driver_location+1);
        }
    }
    nir_shader_gather_info(nir,nir_shader_get_entrypoint(nir));
    if(block && stage==MESA_SHADER_COMPUTE)for(unsigned i=0;i<3;i++)block[i]=nir->info.workgroup_size[i];
    if(so) {
        nir_xfb_info *xfb=rzalloc_size(nir,nir_xfb_info_size(so->Count));
        if(!xfb){ralloc_free(nir);return NULL;}
        xfb->output_count=so->Count;
        for(unsigned i=0;i<4;i++){xfb->buffers[i].stride=so->Strides[i];nir->info.xfb_stride[i]=so->Strides[i]/4;}
        for(unsigned i=0;i<so->Count;i++) {
            unsigned buffer=so->Outputs[i].Buffer,stream=so->Outputs[i].Stream;
            xfb->buffers_written|=1u<<buffer;xfb->streams_written|=1u<<stream;
            xfb->buffer_to_stream[buffer]=stream;xfb->buffers[buffer].varying_count++;
            xfb->outputs[i]=(nir_xfb_output_info){.buffer=buffer,.offset=so->Outputs[i].Offset,
                .location=so->Outputs[i].Location,.component_mask=so->Outputs[i].Mask,
                .component_offset=ffs(so->Outputs[i].Mask)-1};
        }
        qsort(xfb->outputs,so->Count,sizeof(xfb->outputs[0]),compare_xfb);nir->xfb_info=xfb;
    }
    context->screen->finalize_nir(context->screen,nir,true);
    nir_shader_tex_pass(nir,lower_texture,nir_metadata_control_flow,NULL);
    struct pipe_shader_state shader={0};shader.type=PIPE_SHADER_IR_NIR;shader.ir.nir=nir;
    switch(stage) {
    case MESA_SHADER_VERTEX:return context->create_vs_state(context,&shader);
    case MESA_SHADER_FRAGMENT:return context->create_fs_state(context,&shader);
    case MESA_SHADER_GEOMETRY:return context->create_gs_state(context,&shader);
    case MESA_SHADER_TESS_CTRL:return context->create_tcs_state(context,&shader);
    case MESA_SHADER_TESS_EVAL:return context->create_tes_state(context,&shader);
    case MESA_SHADER_COMPUTE:{struct pipe_compute_state compute={0};compute.ir_type=PIPE_SHADER_IR_NIR;compute.prog=nir;return context->create_compute_state(context,&compute);}
    default:ralloc_free(nir);return NULL;
    }
}
void *WINAPI A830GpuShader(struct pipe_context *context,unsigned stage,const uint32_t *words,size_t count,unsigned block[3]) {
    return shader_create(context,stage,words,count,block,NULL);
}
void *WINAPI A830GpuShaderStreamOutput(struct pipe_context *context,unsigned stage,const uint32_t *words,size_t count,const A830_NATIVE_STREAM_OUTPUT *so) {
    return shader_create(context,stage,words,count,NULL,so);
}
