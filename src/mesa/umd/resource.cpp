#include "umd.h"

static bool imported_image(Device *d,Resource *r) {
 A830_ALLOCATION_ADDRESS a={A830_ABI_VERSION,r->allocation,0,0,0,0,0};D3DDDICB_ESCAPE e{};e.hDevice=d->runtime;e.hContext=d->context;e.pPrivateDriverData=&a;e.PrivateDriverDataSize=sizeof(a);
 HRESULT hr=d->callbacks.pfnEscapeCb(d->adapter,&e);if(FAILED(hr)){error(d,hr);return false;}
 r->pitch=a.Pitch;return image_create(d,r,a.Address,a.Size);
}
static UINT image_copies(Resource *r,UINT sub,UINT x,UINT y,UINT z,UINT width,UINT height,UINT depth,VkBufferImageCopy (&copies)[2]) {
 copies[0].imageSubresource={aspects(r->format),sub%r->levels,sub/r->levels,1};copies[0].imageOffset={(INT)x,(INT)y,(INT)z};copies[0].imageExtent={width,height,depth};
 if(r->format!=DXGI_FORMAT_NV12)return 1;
 copies[0].imageSubresource.aspectMask=VK_IMAGE_ASPECT_PLANE_0_BIT;copies[1]=copies[0];
 copies[1].bufferOffset=(VkDeviceSize)width*height;copies[1].imageSubresource.aspectMask=VK_IMAGE_ASPECT_PLANE_1_BIT;
 copies[1].imageOffset.x/=2;copies[1].imageOffset.y/=2;copies[1].imageExtent.width/=2;copies[1].imageExtent.height/=2;return 2;
}
SIZE_T APIENTRY CalcPrivateResourceSize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATERESOURCE *) {return sizeof(Resource);}
SIZE_T APIENTRY CalcPrivateOpenedResourceSize(D3D10DDI_HDEVICE,const D3D10DDIARG_OPENRESOURCE *) {return sizeof(Resource);}
void APIENTRY CreateResource(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATERESOURCE *p,D3D10DDI_HRESOURCE hr,D3D10DDI_HRTRESOURCE rt) {
 Device *d=dev(h);auto *r=new(hr.pDrvPrivate) Resource{};r->format=p->Format;r->vkformat=format(p->Format);r->dimension=p->ResourceDimension;r->bind=p->BindFlags;r->runtime=rt.handle;
 r->levels=std::max(1u,p->MipLevels);r->layers=std::max(1u,p->ArraySize);r->width=p->pMipInfoList[0].TexelWidth;r->height=p->pMipInfoList[0].TexelHeight;r->depth=p->pMipInfoList[0].TexelDepth;
 if(p->pPrimaryDesc)p->pPrimaryDesc->DriverFlags=0;
 if(!r->width){error(d,E_INVALIDARG);return;}
 if(r->format==DXGI_FORMAT_NV12 && (((r->width|r->height)&1) || r->dimension!=D3D10DDIRESOURCE_TEXTURE2D || r->levels!=1 || r->depth>1)){error(d,E_INVALIDARG);return;}
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
     unsigned native=r->format==DXGI_FORMAT_A8_UNORM?D3DDDIFMT_A8:r->format==DXGI_FORMAT_NV12?A830_FORMAT_NV12:r->format==DXGI_FORMAT_B8G8R8A8_UNORM?D3DDDIFMT_A8R8G8B8:
       r->format==DXGI_FORMAT_R8G8B8A8_UNORM?D3DDDIFMT_A8B8G8R8:
       r->format==DXGI_FORMAT_R10G10B10A2_UNORM?D3DDDIFMT_A2B10G10R10:
       r->format==DXGI_FORMAT_R16G16B16A16_FLOAT?D3DDDIFMT_A16B16G16R16F:0;
     unsigned bytes=A830PixelBytes(native);
     if(!bytes || r->dimension!=D3D10DDIRESOURCE_TEXTURE2D || r->layers*r->levels!=1 || r->width>(UINT_MAX-255)/bytes){error(d,DXGI_ERROR_UNSUPPORTED);return;}
     A830_RESOURCE data={A830_ABI_VERSION,0,r->width,r->height,(r->width*bytes+255)&~255u,native,A830_RESOURCE_OWNED|(p->pPrimaryDesc?A830_RESOURCE_PRIMARY:0u)};
     data.Rotation=p->pPrimaryDesc?(unsigned)p->pPrimaryDesc->ModeDesc.Rotation:(unsigned)D3DDDI_ROTATION_IDENTITY;
     UINT rows=data.Height+(r->format==DXGI_FORMAT_NV12?data.Height/2:0);
     if(rows>0x4000000u/data.Pitch){error(d,E_OUTOFMEMORY);return;}data.Size=data.Pitch*rows;
     if((r->format==DXGI_FORMAT_A8_UNORM || r->format==DXGI_FORMAT_NV12) && !image_layout(d,r,&data.Pitch,&data.Size))return;
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
 r->format=a->Format==D3DDDIFMT_A8?DXGI_FORMAT_A8_UNORM:a->Format==A830_FORMAT_NV12?DXGI_FORMAT_NV12:a->Format==D3DDDIFMT_A8R8G8B8 || a->Format==D3DDDIFMT_X8R8G8B8?DXGI_FORMAT_B8G8R8A8_UNORM:
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
 if(r->storage.external)d->shared_access.erase(r->storage.address);
 if(d->recording){d->temporary.push_back(r->mapping.staging);d->temporary.push_back(r->storage);r->mapping.staging={};r->storage={};}
 else {storage_recycle(d,r->mapping.staging);storage_recycle(d,r->storage);}
 if(r->allocation && !r->imported){D3DDDICB_DEALLOCATE a{};a.hResource=r->runtime;d->callbacks.pfnDeallocateCb(d->runtime,&a);}
 r->~Resource();
}
void APIENTRY ResourceMap(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr,UINT sub,D3D10_DDI_MAP mode,UINT flags,D3D10DDI_MAPPED_SUBRESOURCE *out) {
 Device *d=dev(h);Resource *r=res(hr);memset(out,0,sizeof(*out));bool wait=!(flags&D3D10_DDI_MAP_FLAG_DONOTWAIT);
 if(r->storage.buffer) {
   if(mode==D3D10_DDI_MAP_WRITE_DISCARD){if(!buffer_rename(d,r))return;}
   else if(mode!=D3D10_DDI_MAP_WRITE_NOOVERWRITE && !wait_for(d,r->storage.used,wait))return;
   if((mode==D3D10_DDI_MAP_READ || mode==D3D10_DDI_MAP_READWRITE) && !sync_memory(d,r->storage,true))return;
   out->pData=r->storage.map;out->RowPitch=out->DepthPitch=r->width;r->mapping.mode=mode;return;
 }
 if(sub>=r->levels*r->layers){error(d,E_INVALIDARG);return;}
 Mapping &m=r->mapping;
 if(m.pending) {
   if(!wait_for(d,m.pending,wait))return;
   if(m.sub==sub && m.mode==(UINT)mode && r->storage.used==m.pending) {
     if(!sync_memory(d,m.staging,true))return;m.pending=0;out->pData=m.staging.map;out->RowPitch=m.row;out->DepthPitch=m.slice;return;
   }
   m.pending=0;
 }
 bool read=mode==D3D10_DDI_MAP_READ || mode==D3D10_DDI_MAP_READWRITE;
 if(read && !wait && !wait_for(d,r->storage.used,false))return;
 UINT level=sub%r->levels,width=std::max(1u,r->width>>level),height=std::max(1u,r->height>>level),depth=std::max(1u,r->depth>>level);
 buffer_recycle(d,m.staging);m.sub=sub;m.mode=mode;m.row=width*pixel_bytes(r->format);m.slice=m.row*(height+(r->format==DXGI_FORMAT_NV12?height/2:0));
 if(!buffer_create(d,m.staging,(VkDeviceSize)m.slice*depth,0))return;
 if(read) {
   access(d,r->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,false,level,1,sub/r->levels,1);access(d,m.staging,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);VkBufferImageCopy copies[2]{};UINT count=image_copies(r,sub,0,0,0,width,height,depth,copies);
   d->vk.CmdCopyImageToBuffer(d->cmd,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,m.staging.buffer,count,copies);m.pending=d->next_submit;
   if(!wait_for(d,m.pending,wait) || !sync_memory(d,m.staging,true))return;m.pending=0;
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
   UINT level=m.sub%r->levels;access(d,m.staging,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);access(d,r->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,false,level,1,m.sub/r->levels,1);VkBufferImageCopy copies[2]{};UINT count=image_copies(r,m.sub,0,0,0,std::max(1u,r->width>>level),std::max(1u,r->height>>level),std::max(1u,r->depth>>level),copies);
   d->vk.CmdCopyBufferToImage(d->cmd,m.staging.buffer,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,count,copies);d->temporary.push_back(m.staging);m.staging={};
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
     access(d,r->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);d->vk.CmdUpdateBuffer(d->cmd,r->storage.buffer,x,size,data);return;
   } else if(r->storage.used>d->completed) {
     if(!((x|size)&3)) {
       VkDeviceSize offset;Storage *staging=upload_space(d,size,offset);if(!staging)return;
       memcpy((char *)staging->map+offset,data,size);if(!sync_memory(d,*staging,false,offset,size))return;
       access(d,*staging,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);access(d,r->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
       VkBufferCopy copy{offset,x,size};d->vk.CmdCopyBuffer(d->cmd,staging->buffer,r->storage.buffer,1,&copy);return;
     }
     if(!wait_for(d,r->storage.used,true))return;
   }
   memcpy((char *)r->storage.map+x,data,size);sync_memory(d,r->storage,false);return;
 }
 UINT level=sub%r->levels;
 UINT x=box?box->left:0,y=box?box->top:0,z=box?box->front:0;
 UINT w=box?box->right-x:std::max(1u,r->width>>level),height=box?box->bottom-y:std::max(1u,r->height>>level),depth=box?box->back-z:std::max(1u,r->depth>>level);
 if(sub>=r->levels*r->layers || x>r->width || w>r->width-x || y>r->height || height>r->height-y || z>r->depth || depth>r->depth-z ||
    (r->format==DXGI_FORMAT_NV12 && ((x|y|w|height)&1))){error(d,E_INVALIDARG);return;}
 UINT rows=height+(r->format==DXGI_FORMAT_NV12?height/2:0);
 UINT row=w*pixel_bytes(r->format);if(!pitch)pitch=row;if(!slice)slice=pitch*rows;
 if(pitch<row){error(d,E_INVALIDARG);return;}
 VkDeviceSize size=(VkDeviceSize)row*rows*depth,offset;Storage *staging=upload_space(d,size,offset);if(!staging)return;
 char *mapped=(char *)staging->map+offset;
 if(pitch==row && (depth==1 || slice==row*rows))memcpy(mapped,data,(size_t)size);
 else for(UINT iz=0;iz<depth;iz++)for(UINT iy=0;iy<rows;iy++)memcpy(mapped+((size_t)iz*rows+iy)*row,(const char *)data+(size_t)iz*slice+(size_t)iy*pitch,row);
 if(!sync_memory(d,*staging,false,offset,size))return;
 access(d,*staging,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);access(d,r->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,false,level,1,sub/r->levels,1);
 VkBufferImageCopy copies[2]{};UINT count=image_copies(r,sub,x,y,z,w,height,depth,copies);for(UINT i=0;i<count;i++)copies[i].bufferOffset+=offset;
 d->vk.CmdCopyBufferToImage(d->cmd,staging->buffer,r->storage.image,VK_IMAGE_LAYOUT_GENERAL,count,copies);
}
void APIENTRY ResourceCopyRegion(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hd,UINT ds,UINT x,UINT y,UINT z,D3D10DDI_HRESOURCE hs,UINT ss,const D3D10_DDI_BOX *box) {
 Device *d=dev(h);Resource *dst=res(hd),*src=res(hs);
 capture_present_image(d,src);
 bool same=dst==src || (src->storage.external && src->storage.address==dst->storage.address);
 if(same && ds==ss && x==(box?box->left:0u) && y==(box?box->top:0u) && z==(box?box->front:0u))return;
 access(d,src->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT,false,ss%src->levels,1,ss/src->levels,1);access(d,dst->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,false,ds%dst->levels,1,ds/dst->levels,1);
 if(dst->storage.buffer && src->storage.buffer) {
   VkBufferCopy copy{box?(VkDeviceSize)box->left:0,x,box?(VkDeviceSize)(box->right-box->left):src->width};d->vk.CmdCopyBuffer(d->cmd,src->storage.buffer,dst->storage.buffer,1,&copy);
 } else if(dst->storage.image && src->storage.image) {
   UINT level=ss%src->levels,sx=box?box->left:0,sy=box?box->top:0,sz=box?box->front:0;
   VkImageCopy copy{};copy.srcSubresource={aspects(src->format),level,ss/src->levels,1};copy.dstSubresource={aspects(dst->format),ds%dst->levels,ds/dst->levels,1};copy.srcOffset={(INT)sx,(INT)sy,(INT)sz};copy.dstOffset={(INT)x,(INT)y,(INT)z};
   copy.extent={box?box->right-sx:std::max(1u,src->width>>level),box?box->bottom-sy:std::max(1u,src->height>>level),box?box->back-sz:std::max(1u,src->depth>>level)};
   if(src->format==DXGI_FORMAT_NV12 || dst->format==DXGI_FORMAT_NV12) {
     if(src->format!=dst->format || ((sx|sy|x|y|copy.extent.width|copy.extent.height)&1)){error(d,E_INVALIDARG);return;}
     copy.srcSubresource.aspectMask=copy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_PLANE_0_BIT;
     d->vk.CmdCopyImage(d->cmd,src->storage.image,VK_IMAGE_LAYOUT_GENERAL,dst->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
     copy.srcSubresource.aspectMask=copy.dstSubresource.aspectMask=VK_IMAGE_ASPECT_PLANE_1_BIT;
     copy.srcOffset.x/=2;copy.srcOffset.y/=2;copy.dstOffset.x/=2;copy.dstOffset.y/=2;copy.extent.width/=2;copy.extent.height/=2;
   }
   d->vk.CmdCopyImage(d->cmd,src->storage.image,VK_IMAGE_LAYOUT_GENERAL,dst->storage.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
 } else {error(d,DXGI_ERROR_UNSUPPORTED);return;}

}
void APIENTRY ResourceCopy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,D3D10DDI_HRESOURCE src) {
 Resource *r=res(src);for(UINT i=0;i<r->levels*r->layers;i++)ResourceCopyRegion(h,dst,i,0,0,0,src,i,nullptr);
}
BOOL APIENTRY ResourceIsStagingBusy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr){Resource *r=res(hr);return !ready(dev(h),std::max(r->storage.used,r->mapping.pending));}
void APIENTRY ResourceReadAfterWriteHazard(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE hr){Resource *r=res(hr);if(r)access(dev(h),r->storage,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,r->storage.image?VK_ACCESS_SHADER_READ_BIT:VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT);}
void APIENTRY ShaderResourceViewReadAfterWriteHazard(D3D10DDI_HDEVICE h,D3D10DDI_HSHADERRESOURCEVIEW hv,D3D10DDI_HRESOURCE hr){View *v=object<View>(hv);Resource *r=res(hr);if(v && r)access(dev(h),r->storage,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,false,v->level,v->levels,v->layer,v->layers);}
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
bool clear_attachment(Device *d,View *v,const VkClearAttachment &value) {
 if(!d->render_pass || v->layers!=1)return false;
 VkClearAttachment clear=value;bool found=false;
 if(clear.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT) {
   UINT position=0;
   for(UINT i=0;i<d->pass_targets;i++)if(d->pass_formats[i]!=VK_FORMAT_UNDEFINED) {
     if(d->pass_views[position++]==v->image){clear.colorAttachment=i;found=true;break;}
   }
 } else found=d->pass_formats[8]!=VK_FORMAT_UNDEFINED && d->pass_views.back()==v->image;
 if(!found)return false;
 VkClearRect rect{{{0,0},{std::max(1u,v->resource->width>>v->level),std::max(1u,v->resource->height>>v->level)}},0,1};
 d->vk.CmdClearAttachments(d->cmd,1,&clear,1,&rect);v->resource->storage.discarded=0;return true;
}
void APIENTRY ClearRenderTargetView(D3D10DDI_HDEVICE h,D3D10DDI_HRENDERTARGETVIEW hv,FLOAT color[4]) {
 Device *d=dev(h);View *v=object<View>(hv);if(!view_create(d,v))return;VkClearAttachment clear{};clear.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;memcpy(clear.clearValue.color.float32,color,16);if(clear_attachment(d,v,clear))return;access(d,v->resource->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,false,v->level,1,v->layer,v->layers);VkClearColorValue c;memcpy(c.float32,color,sizeof(c.float32));VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,v->level,1,v->layer,v->layers};
 d->vk.CmdClearColorImage(d->cmd,v->resource->storage.image,VK_IMAGE_LAYOUT_GENERAL,&c,1,&range);
}
void APIENTRY ClearDepthStencilView(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILVIEW hv,UINT flags,FLOAT depth,UINT8 stencil) {
 Device *d=dev(h);View *v=object<View>(hv);if(!view_create(d,v))return;VkClearAttachment clear{};clear.aspectMask=(flags&D3D10_DDI_CLEAR_DEPTH?VK_IMAGE_ASPECT_DEPTH_BIT:0u)|(flags&D3D10_DDI_CLEAR_STENCIL?VK_IMAGE_ASPECT_STENCIL_BIT:0u);clear.clearValue.depthStencil={depth,stencil};if(clear_attachment(d,v,clear))return;access(d,v->resource->storage,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,false,v->level,1,v->layer,v->layers);VkClearDepthStencilValue value{depth,stencil};VkImageSubresourceRange range{0,v->level,1,v->layer,v->layers};if(flags&D3D10_DDI_CLEAR_DEPTH)range.aspectMask|=VK_IMAGE_ASPECT_DEPTH_BIT;if(flags&D3D10_DDI_CLEAR_STENCIL)range.aspectMask|=VK_IMAGE_ASPECT_STENCIL_BIT;
 d->vk.CmdClearDepthStencilImage(d->cmd,v->resource->storage.image,VK_IMAGE_LAYOUT_GENERAL,&value,1,&range);
}
