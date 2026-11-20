#pragma once
struct Layout {std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC> elements;};
struct Blend {D3D12DDI_BLEND_DESC desc;};
struct Depth {D3D12DDI_DEPTH_STENCIL_DESC desc;};
struct Raster {D3D12DDI_RASTERIZER_DESC desc;};
struct Pipeline {
    Device *device;
    struct Data {
        Device *device;
        std::array<Shader,6> shaders;
        std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC> elements;
        void *blend=nullptr,*depth=nullptr,*raster=nullptr;
        UINT sample_mask=UINT_MAX,cut=0;
        ~Data(){pipe_context *c=device->compiler;if(blend)c->delete_blend_state(c,blend);if(depth)c->delete_depth_stencil_alpha_state(c,depth);if(raster)c->delete_rasterizer_state(c,raster);}
    };
    std::shared_ptr<Data> data;
};
struct State {
    std::shared_ptr<Pipeline::Data> pipeline;
    struct RootArguments {Root *root=nullptr;UINT64 address[64]={};UINT constants[64][64]={};} roots[2];
    DescriptorHeap *heaps[2]={};
    std::array<D3D12DDI_VERTEX_BUFFER_VIEW,32> vertices{};
    D3D12DDI_INDEX_BUFFER_VIEW index{};
    pipe_framebuffer_state framebuffer{};
    pipe_viewport_state viewports[16]={};
    pipe_scissor_state scissors[16]={};
    UINT viewport_count=0,scissor_count=0;
    pipe_blend_color blend{};
    pipe_stencil_ref stencil{};
    mesa_prim topology=MESA_PRIM_UNKNOWN;
    UINT patch_vertices=0;
    Resource *predicate=nullptr;UINT64 predicate_offset=0;D3D12DDI_PREDICATION_OP predicate_op{};
    D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW stream_targets[4]={};
};
SIZE_T APIENTRY layout_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATEELEMENTLAYOUT *){return sizeof(Layout);}
void APIENTRY create_layout(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATEELEMENTLAYOUT *p,D3D12DDI_HELEMENTLAYOUT h) {
    Layout *l=new(h.pDrvPrivate) Layout;
    if(!p || p->NumElements>32 || (p->NumElements && !p->pVertexElements)){error(object<Device>(hd),E_INVALIDARG);return;}
    try {if(p->NumElements)l->elements.assign(p->pVertexElements,p->pVertexElements+p->NumElements);}catch(const std::bad_alloc &){error(object<Device>(hd),E_OUTOFMEMORY);}
}
void APIENTRY destroy_layout(D3D12DDI_HDEVICE,D3D12DDI_HELEMENTLAYOUT h){object<Layout>(h)->~Layout();}
#define SIMPLE_STATE(Name,Type,Desc,Handle) \
SIZE_T APIENTRY Name##_size(D3D12DDI_HDEVICE,const Desc *){return sizeof(Type);} \
void APIENTRY create_##Name(D3D12DDI_HDEVICE,const Desc *p,Handle h){new(h.pDrvPrivate) Type{*p};} \
void APIENTRY destroy_##Name(D3D12DDI_HDEVICE,Handle h){object<Type>(h)->~Type();}
SIMPLE_STATE(blend,Blend,D3D12DDI_BLEND_DESC,D3D12DDI_HBLENDSTATE)
SIMPLE_STATE(depth,Depth,D3D12DDI_DEPTH_STENCIL_DESC,D3D12DDI_HDEPTHSTENCILSTATE)
SIMPLE_STATE(raster,Raster,D3D12DDI_RASTERIZER_DESC,D3D12DDI_HRASTERIZERSTATE)
#undef SIMPLE_STATE
UINT blend_factor(D3D12DDI_BLEND b) {
    static const UINT factors[]={0,PIPE_BLENDFACTOR_ZERO,PIPE_BLENDFACTOR_ONE,PIPE_BLENDFACTOR_SRC_COLOR,PIPE_BLENDFACTOR_INV_SRC_COLOR,
        PIPE_BLENDFACTOR_SRC_ALPHA,PIPE_BLENDFACTOR_INV_SRC_ALPHA,PIPE_BLENDFACTOR_DST_ALPHA,PIPE_BLENDFACTOR_INV_DST_ALPHA,
        PIPE_BLENDFACTOR_DST_COLOR,PIPE_BLENDFACTOR_INV_DST_COLOR,PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE,0,0,
        PIPE_BLENDFACTOR_CONST_COLOR,PIPE_BLENDFACTOR_INV_CONST_COLOR,PIPE_BLENDFACTOR_SRC1_COLOR,PIPE_BLENDFACTOR_INV_SRC1_COLOR,
        PIPE_BLENDFACTOR_SRC1_ALPHA,PIPE_BLENDFACTOR_INV_SRC1_ALPHA};
    return static_cast<UINT>(b)<std::size(factors)?factors[b]:PIPE_BLENDFACTOR_ONE;
}
UINT stencil_op(D3D12DDI_STENCIL_OP op) {
    static const UINT ops[]={0,PIPE_STENCIL_OP_KEEP,PIPE_STENCIL_OP_ZERO,PIPE_STENCIL_OP_REPLACE,PIPE_STENCIL_OP_INCR,
        PIPE_STENCIL_OP_DECR,PIPE_STENCIL_OP_INVERT,PIPE_STENCIL_OP_INCR_WRAP,PIPE_STENCIL_OP_DECR_WRAP};return ops[op];
}
SIZE_T APIENTRY pipeline_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *){return sizeof(Pipeline);}
HRESULT APIENTRY create_pipeline(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_PIPELINE_STATE_0001 *p) {
    Device *d=object<Device>(hd);if(!p || p->NodeMask!=1 || p->NumRenderTargets>8 || (!p->hComputeShader.pDrvPrivate && !p->SampleDesc.Count) || p->SampleDesc.Quality)return E_INVALIDARG;
    Pipeline *pipeline=new(p->hDrvPipelineState.pDrvPrivate) Pipeline{d};HRESULT hr=S_OK;
    try {
        auto state=std::make_shared<Pipeline::Data>();state->device=d;
        const D3D12DDI_HSHADER shaders[]={p->hVertexShader,p->hHullShader,p->hDomainShader,p->hGeometryShader,p->hPixelShader,p->hComputeShader};
        for(UINT i=0;i<6;i++)if(shaders[i].pDrvPrivate) {
            Shader *s=object<Shader>(shaders[i]);if(s->device!=d || (!s->state && !(i==3 && s->stream_output)) || s->stage!=i)throw E_INVALIDARG;state->shaders[i]=*s;
        }
        if(p->hComputeShader.pDrvPrivate){pipeline->data=std::move(state);return S_OK;}
        if(p->hElementLayout.pDrvPrivate)state->elements=object<Layout>(p->hElementLayout)->elements;
        pipe_blend_state blend{};blend.independent_blend_enable=1;
        for(auto &b:blend.rt)b.colormask=15;
        if(p->hBlendState.pDrvPrivate) {
            const auto &desc=object<Blend>(p->hBlendState)->desc;blend.alpha_to_coverage=desc.AlphaToCoverageEnable;
            for(UINT i=0;i<8;i++) {
                const auto &b=desc.RenderTarget[desc.IndependentBlendEnable?i:0];auto &out=blend.rt[i];
                out.blend_enable=b.BlendEnable;out.rgb_func=b.BlendOp-1;out.alpha_func=b.BlendOpAlpha-1;
                out.rgb_src_factor=blend_factor(b.SrcBlend);out.rgb_dst_factor=blend_factor(b.DestBlend);
                out.alpha_src_factor=blend_factor(b.SrcBlendAlpha);out.alpha_dst_factor=blend_factor(b.DestBlendAlpha);out.colormask=b.RenderTargetWriteMask;
                if(b.LogicOpEnable){blend.logicop_enable=1;blend.logicop_func=b.LogicOp;}
            }
        }
        pipe_depth_stencil_alpha_state depth{};
        if(p->hDepthStencilState.pDrvPrivate) {
            const auto &desc=object<Depth>(p->hDepthStencilState)->desc;depth.depth_enabled=desc.DepthEnable;depth.depth_writemask=desc.DepthWriteMask;depth.depth_func=desc.DepthFunc-1;
            const D3D12DDI_DEPTH_STENCILOP_DESC faces[]={desc.FrontFace,desc.BackFace};
            for(UINT i=0;i<2;i++) {
                auto &s=depth.stencil[i];s.enabled=desc.StencilEnable && (i?desc.BackEnable:desc.FrontEnable);s.func=faces[i].StencilFunc-1;
                s.fail_op=stencil_op(faces[i].StencilFailOp);s.zfail_op=stencil_op(faces[i].StencilDepthFailOp);s.zpass_op=stencil_op(faces[i].StencilPassOp);
                s.valuemask=desc.StencilReadMask;s.writemask=desc.StencilWriteMask;
            }
        }
        pipe_rasterizer_state raster{};raster.depth_clip_near=raster.depth_clip_far=true;raster.half_pixel_center=true;raster.scissor=true;raster.clip_halfz=true;
        raster.rasterizer_discard=state->shaders[3].stream_output && state->shaders[3].stream_output->RasterizedStream==UINT_MAX;
        if(state->shaders[3].stream_output && !raster.rasterizer_discard)raster.rasterization_stream=state->shaders[3].stream_output->RasterizedStream;
        raster.line_width=1;raster.point_size=1;raster.multisample=p->SampleDesc.Count>1;
        if(p->hRasterizerState.pDrvPrivate) {
            const auto &desc=object<Raster>(p->hRasterizerState)->desc;
            if(desc.ForcedSampleCount || desc.ConservativeRasterizationMode)throw DXGI_ERROR_UNSUPPORTED;
            raster.front_ccw=desc.FrontCounterClockwise;raster.cull_face=desc.CullMode==D3D12DDI_CULL_MODE_NONE?PIPE_FACE_NONE:desc.CullMode==D3D12DDI_CULL_MODE_FRONT?PIPE_FACE_FRONT:PIPE_FACE_BACK;
            raster.fill_front=raster.fill_back=desc.FillMode==D3D12DDI_FILL_MODE_WIREFRAME?PIPE_POLYGON_MODE_LINE:PIPE_POLYGON_MODE_FILL;
            raster.depth_clip_near=raster.depth_clip_far=desc.DepthClipEnable;raster.line_smooth=desc.AntialiasedLineEnable;
            raster.offset_units=static_cast<float>(desc.DepthBias);raster.offset_scale=desc.SlopeScaledDepthBias;raster.offset_clamp=desc.DepthBiasClamp;
            raster.offset_tri=desc.DepthBias || desc.SlopeScaledDepthBias;
        }
        pipe_context *c=d->compiler;state->blend=c->create_blend_state(c,&blend);state->depth=c->create_depth_stencil_alpha_state(c,&depth);state->raster=c->create_rasterizer_state(c,&raster);
        if(!state->blend || !state->depth || !state->raster)throw E_OUTOFMEMORY;
        state->sample_mask=p->SampleMask;state->cut=p->IBStripCutValue;pipeline->data=std::move(state);
    } catch(HRESULT result){hr=result;}
      catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
    if(FAILED(hr))pipeline->~Pipeline();return hr;
}
void APIENTRY destroy_pipeline(D3D12DDI_HDEVICE,D3D12DDI_HPIPELINESTATE h){object<Pipeline>(h)->~Pipeline();}
void APIENTRY set_pipeline(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HPIPELINESTATE p) {
    List *l=object<List>(h);Pipeline *pipeline=object<Pipeline>(p);
    if(pipeline && (pipeline->device!=l->device || !pipeline->data)){error(l,E_INVALIDARG);return;}
    auto state=pipeline?pipeline->data:nullptr;record(l,[state](Queue *q){q->state->pipeline=state;return S_OK;});
}
template<bool Compute> void APIENTRY set_root(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HROOTSIGNATURE root) {
    Root *r=object<Root>(root);record(object<List>(h),[r](Queue *q){auto &args=q->state->roots[Compute];if(args.root!=r){args={};args.root=r;}return S_OK;});
}
template<bool Compute> void APIENTRY set_root_address(D3D12DDI_HCOMMANDLIST h,UINT index,UINT64 va) {
    List *l=object<List>(h);if(index>=64){error(l,E_INVALIDARG);return;}
    record(l,[index,va](Queue *q){q->state->roots[Compute].address[index]=va;return S_OK;});
}
template<bool Compute> void APIENTRY set_root_table(D3D12DDI_HCOMMANDLIST h,UINT index,D3D12DDI_GPU_DESCRIPTOR_HANDLE base){set_root_address<Compute>(h,index,base.ptr);}
template<bool Compute> void APIENTRY set_constants(D3D12DDI_HCOMMANDLIST h,UINT index,UINT count,const void *data,UINT offset) {
    List *l=object<List>(h);if(index>=64 || offset>64 || count>64-offset || (count && !data)){error(l,E_INVALIDARG);return;}
    try {std::vector<UINT> words(count);if(count)memcpy(words.data(),data,count*4);
        record(l,[index,offset,words=std::move(words)](Queue *q){memcpy(q->state->roots[Compute].constants[index]+offset,words.data(),words.size()*4);return S_OK;});
    }catch(const std::bad_alloc &){error(l,E_OUTOFMEMORY);}
}
template<bool Compute> void APIENTRY set_constant(D3D12DDI_HCOMMANDLIST h,UINT index,UINT word,UINT offset){set_constants<Compute>(h,index,1,&word,offset);}
void APIENTRY set_heaps(D3D12DDI_HCOMMANDLIST h,UINT count,D3D12DDI_HDESCRIPTORHEAP *heaps) {
    List *l=object<List>(h);if(count>2 || (count && !heaps)){error(l,E_INVALIDARG);return;}std::array<DescriptorHeap *,2> slots{};
    for(UINT i=0;i<count;i++) {auto *heap=object<DescriptorHeap>(heaps[i]);if(!heap || heap->device!=l->device || heap->type>1){error(l,E_INVALIDARG);return;}slots[heap->type]=heap;}
    record(l,[slots](Queue *q){q->state->heaps[0]=slots[0];q->state->heaps[1]=slots[1];return S_OK;});
}
void APIENTRY set_topology(D3D12DDI_HCOMMANDLIST h,D3D12DDI_PRIMITIVE_TOPOLOGY topology) {
    UINT t=static_cast<UINT>(topology),patch=0;mesa_prim prim=MESA_PRIM_TRIANGLES;
    switch(t) {case 0:prim=MESA_PRIM_UNKNOWN;break;case 1:prim=MESA_PRIM_POINTS;break;case 2:prim=MESA_PRIM_LINES;break;case 3:prim=MESA_PRIM_LINE_STRIP;break;
        case 4:break;case 5:prim=MESA_PRIM_TRIANGLE_STRIP;break;case 10:prim=MESA_PRIM_LINES_ADJACENCY;break;case 11:prim=MESA_PRIM_LINE_STRIP_ADJACENCY;break;
        case 12:prim=MESA_PRIM_TRIANGLES_ADJACENCY;break;case 13:prim=MESA_PRIM_TRIANGLE_STRIP_ADJACENCY;break;
        default:if(t>=33 && t<=64){prim=MESA_PRIM_PATCHES;patch=t-32;}else {error(object<List>(h),E_INVALIDARG);return;}
    }
    record(object<List>(h),[prim,patch](Queue *q){q->state->topology=prim;q->state->patch_vertices=patch;return S_OK;});
}
void APIENTRY set_viewports(D3D12DDI_HCOMMANDLIST h,UINT count,const D3D12DDI_VIEWPORT *p) {
    List *l=object<List>(h);if(count>16 || (count && !p)){error(l,E_INVALIDARG);return;}std::array<pipe_viewport_state,16> v{};
    for(UINT i=0;i<count;i++){v[i].scale[0]=p[i].Width*0.5f;v[i].scale[1]=-p[i].Height*0.5f;v[i].scale[2]=p[i].MaxDepth-p[i].MinDepth;
        v[i].translate[0]=p[i].TopLeftX+v[i].scale[0];v[i].translate[1]=p[i].TopLeftY-v[i].scale[1];v[i].translate[2]=p[i].MinDepth;}
    record(l,[v,count](Queue *q){memcpy(q->state->viewports,v.data(),sizeof(v));q->state->viewport_count=count;return S_OK;});
}
void APIENTRY set_scissors(D3D12DDI_HCOMMANDLIST h,UINT count,const D3D12DDI_RECT *p) {
    List *l=object<List>(h);if(count>16 || (count && !p)){error(l,E_INVALIDARG);return;}std::array<pipe_scissor_state,16> v{};
    for(UINT i=0;i<count;i++){v[i].minx=std::max<LONG>(0,p[i].left);v[i].miny=std::max<LONG>(0,p[i].top);v[i].maxx=std::max<LONG>(0,p[i].right);v[i].maxy=std::max<LONG>(0,p[i].bottom);}
    record(l,[v,count](Queue *q){memcpy(q->state->scissors,v.data(),sizeof(v));q->state->scissor_count=count;return S_OK;});
}
void APIENTRY set_blend(D3D12DDI_HCOMMANDLIST h,const FLOAT *p){pipe_blend_color color{};memcpy(color.color,p,16);record(object<List>(h),[color](Queue *q){q->state->blend=color;return S_OK;});}
void APIENTRY set_stencil(D3D12DDI_HCOMMANDLIST h,UINT ref){record(object<List>(h),[ref](Queue *q){q->state->stencil.ref_value[0]=q->state->stencil.ref_value[1]=static_cast<BYTE>(ref);return S_OK;});}
void APIENTRY set_index(D3D12DDI_HCOMMANDLIST h,const D3D12DDI_INDEX_BUFFER_VIEW *p){D3D12DDI_INDEX_BUFFER_VIEW view=p?*p:D3D12DDI_INDEX_BUFFER_VIEW{};record(object<List>(h),[view](Queue *q){q->state->index=view;return S_OK;});}
void APIENTRY set_vertices(D3D12DDI_HCOMMANDLIST h,UINT first,UINT count,const D3D12DDI_VERTEX_BUFFER_VIEW *p) {
    List *l=object<List>(h);if(first>32 || count>32-first){error(l,E_INVALIDARG);return;}std::array<D3D12DDI_VERTEX_BUFFER_VIEW,32> views{};if(p)memcpy(views.data(),p,count*sizeof(*p));
    record(l,[first,count,views](Queue *q){memcpy(q->state->vertices.data()+first,views.data(),count*sizeof(views[0]));return S_OK;});
}
void APIENTRY set_targets(D3D12DDI_HCOMMANDLIST h,UINT count,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *p,BOOL single,const D3D12DDI_CPU_DESCRIPTOR_HANDLE *depth) {
    List *l=object<List>(h);if(count>8 || (count && !p)){error(l,E_INVALIDARG);return;}pipe_framebuffer_state fb{};fb.nr_cbufs=static_cast<BYTE>(count);
    for(UINT i=0;i<count;i++)fb.cbufs[i]=descriptor(single?D3D12DDI_CPU_DESCRIPTOR_HANDLE{p[0].ptr+i*sizeof(Descriptor)}:p[i])->surface;
    if(depth && depth->ptr)fb.zsbuf=descriptor(*depth)->surface;
    bool dimensions=false;for(UINT i=0;i<=count;i++) {const auto &s=i==count?fb.zsbuf:fb.cbufs[i];if(!s.texture)continue;
        UINT w=std::max(1u,s.texture->width0>>s.level),height=std::max(1u,s.texture->height0>>s.level);
        fb.width=dimensions?std::min(fb.width,w):w;fb.height=dimensions?std::min(fb.height,height):height;fb.layers=s.last_layer-s.first_layer+1;dimensions=true;}
    record(l,[fb](Queue *q){q->state->framebuffer=fb;return S_OK;});
}
