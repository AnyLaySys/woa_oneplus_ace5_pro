/* Detect in-flight dynamic VB/CB overwrite without a wait between draws. */
static int DynamicUpdates(ID3D11Device *device,ID3D11DeviceContext *context) {
    const char *shader="cbuffer C:register(b0){float4 color;} float4 vs(float2 p:POSITION):SV_Position{return float4(p,0,1);} float4 ps():SV_Target{return color;}";
    ID3DBlob *vsCode=NULL,*psCode=NULL,*errors=NULL;
    CHECK(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"vs","vs_4_0",0,0,&vsCode,&errors));
    CHECK(D3DCompile(shader,strlen(shader),NULL,NULL,NULL,"ps","ps_4_0",0,0,&psCode,&errors));
    ID3D11VertexShader *vs=NULL;ID3D11PixelShader *ps=NULL;ID3D11InputLayout *layout=NULL;
    CHECK(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),NULL,&vs));
    CHECK(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),NULL,&ps));
    D3D11_INPUT_ELEMENT_DESC element={"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0};
    CHECK(device->CreateInputLayout(&element,1,vsCode->GetBufferPointer(),vsCode->GetBufferSize(),&layout));
    ID3D11Buffer *vertices=NULL,*defaultVertices=NULL,*constants=NULL,*dynamicConstants=NULL;
    D3D11_BUFFER_DESC buffer={};buffer.ByteWidth=64*12*sizeof(float);buffer.Usage=D3D11_USAGE_DYNAMIC;
    buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    CHECK(device->CreateBuffer(&buffer,NULL,&vertices));
    buffer.Usage=D3D11_USAGE_DEFAULT;buffer.CPUAccessFlags=0;CHECK(device->CreateBuffer(&buffer,NULL,&defaultVertices));
    buffer.ByteWidth=4*sizeof(float);buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;buffer.CPUAccessFlags=0;
    CHECK(device->CreateBuffer(&buffer,NULL,&constants));
    buffer.Usage=D3D11_USAGE_DYNAMIC;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    CHECK(device->CreateBuffer(&buffer,NULL,&dynamicConstants));
    ID3D11Texture2D *image=NULL,*staging=NULL;ID3D11RenderTargetView *view=NULL;
    D3D11_TEXTURE2D_DESC texture={};texture.Width=512;texture.Height=64;texture.MipLevels=texture.ArraySize=1;
    texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;texture.SampleDesc.Count=1;texture.Usage=D3D11_USAGE_DEFAULT;texture.BindFlags=D3D11_BIND_RENDER_TARGET;
    CHECK(device->CreateTexture2D(&texture,NULL,&image));CHECK(device->CreateRenderTargetView(image,NULL,&view));
    texture.Usage=D3D11_USAGE_STAGING;texture.BindFlags=0;texture.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&texture,NULL,&staging));
    context->VSSetShader(vs,NULL,0);context->PSSetShader(ps,NULL,0);context->IASetInputLayout(layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride=2*sizeof(float),offset=0;context->IASetVertexBuffers(0,1,&vertices,&stride,&offset);
    D3D11_VIEWPORT viewport={0,0,512,64,0,1};context->RSSetViewports(1,&viewport);context->OMSetRenderTargets(1,&view,NULL);
    for(unsigned mode=0;mode<6;mode++) {
        const float black[]={0,0,0,1};context->ClearRenderTargetView(view,black);
        ID3D11Buffer *cb=(mode&1)?dynamicConstants:constants;context->PSSetConstantBuffers(0,1,&cb);
        for(unsigned i=0;i<64;i++) {
            float left=-1.0f+(float)i/32.0f,right=left+1.0f/32.0f;
            const float positions[]={left,-1,left,1,right,-1,right,-1,left,1,right,1};
            UINT vertexOffset=mode>=4?i*(UINT)sizeof(positions):0;ID3D11Buffer *vb=mode==4?defaultVertices:vertices;
            D3D11_MAPPED_SUBRESOURCE mapped={};
            if(mode==4){D3D11_BOX partial={vertexOffset,0,0,vertexOffset+(UINT)sizeof(positions),1,1};context->UpdateSubresource(vb,0,&partial,positions,0,0);}
            else {CHECK(context->Map(vb,0,mode==5 && i?D3D11_MAP_WRITE_NO_OVERWRITE:D3D11_MAP_WRITE_DISCARD,0,&mapped));memcpy((BYTE *)mapped.pData+vertexOffset,positions,sizeof(positions));context->Unmap(vb,0);}
            context->IASetVertexBuffers(0,1,&vb,&stride,&vertexOffset);
            const float color[]={i&1?0.0f:1.0f,i&1?1.0f:0.0f,0,1};
            if(mode&1) {
                CHECK(context->Map(cb,0,D3D11_MAP_WRITE_DISCARD,0,&mapped));memcpy(mapped.pData,color,sizeof(color));context->Unmap(cb,0);
            } else context->UpdateSubresource(cb,0,NULL,color,0,0);
            context->Draw(6,0);
        }
        context->CopyResource(staging,image);D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&mapped));
        unsigned mismatches=0,first=0,actual=0;
        for(unsigned i=0;i<64;i++) {
            unsigned pixel=*(unsigned *)((BYTE *)mapped.pData+32*mapped.RowPitch+(i*8+4)*4);
            unsigned expected=i&1?0xff00ff00u:0xffff0000u;
            if(pixel!=expected){if(!mismatches){first=i;actual=pixel;}mismatches++;}
        }
        context->Unmap(staging,0);
        printf("{\"round\":%u,\"dynamic_vb\":\"%s\",\"constant_buffer\":\"%s\",\"draws_before_wait\":64,\"mismatched_stripes\":%u,\"first_stripe\":%u,\"first_actual\":\"%08x\",\"result\":\"%s\"}\n",mode,mode==5?"NO_OVERWRITE":mode==4?"PARTIAL_UPDATE":"DISCARD",mode&1?"DISCARD":"UPDATE_SUBRESOURCE",mismatches,first,actual,mismatches?"FAIL":"PASS");
        if(mismatches)return 1;
    }
    context->ClearState();view->Release();image->Release();staging->Release();vertices->Release();defaultVertices->Release();constants->Release();dynamicConstants->Release();
    layout->Release();vs->Release();ps->Release();vsCode->Release();psCode->Release();return 0;
}
