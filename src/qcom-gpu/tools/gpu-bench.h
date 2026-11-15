#pragma once
/* Included by d3d-test.cpp so the benchmark uses the same strict adapter selection. */
static HRESULT WaitBenchQuery(ID3D11DeviceContext *context,ID3D11Query *query,void *data,UINT size) {
    ULONGLONG deadline=GetTickCount64()+15000;
    HRESULT hr;
    while((hr=context->GetData(query,data,size,D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE) {
        if(GetTickCount64()>=deadline)return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        Sleep(1);
    }
    return hr;
}
static int Benchmark(ID3D11Device *device,ID3D11DeviceContext *ctx) {
    const char shader[]=
        "float4 vs(uint i:SV_VertexID):SV_Position{float2 p=float2((i<<1)&2,i&2);return float4(p*2-1,0,1);}"
        "float4 ps():SV_Target{return float4(.2,.4,.7,.13);}";
    ID3DBlob *vs=NULL,*ps=NULL;
    CHECK(D3DCompile(shader,sizeof(shader)-1,NULL,NULL,NULL,"vs","vs_4_0",0,0,&vs,NULL));
    CHECK(D3DCompile(shader,sizeof(shader)-1,NULL,NULL,NULL,"ps","ps_4_0",0,0,&ps,NULL));
    ID3D11VertexShader *vertex=NULL;ID3D11PixelShader *pixel=NULL;
    CHECK(device->CreateVertexShader(vs->GetBufferPointer(),vs->GetBufferSize(),NULL,&vertex));
    CHECK(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),NULL,&pixel));
    D3D11_RASTERIZER_DESC raster={};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    ID3D11RasterizerState *rs=NULL;CHECK(device->CreateRasterizerState(&raster,&rs));ctx->RSSetState(rs);
    D3D11_BLEND_DESC bd={};auto &rt=bd.RenderTarget[0];
    rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_ZERO;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *blend=NULL;CHECK(device->CreateBlendState(&bd,&blend));ctx->OMSetBlendState(blend,NULL,~0u);
    ctx->VSSetShader(vertex,NULL,0);ctx->PSSetShader(pixel,NULL,0);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    D3D11_QUERY_DESC q={D3D11_QUERY_EVENT,0};ID3D11Query *done=NULL;CHECK(device->CreateQuery(&q,&done));
    for(UINT smallDraw=0;smallDraw<2;smallDraw++) {
        UINT width=smallDraw?8:1264,height=smallDraw?8:2780,draws=smallDraw?20000:1024;
        D3D11_TEXTURE2D_DESC td={};td.Width=width;td.Height=height;td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D *target=NULL;ID3D11RenderTargetView *view=NULL;
        CHECK(device->CreateTexture2D(&td,NULL,&target));CHECK(device->CreateRenderTargetView(target,NULL,&view));
        D3D11_VIEWPORT vp={0,0,(float)width,(float)height,0,1};ctx->RSSetViewports(1,&vp);ctx->OMSetRenderTargets(1,&view,NULL);
        const float clear[4]={};ctx->ClearRenderTargetView(view,clear);ctx->Draw(3,0);ctx->End(done);ctx->Flush();
        BOOL completed=FALSE;CHECK(WaitBenchQuery(ctx,done,&completed,sizeof(completed)));if(!completed)return 1;
        ID3D11Query *disjoint=NULL,*first=NULL,*last=NULL;
        q.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;
        bool timestamp=SUCCEEDED(device->CreateQuery(&q,&disjoint));
        q.Query=D3D11_QUERY_TIMESTAMP;
        timestamp=timestamp && SUCCEEDED(device->CreateQuery(&q,&first)) && SUCCEEDED(device->CreateQuery(&q,&last));
        LARGE_INTEGER start,end;QueryPerformanceCounter(&start);
        if(timestamp){ctx->Begin(disjoint);ctx->End(first);}
        for(UINT i=0;i<draws;i++)ctx->Draw(3,0);
        if(timestamp){ctx->End(last);ctx->End(disjoint);}
        ctx->End(done);ctx->Flush();completed=FALSE;
        CHECK(WaitBenchQuery(ctx,done,&completed,sizeof(completed)));if(!completed)return 1;
        QueryPerformanceCounter(&end);
        double seconds=(double)(end.QuadPart-start.QuadPart)/(double)frequency.QuadPart;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing={};UINT64 a=0,b=0;
        bool valid=timestamp && ctx->GetData(disjoint,&timing,sizeof(timing),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
            ctx->GetData(first,&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK &&
            ctx->GetData(last,&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK && !timing.Disjoint && timing.Frequency && b>a;
        printf("{\"benchmark\":\"%s\",\"width\":%u,\"height\":%u,\"draws\":%u,\"completion_ms\":%.3f,\"gpu_ms\":",
            smallDraw?"small_draw":"blended_fill",width,height,draws,seconds*1000.);
        if(valid)printf("%.3f",(double)(b-a)*1000./(double)timing.Frequency);else printf("null");
        printf(",\"draws_per_second_end_to_end\":%.2f,\"pixels_per_second_end_to_end\":%.2f}\n",draws/seconds,(double)draws*width*height/seconds);
        if(last)last->Release();if(first)first->Release();if(disjoint)disjoint->Release();
        ctx->OMSetRenderTargets(0,NULL,NULL);view->Release();target->Release();
    }
    ctx->ClearState();done->Release();blend->Release();rs->Release();vertex->Release();pixel->Release();vs->Release();ps->Release();return 0;
}
