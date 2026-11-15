static const unsigned SharedSamplerColors[16]={
    0xffff0000,0xff00ff00,0xff0000ff,0xffffff00,
    0xffff00ff,0xff00ffff,0xffffffff,0xff000000,
    0xff00ff00,0xffff0000,0xff000000,0xffffffff,
    0xff0000ff,0xffffff00,0xff00ffff,0xffff00ff
};
static int SharedSamplerControl(ID3D11DeviceContext *context,ID3D11Texture2D *target,ID3D11Texture2D *staging,const char *name,unsigned expected) {
    context->CopyResource(staging,target);D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&mapped));
    unsigned center=*(unsigned *)((BYTE *)mapped.pData+32*mapped.RowPitch+32*4),matching=0;
    for(unsigned y=0;y<64;y++)for(unsigned x=0;x<64;x++)matching+=*(unsigned *)((BYTE *)mapped.pData+y*mapped.RowPitch+x*4)==expected;
    context->Unmap(staging,0);
    printf("{\"sampler_control\":\"%s\",\"center\":\"%08x\",\"expected\":\"%08x\",\"matching_pixels\":%u,\"result\":\"%s\"}\n",name,center,expected,matching,matching==4096?"PASS":"FAIL");
    return matching==4096?0:1;
}
static int SharedSamplerChild(ID3D11Device *device,ID3D11DeviceContext *context,HANDLE handle,bool waitForGpu) {
    ID3D11Texture2D *image=NULL;ID3D11RenderTargetView *view=NULL;IDXGIKeyedMutex *mutex=NULL;
    CHECK(device->OpenSharedResource(handle,__uuidof(ID3D11Texture2D),(void **)&image));
    CHECK(image->QueryInterface(__uuidof(IDXGIKeyedMutex),(void **)&mutex));
    CHECK(device->CreateRenderTargetView(image,NULL,&view));
    ID3D11Query *completion=NULL;
    if(waitForGpu){D3D11_QUERY_DESC query={D3D11_QUERY_EVENT,0};CHECK(device->CreateQuery(&query,&completion));}
    for(unsigned i=0;i<16;i++) {
        HRESULT acquired=mutex->AcquireSync(0,5000);
        if(acquired!=S_OK){printf("{\"child_acquire\":\"%08lx\",\"iteration\":%u}\n",(unsigned long)acquired,i);return 1;}
        unsigned c=SharedSamplerColors[i];const float color[]={((c>>16)&255)/255.0f,((c>>8)&255)/255.0f,(c&255)/255.0f,1};
        context->ClearRenderTargetView(view,color);context->Flush();CHECK(device->GetDeviceRemovedReason());
        if(waitForGpu) {
            context->End(completion);context->Flush();BOOL complete=FALSE;HRESULT hr=S_FALSE;ULONGLONG deadline=GetTickCount64()+5000;
            while(hr==S_FALSE && GetTickCount64()<deadline){hr=context->GetData(completion,&complete,sizeof(complete),0);if(hr==S_FALSE)Sleep(1);}
            if(hr!=S_OK || !complete){printf("{\"diagnostic_child_query\":\"%08lx\",\"iteration\":%u}\n",(unsigned long)hr,i);return 1;}
        }
        CHECK(mutex->ReleaseSync(1));
    }
    if(completion)completion->Release();view->Release();mutex->Release();image->Release();return 0;
}
static int SharedSamplerTest(ID3D11Device *device,ID3D11DeviceContext *context,bool waitForGpu,bool fullViewport,unsigned rebindMask) {
    ID3D11Texture2D *shared=NULL,*target=NULL,*staging=NULL,*sourceStaging=NULL;IDXGIKeyedMutex *mutex=NULL;
    D3D11_TEXTURE2D_DESC desc={};desc.Width=desc.Height=64;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    CHECK(device->CreateTexture2D(&desc,NULL,&shared));CHECK(shared->QueryInterface(__uuidof(IDXGIKeyedMutex),(void **)&mutex));
    IDXGIResource *resource=NULL;HANDLE sharedHandle=NULL;CHECK(shared->QueryInterface(__uuidof(IDXGIResource),(void **)&resource));
    CHECK(resource->GetSharedHandle(&sharedHandle));resource->Release();
    desc.MiscFlags=0;desc.BindFlags=D3D11_BIND_RENDER_TARGET;CHECK(device->CreateTexture2D(&desc,NULL,&target));
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;CHECK(device->CreateTexture2D(&desc,NULL,&staging));CHECK(device->CreateTexture2D(&desc,NULL,&sourceStaging));
    ID3D11ShaderResourceView *srv=NULL;ID3D11RenderTargetView *rtv=NULL;ID3D11SamplerState *sampler=NULL;
    CHECK(device->CreateShaderResourceView(shared,NULL,&srv));CHECK(device->CreateRenderTargetView(target,NULL,&rtv));
    D3D11_SAMPLER_DESC sample={};sample.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;sample.AddressU=sample.AddressV=sample.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sample.ComparisonFunc=D3D11_COMPARISON_NEVER;sample.MaxLOD=D3D11_FLOAT32_MAX;CHECK(device->CreateSamplerState(&sample,&sampler));
    const char *shader="Texture2D<float4> image:register(t0);SamplerState smp:register(s0);float4 vs(uint id:SV_VertexID):SV_Position{float2 p[3]={float2(-1,-1),float2(-1,3),float2(3,-1)};return float4(p[id],0,1);}float4 ps():SV_Target{return image.Sample(smp,float2(.5,.5));}";
    ID3DBlob *vsCode=NULL,*psCode=NULL,*constantCode=NULL,*errors=NULL;ID3D11VertexShader *vs=NULL;ID3D11PixelShader *ps=NULL,*constantPS=NULL;
    CHECK(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"vs","vs_4_0",0,0,&vsCode,&errors));
    CHECK(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"ps","ps_4_0",0,0,&psCode,&errors));
    CHECK(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),NULL,&vs));
    CHECK(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),NULL,&ps));
    const char *constantShader="float4 ps():SV_Target{return float4(0,1,0,1);}";
    CHECK(D3DCompile(constantShader,strlen(constantShader),NULL,NULL,NULL,"ps","ps_4_0",0,0,&constantCode,&errors));
    CHECK(device->CreatePixelShader(constantCode->GetBufferPointer(),constantCode->GetBufferSize(),NULL,&constantPS));
    D3D11_RASTERIZER_DESC raster={};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    ID3D11RasterizerState *rs=NULL;CHECK(device->CreateRasterizerState(&raster,&rs));context->RSSetState(rs);
    context->VSSetShader(vs,NULL,0);context->PSSetShader(ps,NULL,0);context->IASetInputLayout(NULL);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetRenderTargets(1,&rtv,NULL);context->PSSetShaderResources(0,1,&srv);context->PSSetSamplers(0,1,&sampler);
    const float black[]={0,0,0,1};context->ClearRenderTargetView(rtv,black);
    D3D11_VIEWPORT controlViewport={0,0,64,64,0,1};context->RSSetViewports(1,&controlViewport);
    D3D11_RECT controlScissor={0,0,64,64};context->RSSetScissorRects(1,&controlScissor);
    const float magenta[]={1,0,1,1};context->ClearRenderTargetView(rtv,magenta);
    int controlsFailed=SharedSamplerControl(context,target,staging,"clear_and_copy",0xffff00ff);
    context->PSSetShader(constantPS,NULL,0);context->Draw(3,0);
    controlsFailed+=SharedSamplerControl(context,target,staging,"constant_ps_same_vs_rtv_viewport",0xff00ff00);
    // Same-process private texture: no shared-handle or external ownership path.
    D3D11_TEXTURE2D_DESC localDesc={};shared->GetDesc(&localDesc);localDesc.MiscFlags=0;
    ID3D11Texture2D *local=NULL;ID3D11RenderTargetView *localRTV=NULL;ID3D11ShaderResourceView *localSRV=NULL;
    CHECK(device->CreateTexture2D(&localDesc,NULL,&local));CHECK(device->CreateRenderTargetView(local,NULL,&localRTV));CHECK(device->CreateShaderResourceView(local,NULL,&localSRV));
    const float red[]={1,0,0,1};context->ClearRenderTargetView(localRTV,red);context->PSSetShader(ps,NULL,0);context->PSSetShaderResources(0,1,&localSRV);
    context->Draw(3,0);controlsFailed+=SharedSamplerControl(context,target,staging,"same_process_private_clear_to_srv",0xffff0000);
    HRESULT ownership=mutex->AcquireSync(0,5000);if(ownership!=S_OK){printf("{\"control_acquire\":\"%08lx\"}\n",(unsigned long)ownership);return 1;}
    ID3D11RenderTargetView *sharedRTV=NULL;CHECK(device->CreateRenderTargetView(shared,NULL,&sharedRTV));
    context->ClearRenderTargetView(sharedRTV,red);context->Flush();CHECK(mutex->ReleaseSync(1));ownership=mutex->AcquireSync(1,5000);
    if(ownership!=S_OK){printf("{\"control_acquire\":\"%08lx\"}\n",(unsigned long)ownership);return 1;}
    context->PSSetShaderResources(0,1,&srv);context->Draw(3,0);context->Flush();CHECK(mutex->ReleaseSync(0));
    controlsFailed+=SharedSamplerControl(context,target,staging,"same_process_shared_clear_to_srv",0xffff0000);
    int baselineControlsFailed=controlsFailed;
    context->ClearRenderTargetView(rtv,magenta);context->PSSetShader(constantPS,NULL,0);
    for(unsigned i=0;i<16;i++) {
        D3D11_VIEWPORT viewport={0,(float)(i*4),64,4,0,1};context->RSSetViewports(1,&viewport);context->Draw(3,0);
    }
    controlsFailed+=SharedSamplerControl(context,target,staging,"constant_ps_16_strip_viewports",0xff00ff00);
    context->ClearRenderTargetView(rtv,magenta);context->PSSetShader(ps,NULL,0);context->PSSetShaderResources(0,1,&localSRV);
    for(unsigned i=0;i<16;i++) {
        D3D11_VIEWPORT viewport={0,(float)(i*4),64,4,0,1};context->RSSetViewports(1,&viewport);context->Draw(3,0);
    }
    controlsFailed+=SharedSamplerControl(context,target,staging,"same_process_srv_16_strip_viewports",0xffff0000);
    context->PSSetShaderResources(0,1,&srv);
    localSRV->Release();localRTV->Release();local->Release();sharedRTV->Release();
    if(controlsFailed && (!fullViewport || baselineControlsFailed)){printf("{\"sampler_controls\":\"FAIL\",\"failed_controls\":%d,\"cross_process_run\":false}\n",controlsFailed);return 1;}
    if(controlsFailed)printf("{\"strip_controls_failed\":%d,\"full_viewport_diagnostic_continues\":true}\n",controlsFailed);
    context->ClearRenderTargetView(rtv,black);
    WCHAR executable[MAX_PATH],command[MAX_PATH+100];if(!GetModuleFileNameW(NULL,executable,MAX_PATH))return 1;
    if(swprintf_s(command,L"\"%s\" %s %llu",executable,waitForGpu?L"shared-sampler-child-wait":L"shared-sampler-child",(unsigned long long)(uintptr_t)sharedHandle)<0)return 1;
    STARTUPINFOW startup={};startup.cb=sizeof(startup);PROCESS_INFORMATION process={};
    if(!CreateProcessW(executable,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process))return 1;
    for(unsigned i=0;i<16;i++) {
        HRESULT acquired=mutex->AcquireSync(1,5000);
        if(acquired!=S_OK){printf("{\"parent_acquire\":\"%08lx\",\"iteration\":%u}\n",(unsigned long)acquired,i);TerminateProcess(process.hProcess,1);CloseHandle(process.hThread);CloseHandle(process.hProcess);return 1;}
        ID3D11ShaderResourceView *runtimeBound=NULL;context->PSGetShaderResources(0,1,&runtimeBound);
        printf("{\"runtime_srv_after_acquire\":%u,\"bound\":\"%p\",\"expected\":\"%p\",\"same\":%s,\"null\":%s}\n",i,(void *)runtimeBound,(void *)srv,runtimeBound==srv?"true":"false",runtimeBound?"false":"true");
        if(runtimeBound)runtimeBound->Release();
        // ReleaseSync explicitly unsets this resource from the pipeline.
        // Restore the SRV after acquiring ownership; preserving a prior API
        // binding across ReleaseSync is not part of the DXGI contract.
        context->PSSetShaderResources(0,1,&srv);
        if(rebindMask) {
            // Force runtime callbacks instead of relying on cached API state.
            ID3D11ShaderResourceView *emptySRV=NULL;ID3D11SamplerState *emptySampler=NULL;
            ID3D11Buffer *emptyVB=NULL;UINT zero=0;
            if(rebindMask&1u)context->PSSetShaderResources(0,1,&emptySRV);
            if(rebindMask&2u)context->PSSetSamplers(0,1,&emptySampler);
            if(rebindMask&4u){context->VSSetShader(NULL,NULL,0);context->PSSetShader(NULL,NULL,0);}
            if(rebindMask&8u)context->OMSetRenderTargets(0,NULL,NULL);
            if(rebindMask&16u){context->RSSetViewports(0,NULL);context->RSSetState(NULL);}
            if(rebindMask&32u){context->IASetVertexBuffers(0,1,&emptyVB,&zero,&zero);context->IASetInputLayout(NULL);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED);}
            if(rebindMask&1u)context->PSSetShaderResources(0,1,&srv);
            if(rebindMask&2u)context->PSSetSamplers(0,1,&sampler);
            if(rebindMask&4u){context->VSSetShader(vs,NULL,0);context->PSSetShader(ps,NULL,0);}
            if(rebindMask&8u)context->OMSetRenderTargets(1,&rtv,NULL);
            if(rebindMask&16u){context->RSSetState(rs);context->RSSetScissorRects(1,&controlScissor);}
            if(rebindMask&32u){context->IASetVertexBuffers(0,1,&emptyVB,&zero,&zero);context->IASetInputLayout(NULL);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);}
        }
        D3D11_VIEWPORT viewport={0,fullViewport?0.0f:(float)(i*4),64,fullViewport?64.0f:4.0f,0,1};context->RSSetViewports(1,&viewport);
        context->Draw(3,0);
        // Record a GPU copy only after sampling, never a CPU wait before Draw.
        D3D11_BOX sourceBox={32,32,0,33,33,1};context->CopySubresourceRegion(sourceStaging,0,0,i,0,shared,0,&sourceBox);
        context->Flush();CHECK(device->GetDeviceRemovedReason());CHECK(mutex->ReleaseSync(0));
    }
    printf("{\"cross_process_full_viewport\":%s,\"force_parent_rebind\":%s,\"rebind_mask\":%u}\n",fullViewport?"true":"false",rebindMask?"true":"false",rebindMask);
    DWORD childExit=1;if(WaitForSingleObject(process.hProcess,5000)==WAIT_OBJECT_0)GetExitCodeProcess(process.hProcess,&childExit);else TerminateProcess(process.hProcess,1);
    CloseHandle(process.hThread);CloseHandle(process.hProcess);
    context->CopyResource(staging,target);D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&mapped));
    unsigned mismatches=0;
    for(unsigned i=0;i<16;i++) {
        unsigned actual=*(unsigned *)((BYTE *)mapped.pData+(i*4+2)*mapped.RowPitch+32*4);
        unsigned expected=SharedSamplerColors[fullViewport?15:i];
        if(actual!=expected){printf("{\"shared_sampler_mismatch\":%u,\"actual\":\"%08x\",\"expected\":\"%08x\"}\n",i,actual,expected);mismatches++;}
    }
    context->Unmap(staging,0);
    CHECK(context->Map(sourceStaging,0,D3D11_MAP_READ,0,&mapped));unsigned sourceMismatches=0;
    for(unsigned i=0;i<16;i++) {
        unsigned actual=*(unsigned *)((BYTE *)mapped.pData+i*mapped.RowPitch);
        if(actual!=SharedSamplerColors[i]){printf("{\"shared_source_copy_mismatch\":%u,\"actual\":\"%08x\",\"expected\":\"%08x\"}\n",i,actual,SharedSamplerColors[i]);sourceMismatches++;}
    }
    context->Unmap(sourceStaging,0);printf("{\"source_copy_mismatches\":%u,\"diagnostic_child_gpu_completion\":%s}\n",sourceMismatches,waitForGpu?"true":"false");
    mismatches+=sourceMismatches;
    context->ClearState();context->Flush();
    printf("{\"cross_process_srv_sampling\":\"%s\",\"color_changes\":16,\"cpu_readbacks\":1,\"ownership_sync\":\"DXGI_KEYED_MUTEX\",\"child_exit\":%lu,\"mismatches\":%u}\n",!childExit && !mismatches?"PASS":"FAIL",childExit,mismatches);
    rs->Release();ps->Release();constantPS->Release();vs->Release();psCode->Release();constantCode->Release();vsCode->Release();sampler->Release();srv->Release();rtv->Release();mutex->Release();shared->Release();target->Release();staging->Release();sourceStaging->Release();
    return childExit || mismatches?1:0;
}
