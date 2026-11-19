#pragma once
struct CommandSignature {
    struct Data {Root *root;UINT stride;bool compute;std::vector<D3D12DDI_INDIRECT_ARGUMENT_DESC> args;};
    Device *device;std::shared_ptr<Data> data;
};
UINT argument_bytes(const D3D12DDI_INDIRECT_ARGUMENT_DESC &a) {
    static const UINT sizes[]={16,20,12,16,16,0,8,8,8};
    return a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT?a.Constant.Num32BitValuesToSet*4:
        static_cast<UINT>(a.Type)<std::size(sizes)?sizes[a.Type]:0;
}
SIZE_T APIENTRY signature_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *){return sizeof(CommandSignature);}
HRESULT APIENTRY create_signature(D3D12DDI_HDEVICE h,const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001 *p,D3D12DDI_HCOMMANDSIGNATURE out) {
    Device *d=object<Device>(h);
    if(!p || !out.pDrvPrivate || !p->ByteStride || (p->ByteStride&3) || !p->NumArgumentDescs || !p->pArgumentDescs || p->NodeMask!=1)return E_INVALIDARG;
    Root *root=object<Root>(p->hRootSignature);if(root && root->device!=d)return E_INVALIDARG;
    try {
        auto data=std::make_shared<CommandSignature::Data>();data->root=root;data->stride=p->ByteStride;
        data->args.assign(p->pArgumentDescs,p->pArgumentDescs+p->NumArgumentDescs);
        auto end=data->args.back().Type;if(end>D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH)return E_INVALIDARG;
        data->compute=end==D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH;UINT64 bytes=0;
        for(UINT i=0;i<data->args.size();i++) {
            const auto &a=data->args[i];UINT size=argument_bytes(a);if(!size || (i+1<data->args.size() && a.Type<=D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH))return E_INVALIDARG;
            bytes+=size;if(bytes>p->ByteStride)return E_INVALIDARG;
            if(a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW && (a.VertexBuffer.Slot>=32 || data->compute))return E_INVALIDARG;
            if(a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW && data->compute)return E_INVALIDARG;
            if(a.Type>=D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT) {
                UINT index=a.ConstantBufferView.RootParameterIndex;
                if(!root || index>=root->parameters.size())return E_INVALIDARG;
                const auto &param=root->parameters[index].desc;
                if(a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT) {
                    if(param.ParameterType!=D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS || a.Constant.DestOffsetIn32BitValues>param.Constants.Num32BitValues ||
                       a.Constant.Num32BitValuesToSet>param.Constants.Num32BitValues-a.Constant.DestOffsetIn32BitValues)return E_INVALIDARG;
                } else if(static_cast<UINT>(param.ParameterType)!=static_cast<UINT>(a.Type)-4)return E_INVALIDARG;
            }
        }
        new(out.pDrvPrivate) CommandSignature{d,std::move(data)};return S_OK;
    }catch(const std::bad_alloc &){return E_OUTOFMEMORY;}
}
void APIENTRY destroy_signature(D3D12DDI_HDEVICE,D3D12DDI_HCOMMANDSIGNATURE h){object<CommandSignature>(h)->~CommandSignature();}
HRESULT indirect(Queue *q,const CommandSignature::Data &signature,UINT max,Resource *arguments,UINT64 offset,Resource *counter,UINT64 counter_offset) {
    bool execute=true;HRESULT hr=predicate(q,execute);if(FAILED(hr) || !execute)return hr;
    auto &root=q->state->roots[signature.compute];if(signature.root && root.root!=signature.root)return E_INVALIDARG;
    UINT count=max;if(counter){UINT value;hr=read_buffer(q,counter,counter_offset,&value,4);if(FAILED(hr))return hr;count=std::min(max,value);}
    try {
        std::vector<BYTE> data(static_cast<size_t>(count)*signature.stride);
        hr=read_buffer(q,arguments,offset,data.data(),static_cast<UINT>(data.size()));if(FAILED(hr))return hr;
        for(UINT i=0;i<count && SUCCEEDED(hr);i++) {
            const BYTE *at=data.data()+static_cast<size_t>(i)*signature.stride;
            for(const auto &a:signature.args) {
                switch(a.Type) {
                case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW:{D3D12DDI_DRAW_ARGUMENTS v;memcpy(&v,at,sizeof(v));hr=draw(q,v.VertexCountPerInstance,v.InstanceCount,v.StartVertexLocation,0,v.StartInstanceLocation,false);break;}
                case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:{D3D12DDI_DRAW_INDEXED_ARGUMENTS v;memcpy(&v,at,sizeof(v));hr=draw(q,v.IndexCountPerInstance,v.InstanceCount,v.StartIndexLocation,v.BaseVertexLocation,v.StartInstanceLocation,true);break;}
                case D3D12DDI_INDIRECT_ARGUMENT_TYPE_DISPATCH:{D3D12DDI_DISPATCH_ARGUMENTS v;memcpy(&v,at,sizeof(v));hr=dispatch(q,v.ThreadGroupCountX,v.ThreadGroupCountY,v.ThreadGroupCountZ);break;}
                case D3D12DDI_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:memcpy(&q->state->vertices[a.VertexBuffer.Slot],at,16);break;
                case D3D12DDI_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:memcpy(&q->state->index,at,16);break;
                case D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT:memcpy(root.constants[a.Constant.RootParameterIndex]+a.Constant.DestOffsetIn32BitValues,at,argument_bytes(a));break;
                default:memcpy(&root.address[a.ConstantBufferView.RootParameterIndex],at,8);break;
                }
                at+=argument_bytes(a);if(FAILED(hr))break;
            }
        }
    }catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
    for(const auto &a:signature.args) {
        if(a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW)q->state->vertices[a.VertexBuffer.Slot]={};
        else if(a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW)q->state->index={};
        else if(a.Type==D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT)memset(root.constants[a.Constant.RootParameterIndex],0,sizeof(root.constants[0]));
        else if(a.Type>D3D12DDI_INDIRECT_ARGUMENT_TYPE_CONSTANT)root.address[a.ConstantBufferView.RootParameterIndex]=0;
    }
    return hr;
}
void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HCOMMANDSIGNATURE hs,UINT max,D3D12DDIARG_BUFFER_PLACEMENT args,D3D12DDIARG_BUFFER_PLACEMENT counter) {
    List *l=object<List>(h);auto *s=object<CommandSignature>(hs);Resource *a=object<Resource>(args.BaseAddress.UMD.hResource),*c=object<Resource>(counter.BaseAddress.UMD.hResource);
    if(!s || s->device!=l->device || !a || a->heap->device!=l->device || a->pipe->target!=PIPE_BUFFER ||
       (args.BaseAddress.UMD.Offset&3) || args.BaseAddress.UMD.Offset>a->size || static_cast<UINT64>(max)*s->data->stride>a->size-args.BaseAddress.UMD.Offset ||
       (c && (c->heap->device!=l->device || c->pipe->target!=PIPE_BUFFER || (counter.BaseAddress.UMD.Offset&3) || counter.BaseAddress.UMD.Offset>c->size || 4>c->size-counter.BaseAddress.UMD.Offset))) {
        error(l,E_INVALIDARG);return;
    }
    auto signature=s->data;record(l,[=](Queue *q){return indirect(q,*signature,max,a,args.BaseAddress.UMD.Offset,c,counter.BaseAddress.UMD.Offset);});
}
