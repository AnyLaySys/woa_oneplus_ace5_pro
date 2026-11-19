#pragma once
#include "compiler/shader_enums.h"
SIZE_T APIENTRY stream_shader_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT *,const D3D12DDIARG_STAGE_IO_SIGNATURES *){return sizeof(Shader);}
void APIENTRY create_stream_shader(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT *p,D3D12DDI_HSHADER h,const D3D12DDIARG_STAGE_IO_SIGNATURES *io,D3D12DDI_CREATE_SHADER_FLAGS) {
    Device *d=object<Device>(hd);Shader *s=new(h.pDrvPrivate) Shader{};s->device=d;s->stage=3;s->root=p?object<Root>(p->hRootSignature):nullptr;
    if(!p || p->NumEntries>64 || p->NumStrides>4 || (p->NumEntries && !p->pOutputStreamDecl) || (p->NumStrides && !p->BufferStridesInBytes) ||
       (p->RasterizedStream!=UINT_MAX && p->RasterizedStream>3) || !s->root || s->root->device!=d || !io || (io->NumOutputSignatureEntries && !io->pOutputSignatureDeprecated)) {error(d,E_INVALIDARG);return;}
    try {
        auto so=std::make_shared<A830_NATIVE_STREAM_OUTPUT>();so->RasterizedStream=p->RasterizedStream;UINT offsets[4]={},streams[4]={UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX};
        for(UINT i=0;i<p->NumStrides;i++){if((p->BufferStridesInBytes[i]&3) || p->BufferStridesInBytes[i]>2048)throw E_INVALIDARG;so->Strides[i]=p->BufferStridesInBytes[i];}
        for(UINT i=0;i<p->NumEntries;i++) {
            const auto &entry=p->pOutputStreamDecl[i];UINT mask=entry.RegisterMask;
            if(entry.OutputSlot>=p->NumStrides || entry.Stream>3 || !mask || (mask&~15u) || (streams[entry.OutputSlot]!=UINT_MAX && streams[entry.OutputSlot]!=entry.Stream))throw E_INVALIDARG;
            streams[entry.OutputSlot]=entry.Stream;UINT bytes=0;for(UINT m=mask;m;m>>=1)bytes+=(m&1)*4;
            if(offsets[entry.OutputSlot]>so->Strides[entry.OutputSlot] || bytes>so->Strides[entry.OutputSlot]-offsets[entry.OutputSlot])throw E_INVALIDARG;
            if(entry.RegisterIndex!=UINT_MAX) {
                UINT location=UINT_MAX,clip=0,cull=0;
                for(UINT n=0;n<io->NumOutputSignatureEntries;n++) {
                    const auto &sig=io->pOutputSignatureDeprecated[n];UINT at=UINT_MAX;
                    switch(sig.SystemValue){case D3D10_SB_NAME_UNDEFINED:at=VARYING_SLOT_VAR0+sig.Register;break;
                        case D3D10_SB_NAME_POSITION:at=VARYING_SLOT_POS;break;case D3D10_SB_NAME_CLIP_DISTANCE:at=VARYING_SLOT_CLIP_DIST0+clip++;break;
                        case D3D10_SB_NAME_CULL_DISTANCE:at=VARYING_SLOT_CULL_DIST0+cull++;break;case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:at=VARYING_SLOT_LAYER;break;
                        case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:at=VARYING_SLOT_VIEWPORT;break;case D3D10_SB_NAME_PRIMITIVE_ID:at=VARYING_SLOT_PRIMITIVE_ID;break;default:break;}
                    if(sig.Register==entry.RegisterIndex){if((mask&sig.Mask)!=mask)throw E_INVALIDARG;location=at;break;}
                }
                if(location==UINT_MAX || location>=VARYING_SLOT_MAX)throw E_INVALIDARG;
                so->Outputs[so->Count++]={location,mask,entry.OutputSlot,entry.Stream,offsets[entry.OutputSlot]};
            }
            offsets[entry.OutputSlot]+=bytes;
        }
        if(p->pShaderCode){HRESULT hr=compile_shader(d,p->pShaderCode,s->root,s,io);if(FAILED(hr))throw hr;if(s->stage!=3)throw E_INVALIDARG;}
        s->stream_output=std::move(so);
    }catch(HRESULT hr){error(d,hr);}catch(const std::bad_alloc &){error(d,E_OUTOFMEMORY);}
}
void APIENTRY set_stream_targets(D3D12DDI_HCOMMANDLIST h,UINT first,UINT count,const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW *views) {
    List *l=object<List>(h);if(first>4 || count>4-first){error(l,E_INVALIDARG);return;}
    std::array<D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW,4> copy{};if(views)std::copy(views,views+count,copy.begin());
    record(l,[=](Queue *q){for(UINT i=0;i<count;i++)q->state->stream_targets[first+i]=copy[i];return S_OK;});
}
pipe_resource *stream_slice(Device *d,UINT64 va,UINT bytes) {
    UINT offset=0;Resource *r=find_resource(d,va,bytes,&offset);if(!r)return nullptr;
    pipe_resource tmpl{};tmpl.target=PIPE_BUFFER;tmpl.format=PIPE_FORMAT_R8_UINT;tmpl.width0=bytes;tmpl.height0=tmpl.depth0=tmpl.array_size=1;tmpl.bind=PIPE_BIND_STREAM_OUTPUT;
    A830_NATIVE_LAYOUT layout{};return d->gpu_resource(d->screen,&tmpl,&r->heap->memory,r->heap->offset+r->offset+offset,&layout);
}
HRESULT bind_stream_targets(Queue *q) {
    pipe_stream_output_target *targets[4]={};UINT offsets[4]={UINT_MAX,UINT_MAX,UINT_MAX,UINT_MAX};HRESULT hr=S_OK;UINT count=0;
    for(UINT i=0;i<4;i++) {
        const auto &view=q->state->stream_targets[i];if(!view.BufferLocation)continue;
        if(!view.SizeInBytes || view.SizeInBytes>GPU_MAX_BUFFER_SIZE || (view.SizeInBytes&3) || (view.BufferLocation&3) || (view.BufferFilledSizeLocation&7)){hr=E_INVALIDARG;break;}
        pipe_resource *buffer=stream_slice(q->device,view.BufferLocation,static_cast<UINT>(view.SizeInBytes)),*counter=stream_slice(q->device,view.BufferFilledSizeLocation,8);
        if(buffer && counter)targets[i]=q->device->gpu_stream_target(q->draw,buffer,counter);
        drop_resource(q->device,buffer);drop_resource(q->device,counter);if(!targets[i]){hr=E_OUTOFMEMORY;break;}count=i+1;
    }
    if(SUCCEEDED(hr))q->draw->set_stream_output_targets(q->draw,count,targets,offsets,q->state->topology);
    for(auto *target:targets)if(target && !InterlockedDecrement(reinterpret_cast<volatile LONG *>(&target->reference.count)))q->draw->stream_output_target_destroy(q->draw,target);
    return hr;
}
