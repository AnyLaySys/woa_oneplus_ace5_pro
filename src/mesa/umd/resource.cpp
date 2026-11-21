#include "umd.h"

static bool imported_image(Device *d,Resource *r) {
 A830_ALLOCATION_ADDRESS a={A830_ABI_VERSION,r->allocation,0,0,0,0,0};D3DDDICB_ESCAPE e{};e.hDevice=d->runtime;e.hContext=d->context;e.pPrivateDriverData=&a;e.PrivateDriverDataSize=sizeof(a);
 HRESULT hr=d->callbacks.pfnEscapeCb(d->adapter,&e);if(FAILED(hr)){error(d,hr);return false;}
 r->pitch=a.Pitch;return image_create(d,r,a.Address,a.Size);
}
SIZE_T APIENTRY CalcPrivateResourceSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATERESOURCE *) {return sizeof(Resource);}
SIZE_T APIENTRY CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE,const D3D10DDIARG_OPENRESOURCE *) {return sizeof(Resource);}
void APIENTRY CreateResource(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATERESOURCE *p,D3D10DDI_HRESOURCE hr,D3D10DDI_HRTRESOURCE rt) {
 Device *d=dev(h);auto *r=new(hr.pDrvPrivate) Resource{};r->format=p->Format;r->vkformat=format(p->Format);r->dimension=p->ResourceDimension;r->bind=p->BindFlags;r->runtime=rt.handle;
 r->levels=std::max(1u,p->MipLevels);r->layers=std::max(1u,p->ArraySize);r->width=p->pMipInfoList[0].TexelWidth;r->height=p->pMipInfoList[0].TexelHeight;r->depth=p->pMipInfoList[0].TexelDepth;
 if(p->pPrimaryDesc)p->pPrimaryDesc->DriverFlags=0;
 if(!r->width){error(d,E_INVALIDARG);return;}
 if(r->dimension==D3D10DDIRESOURCE_BUFFER) {
   r->height=r->depth=1;r->layers=r->levels=1;
   if(!buffer_create(d,r->storage,r->width,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT))return;
 } else {
   r->height=std::max(1u,r->height);r->depth=std::max(1u,r->depth);
   if(p->SampleDesc.Count>1){error(d,DXGI_ERROR_UNSUPPORTED);return;}
   if(p->BindFlags&D3D10_DDI_BIND_DEPTH_STENCIL) {
     if(p->Format==DXGI_FORMAT_R24G8_TYPELESS)r->format=DXGI_FORMAT_D24_UNORM_S8_UINT;
     if(p->Format==DXGI_FORMAT_R32_TYPELESS)r->format=DXGI_FORMAT_D32_FLOAT;
     if(p->Format==DXGI_FORMAT_R16_TYPELESS)r->format=DXGI_FORMAT_D16_UNORM;r->vkformat=format(r->format);
   }
   bool shared=(p->MiscFlags&D3D10_DDI_RESOURCE_MISC_SHARED)||(p->BindFlags&D3D10_DDI_BIND_PRESENT);
   if(shared) {
     unsigned native=r->format==DXGI_FORMAT_B8G8R8A8_UNORM?D3DDDIFMT_A8R8G8B8:
       r->format==DXGI_FORMAT_R8G8B8A8_UNORM?D3DDDIFMT_A8B8G8R8:
       r->format==DXGI_FORMAT_R10G10B10A2_UNORM?D3DDDIFMT_A2B10G10R10:
       r->format==DXGI_FORMAT_R16G16B16A16_FLOAT?D3DDDIFMT_A16B16G16R16F:0;
     unsigned bytes=A830PixelBytes(native);
     if(!bytes || r->dimension!=D3D10DDIRESOURCE_TEXTURE2D || r->layers*r->levels!=1 || r->width>(UINT_MAX-255)/bytes){error(d,DXGI_ERROR_UNSUPPORTED);return;}
     A830_RESOURCE data={A830_ABI_VERSION,0,r->width,r->height,(r->width*bytes+255)&~255u,native,A830_RESOURCE_OWNED|(p->pPrimaryDesc?A830_RESOURCE_PRIMARY:0u)};
     data.Rotation=p->pPrimaryDesc?(unsigned)p->pPrimaryDesc->ModeDesc.Rotation:(unsigned)D3DDDI_ROTATION_IDENTITY;
     if(data.Height>0x4000000u/data.Pitch){error(d,E_OUTOFMEMORY);return;}data.Size=data.Pitch*data.Height;
     D3DDDI_ALLOCATIONINFO info{};info.pPrivateDriverData=&data;info.PrivateDriverDataSize=sizeof(data);info.Flags.Primary=p->pPrimaryDesc!=nullptr;
     D3DDDICB_ALLOCATE a{};a.pPrivateDriverData=&data;a.PrivateDriverDataSize=sizeof(data);a.hResource=rt.handle;a.NumAllocations=1;a.pAllocationInfo=&info;
     HRESULT result=d->callbacks.pfnAllocateCb(d->runtime,&a);if(FAILED(result)){error(d,result);return;}
     r->allocation=info.hAllocation;r->kmresource=a.hKMResource;r->pitch=data.Pitch;
     if(!imported_image(d,r))return;
   } else if(!image_create(d,r))return;
 }
 if(p->pInitialDataUP)for(UINT i=0;i<r->levels*r->layers;i++) {
   auto &data=p->pInitialDataUP[i];ResourceUpdateSubResourceUP(h,hr,i,nullptr,data.pSysMem,data.SysMemPitch,data.SysMemSlicePitch);
 }
}
void APIENTRY OpenResource(D3D10DDI_HDEVICE h,const D3D10DDIARG_OPENRESOURCE *p,D3D10DDI_HRESOURCE hr,D3D10DDI_HRTRESOURCE rt) {
 Device *d=dev(h);auto *r=new(hr.pDrvPrivate) Resource{};
 if(p->NumAllocations!=1 || p->PrivateDriverDataSize!=sizeof(A830_RESOURCE) || !p->pPrivateDriverData){error(d,E_INVALIDARG);return;}
 auto *a=(const A830_RESOURCE *)p->pPrivateDriverData;
 if(a->Version!=A830_ABI_VERSION || !a->Width || !a->Height || !(a->Flags&A830_RESOURCE_OWNED)){error(d,E_INVALIDARG);return;}
 r->format=a->Format==D3DDDIFMT_A8R8G8B8 || a->Format==D3DDDIFMT_X8R8G8B8?DXGI_FORMAT_B8G8R8A8_UNORM:
   a->Format==D3DDDIFMT_A8B8G8R8?DXGI_FORMAT_R8G8B8A8_UNORM:
   a->Format==D3DDDIFMT_A2B10G10R10?DXGI_FORMAT_R10G10B10A2_UNORM:
   a->Format==D3DDDIFMT_A16B16G16R16F?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_UNKNOWN;
 if(r->format==DXGI_FORMAT_UNKNOWN){error(d,DXGI_ERROR_UNSUPPORTED);return;}
 r->vkformat=format(r->format);r->width=a->Width;r->height=a->Height;r->pitch=a->Pitch;r->dimension=D3D10DDIRESOURCE_TEXTURE2D;r->bind=D3D10_DDI_BIND_RENDER_TARGET|D3D10_DDI_BIND_SHADER_RESOURCE;
 r->allocation=p->pOpenAllocationInfo[0].hAllocation;r->kmresource=p->hKMResource.handle;r->runtime=rt.handle;r->imported=true;imported_image(d,r);
}
void APIENTRY DestroyResource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr) {
 Device *d=dev(h);Resource *r=res(hr);
 if(r->allocation)flush(d,true);
 if(d->recording){d->temporary.push_back(r->mapping.staging);d->temporary.push_back(r->storage);r->mapping.staging={};r->storage={};}
 else {buffer_recycle(d,r->mapping.staging);storage_recycle(d,r->storage);}
 if(r->allocation && !r->imported){D3DDDICB_DEALLOCATE a{};a.hResource=r->runtime;d->callbacks.pfnDeallocateCb(d->runtime,&a);}
 r->~Resource();
}
void APIENTRY ResourceMap(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr,UINT sub,D3D10_DDI_MAP mode,UINT,D3D10DDI_MAPPED_SUBRESOURCE *out) {
 Device *d=dev(h);Resource *r=res(hr);memset(out,0,sizeof(*out));

 if(r->storage.buffer) {
   if(mode==D3D10_DDI_MAP_WRITE_DISCARD){if(!buffer_rename(d,r))return;}
   else if(mode!=D3D10_DDI_MAP_WRITE_NOOVERWRITE){if(!flush(d,true))return;}
   if(mode==D3D10_DDI_MAP_READ || mode==D3D10_DDI_MAP_READWRITE)sync_memory(d,r->storage,true);
   out->pData=r->storage.map;out->RowPitch=out->DepthPitch=r->width;r->mapping.mode=mode;return;
 }
 if(sub>=r->levels*r->layers){error(d,E_INVALIDARG);return;}
 UINT level=sub%r->levels,layer=sub/r->levels,width=std::max(1u,r->width>>level),height=std::max(1u,r->height>>level),depth=std::max(1u,r->depth>>level);
 Mapping &m=r->mapping;buffer_recycle(d,m.staging);m.sub=sub;m.mode=mode;m.row=width*pixel_bytes(r->format);m.slice=m.row*height;
 if(!buffer_create(d,m.staging,(VkDeviceSize)m.slice*depth,0))return;
 if(mode==D3D10_DDI_MAP_READ || mode==D3D10_DDI_MAP_READWRITE) {
   barrier(d);VkBufferImageCopy copy{};copy.imageSubresource={aspects(r->format),level,layer,1};copy.imageExtent={width,height,depth};
   d->vk.CmdCopyImageToBuffer(d->cmd,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,m.staging.buffer,1,&copy);
   if(!flush(d,true) || !sync_memory(d,m.staging,true))return;
 }
 out->pData=m.staging.map;out->RowPitch=m.row;out->DepthPitch=m.slice;
}
void APIENTRY ResourceUnmap(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr,UINT) {
 Device *d=dev(h);Resource *r=res(hr);Mapping &m=r->mapping;

 if(r->storage.buffer) {
   if(m.mode!=D3D10_DDI_MAP_READ)sync_memory(d,r->storage,false);return;
 }
 if(!m.staging.buffer)return;
 if(m.mode!=D3D10_DDI_MAP_READ) {
   if(!sync_memory(d,m.staging,false))return;
   UINT level=m.sub%r->levels,layer=m.sub/r->levels;barrier(d);VkBufferImageCopy copy{};copy.imageSubresource={aspects(r->format),level,layer,1};copy.imageExtent={std::max(1u,r->width>>level),std::max(1u,r->height>>level),std::max(1u,r->depth>>level)};
   r->storage.used=d->next_submit;d->vk.CmdCopyBufferToImage(d->cmd,m.staging.buffer,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);d->temporary.push_back(m.staging);m.staging={};
 } else buffer_recycle(d,m.staging);
}
void APIENTRY ResourceUpdateSubResourceUP(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr,UINT sub,const D3D10_DDI_BOX *box,const void *data,UINT pitch,UINT slice) {
 Device *d=dev(h);Resource *r=res(hr);if(!data)return;

 if(r->storage.buffer) {
   UINT x=box?box->left:0,size=box?box->right-box->left:r->width;
   if(x>r->width || size>r->width-x){error(d,E_INVALIDARG);return;}
   if(!size)return;
   if(!x && size==r->width){if(!buffer_rename(d,r))return;}
   else if((r->storage.used>d->completed) && !((x|size)&3) && size<=65536) {
     barrier(d);r->storage.used=d->next_submit;d->vk.CmdUpdateBuffer(d->cmd,r->storage.buffer,x,size,data);return;
   } else if(r->storage.used>d->completed){if(!flush(d,true))return;}
   memcpy((char *)r->storage.map+x,data,size);sync_memory(d,r->storage,false);return;
 }
 UINT level=sub%r->levels,layer=sub/r->levels;
 UINT x=box?box->left:0,y=box?box->top:0,z=box?box->front:0;
 UINT w=box?box->right-x:std::max(1u,r->width>>level),height=box?box->bottom-y:std::max(1u,r->height>>level),depth=box?box->back-z:std::max(1u,r->depth>>level);
 UINT row=w*pixel_bytes(r->format);if(!pitch)pitch=row;if(!slice)slice=pitch*height;
 Storage staging;if(!buffer_create(d,staging,(VkDeviceSize)row*height*depth,0))return;
 for(UINT iz=0;iz<depth;iz++)for(UINT iy=0;iy<height;iy++)memcpy((char *)staging.map+((size_t)iz*height+iy)*row,(const char *)data+(size_t)iz*slice+(size_t)iy*pitch,row);
 if(!sync_memory(d,staging,false)){storage_destroy(d,staging);return;}
 barrier(d);VkBufferImageCopy copy{};copy.imageSubresource={aspects(r->format),level,layer,1};copy.imageOffset={(INT)x,(INT)y,(INT)z};copy.imageExtent={w,height,depth};r->storage.used=d->next_submit;d->vk.CmdCopyBufferToImage(d->cmd,staging.buffer,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);d->temporary.push_back(staging);
}
void APIENTRY ResourceCopyRegion(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hd,UINT ds,UINT x,UINT y,UINT z,D3D10DDI_HRESOURCE hs,UINT ss,const D3D10_DDI_BOX *box) {
 Device *d=dev(h);Resource *dst=res(hd),*src=res(hs);barrier(d);dst->storage.used=src->storage.used=d->next_submit;

 if(dst->storage.buffer && src->storage.buffer) {
   VkBufferCopy copy{box?(VkDeviceSize)box->left:0,x,box?(VkDeviceSize)(box->right-box->left):src->width};d->vk.CmdCopyBuffer(d->cmd,src->storage.buffer,dst->storage.buffer,1,&copy);
 } else if(dst->storage.image && src->storage.image) {
   UINT level=ss%src->levels,sx=box?box->left:0,sy=box?box->top:0,sz=box?box->front:0;
   VkImageCopy copy{};copy.srcSubresource={aspects(src->format),level,ss/src->levels,1};copy.dstSubresource={aspects(dst->format),ds%dst->levels,ds/dst->levels,1};copy.srcOffset={(INT)sx,(INT)sy,(INT)sz};copy.dstOffset={(INT)x,(INT)y,(INT)z};
   copy.extent={box?box->right-sx:std::max(1u,src->width>>level),box?box->bottom-sy:std::max(1u,src->height>>level),box?box->back-sz:std::max(1u,src->depth>>level)};
   d->vk.CmdCopyImage(d->cmd,src->storage.image,VK_IMAGE_LAYOUT_GENERAL,dst->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
 } else {error(d,DXGI_ERROR_UNSUPPORTED);return;}

}
void APIENTRY ResourceCopy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,D3D10DDI_HRESOURCE src) {
 Resource *r=res(src);for(UINT i=0;i<r->levels*r->layers;i++)ResourceCopyRegion(h,dst,i,0,0,0,src,i,nullptr);
}
BOOL APIENTRY ResourceIsStagingBusy(D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE){return FALSE;}
void APIENTRY ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE){barrier(dev(h));}
void APIENTRY ShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE h,D3D10DDI_HSHADERRESOURCEVIEW,D3D10DDI_HRESOURCE){barrier(dev(h));}
void APIENTRY ResourceResolveSubResource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT ds,D3D10DDI_HRESOURCE src,UINT ss,DXGI_FORMAT){ResourceCopyRegion(h,dst,ds,0,0,0,src,ss,nullptr);}

