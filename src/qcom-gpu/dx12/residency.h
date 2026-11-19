#pragma once
HRESULT object_allocations(Device *d,UINT count,const D3D12DDI_HANDLE_AND_TYPE *objects,std::vector<UINT> &handles,std::vector<UINT> *indices=nullptr) {
    if(count && !objects)return E_INVALIDARG;
    try {for(UINT i=0;i<count;i++) {
        if(!objects[i].Handle)return E_INVALIDARG;
        if(objects[i].Type==D3D12DDI_HT_HEAP) {
            Heap *h=static_cast<Heap *>(objects[i].Handle);if(h->device!=d || !h->memory.Handle)return E_INVALIDARG;
            handles.push_back(h->memory.Handle);if(indices)indices->push_back(i);
        } else if(objects[i].Type!=D3D12DDI_HT_PIPELINE_STATE && objects[i].Type!=D3D12DDI_HT_DESCRIPTOR_HEAP &&
            objects[i].Type!=D3D12DDI_HT_QUERY_HEAP && objects[i].Type!=D3D12DDI_HT_COMMAND_SIGNATURE)return E_INVALIDARG;
    }}catch(const std::bad_alloc &){return E_OUTOFMEMORY;}return S_OK;
}
HRESULT APIENTRY make_resident(D3D12DDI_HDEVICE hd,D3D12DDIARG_MAKERESIDENT_0001 *p) {
    Device *d=object<Device>(hd);if(!p || p->NumAdapters!=1 || !p->pRTPagingQueue || !p->pPagingFenceValue)return E_INVALIDARG;
    std::vector<UINT> handles;HRESULT hr=object_allocations(d,p->NumObjects,p->pObjects,handles);p->WaitMask=0;p->pPagingFenceValue[0]=0;
    if(FAILED(hr) || handles.empty())return hr;
    D3DDDI_MAKERESIDENT request{};request.NumAllocations=static_cast<UINT>(handles.size());request.AllocationList=handles.data();request.Flags=p->Flags;
    hr=d->core.pfnMakeResidentCb(d->runtime,p->pRTPagingQueue[0],&request);
    if(hr==E_PENDING){p->pPagingFenceValue[0]=request.PagingFenceValue;p->WaitMask=1;}return hr;
}
HRESULT APIENTRY evict(D3D12DDI_HDEVICE hd,const D3D12DDIARG_EVICT *p) {
    Device *d=object<Device>(hd);if(!p)return E_INVALIDARG;std::vector<UINT> handles;HRESULT hr=object_allocations(d,p->NumObjects,p->pObjects,handles);
    if(FAILED(hr) || handles.empty())return hr;
    D3DDDICB_EVICT request{};request.NumAllocations=static_cast<UINT>(handles.size());request.AllocationList=handles.data();request.Flags=p->Flags;
    return d->core.pfnEvictCb(d->runtime,&request);
}
HRESULT APIENTRY offer(D3D12DDI_HDEVICE hd,const D3D12DDIARG_OFFERRESOURCES *p) {
    Device *d=object<Device>(hd);if(!p)return E_INVALIDARG;std::vector<UINT> handles;HRESULT hr=object_allocations(d,p->NumObjects,p->pObjects,handles);
    if(FAILED(hr) || handles.empty())return hr;
    D3D12DDICB_OFFERALLOCATIONS request{static_cast<UINT>(handles.size()),handles.data(),p->Priority};return d->core.pfnOfferAllocationsCb(d->runtime,&request);
}
HRESULT APIENTRY reclaim(D3D12DDI_HDEVICE hd,D3D12DDIARG_RECLAIMRESOURCES_0001 *p) {
    Device *d=object<Device>(hd);if(!p || p->NumAdapters!=1 || !p->pRTPagingQueue || !p->pPagingFenceValue)return E_INVALIDARG;
    std::vector<UINT> handles,indices;HRESULT hr=object_allocations(d,p->NumObjects,p->pObjects,handles,&indices);p->WaitMask=0;p->pPagingFenceValue[0]=0;
    if(p->pDiscarded)memset(p->pDiscarded,0,p->NumObjects*sizeof(BOOL));if(FAILED(hr) || handles.empty())return hr;
    try {
        std::vector<BOOL> discarded(handles.size());D3D12DDICB_RECLAIMALLOCATIONS2 request{static_cast<UINT>(handles.size()),handles.data(),discarded.data(),0};
        hr=d->core.pfnReclaimAllocations2Cb(d->runtime,p->pRTPagingQueue[0],&request);
        if(hr==E_PENDING){p->pPagingFenceValue[0]=request.PagingFenceValue;p->WaitMask=1;}
        if((SUCCEEDED(hr) || hr==E_PENDING) && p->pDiscarded)for(size_t i=0;i<indices.size();i++)p->pDiscarded[indices[i]]=discarded[i];
    }catch(const std::bad_alloc &){hr=E_OUTOFMEMORY;}return hr;
}
