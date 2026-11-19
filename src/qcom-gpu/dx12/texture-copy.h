#pragma once
struct CopyView {Resource *resource;UINT level,layer,width,height,depth,pitch,slice;UINT64 offset;DXGI_FORMAT format;bool buffer;};
bool copy_view(const D3D12DDIARG_BUFFER_PLACEMENT *placement,D3D12DDIARG_PLACED_RESOURCE layout,CopyView &v) {
    if(!placement)return false;v={};v.resource=object<Resource>(placement->BaseAddress.UMD.hResource);if(!v.resource)return false;
    Resource *r=v.resource;UINT64 offset=placement->BaseAddress.UMD.Offset;v.buffer=r->pipe->target==PIPE_BUFFER;
    if(layout.Layout==D3D12DDI_RL_SELECT_SUBRESOURCE) {
        if(v.buffer || offset>=static_cast<UINT64>(r->desc.MipLevels)*r->layout.Layers)return false;
        v.level=static_cast<UINT>(offset%r->desc.MipLevels);v.layer=static_cast<UINT>(offset/r->desc.MipLevels);
        v.width=std::max(1u,r->pipe->width0>>v.level);v.height=std::max(1u,r->pipe->height0>>v.level);
        v.depth=r->pipe->target==PIPE_TEXTURE_3D?std::max(1u,static_cast<UINT>(r->pipe->depth0)>>v.level):1;v.format=r->desc.Format;return true;
    }
    if(!v.buffer || !layout.pLayout)return false;UINT block=1;
    if(layout.Layout==D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED) {
        const auto &p=*static_cast<const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT *>(layout.pLayout);
        v.format=p.Format;v.width=p.VirtualWidth;v.height=p.VirtualHeight;v.depth=p.VirtualDepth;v.pitch=p.Pitch;v.slice=p.SlicePitch;
    } else if(layout.Layout==D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED) {
        const auto &p=*static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT *>(layout.pLayout);
        v.format=p.Format;block=(v.format>=DXGI_FORMAT_BC1_TYPELESS && v.format<=DXGI_FORMAT_BC5_SNORM) ||
            (v.format>=DXGI_FORMAT_BC6H_TYPELESS && v.format<=DXGI_FORMAT_BC7_UNORM_SRGB)?4:1;
        v.width=p.PhysicalWidth*block;v.height=p.PhysicalHeight*block;v.depth=p.PhysicalDepth;v.pitch=p.Pitch;v.slice=p.SlicePitch;
    } else return false;
    v.offset=offset;
    return v.width && v.height && v.depth && v.pitch && v.slice && offset<=r->size &&
        static_cast<UINT64>(v.slice)*v.depth<=r->size-offset;
}
void APIENTRY copy_texture(D3D12DDI_HCOMMANDLIST h,const D3D12DDIARG_BUFFER_PLACEMENT *dst,D3D12DDIARG_PLACED_RESOURCE dst_layout,UINT x,UINT y,UINT z,
    const D3D12DDIARG_BUFFER_PLACEMENT *src,D3D12DDIARG_PLACED_RESOURCE src_layout,const D3D12DDI_BOX *region) {
    List *l=object<List>(h);CopyView s{},d{};if(!copy_view(src,src_layout,s) || !copy_view(dst,dst_layout,d) ||
        s.resource->heap->device!=l->device || d.resource->heap->device!=l->device){error(l,E_INVALIDARG);return;}
    D3D12DDI_BOX r=region?*region:D3D12DDI_BOX{0,0,0,static_cast<LONG>(s.width),static_cast<LONG>(s.height),static_cast<LONG>(s.depth)};
    if(r.Left<0 || r.Top<0 || r.Front<0 || r.Right<r.Left || r.Bottom<r.Top || r.Back<r.Front ||
       static_cast<UINT>(r.Right)>s.width || static_cast<UINT>(r.Bottom)>s.height || static_cast<UINT>(r.Back)>s.depth ||
       x>d.width || y>d.height || z>d.depth || static_cast<UINT>(r.Right-r.Left)>d.width-x ||
       static_cast<UINT>(r.Bottom-r.Top)>d.height-y || static_cast<UINT>(r.Back-r.Front)>d.depth-z){error(l,E_INVALIDARG);return;}
    record(l,[=](Queue *q) {
        UINT width=r.Right-r.Left,height=r.Bottom-r.Top,depth=r.Back-r.Front;if(!width || !height || !depth)return S_OK;
        pipe_box sb{};sb.x=r.Left;sb.y=static_cast<int16_t>(r.Top);sb.z=static_cast<int16_t>(r.Front+s.layer);sb.width=width;sb.height=static_cast<int16_t>(height);sb.depth=static_cast<int16_t>(depth);
        if(!s.buffer && !d.buffer){q->draw->resource_copy_region(q->draw,d.resource->pipe,d.level,x,y,z+d.layer,s.resource->pipe,s.level,&sb);return S_OK;}
        UINT block=(s.format>=DXGI_FORMAT_BC1_TYPELESS && s.format<=DXGI_FORMAT_BC5_SNORM) ||
            (s.format>=DXGI_FORMAT_BC6H_TYPELESS && s.format<=DXGI_FORMAT_BC7_UNORM_SRGB)?4:1;
        UINT element=format_bytes(s.format),rows=(height+block-1)/block,row_bytes=((width+block-1)/block)*element;
        if(!element || format_bytes(d.format)!=element || (r.Left%block) || (r.Top%block) || x%block || y%block)return E_INVALIDARG;
        pipe_box db{};db.x=x;db.y=static_cast<int16_t>(y);db.z=static_cast<int16_t>(z+d.layer);db.width=width;db.height=static_cast<int16_t>(height);db.depth=static_cast<int16_t>(depth);
        pipe_transfer *sm=nullptr,*dm=nullptr;void *sp,*dp;
        UINT64 source_offset=s.offset+static_cast<UINT64>(r.Front)*s.slice+static_cast<UINT64>(r.Top/block)*s.pitch+static_cast<UINT64>(r.Left/block)*element;
        UINT64 destination_offset=d.offset+static_cast<UINT64>(z)*d.slice+static_cast<UINT64>(y/block)*d.pitch+static_cast<UINT64>(x/block)*element;
        UINT64 source_bytes=s.buffer?static_cast<UINT64>(depth-1)*s.slice+static_cast<UINT64>(rows-1)*s.pitch+row_bytes:0;
        UINT64 destination_bytes=d.buffer?static_cast<UINT64>(depth-1)*d.slice+static_cast<UINT64>(rows-1)*d.pitch+row_bytes:0;
        if((s.buffer && (source_offset>s.resource->size || source_bytes>s.resource->size-source_offset)) ||
           (d.buffer && (destination_offset>d.resource->size || destination_bytes>d.resource->size-destination_offset)))return E_INVALIDARG;
        if(s.buffer){sb={};sb.x=static_cast<int>(source_offset);sb.width=static_cast<int>(source_bytes);sb.height=sb.depth=1;sp=q->draw->buffer_map(q->draw,s.resource->pipe,0,PIPE_MAP_READ,&sb,&sm);}
        else sp=q->draw->texture_map(q->draw,s.resource->pipe,s.level,PIPE_MAP_READ,&sb,&sm);
        if(!sp)return E_OUTOFMEMORY;
        if(d.buffer){db={};db.x=static_cast<int>(destination_offset);db.width=static_cast<int>(destination_bytes);db.height=db.depth=1;dp=q->draw->buffer_map(q->draw,d.resource->pipe,0,PIPE_MAP_WRITE,&db,&dm);}
        else dp=q->draw->texture_map(q->draw,d.resource->pipe,d.level,PIPE_MAP_WRITE,&db,&dm);
        if(dp)for(UINT i=0;i<depth;i++)for(UINT j=0;j<rows;j++)memcpy(static_cast<BYTE *>(dp)+static_cast<size_t>(i)*(d.buffer?d.slice:dm->layer_stride)+static_cast<size_t>(j)*(d.buffer?d.pitch:dm->stride),
            static_cast<const BYTE *>(sp)+static_cast<size_t>(i)*(s.buffer?s.slice:sm->layer_stride)+static_cast<size_t>(j)*(s.buffer?s.pitch:sm->stride),row_bytes);
        if(dp){if(d.buffer)q->draw->buffer_unmap(q->draw,dm);else q->draw->texture_unmap(q->draw,dm);}
        if(s.buffer)q->draw->buffer_unmap(q->draw,sm);else q->draw->texture_unmap(q->draw,sm);return dp?S_OK:E_OUTOFMEMORY;
    });
}
void APIENTRY resolve_texture(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HRESOURCE dst,UINT ds,D3D12DDI_HRESOURCE src,UINT ss,DXGI_FORMAT f) {
    List *l=object<List>(h);Resource *d=object<Resource>(dst),*s=object<Resource>(src);
    if(!d || !s || ds>=d->desc.MipLevels*d->layout.Layers || ss>=s->desc.MipLevels*s->layout.Layers){error(l,E_INVALIDARG);return;}
    record(l,[=](Queue *q){pipe_blit_info blit{};blit.src.resource=s->pipe;blit.dst.resource=d->pipe;blit.src.level=ss%s->desc.MipLevels;blit.dst.level=ds%d->desc.MipLevels;
        blit.src.format=blit.dst.format=format(f);blit.src.box.z=static_cast<int16_t>(ss/s->desc.MipLevels);blit.dst.box.z=static_cast<int16_t>(ds/d->desc.MipLevels);
        blit.src.box.width=blit.dst.box.width=std::max(1u,s->pipe->width0>>blit.src.level);blit.src.box.height=blit.dst.box.height=static_cast<int16_t>(std::max(1u,s->pipe->height0>>blit.src.level));
        blit.src.box.depth=blit.dst.box.depth=1;blit.mask=PIPE_MASK_RGBA;blit.filter=PIPE_TEX_FILTER_NEAREST;q->draw->blit(q->draw,&blit);return S_OK;});
}
