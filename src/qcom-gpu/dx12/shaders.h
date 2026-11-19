#pragma once

struct Root {
    struct Parameter { D3D12DDI_ROOT_PARAMETER desc;std::vector<D3D12DDI_DESCRIPTOR_RANGE> ranges; };
    Device *device;
    std::vector<Parameter> parameters;
    std::vector<D3D12DDI_STATIC_SAMPLER> samplers;
};
struct Shader {
    struct Binary {
        Device *device;unsigned stage;void *state;
        Binary(Device *d,unsigned s,void *p):device(d),stage(s),state(p){}
        ~Binary() {
            pipe_context *c=device->compiler;
            const decltype(c->delete_vs_state) functions[]={c->delete_vs_state,c->delete_tcs_state,c->delete_tes_state,c->delete_gs_state,c->delete_fs_state,c->delete_compute_state};
            AcquireSRWLockExclusive(&device->compiler_lock);functions[stage](c,state);ReleaseSRWLockExclusive(&device->compiler_lock);
        }
    };
    struct Binding { vkd3d_shader_descriptor_type type;UINT parameter,offset,slot,count;bool constants,table,static_sampler; };
    Device *device;
    Root *root;
    unsigned stage;
    unsigned block[3]={1,1,1};
    void *state=nullptr;
    std::shared_ptr<Binary> binary;
    std::vector<UINT> spirv;
    std::vector<Binding> bindings;
    std::shared_ptr<A830_NATIVE_STREAM_OUTPUT> stream_output;
};
SIZE_T APIENTRY root_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *){return sizeof(Root);}
HRESULT APIENTRY create_root(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0001 *p,D3D12DDI_HROOTSIGNATURE h) {
    if(!p || !p->pRootSignature || p->NodeMask!=1)return E_INVALIDARG;
    const auto &desc=*p->pRootSignature;
    if(desc.NumParameters>64 || desc.NumStaticSamplers>2032 || (desc.NumParameters && !desc.pRootParameters) ||
       (desc.NumStaticSamplers && !desc.pStaticSamplers) || (desc.Flags&~0x7fu))return E_INVALIDARG;
    Root *root=new(h.pDrvPrivate) Root{object<Device>(hd)};
    try {
        UINT cost=0;root->parameters.reserve(desc.NumParameters);
        for(UINT i=0;i<desc.NumParameters;i++) {
            const auto &param=desc.pRootParameters[i];Root::Parameter item{param};
            if(param.ShaderVisibility>D3D12DDI_SHADER_VISIBILITY_PIXEL || param.ParameterType>D3D12DDI_ROOT_PARAMETER_TYPE_UAV)throw E_INVALIDARG;
            if(param.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
                if(!param.DescriptorTable.NumDescriptorRanges || !param.DescriptorTable.pDescriptorRanges)throw E_INVALIDARG;
                item.ranges.assign(param.DescriptorTable.pDescriptorRanges,param.DescriptorTable.pDescriptorRanges+param.DescriptorTable.NumDescriptorRanges);
                UINT64 end=0;bool sampler=item.ranges[0].RangeType==D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                for(auto &range:item.ranges) {
                    if(range.RangeType>D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER || !range.NumDescriptors ||
                       sampler!=(range.RangeType==D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER))throw E_INVALIDARG;
                    if(range.OffsetInDescriptorsFromTableStart==UINT_MAX) {
                        if(end>UINT_MAX)throw E_INVALIDARG;
                        range.OffsetInDescriptorsFromTableStart=static_cast<UINT>(end);
                    }
                    end=static_cast<UINT64>(range.OffsetInDescriptorsFromTableStart)+range.NumDescriptors;
                }
                ++cost;
            } else if(param.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
                if(!param.Constants.Num32BitValues || param.Constants.Num32BitValues>64)throw E_INVALIDARG;
                cost+=param.Constants.Num32BitValues;
            } else cost+=2;
            if(cost>64)throw E_INVALIDARG;
            root->parameters.push_back(std::move(item));
        }
        if(desc.NumStaticSamplers)root->samplers.assign(desc.pStaticSamplers,desc.pStaticSamplers+desc.NumStaticSamplers);
        return S_OK;
    } catch(HRESULT hr){root->~Root();return hr;}
      catch(const std::bad_alloc &){root->~Root();return E_OUTOFMEMORY;}
}
void APIENTRY destroy_root(D3D12DDI_HDEVICE,D3D12DDI_HROOTSIGNATURE h){object<Root>(h)->~Root();}
bool visible(D3D12DDI_SHADER_VISIBILITY visibility,unsigned stage) {
    static const UINT stages[]={1,2,3,4,5,0};return !visibility || stages[stage]==visibility;
}
bool root_binding(Root *root,const vkd3d_shader_descriptor_info &desc,unsigned stage,Shader::Binding &out) {
    const UINT types[]={D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV,D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV,
        D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV,D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER};
    UINT kind=types[desc.type];out.type=desc.type;out.count=desc.count;
    for(UINT i=0;i<root->parameters.size();i++) {
        auto &item=root->parameters[i];const auto &p=item.desc;if(!visible(p.ShaderVisibility,stage))continue;
        if(p.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
            for(const auto &range:item.ranges)if(range.RangeType==kind && range.RegisterSpace==desc.register_space &&
               desc.register_index>=range.BaseShaderRegister && desc.register_index-range.BaseShaderRegister<range.NumDescriptors &&
               desc.count<=range.NumDescriptors-(desc.register_index-range.BaseShaderRegister)) {
                out.parameter=i;out.offset=range.OffsetInDescriptorsFromTableStart+desc.register_index-range.BaseShaderRegister;
                out.table=true;return true;
            }
        } else {
            bool constants=p.ParameterType==D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            UINT type=constants?D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV:p.ParameterType-2;
            if(!constants)type=type==0?D3D12DDI_DESCRIPTOR_RANGE_TYPE_CBV:type==1?D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV:D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV;
            UINT reg=constants?p.Constants.ShaderRegister:p.Descriptor.ShaderRegister;
            UINT space=constants?p.Constants.RegisterSpace:p.Descriptor.RegisterSpace;
            if(type==kind && space==desc.register_space && reg==desc.register_index && desc.count==1) {
                out.parameter=i;out.constants=constants;return true;
            }
        }
    }
    if(desc.type==VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER && desc.count==1)for(UINT i=0;i<root->samplers.size();i++) {
        const auto &s=root->samplers[i];if(s.ShaderRegister==desc.register_index && s.RegisterSpace==desc.register_space && visible(s.ShaderVisibility,stage)) {
            out.parameter=i;out.static_sampler=true;return true;
        }
    }
    return false;
}
void delete_shader(Shader *s) {
    s->binary.reset();s->state=nullptr;
}
template<class IO> HRESULT compile_shader(Device *d,const UINT *code,Root *root,Shader *s,const IO *io) {
    if(!code || code[1]<2 || code[1]>1048576 || !root || root->device!=d)return E_INVALIDARG;
    UINT type=code[0]>>16;if(type>5)return DXGI_ERROR_UNSUPPORTED;
    static const unsigned stages[]={4,0,3,1,2,5};s->device=d;s->root=root;s->stage=stages[type];
    vkd3d_shader_code dxbc{},spirv{};vkd3d_shader_scan_descriptor_info scan{};
    HRESULT hr=DXGI_ERROR_UNSUPPORTED;
    try {
        if(io && ((io->NumInputSignatureEntries && !io->pInputSignatureDeprecated) ||
           (io->NumOutputSignatureEntries && !io->pOutputSignatureDeprecated)))return E_INVALIDARG;
        auto input=A830Signature(io?io->pInputSignatureDeprecated:nullptr,io?io->NumInputSignatureEntries:0,type==0,false);
        auto output=A830Signature(io?io->pOutputSignatureDeprecated:nullptr,io?io->NumOutputSignatureEntries:0,type==0,true);
        vkd3d_shader_dxbc_section_desc sections[4]={{0x4e475349,{input.data(),input.size()*4}},
            {0x4e47534f,{output.data(),output.size()*4}},{((code[0]>>4)&15)>=5?0x58454853u:0x52444853u,{code,static_cast<size_t>(code[1])*4}}};
        std::vector<uint32_t> patch;
        if constexpr(std::is_same_v<IO,D3D12DDIARG_TESSELLATION_IO_SIGNATURES>) {
            if(io){patch=A830Signature(io->pPatchConstantSignatureDeprecated,io->NumPatchConstantSignatureEntries,false,type==3);
                sections[3]={0x47534350,{patch.data(),patch.size()*4}};}
        }
        if(vkd3d_shader_serialize_dxbc(patch.empty()?3:4,sections,&dxbc,nullptr)<0)throw E_FAIL;
        scan.type=VKD3D_SHADER_STRUCTURE_TYPE_SCAN_DESCRIPTOR_INFO;
        vkd3d_shader_compile_info ci{};ci.type=VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;ci.next=&scan;ci.source=dxbc;
        ci.source_type=VKD3D_SHADER_SOURCE_DXBC_TPF;ci.target_type=VKD3D_SHADER_TARGET_SPIRV_BINARY;ci.log_level=VKD3D_SHADER_LOG_NONE;
        if(vkd3d_shader_scan(&ci,nullptr)<0)throw DXGI_ERROR_UNSUPPORTED;
        std::vector<vkd3d_shader_resource_binding> bindings;UINT slots[4]={};
        static const UINT bases[]={32,160,0,16};
        for(UINT i=0;i<scan.descriptor_count;i++) {
            const auto &desc=scan.descriptors[i];Shader::Binding binding{};
            const UINT limits[]={128,16,16,16};
            if(desc.type>VKD3D_SHADER_DESCRIPTOR_TYPE_SAMPLER || !desc.count ||
               desc.count>limits[desc.type]-slots[desc.type] || !root_binding(root,desc,s->stage,binding))throw DXGI_ERROR_UNSUPPORTED;
            binding.slot=slots[desc.type];slots[desc.type]+=desc.count;s->bindings.push_back(binding);
            vkd3d_shader_resource_binding b{};b.type=desc.type;b.register_space=desc.register_space;b.register_index=desc.register_index;
            b.shader_visibility=VKD3D_SHADER_VISIBILITY_ALL;b.flags=VKD3D_SHADER_BINDING_FLAG_IMAGE|VKD3D_SHADER_BINDING_FLAG_BUFFER;
            b.binding={0,bases[desc.type]+binding.slot,desc.count};bindings.push_back(b);
        }
        vkd3d_shader_interface_info iface{};iface.type=VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO;iface.bindings=bindings.data();iface.binding_count=static_cast<UINT>(bindings.size());
        vkd3d_shader_spirv_target_info target{};target.type=VKD3D_SHADER_STRUCTURE_TYPE_SPIRV_TARGET_INFO;
        const vkd3d_shader_spirv_extension extension=VKD3D_SHADER_SPIRV_EXTENSION_EXT_DESCRIPTOR_INDEXING;
        target.environment=VKD3D_SHADER_SPIRV_ENVIRONMENT_VULKAN_1_1;target.extensions=&extension;target.extension_count=1;iface.next=&target;ci.next=&iface;
        vkd3d_shader_compile_option options[]={{VKD3D_SHADER_COMPILE_OPTION_STRIP_DEBUG,1},
            {VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV,VKD3D_SHADER_COMPILE_OPTION_BUFFER_UAV_STORAGE_BUFFER}};
        ci.options=options;ci.option_count=2;
        if(vkd3d_shader_compile(&ci,&spirv,nullptr)<0)throw DXGI_ERROR_UNSUPPORTED;
        s->spirv.resize(spirv.size/4);memcpy(s->spirv.data(),spirv.code,spirv.size);
        AcquireSRWLockExclusive(&d->compiler_lock);
        s->state=d->gpu_shader(d->compiler,s->stage,static_cast<const UINT *>(spirv.code),spirv.size/4,s->block);
        ReleaseSRWLockExclusive(&d->compiler_lock);hr=s->state?S_OK:E_FAIL;
        if(s->state) {
            try {s->binary=std::make_shared<Shader::Binary>(d,s->stage,s->state);}
            catch(const std::bad_alloc &){Shader::Binary cleanup{d,s->stage,s->state};s->state=nullptr;throw;}
        }
    } catch(HRESULT result){hr=result;}
      catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}
    vkd3d_shader_free_shader_code(&spirv);vkd3d_shader_free_shader_code(&dxbc);vkd3d_shader_free_scan_descriptor_info(&scan);return hr;
}
SIZE_T APIENTRY shader_size(D3D12DDI_HDEVICE,const UINT *,D3D12DDI_HROOTSIGNATURE,const D3D12DDIARG_STAGE_IO_SIGNATURES *){return sizeof(Shader);}
SIZE_T APIENTRY tess_size(D3D12DDI_HDEVICE,const UINT *,D3D12DDI_HROOTSIGNATURE,const D3D12DDIARG_TESSELLATION_IO_SIGNATURES *){return sizeof(Shader);}
void APIENTRY create_shader(D3D12DDI_HDEVICE hd,const UINT *code,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HSHADER h,
    const D3D12DDIARG_STAGE_IO_SIGNATURES *io,D3D12DDI_CREATE_SHADER_FLAGS) {
    Shader *s=new(h.pDrvPrivate) Shader{};HRESULT hr=compile_shader(object<Device>(hd),code,object<Root>(root),s,io);
    if(FAILED(hr))error(object<Device>(hd),hr);
}
void APIENTRY create_compute(D3D12DDI_HDEVICE hd,const UINT *code,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HSHADER h,D3D12DDI_CREATE_SHADER_FLAGS flags) {
    create_shader(hd,code,root,h,nullptr,flags);
}
void APIENTRY create_tess(D3D12DDI_HDEVICE hd,const UINT *code,D3D12DDI_HROOTSIGNATURE root,D3D12DDI_HSHADER h,
    const D3D12DDIARG_TESSELLATION_IO_SIGNATURES *io,D3D12DDI_CREATE_SHADER_FLAGS) {
    Shader *s=new(h.pDrvPrivate) Shader{};HRESULT hr=compile_shader(object<Device>(hd),code,object<Root>(root),s,io);
    if(FAILED(hr))error(object<Device>(hd),hr);
}
void APIENTRY destroy_shader(D3D12DDI_HDEVICE,D3D12DDI_HSHADER h){Shader *s=object<Shader>(h);delete_shader(s);s->~Shader();}
