#pragma once
struct QueryHeap {
    struct Entry {pipe_query *query=nullptr;UINT type=UINT_MAX;};
    Device *device;D3D12DDI_QUERY_HEAP_TYPE type;std::vector<Entry> entries;
};
SIZE_T APIENTRY query_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *){return sizeof(QueryHeap);}
HRESULT APIENTRY create_query_heap(D3D12DDI_HDEVICE hd,const D3D12DDIARG_CREATE_QUERY_HEAP_0001 *p,D3D12DDI_HQUERYHEAP h) {
    if(!p || !p->Count || p->NodeMask!=1 || p->Type>D3D12DDI_QUERY_HEAP_TYPE_SO_STATISTICS)return E_INVALIDARG;
    auto *heap=new(h.pDrvPrivate) QueryHeap{object<Device>(hd),p->Type};
    try {heap->entries.resize(p->Count);return S_OK;}catch(const std::bad_alloc &){heap->~QueryHeap();return E_OUTOFMEMORY;}
}
void APIENTRY destroy_query_heap(D3D12DDI_HDEVICE,D3D12DDI_HQUERYHEAP h) {
    auto *heap=object<QueryHeap>(h);for(auto &entry:heap->entries)if(entry.query)heap->device->compiler->destroy_query(heap->device->compiler,entry.query);heap->~QueryHeap();
}
pipe_query *query(Queue *q,QueryHeap *heap,UINT index,D3D12DDI_QUERY_TYPE type) {
    if(!heap || heap->device!=q->device || index>=heap->entries.size() || type>D3D12DDI_QUERY_TYPE_SO_STATISTICS_STREAM3)return nullptr;
    static const UINT types[]={PIPE_QUERY_OCCLUSION_COUNTER,PIPE_QUERY_OCCLUSION_PREDICATE,PIPE_QUERY_TIMESTAMP,PIPE_QUERY_PIPELINE_STATISTICS,
        PIPE_QUERY_SO_STATISTICS,PIPE_QUERY_SO_STATISTICS,PIPE_QUERY_SO_STATISTICS,PIPE_QUERY_SO_STATISTICS};
    auto &entry=heap->entries[index];if(entry.type!=type){if(entry.query)q->draw->destroy_query(q->draw,entry.query);entry.query=q->draw->create_query(q->draw,types[type],type>=4?type-4:0);entry.type=type;}return entry.query;
}
template<bool End> void APIENTRY begin_end_query(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HQUERYHEAP heap,D3D12DDI_QUERY_TYPE type,UINT index) {
    record(object<List>(h),[=](Queue *q){pipe_query *p=query(q,object<QueryHeap>(heap),index,type);if(!p)return DXGI_ERROR_UNSUPPORTED;
        if(End)return q->draw->end_query(q->draw,p)?S_OK:E_FAIL;return q->draw->begin_query(q->draw,p)?S_OK:E_FAIL;});
}
void APIENTRY resolve_queries(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HQUERYHEAP heap,D3D12DDI_QUERY_TYPE type,UINT first,UINT count,D3D12DDI_HRESOURCE dst,UINT64 offset) {
    List *l=object<List>(h);QueryHeap *queries=object<QueryHeap>(heap);Resource *r=object<Resource>(dst);
    UINT size=type==D3D12DDI_QUERY_TYPE_PIPELINE_STATISTICS?88:type>=D3D12DDI_QUERY_TYPE_SO_STATISTICS_STREAM0?16:8;
    if(!queries || !r || r->pipe->target!=PIPE_BUFFER || first>queries->entries.size() || count>queries->entries.size()-first ||
       offset>r->size || static_cast<UINT64>(count)*size>r->size-offset || (offset&7)){error(l,E_INVALIDARG);return;}
    record(l,[=](Queue *q){for(UINT i=0;i<count;i++) {
        pipe_query *p=query(q,queries,first+i,type);if(!p)return DXGI_ERROR_UNSUPPORTED;
        for(UINT word=0;word<size/8;word++)q->draw->get_query_result_resource(q->draw,p,PIPE_QUERY_WAIT,PIPE_QUERY_TYPE_U64,word,r->pipe,static_cast<UINT>(offset)+i*size+word*8);
    }return S_OK;});
}
void APIENTRY set_predication(D3D12DDI_HCOMMANDLIST h,D3D12DDI_HRESOURCE resource,UINT64 offset,D3D12DDI_PREDICATION_OP op) {
    List *l=object<List>(h);Resource *r=object<Resource>(resource);
    if(r && (r->pipe->target!=PIPE_BUFFER || offset>r->size || 8>r->size-offset || (offset&7))){error(l,E_INVALIDARG);return;}
    record(l,[=](Queue *q){q->state->predicate=r;q->state->predicate_offset=offset;q->state->predicate_op=op;return S_OK;});
}
HRESULT read_buffer(Queue *q,Resource *r,UINT64 offset,void *out,UINT bytes) {
    if(!r || r->heap->device!=q->device || r->pipe->target!=PIPE_BUFFER || offset>r->size || bytes>r->size-offset)return E_INVALIDARG;
    if(!bytes)return S_OK;
    pipe_box box{static_cast<int>(offset),static_cast<int>(bytes),0,1,0,1};pipe_transfer *transfer=nullptr;
    void *p=q->draw->buffer_map(q->draw,r->pipe,0,PIPE_MAP_READ,&box,&transfer);if(!p)return E_OUTOFMEMORY;
    memcpy(out,p,bytes);q->draw->buffer_unmap(q->draw,transfer);return S_OK;
}
HRESULT predicate(Queue *q,bool &execute) {
    State &s=*q->state;execute=true;if(!s.predicate)return S_OK;
    UINT64 value;HRESULT hr=read_buffer(q,s.predicate,s.predicate_offset,&value,8);if(FAILED(hr))return hr;
    execute=s.predicate_op==D3D12DDI_PREDICATION_OP_EQUAL_ZERO?value!=0:value==0;return S_OK;
}
