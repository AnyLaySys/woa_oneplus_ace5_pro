#pragma once
HRESULT bind_resources(Queue *q,const Shader &shader) {
    State &state=*q->state;auto &root=state.roots[shader.stage==5];pipe_context *c=q->draw;
    if(!shader.state)return S_OK;
    if(root.root!=shader.root)return E_INVALIDARG;
    std::array<pipe_sampler_view *,128> views{};std::array<void *,16> samplers{};pipe_image_view images[16]={};pipe_shader_buffer buffers[16]={};
    HRESULT hr=S_OK;unsigned view_count=0,sampler_count=0,image_count=0;
    for(const auto &b:shader.bindings)for(UINT n=0;n<b.count;n++) {
        Descriptor fallback;Descriptor *desc=&fallback;unsigned slot=b.slot+n;
        if(b.static_sampler) {
            const auto &s=shader.root->samplers[b.parameter];fallback.kind=Descriptor::Sampler;fallback.sampler=sampler_state(s);
            if(s.BorderColor==D3D12DDI_STATIC_BORDER_COLOR_OPAQUE_WHITE)for(float &v:fallback.sampler.border_color.f)v=1;
            else if(s.BorderColor==D3D12DDI_STATIC_BORDER_COLOR_OPAQUE_BLACK)fallback.sampler.border_color.f[3]=1;
        } else if(b.table) {
            DescriptorHeap *heap=state.heaps[b.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER];UINT64 address=root.address[b.parameter];
            if(!heap || address<reinterpret_cast<UINT64>(heap->entries.data())){hr=E_INVALIDARG;break;}
            UINT64 bytes=address-reinterpret_cast<UINT64>(heap->entries.data()),index=bytes/sizeof(Descriptor);
            if(bytes%sizeof(Descriptor) || index>=heap->entries.size() || b.offset+n>=heap->entries.size()-index){hr=E_INVALIDARG;break;}
            desc=&heap->entries[static_cast<size_t>(index+b.offset+n)];
        } else if(!b.constants) {
            UINT offset=0;Resource *r=find_resource(q->device,root.address[b.parameter],1,&offset);
            if(!r){hr=E_INVALIDARG;break;}
            if(b.type==VKD3D_SHADER_DESCRIPTOR_TYPE_CBV){fallback.kind=Descriptor::CBV;fallback.cb={root.address[b.parameter],static_cast<UINT>(std::min<UINT64>(65536,r->size-offset)),0};}
            else if(b.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SRV){fallback.kind=Descriptor::SRV;fallback.srv.texture=r->pipe;fallback.srv.target=PIPE_BUFFER;fallback.srv.format=PIPE_FORMAT_R32_UINT;
                fallback.srv.u.buf.offset=offset;fallback.srv.u.buf.size=static_cast<UINT>(r->size-offset);fallback.srv.swizzle_r=0;fallback.srv.swizzle_g=1;fallback.srv.swizzle_b=2;fallback.srv.swizzle_a=3;}
            else {fallback.kind=Descriptor::UAV;fallback.uav.resource=r->pipe;fallback.uav.format=PIPE_FORMAT_R32_UINT;fallback.uav.u.buf.offset=offset;fallback.uav.u.buf.size=static_cast<UINT>(r->size-offset);fallback.uav.access=fallback.uav.shader_access=PIPE_IMAGE_ACCESS_READ_WRITE;}
        }
        switch(b.type) {
        case VKD3D_SHADER_DESCRIPTOR_TYPE_CBV:{
            pipe_constant_buffer cb{};
            if(b.constants){cb.user_buffer=root.constants[b.parameter];cb.buffer_size=shader.root->parameters[b.parameter].desc.Constants.Num32BitValues*4;}
            else if(desc->kind==Descriptor::CBV && desc->cb.BufferLocation) {
                UINT offset=0;Resource *r=find_resource(q->device,desc->cb.BufferLocation,desc->cb.SizeInBytes,&offset);
                if(!r){hr=E_INVALIDARG;break;}cb.buffer=r->pipe;cb.buffer_offset=offset;cb.buffer_size=desc->cb.SizeInBytes;
            }
            c->set_constant_buffer(c,static_cast<mesa_shader_stage>(shader.stage),slot,&cb);break;
        }
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SRV:
            view_count=std::max(view_count,slot+1);
            if(desc->kind==Descriptor::SRV && desc->srv.texture){views[slot]=c->create_sampler_view(c,desc->srv.texture,&desc->srv);if(!views[slot])hr=E_OUTOFMEMORY;}break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER:
            sampler_count=std::max(sampler_count,slot+1);if(desc->kind==Descriptor::Sampler){samplers[slot]=c->create_sampler_state(c,&desc->sampler);if(!samplers[slot])hr=E_OUTOFMEMORY;}break;
        case VKD3D_SHADER_DESCRIPTOR_TYPE_UAV:
            image_count=std::max(image_count,slot+1);if(desc->kind==Descriptor::UAV){images[slot]=desc->uav;
                if(desc->uav.resource && desc->uav.resource->target==PIPE_BUFFER)buffers[slot]={desc->uav.resource,desc->uav.u.buf.offset,desc->uav.u.buf.size};}break;
        default:hr=E_INVALIDARG;break;
        }
        if(FAILED(hr))break;
    }
    if(SUCCEEDED(hr)) {
        mesa_shader_stage stage=static_cast<mesa_shader_stage>(shader.stage);
        q->device->gpu_views(c,shader.stage,view_count,views.data(),sampler_count,samplers.data());
        c->bind_sampler_states(c,stage,0,sampler_count,samplers.data());c->set_shader_images(c,stage,0,image_count,16-image_count,images);
        c->set_shader_buffers(c,stage,0,image_count,buffers,UINT_MAX);
    }
    for(auto *view:views)if(view && !--view->reference.count)c->sampler_view_destroy(c,view);
    if(SUCCEEDED(hr)) {
        try {for(UINT i=0;i<sampler_count;i++)if(samplers[i]){q->samplers.push_back(samplers[i]);samplers[i]=nullptr;}}
        catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
    }
    if(FAILED(hr))for(auto sampler:samplers)if(sampler)c->delete_sampler_state(c,sampler);
    return hr;
}
void release_samplers(Queue *q) {
    void *empty[16]={};for(unsigned stage=0;stage<6;stage++)q->draw->bind_sampler_states(q->draw,static_cast<mesa_shader_stage>(stage),0,16,empty);
    for(unsigned stage=0;stage<6;stage++)q->device->gpu_views(q->draw,stage,0,nullptr,0,nullptr);
    for(void *sampler:q->samplers)q->draw->delete_sampler_state(q->draw,sampler);q->samplers.clear();
}
void delete_queue_shaders(Queue *q) {
    pipe_context *c=q->draw;
    const decltype(c->bind_vs_state) bind[]={c->bind_vs_state,c->bind_tcs_state,c->bind_tes_state,c->bind_gs_state,c->bind_fs_state,c->bind_compute_state};
    const decltype(c->delete_vs_state) destroy[]={c->delete_vs_state,c->delete_tcs_state,c->delete_tes_state,c->delete_gs_state,c->delete_fs_state,c->delete_compute_state};
    for(unsigned i=0;i<6;i++)if(q->shaders[i]){bind[i](c,nullptr);destroy[i](c,q->shaders[i]);q->shaders[i]=nullptr;}
}
HRESULT apply_pipeline(Queue *q,bool compute) {
    State &s=*q->state;if(!s.pipeline)return E_INVALIDARG;auto &p=*s.pipeline;pipe_context *c=q->draw;
    release_samplers(q);
    if(q->bound_pipeline!=s.pipeline) {
        delete_queue_shaders(q);q->bound_pipeline.reset();
        for(unsigned i=0;i<6;i++)if(p.shaders[i].state) {
            const auto &shader=p.shaders[i];const auto &so=p.shaders[3].stream_output;
            unsigned last=p.shaders[3].state?3:p.shaders[2].state?2:0;
            q->shaders[i]=so && i==last?q->device->gpu_shader_so(c,i,shader.spirv.data(),shader.spirv.size(),so.get()):
                q->device->gpu_shader(c,i,shader.spirv.data(),shader.spirv.size(),nullptr);
            if(!q->shaders[i]){delete_queue_shaders(q);return E_OUTOFMEMORY;}
        }
        q->bound_pipeline=s.pipeline;
    }
    if(compute) {if(!q->shaders[5])return E_INVALIDARG;c->bind_compute_state(c,q->shaders[5]);return bind_resources(q,p.shaders[5]);}
    if(!p.shaders[0].state || !s.framebuffer.width || !s.viewport_count || !s.scissor_count)return E_INVALIDARG;
    c->bind_vs_state(c,q->shaders[0]);c->bind_tcs_state(c,q->shaders[1]);c->bind_tes_state(c,q->shaders[2]);
    c->bind_gs_state(c,q->shaders[3]);c->bind_fs_state(c,q->shaders[4]);
    c->bind_blend_state(c,p.blend);c->bind_depth_stencil_alpha_state(c,p.depth);c->bind_rasterizer_state(c,p.raster);
    c->set_sample_mask(c,p.sample_mask);c->set_blend_color(c,&s.blend);c->set_stencil_ref(c,s.stencil);
    c->set_viewport_states(c,0,s.viewport_count,s.viewports);c->set_scissor_states(c,0,s.scissor_count,s.scissors);c->set_framebuffer_state(c,&s.framebuffer);
    if(s.patch_vertices)c->set_patch_vertices(c,static_cast<BYTE>(s.patch_vertices));
    pipe_vertex_element elements[32]={};pipe_vertex_buffer vertices[32]={};UINT count=0,element_count=0;
    for(const auto &e:p.elements) {
        if(e.InputSlot>=32 || e.InputRegister>=32 || e.AlignedByteOffset>65535)return E_INVALIDARG;
        const auto &v=s.vertices[e.InputSlot];auto &out=elements[e.InputRegister];out.vertex_buffer_index=static_cast<BYTE>(e.InputSlot);
        out.src_offset=static_cast<UINT16>(e.AlignedByteOffset);out.src_format=static_cast<BYTE>(format(e.Format));out.src_stride=v.StrideInBytes;
        out.instance_divisor=e.InputSlotClass?e.InstanceDataStepRate:0;count=std::max(count,e.InputSlot+1);element_count=std::max(element_count,e.InputRegister+1);
        if(v.BufferLocation) {UINT offset=0;Resource *r=find_resource(q->device,v.BufferLocation,v.SizeInBytes,&offset);if(!r)return E_INVALIDARG;vertices[e.InputSlot].buffer.resource=r->pipe;vertices[e.InputSlot].buffer_offset=offset;}
    }
    void *layout=c->create_vertex_elements_state(c,element_count,elements);if(!layout)return E_OUTOFMEMORY;
    c->bind_vertex_elements_state(c,layout);c->set_vertex_buffers(c,count,vertices);
    if(q->vertex_layout)c->delete_vertex_elements_state(c,q->vertex_layout);q->vertex_layout=layout;
    for(UINT i=0;i<5;i++){HRESULT hr=bind_resources(q,p.shaders[i]);if(FAILED(hr))return hr;}return S_OK;
}
HRESULT draw(Queue *q,UINT count,UINT instances,UINT first,INT bias,UINT base_instance,bool indexed) {
    if(!count || !instances)return S_OK;bool execute=true;HRESULT hr=predicate(q,execute);if(FAILED(hr) || !execute)return hr;
    hr=apply_pipeline(q,false);if(FAILED(hr))return hr;
    hr=bind_stream_targets(q);if(FAILED(hr))return hr;
    pipe_draw_info info{};info.mode=static_cast<BYTE>(q->state->topology);info.instance_count=instances;info.start_instance=base_instance;
    if(indexed) {
        const auto &view=q->state->index;UINT offset=0;Resource *r=find_resource(q->device,view.BufferLocation,view.SizeInBytes,&offset);
        UINT size=view.Format==DXGI_FORMAT_R16_UINT?2:view.Format==DXGI_FORMAT_R32_UINT?4:0;
        if(!r || !size || offset%size || first>view.SizeInBytes/size || count>view.SizeInBytes/size-first)return E_INVALIDARG;
        info.index_size=static_cast<UINT16>(size);info.index.resource=r->pipe;first+=offset/size;
        info.primitive_restart=q->state->pipeline->cut!=0;info.restart_index=q->state->pipeline->cut==1?65535:UINT_MAX;
    }
    pipe_draw_start_count_bias range{first,count,bias};q->draw->draw_vbo(q->draw,&info,0,nullptr,&range,1);return S_OK;
}
void APIENTRY draw_instanced(D3D12DDI_HCOMMANDLIST h,UINT count,UINT instances,UINT first,UINT base_instance) {
    record(object<List>(h),[=](Queue *q){return draw(q,count,instances,first,0,base_instance,false);});
}
void APIENTRY draw_indexed(D3D12DDI_HCOMMANDLIST h,UINT count,UINT instances,UINT first,INT bias,UINT base_instance) {
    record(object<List>(h),[=](Queue *q){return draw(q,count,instances,first,bias,base_instance,true);});
}
HRESULT dispatch(Queue *q,UINT x,UINT y,UINT z) {
    if(!x || !y || !z)return S_OK;bool execute=true;HRESULT hr=predicate(q,execute);if(FAILED(hr) || !execute)return hr;
    hr=apply_pipeline(q,true);if(FAILED(hr))return hr;
    pipe_grid_info info{};info.work_dim=3;info.grid[0]=x;info.grid[1]=y;info.grid[2]=z;
    memcpy(info.block,q->state->pipeline->shaders[5].block,sizeof(info.block));q->draw->launch_grid(q->draw,&info);return S_OK;
}
void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST h,UINT x,UINT y,UINT z) {
    record(object<List>(h),[=](Queue *q){return dispatch(q,x,y,z);});
}
void APIENTRY copy_buffer(D3D12DDI_HCOMMANDLIST h,D3D12DDIARG_BUFFER_PLACEMENT dst,D3D12DDIARG_BUFFER_PLACEMENT src,UINT64 bytes) {
    List *l=object<List>(h);Resource *s=object<Resource>(src.BaseAddress.UMD.hResource),*d=object<Resource>(dst.BaseAddress.UMD.hResource);
    UINT64 so=src.BaseAddress.UMD.Offset,at=dst.BaseAddress.UMD.Offset;
    if(!s || !d || s->heap->device!=l->device || d->heap->device!=l->device || s->pipe->target!=PIPE_BUFFER || d->pipe->target!=PIPE_BUFFER ||
       so>s->size || at>d->size || bytes>s->size-so || bytes>d->size-at){error(l,E_INVALIDARG);return;}
    record(l,[=](Queue *q){if(bytes){pipe_box box{static_cast<int>(so),static_cast<int>(bytes),0,1,0,1};q->draw->resource_copy_region(q->draw,d->pipe,0,static_cast<UINT>(at),0,0,s->pipe,0,&box);}return S_OK;});
}
void APIENTRY copy_resource(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HRESOURCE dst,D3D12DDI_HRESOURCE src) {
    Resource *s=object<Resource>(src),*d=object<Resource>(dst);List *l=object<List>(h);
    if(!s || !d || s->pipe->target!=d->pipe->target || s->pipe->width0!=d->pipe->width0 || s->pipe->height0!=d->pipe->height0 ||
       s->pipe->depth0!=d->pipe->depth0 || s->pipe->array_size!=d->pipe->array_size || s->pipe->last_level!=d->pipe->last_level){error(l,E_INVALIDARG);return;}
    record(l,[=](Queue *q){for(UINT level=0;level<=s->pipe->last_level;level++){
        pipe_box box{};box.width=std::max(1u,s->pipe->width0>>level);box.height=static_cast<int16_t>(std::max(1u,s->pipe->height0>>level));
        box.depth=static_cast<int16_t>(s->pipe->target==PIPE_TEXTURE_3D?std::max(1u,static_cast<UINT>(s->pipe->depth0)>>level):s->pipe->array_size);
        q->draw->resource_copy_region(q->draw,d->pipe,level,0,0,0,s->pipe,level,&box);}return S_OK;});
}
template<class Clear> void clear_rectangles(List *l,pipe_surface surface,UINT count,const D3D12DDI_RECT *rects,Clear clear) {
    if(!surface.texture || (count && !rects)){error(l,E_INVALIDARG);return;}
    try {
        std::vector<D3D12DDI_RECT> regions;
        if(count)regions.assign(rects,rects+count);
        else regions.push_back({0,0,static_cast<LONG>(std::max(1u,surface.texture->width0>>surface.level)),static_cast<LONG>(std::max(1u,surface.texture->height0>>surface.level))});
        record(l,[surface,regions=std::move(regions),clear](Queue *q)mutable{for(const auto &r:regions) {
            if(r.left<0 || r.top<0 || r.right<r.left || r.bottom<r.top)return E_INVALIDARG;clear(q->draw,&surface,r);}
            return S_OK;});
    }catch(const std::bad_alloc &){error(l,E_OUTOFMEMORY);}
}
void APIENTRY clear_rtv(D3D12DDI_HCOMMANDLIST h,D3D12DDI_CPU_DESCRIPTOR_HANDLE view,const FLOAT *color,UINT count,const D3D12DDI_RECT *rects) {
    pipe_color_union value{};memcpy(value.f,color,16);clear_rectangles(object<List>(h),descriptor(view)->surface,count,rects,
        [value](pipe_context *c,pipe_surface *s,const D3D12DDI_RECT &r){c->clear_render_target(c,s,&value,r.left,r.top,r.right-r.left,r.bottom-r.top,false);});
}
void APIENTRY clear_dsv(D3D12DDI_HCOMMANDLIST h,D3D12DDI_CPU_DESCRIPTOR_HANDLE view,UINT flags,FLOAT depth,UINT8 stencil,UINT count,const D3D12DDI_RECT *rects) {
    clear_rectangles(object<List>(h),descriptor(view)->surface,count,rects,[=](pipe_context *c,pipe_surface *s,const D3D12DDI_RECT &r){
        c->clear_depth_stencil(c,s,((flags&1)?PIPE_CLEAR_DEPTH:0)|((flags&2)?PIPE_CLEAR_STENCIL:0),depth,stencil,r.left,r.top,r.right-r.left,r.bottom-r.top,false);});
}
template<bool Integer,class Value> void APIENTRY clear_uav(D3D12DDI_HCOMMANDLIST h,D3D12DDI_GPU_DESCRIPTOR_HANDLE,D3D12DDI_CPU_DESCRIPTOR_HANDLE view,D3D12DDI_HRESOURCE resource,const Value *color,UINT count,const D3D12DDI_RECT *rects) {
    List *l=object<List>(h);Descriptor desc=*descriptor(view);Resource *r=object<Resource>(resource);
    if(desc.kind!=Descriptor::UAV || !r || desc.uav.resource!=r->pipe || (count && !rects)){error(l,E_INVALIDARG);return;}
    try {pipe_color_union value{};memcpy(&value,color,16);std::vector<pipe_box> boxes(count);
        for(UINT i=0;i<count;i++){boxes[i].x=rects[i].left;boxes[i].y=static_cast<int16_t>(rects[i].top);boxes[i].width=rects[i].right-rects[i].left;boxes[i].height=static_cast<int16_t>(rects[i].bottom-rects[i].top);}
        record(l,[desc,value,boxes=std::move(boxes)](Queue *q){return q->device->gpu_clear(q->draw,&desc.uav,&value,Integer,desc.stride,static_cast<UINT>(boxes.size()),boxes.data());});
    }catch(const std::bad_alloc &){error(l,E_OUTOFMEMORY);}
}
void APIENTRY resource_barrier(D3D12DDI_HCOMMANDLIST h,UINT,const D3D12DDIARG_RESOURCE_BARRIER_0003 *) {
    record(object<List>(h),[](Queue *q){q->draw->memory_barrier(q->draw,PIPE_BARRIER_ALL);q->draw->texture_barrier(q->draw,0);return S_OK;});
}
void APIENTRY execute_bundle(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HCOMMANDLIST bundle) {
    List *l=object<List>(h),*b=object<List>(bundle);if(!b || !b->closed || b->device!=l->device || FAILED(b->failure)){error(l,E_INVALIDARG);return;}
    auto recording=b->recording;record(l,[recording](Queue *q){for(const auto &op:recording->ops){HRESULT hr=op(q);if(FAILED(hr))return hr;}return S_OK;});
}
void APIENTRY discard_resource(D3D12DDI_HCOMMANDLIST,D3D12DDI_HRESOURCE,const D3D12DDIARG_DISCARD_RESOURCE_0003 *){}
void APIENTRY set_marker(D3D12DDI_HCOMMANDLIST,UINT64){}
void APIENTRY clear_roots(D3D12DDI_HCOMMANDLIST h) {
    record(object<List>(h),[](Queue *q){for(auto &args:q->state->roots){Root *root=args.root;args={};args.root=root;}return S_OK;});
}
