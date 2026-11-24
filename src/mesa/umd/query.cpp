#include "umd.h"

SIZE_T APIENTRY CalcPrivateQuerySize(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEQUERY *){return sizeof(Query);}
void APIENTRY CreateQuery(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEQUERY *p,D3D10DDI_HQUERY hq,D3D10DDI_HRTQUERY) {
 Device *d=dev(h);auto *q=new(hq.pDrvPrivate) Query{};q->type=p->Query;
 if(q->type==D3D10DDI_QUERY_OCCLUSION || q->type==D3D10DDI_QUERY_OCCLUSIONPREDICATE) {
   VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};ci.queryType=VK_QUERY_TYPE_OCCLUSION;ci.queryCount=256;check(d,d->vk.CreateQueryPool(d->device,&ci,nullptr,&q->pool));
 } else if(q->type!=D3D10DDI_QUERY_EVENT && q->type!=D3D10DDI_QUERY_TIMESTAMPDISJOINT)error(d,DXGI_ERROR_UNSUPPORTED);
 d->queries.push_back(q);
}
void APIENTRY DestroyQuery(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY hq){Device *d=dev(h);Query *q=object<Query>(hq);if(q->pool){if((d->recording || d->submitted || d->pending.submitted) && begin(d))d->retired_queries.push_back(q->pool);else d->vk.DestroyQueryPool(d->device,q->pool,nullptr);}d->queries.erase(std::remove(d->queries.begin(),d->queries.end(),q),d->queries.end());q->~Query();}
void APIENTRY QueryBegin(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY hq){Device *d=dev(h);Query *q=object<Query>(hq);q->ended=false;q->active=true;q->count=0;q->serial=0;if(q->pool){if(!begin(d))return;end_pass(d);d->vk.CmdPipelineBarrier(d->cmd,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,nullptr,0,nullptr,0,nullptr);d->vk.CmdResetQueryPool(d->cmd,q->pool,0,256);}}
void APIENTRY QueryEnd(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY hq){Device *d=dev(h);Query *q=object<Query>(hq);q->active=false;if(!begin(d))return;q->serial=d->next_submit;q->ended=true;}
void APIENTRY QueryGetData(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY hq,void *data,UINT bytes,UINT flags) {
 Device *d=dev(h);Query *q=object<Query>(hq);if(!q->ended){d->errors.pfnSetErrorCb(d->core,DXGI_DDI_ERR_WASSTILLDRAWING);return;}
 if(q->serial==d->next_submit && !(flags&D3D10_DDI_GET_DATA_DO_NOT_FLUSH) && !flush(d))return;
 if(!ready(d,q->serial)){if(SUCCEEDED(d->failure))d->errors.pfnSetErrorCb(d->core,DXGI_DDI_ERR_WASSTILLDRAWING);return;}
 if(!data || !bytes)return;
 if(q->type==D3D10DDI_QUERY_EVENT){if(bytes>=sizeof(BOOL))*(BOOL *)data=TRUE;}
 else if(q->type==D3D10DDI_QUERY_TIMESTAMPDISJOINT){if(bytes>=sizeof(D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT)){auto *v=(D3D10_DDI_QUERY_DATA_TIMESTAMP_DISJOINT *)data;v->Frequency=(UINT64)(1000000000.0/d->properties.limits.timestampPeriod);v->Disjoint=TRUE;}}
 else if(q->pool){uint64_t sum=0;std::array<uint64_t,256> results{};if(q->count){VkResult result=d->vk.GetQueryPoolResults(d->device,q->pool,0,q->count,(size_t)q->count*8,results.data(),8,VK_QUERY_RESULT_64_BIT);if(result==VK_NOT_READY){d->errors.pfnSetErrorCb(d->core,DXGI_DDI_ERR_WASSTILLDRAWING);return;}if(!check(d,result))return;}for(UINT i=0;i<q->count;i++)sum+=results[i];if(q->type==D3D10DDI_QUERY_OCCLUSIONPREDICATE){if(bytes>=sizeof(BOOL))*(BOOL *)data=!!sum;}else if(bytes>=sizeof(UINT64))*(UINT64 *)data=sum;}
}
void APIENTRY SetPredication(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY hq,BOOL){if(hq.pDrvPrivate)error(dev(h),DXGI_ERROR_UNSUPPORTED);}
void APIENTRY CheckCounterInfo(D3D10DDI_HDEVICE,D3D10DDI_COUNTER_INFO *p){memset(p,0,sizeof(*p));}
void APIENTRY CheckCounter(D3D10DDI_HDEVICE h,D3D10DDI_QUERY,D3D10DDI_COUNTER_TYPE *,UINT *,LPSTR,UINT *,LPSTR,UINT *,LPSTR,UINT *){dev(h)->errors.pfnSetErrorCb(dev(h)->core,E_INVALIDARG);}
