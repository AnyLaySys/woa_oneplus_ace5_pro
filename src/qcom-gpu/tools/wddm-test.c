#include <windows.h>
#include <stdio.h>
#include <d3dkmthk.h>
#include "../wddm/abi.h"

int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    D3DKMT_ADAPTERINFO info[16]={0};D3DKMT_ENUMADAPTERS2 adapters={16,info};
    NTSTATUS status=D3DKMTEnumAdapters2(&adapters);
    printf("{\"enum\":\"%08lx\",\"adapters\":%u}\n",status,adapters.NumAdapters);
    if(status<0)return 1;
    int result=1;
    for(UINT i=0;i<adapters.NumAdapters;i++) {
        A830_ADAPTER_INFO driver={0};D3DKMT_QUERYADAPTERINFO query={info[i].hAdapter,KMTQAITYPE_UMDRIVERPRIVATE,&driver,sizeof(driver)};
        status=D3DKMTQueryAdapterInfo(&query);
        printf("{\"private_status\":\"%08lx\",\"version\":%u,\"expected_version\":%u,\"flags\":%u,\"driver_status\":\"%08x\",\"chip\":\"%08x\"}\n",status,driver.Version,A830_ABI_VERSION,driver.Flags,driver.Status,driver.Chip);
        if(status<0 || driver.Version!=A830_ABI_VERSION || driver.Chip!=0x44050001 || !(driver.Flags&1))continue;
        D3DKMT_CREATEDEVICE device={0};device.hAdapter=info[i].hAdapter;
        status=D3DKMTCreateDevice(&device);printf("{\"create_device\":\"%08lx\"}\n",status);if(status<0)break;
        D3DKMT_CREATECONTEXT context={0};context.hDevice=device.hDevice;context.ClientHint=D3DKMT_CLIENTHINT_UNKNOWN;
        status=D3DKMTCreateContext(&context);printf("{\"create_context\":\"%08lx\"}\n",status);
        if(status>=0) {
            A830_RESOURCE resource={A830_ABI_VERSION,4096,1024,1,4096,D3DDDIFMT_A8R8G8B8};
            D3DDDI_ALLOCATIONINFO allocation={0};allocation.pPrivateDriverData=&resource;allocation.PrivateDriverDataSize=sizeof(resource);
            D3DKMT_CREATEALLOCATION create={0};create.hDevice=device.hDevice;create.NumAllocations=1;create.pAllocationInfo=&allocation;
            create.Flags.CreateResource=1;
            status=D3DKMTCreateAllocation(&create);printf("{\"create_allocation\":\"%08lx\"}\n",status);
            printf("{\"allocation_handle\":%u,\"resource_handle\":%u}\n",allocation.hAllocation,create.hResource);
            if(status>=0) {
                D3DKMT_CREATEPAGINGQUEUE paging={0};paging.hDevice=device.hDevice;
                status=D3DKMTCreatePagingQueue(&paging);printf("{\"create_paging_queue\":\"%08lx\"}\n",status);
                if(status>=0) {
                    D3DDDI_MAKERESIDENT resident={0};resident.hPagingQueue=paging.hPagingQueue;
                    resident.NumAllocations=1;resident.AllocationList=&allocation.hAllocation;resident.Flags.CantTrimFurther=1;
                    status=D3DKMTMakeResident(&resident);printf("{\"make_resident\":\"%08lx\",\"paging_fence\":%llu}\n",status,resident.PagingFenceValue);
                    if(status>=0) {
                        ULONGLONG deadline=GetTickCount64()+10000;
                        while(*(volatile ULONGLONG *)paging.FenceValueCPUVirtualAddress<resident.PagingFenceValue && GetTickCount64()<deadline)Sleep(1);
                    }
                }
                A830_COMMAND command={A830_ABI_VERSION,A830_OP_FILL,0,0,4096,0x83002700};
                memcpy(context.pCommandBuffer,&command,sizeof(command));
                memset(context.pAllocationList,0,sizeof(*context.pAllocationList));context.pAllocationList[0].hAllocation=allocation.hAllocation;
                context.pAllocationList[0].WriteOperation=1;
                D3DKMT_RENDER render={0};render.hContext=context.hContext;render.CommandLength=sizeof(command);render.AllocationCount=1;
                status=D3DKMTRender(&render);printf("{\"render\":\"%08lx\"}\n",status);
                if(status>=0) {
                    D3DKMT_LOCK lock={0};lock.hDevice=device.hDevice;lock.hAllocation=allocation.hAllocation;lock.Flags.ReadOnly=1;
                    status=D3DKMTLock(&lock);printf("{\"lock\":\"%08lx\"}\n",status);
                    if(status>=0) {
                        result=0;volatile ULONG *data=lock.pData;
                        for(UINT n=0;n<1024;n++)if(data[n]!=command.Value){printf("{\"mismatch\":%u,\"value\":\"%08lx\"}\n",n,data[n]);result=1;break;}
                        D3DKMT_UNLOCK unlock={device.hDevice,1,&lock.hAllocation};D3DKMTUnlock(&unlock);
                        printf("{\"gpu_fill_dwords\":1024,\"result\":\"%s\"}\n",result?"FAIL":"PASS");
                    }
                }
                D3DKMT_DESTROYALLOCATION destroy={device.hDevice,create.hResource,NULL,0};D3DKMTDestroyAllocation(&destroy);
                if(paging.hPagingQueue){D3DDDI_DESTROYPAGINGQUEUE destroyPaging={paging.hPagingQueue};D3DKMTDestroyPagingQueue(&destroyPaging);}
            }
            D3DKMT_DESTROYCONTEXT destroyContext={context.hContext};D3DKMTDestroyContext(&destroyContext);
        }
        D3DKMT_DESTROYDEVICE destroyDevice={device.hDevice};D3DKMTDestroyDevice(&destroyDevice);break;
    }
    for(UINT i=0;i<adapters.NumAdapters;i++){D3DKMT_CLOSEADAPTER close={info[i].hAdapter};D3DKMTCloseAdapter(&close);}
    return result;
}