SIZE_T APIENTRY CalcPrivateRenderTargetViewSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATERENDERTARGETVIEW *){return sizeof(View);}
SIZE_T APIENTRY CalcPrivateDepthStencilViewSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *){return sizeof(View);}
SIZE_T APIENTRY CalcPrivateShaderResourceViewSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATESHADERRESOURCEVIEW *){return sizeof(View);}
void APIENTRY CreateRenderTargetView(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATERENDERTARGETVIEW *p,D3D10DDI_HRENDERTARGETVIEW hv,D3D10DDI_HRTRENDERTARGETVIEW) {
 auto *v=new(hv.pDrvPrivate) View{};v->resource=res(p->hDrvResource);v->format=p->Format;
 if(p->ResourceDimension==D3D10DDIRESOURCE_BUFFER){v->first=p->Buffer.FirstElement;v->count=p->Buffer.NumElements;}
 else {v->level=p->Tex2D.MipSlice;v->layer=p->Tex2D.FirstArraySlice;v->layers=std::max(1u,p->Tex2D.ArraySize);}view_create(dev(h),v);
}
void APIENTRY CreateDepthStencilView(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEDEPTHSTENCILVIEW *p,D3D10DDI_HDEPTHSTENCILVIEW hv,D3D10DDI_HRTDEPTHSTENCILVIEW) {
 auto *v=new(hv.pDrvPrivate) View{};v->resource=res(p->hDrvResource);v->format=p->Format;v->level=p->Tex2D.MipSlice;v->layer=p->Tex2D.FirstArraySlice;v->layers=std::max(1u,p->Tex2D.ArraySize);view_create(dev(h),v);
}
void APIENTRY CreateShaderResourceView(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATESHADERRESOURCEVIEW *p,D3D10DDI_HSHADERRESOURCEVIEW hv,D3D10DDI_HRTSHADERRESOURCEVIEW) {
 auto *v=new(hv.pDrvPrivate) View{};v->resource=res(p->hDrvResource);v->format=p->Format;
 if(p->ResourceDimension==D3D10DDIRESOURCE_BUFFER){v->first=p->Buffer.FirstElement;v->count=p->Buffer.NumElements;}
 else {v->level=p->Tex2D.MostDetailedMip;v->levels=p->Tex2D.MipLevels;v->layer=p->Tex2D.FirstArraySlice;v->layers=std::max(1u,p->Tex2D.ArraySize);if(v->levels==~0u)v->levels=v->resource->levels-v->level;}
 view_create(dev(h),v,true);
}
void APIENTRY DestroyRenderTargetView(D3D10DDI_HDEVICE h,D3D10DDI_HRENDERTARGETVIEW hv){Device *d=dev(h);View *v=object<View>(hv);view_destroy(d,v);v->~View();}
void APIENTRY DestroyDepthStencilView(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILVIEW hv){Device *d=dev(h);View *v=object<View>(hv);view_destroy(d,v);v->~View();}
void APIENTRY DestroyShaderResourceView(D3D10DDI_HDEVICE h,D3D10DDI_HSHADERRESOURCEVIEW hv){Device *d=dev(h);View *v=object<View>(hv);view_destroy(d,v);v->~View();}
void APIENTRY ClearRenderTargetView(D3D10DDI_HDEVICE h,D3D10DDI_HRENDERTARGETVIEW hv,FLOAT color[4]) {
 Device *d=dev(h);View *v=object<View>(hv);barrier(d);VkClearColorValue c;memcpy(c.float32,color,sizeof(c.float32));VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,v->level,1,v->layer,v->layers};

 v->resource->storage.used=d->next_submit;d->vk.CmdClearColorImage(d->cmd,v->resource->storage.image,VK_IMAGE_LAYOUT_GENERAL,&c,1,&range);
}
void APIENTRY ClearDepthStencilView(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILVIEW hv,UINT flags,FLOAT depth,UINT8 stencil) {
 Device *d=dev(h);View *v=object<View>(hv);barrier(d);VkClearDepthStencilValue value{depth,stencil};VkImageSubresourceRange range{0,v->level,1,v->layer,v->layers};if(flags&D3D10_DDI_CLEAR_DEPTH)range.aspectMask|=VK_IMAGE_ASPECT_DEPTH_BIT;if(flags&D3D10_DDI_CLEAR_STENCIL)range.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;
 v->resource->storage.used=d->next_submit;d->vk.CmdClearDepthStencilImage(d->cmd,v->resource->storage.image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
}
