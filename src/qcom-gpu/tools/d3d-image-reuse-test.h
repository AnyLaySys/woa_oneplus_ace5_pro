static int ImageReuse(ID3D11Device *device,ID3D11DeviceContext *context) {
    static const UINT sizes[][2]={{64,64},{256,193},{511,127},{192,512},{128,128},{384,240}};
    static const UINT colors[]={0xffff0000u,0xff00ff00u,0xff0000ffu,0xffffff00u,0xffff00ffu,0xff00ffffu};
    unsigned samples=0;
    for(UINT round=0;round<64;round++) {
        UINT width=sizes[round%6][0],height=sizes[round%6][1],color=colors[round%6];
        D3D11_TEXTURE2D_DESC desc={};desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=1;
        desc.Format=round&1?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ID3D11Texture2D *image=NULL,*staging=NULL;ID3D11RenderTargetView *view=NULL;
        CHECK(device->CreateTexture2D(&desc,NULL,&image));CHECK(device->CreateRenderTargetView(image,NULL,&view));
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        CHECK(device->CreateTexture2D(&desc,NULL,&staging));
        const float clear[]={((color>>16)&255)/255.0f,((color>>8)&255)/255.0f,(color&255)/255.0f,1};
        context->ClearRenderTargetView(view,clear);context->CopyResource(staging,image);
        D3D11_MAPPED_SUBRESOURCE mapped={};CHECK(context->Map(staging,0,D3D11_MAP_READ,0,&mapped));
        UINT expected=round&1?(color&0xff00ff00u)|((color>>16)&255)|((color&255)<<16):color;
        const UINT points[][2]={{0,0},{width-1,0},{0,height-1},{width-1,height-1},{width/2,height/2}};
        unsigned mismatches=0;
        for(const auto &point:points){UINT pixel=*(UINT *)((BYTE *)mapped.pData+point[1]*mapped.RowPitch+point[0]*4);if(pixel!=expected)mismatches++;samples++;}
        context->Unmap(staging,0);view->Release();image->Release();staging->Release();
        if(mismatches){printf("{\"image_reuse_round\":%u,\"mismatches\":%u,\"result\":\"FAIL\"}\n",round,mismatches);return 1;}
    }
    printf("{\"private_image_reuse\":\"PASS\",\"textures\":64,\"formats\":2,\"sampled_pixels\":%u}\n",samples);return 0;
}
