#include "cam.h"
#include "gdsc-cycle.h"
#include "csiphy-regs.h"
#include "dphy-regs.h"
#include "dma-address.h"
#include "dma-ready.h"
#include "dma-route.h"
#include "trace-regs.h"
#include "csid-commit.h"
#include "cphy-lanes.h"
#include "../platform/lock.h"
#include "smmu-fault.h"
#include "smmu-restore.h"
#include "frame-route.h"
#include "dma-idle.h"
#include "rdi-frame.h"
#include "frame-map-table.h"
#include "frame-context.h"
#include "tbu-translate.h"
#include "isp-input-check.h"
#include "isp-pixel-check.h"
#include "isp-release.h"
ULONGLONG CamCpuTranslate(PVOID address);

NTSTATUS CamIspPixelCheck(CAM *c,QCAM_ISP_PIXEL_STATE *state) {
    if(!c->Powered||!c->BusOn||c->DmaActive||c->RxActive||c->StreamFile)return STATUS_DEVICE_BUSY;
    NTSTATUS status=CamIspPixelStandbyCheck(c->Base[CamCsid],c->Base[CamTfe],c->Info.Width,c->Info.Height,state);
    if(!NT_SUCCESS(status)&&state->Size==sizeof(*state))for(ULONG i=0;i<state->Count;i++) {
        if(state->Configured[i]!=state->Expected[i]||state->Restored[i]!=state->Before[i]) {
            c->Diag.Step=190;c->Diag.Region=CamTfe;c->Diag.Offset=state->Offsets[i];c->Diag.Mask=MAXULONG;
            c->Diag.Address=0x0aca6000+state->Offsets[i];RtlCopyMemory(c->Diag.Resource,"isp-pix",8);
            c->Diag.Expected=state->Expected[i];c->Diag.Observed=state->Configured[i];
            c->Diag.Count=i;c->Diag.Phase=state->Restored[i]!=state->Before[i]?2:1;break;
        }
    }
    return status;
}

NTSTATUS CamIspInputCheck(CAM *c,QCAM_ISP_INPUT_STATE *state) {
    if(!c->Powered||!c->BusOn||c->DmaActive||c->RxActive||c->StreamFile)return STATUS_DEVICE_BUSY;
    NTSTATUS status=CamIspRaw10Check(c->Base[CamCsid],c->Base[CamTfe],c->Info.Width,c->Info.Height,state);
    if(!NT_SUCCESS(status)&&state->Size==sizeof(*state)) {
        CAM_ISP_RAW10_INPUT plan;
        if(CamIspRaw10Input(&plan,0,0,0,0,c->Info.Width,c->Info.Height))for(ULONG i=0;i<CAM_ISP_INPUT_REGS;i++) {
            if(state->Configured[i]!=plan.Registers[i].Value||state->Restored[i]!=state->Before[i]) {
                c->Diag.Step=189;c->Diag.Region=CamCsid;c->Diag.Offset=plan.Registers[i].Offset;c->Diag.Mask=MAXULONG;
                c->Diag.Address=0x0ad2d000+plan.Registers[i].Offset;RtlCopyMemory(c->Diag.Resource,"isp-in",7);
                c->Diag.Expected=plan.Registers[i].Value;c->Diag.Observed=state->Configured[i];
                c->Diag.Count=i;c->Diag.Phase=state->Restored[i]!=state->Before[i]?2:1;break;
            }
        }
    }
    return status;
}

static NTSTATUS FrameMap(CAM *c,ULONG slot,ULONG original) {
    if(c->FrameMapped||c->FrameContextOwned||c->FrameRouteOwned||c->DmaActive||c->RxActive||c->ScmPending||c->CdmPending||c->TbuPending)
        return STATUS_DEVICE_BUSY;
    if(!c->FrameTables) {
        PHYSICAL_ADDRESS lowest={0},highest,boundary={0};highest.QuadPart=MAXULONG;
        c->FrameTables=MmAllocateContiguousMemorySpecifyCache(CAM_FRAME_MAP_TABLE_BYTES,lowest,highest,boundary,MmNonCached);
        if(!c->FrameTables){return STATUS_INSUFFICIENT_RESOURCES;}
        c->FrameTableAddress=MmGetPhysicalAddress(c->FrameTables);
    }
    if(!CamFrameMapTablePolicy(c->FrameTables,c->FrameTableAddress.QuadPart,
       MmGetPhysicalAddress(c->FrameAllocation).QuadPart,QCAM_ARENA_BYTES,c->PipelineLimit==14&&c->IspFdCoherent)){return STATUS_DEVICE_CONFIGURATION_ERROR;}
    KeMemoryBarrier();
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    PUCHAR base=c->Base[CamSmmu];BOOLEAN used[83]={0};ULONG bank=MAXULONG;
    for(ULONG i=0;i<128;i++) {
        ULONG smr=CamRead(base,0x800+i*4),control=CamRead(base,0xc00+i*4);
        if((smr&0x80000000u)&&!(control&0x30000)&&(control&255)<83)used[control&255]=TRUE;
    }
    for(ULONG i=0;i<16;i++)if(!used[i]) {
        ULONG candidate=CamRead(base,0x80000+i*4096);
        if(!(candidate&1)&&CamFrameContextFormat(CamRead(base,0x1000+i*4))&&
           !CamSmmuFault(CamRead(base,0x80058+i*4096))){bank=i;break;}
    }
    if(bank==MAXULONG){status=STATUS_NOT_SUPPORTED;goto done;}
    ULONG context=0x80000+bank*4096,id2=CamRead(base,0x28);
    if(!(id2&0x1000)){status=STATUS_NOT_SUPPORTED;goto done;}
    const ULONG offsets[]={0x1000+bank*4,0x1800+bank*4,context+4,context+0x10,context+0x30,
        context+0x20,context+0x24,context+0x28,context+0x2c,context+0x34,context+0x38,context+0x3c};
    const ULONG values[]={0x1f300,1,1,0x38010|((id2>>4)&15),0x803020,
        (ULONG)c->FrameTableAddress.QuadPart,(ULONG)(c->FrameTableAddress.QuadPart>>32),0,0,bank,0xff44,0};
    if(!CamFrameContextUnreferenced(base,bank)){status=STATUS_DEVICE_BUSY;goto done;}
    for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++)c->FrameContextBefore[i]=CamRead(base,offsets[i]);
    c->FrameMapBank=bank;c->FrameContextUnassigned=!c->FrameContextBefore[0];
    c->FrameContextSctlr=CamRead(base,context);
    c->FrameContextOwned=TRUE;
    c->Diag.Step=188;c->Diag.Region=CamSmmu;c->Diag.Slot=bank;
    for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++) {
        c->Diag.Offset=offsets[i];c->Diag.Expected=values[i];CamWrite(base,offsets[i],values[i]);
        c->Diag.Observed=CamRead(base,offsets[i]);
        ULONG configuredMask=offsets[i]==context+0x10?0x3801f:offsets[i]==context+0x30?0x00c0ff3f:MAXULONG;
        if((c->Diag.Observed^values[i])&configuredMask){status=STATUS_IO_DEVICE_ERROR;goto done;}
    }
    c->FrameMapBank=bank;c->FrameMapped=TRUE;
    c->Diag.Offset=context;c->Diag.Expected=0x67;
    CamWrite(base,context,0x67);c->Diag.Observed=CamRead(base,context);
    if(!(c->Diag.Observed&1)){status=STATUS_IO_DEVICE_ERROR;goto done;}
    CamWrite(base,context+0x610,0);CamWrite(base,context+0x7f0,0);
    status=CamWait(base,context+0x7f4,1,0,10000);if(!NT_SUCCESS(status))goto done;
    if(CamRead(base,0x800+slot*4)!=0x80001c00||CamRead(base,0xc00+slot*4)!=original){status=STATUS_DEVICE_BUSY;goto done;}
    c->FrameRoute[0]=slot;c->FrameRoute[1]=0x80001c00;c->FrameRoute[2]=original;c->FrameRoute[3]=bank;
    c->FrameRouteOwned=TRUE;
    CamWrite(base,0x800+slot*4,0);
    if(CamRead(base,0x800+slot*4)){status=STATUS_IO_DEVICE_ERROR;goto done;}
    CamWrite(base,0xc00+slot*4,bank);
    if(CamRead(base,0xc00+slot*4)!=bank){status=STATUS_IO_DEVICE_ERROR;goto done;}
    CamWrite(base,0x800+slot*4,0x80001c00);
    if(CamRead(base,0x800+slot*4)!=0x80001c00){status=STATUS_IO_DEVICE_ERROR;goto done;}
    CamWrite(base,context,0x27);
    for(ULONG attempt=0;attempt<3;attempt++) {
        c->TbuPending=TRUE;
        status=CamTbuTranslateExpected(c->Base[CamHfTbu],0x1c00,CAM_FRAME_MAP_IOVA+4096,
            MmGetPhysicalAddress(c->Frame).QuadPart,0x409,c->Trace.FrameTranslation[0]);
        if(status!=STATUS_DEVICE_BUSY)c->TbuPending=FALSE;
        ULONG fsr=CamRead(base,context+0x58);
        if(CamSmmuFault(fsr)) {
            CamWrite(base,context+0x58,fsr);KeMemoryBarrier();
            if(fsr&0x40000000u)CamWrite(base,context+8,1);
        }
        if(NT_SUCCESS(status)||c->TbuPending)break;
    }
    CamWrite(base,context,0x67);
    c->Trace.FrameMapPar=c->Trace.FrameTranslation[0][5];
    if(c->TbuPending){status=STATUS_DEVICE_BUSY;goto done;}
    if((c->Trace.FrameMapPar&1)||
       (c->Trace.FrameMapPar>>12)!=(ULONGLONG)MmGetPhysicalAddress(c->Frame).QuadPart) {
        status=STATUS_DATA_ERROR;goto done;
    }
    c->FrameAddress.QuadPart=CAM_FRAME_MAP_IOVA+4096;c->Trace.FrameAddress=c->FrameAddress.QuadPart;
    c->Trace.FrameMapBank=bank;c->Trace.FrameTableAddress=c->FrameTableAddress.QuadPart;
    c->Trace.SmmuState[7]=bank;c->Trace.SmmuState[5]=0x1f300;c->Trace.SmmuState[6]=0x67;
done:
    c->Diag.Status=status;
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}

static NTSTATUS FrameUnmap(CAM *c) {
    if(!c->FrameMapped&&!c->FrameContextOwned)return STATUS_SUCCESS;
    if(c->DmaActive||c->RxActive||c->BusOn||c->ScmPending||c->CdmPending||c->TbuPending)return STATUS_DEVICE_BUSY;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    if(c->FrameContextOwned) {
        PUCHAR base=c->Base[CamSmmu];ULONG context=0x80000+c->FrameMapBank*4096;
        CamWrite(base,context,0);
        if(CamRead(base,context)&1)status=STATUS_IO_DEVICE_ERROR;
        else {
            CamWrite(base,context+0x610,0);CamWrite(base,context+0x7f0,0);
            status=CamWait(base,context+0x7f4,1,0,10000);
        }
        if(NT_SUCCESS(status)&&c->FrameRouteOwned)status=CamFrameRouteRestore(c);
        if(NT_SUCCESS(status)&&!CamFrameContextUnreferenced(base,c->FrameMapBank))status=STATUS_DEVICE_BUSY;
        if(NT_SUCCESS(status)) {
            status=CamFrameContextRetire(base,c->FrameMapBank,c->FrameContextBefore,c->FrameContextSctlr);
            if(NT_SUCCESS(status))c->FrameContextOwned=FALSE;
        }
    }
    if(NT_SUCCESS(status)){c->FrameMapped=FALSE;c->FrameAddress=MmGetPhysicalAddress(c->Frame);}
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}

NTSTATUS CamFrameRoutePrepare(CAM *c) {
    if(!c->Powered||!c->BusOn)return STATUS_DEVICE_NOT_READY;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    status=CamFrameRouteBind(c);c->Trace.FrameRouteStatus[0]=status;
    c->Diag.Step=184;c->Diag.Region=CamSmmu;c->Diag.Offset=0xc00+c->FrameRoute[0]*4;
    c->Diag.Address=0x15000000+c->Diag.Offset;c->Diag.Expected=c->FrameRoute[3];
    c->Diag.Observed=NT_SUCCESS(status)?CamRead(c->Base[CamSmmu],c->Diag.Offset):0;
    RtlCopyMemory(c->Diag.Resource,"rdi-sid",8);
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}

static NTSTATUS AllocatePdaf(CAM *c) {
    if(!c->PdafEnabled||c->Pdaf)return STATUS_SUCCESS;
    PHYSICAL_ADDRESS lowest={0},highest,boundary={0};highest.QuadPart=0xfffffffffULL;
    c->PdafAllocation=MmAllocateContiguousMemorySpecifyCache(QCAM_PDAF_BYTES*4+8192,lowest,highest,boundary,MmCached);
    if(!c->PdafAllocation)return STATUS_INSUFFICIENT_RESOURCES;
    c->Pdaf=(PUCHAR)c->PdafAllocation+4096;c->PdafAddress=MmGetPhysicalAddress(c->Pdaf);
    if(!c->PdafAddress.QuadPart||(c->PdafAddress.QuadPart&511)||
       (ULONGLONG)c->PdafAddress.QuadPart+QCAM_PDAF_BYTES*4+255>0x1000000000ULL)return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->PdafLatest=ExAllocatePool2(POOL_FLAG_NON_PAGED,QCAM_PDAF_BYTES,'dPcQ');
    if(!c->PdafLatest)return STATUS_INSUFFICIENT_RESOURCES;
    for(ULONG i=0;i<RTL_NUMBER_OF(c->PdafMdl);i++) {
        c->PdafMdl[i]=IoAllocateMdl((PUCHAR)c->Pdaf+i*QCAM_PDAF_BYTES+(i>=2?256:0),QCAM_PDAF_BYTES,FALSE,FALSE,NULL);
        if(!c->PdafMdl[i])return STATUS_INSUFFICIENT_RESOURCES;
        MmBuildMdlForNonPagedPool(c->PdafMdl[i]);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Allocate(CAM *c) {
    BOOLEAN common=(c->PipelineLimit==14||c->PipelineLimit==15)&&c->IspFdCommonBuffer;
    BOOLEAN common32=common&&c->IspFdCommonBuffer32;
    BOOLEAN low=c->PipelineLimit==14&&(c->IspFdLow||common32);
    BOOLEAN uncached=!common&&c->PipelineLimit==14&&(c->IspFdUncached||low);
    if(c->Frame&&((!common&&c->FrameUncached!=uncached)||c->FrameLow!=low||(c->FrameBuffer!=NULL)!=common||
       (common&&c->IspFdSettled&&(!c->FrameGuardMdl||MmGetMdlByteCount(c->FrameGuardMdl)!=QCAM_ARENA_BYTES)))) {
        if(c->StreamFile||c->DmaActive||c->RxActive||c->BusOn||c->ScmPending||c->CdmPending||
           c->TbuPending||c->FrameRouteOwned||c->FrameMapped||c->IspFdOwned)return STATUS_DEVICE_BUSY;
        CamDmaFree(c);
        if(c->Frame)return STATUS_DEVICE_BUSY;
    }
    if(c->Frame)return AllocatePdaf(c);
    NTSTATUS status;
    /* A separate framework adapter keeps the ordinary36-bit adapter's
       existing configuration intact. Both allocations retain their WDF
       logical addresses; no physical address is substituted for an IOVA. */
    WDFDMAENABLER *adapter=common32?&c->DmaEnabler32:&c->DmaEnabler;
    ULONG addressWidth=common32?32:36;
    if(!*adapter) {
        WDF_DMA_ENABLER_CONFIG config;
        WDF_DMA_ENABLER_CONFIG_INIT(&config,WdfDmaProfileScatterGather64,QCAM_ARENA_BYTES);
        config.AddressWidthOverride=addressWidth;
        status=WdfDmaEnablerCreate(c->Device,&config,WDF_NO_OBJECT_ATTRIBUTES,adapter);
        if(!NT_SUCCESS(status))return status;
    }
    PHYSICAL_ADDRESS lowest={0},highest,boundary={0};highest.QuadPart=low?MAXULONG:0xfffffffffULL;
    PHYSICAL_ADDRESS logical={0};
    if(common) {
        WDF_COMMON_BUFFER_CONFIG config;WDF_COMMON_BUFFER_CONFIG_INIT(&config,4095);
        status=WdfCommonBufferCreateWithConfig(*adapter,QCAM_ARENA_BYTES,&config,WDF_NO_OBJECT_ATTRIBUTES,&c->FrameBuffer);
        if(!NT_SUCCESS(status))return status;
        c->FrameAllocation=WdfCommonBufferGetAlignedVirtualAddress(c->FrameBuffer);
        logical=WdfCommonBufferGetAlignedLogicalAddress(c->FrameBuffer);
        if(!CamDmaArenaAddressReady(logical.QuadPart,QCAM_ARENA_BYTES)||
           !CamDmaPageRangeReady(logical.QuadPart,QCAM_ARENA_BYTES,addressWidth)||
           ((ULONG_PTR)c->FrameAllocation&4095))return STATUS_DEVICE_CONFIGURATION_ERROR;
    } else c->FrameAllocation=MmAllocateContiguousMemorySpecifyCache(QCAM_ARENA_BYTES,lowest,highest,boundary,uncached?MmNonCached:MmCached);
    if(!c->FrameAllocation)return STATUS_INSUFFICIENT_RESOURCES;
    if(common)for(ULONG offset=0;offset<QCAM_ARENA_BYTES;offset+=4096) {
        ULONGLONG physical=MmGetPhysicalAddress((PUCHAR)c->FrameAllocation+offset).QuadPart;
        if(!CamDmaPageRangeReady(physical,4096,addressWidth))return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    c->FrameUncached=uncached;
    c->FrameLow=low;
    c->Frame=(PUCHAR)c->FrameAllocation+4096;
    c->FrameGuardMdl=IoAllocateMdl(c->FrameAllocation,common&&c->IspFdSettled?QCAM_ARENA_BYTES:4096,FALSE,FALSE,NULL);
    if(!c->FrameGuardMdl)return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(c->FrameGuardMdl);
    c->FrameContiguous=!common;
    c->FrameAddress=common?logical:MmGetPhysicalAddress(c->Frame);
    if(common)c->FrameAddress.QuadPart+=4096;
    c->Trace.FrameCpuAddress=(ULONGLONG)c->Frame;c->Trace.FrameCpuPar=CamCpuTranslate(c->Frame);
    if(common)c->FrameUncached=(c->Trace.FrameCpuPar>>56)!=255;
    for(ULONG i=0;i<RTL_NUMBER_OF(c->FrameMdl);i++) {
        c->FrameMdl[i]=IoAllocateMdl((PUCHAR)c->Frame+i*QCAM_FRAME_BYTES+(i>=2?256:0),QCAM_FRAME_BYTES,FALSE,FALSE,NULL);
        if(!c->FrameMdl[i])return STATUS_INSUFFICIENT_RESOURCES;
        MmBuildMdlForNonPagedPool(c->FrameMdl[i]);
    }
    if(!c->FrameAddress.QuadPart||(c->FrameAddress.QuadPart&511)||
       (ULONGLONG)c->FrameAddress.QuadPart+QCAM_FRAME_BYTES*4+255>0xfffffffffULL)return STATUS_DEVICE_CONFIGURATION_ERROR;
    return AllocatePdaf(c);
}

static NTSTATUS Map(CAM *c) {
    PUCHAR b=c->Base[CamSmmu];ULONG id0=CamRead(b,0x20),id1=CamRead(b,0x24),id2=CamRead(b,0x28);
    c->Trace.Smmu[0]=id0;c->Trace.Smmu[1]=id1;c->Trace.Smmu[2]=id2;
    for(ULONG i=0;i<4;i++)c->Trace.SmmuGlobalBefore[i]=CamRead(b,i?0x4c+i*4:0x48);
    RtlZeroMemory(c->Trace.SmmuState,sizeof(c->Trace.SmmuState));
    c->Trace.FrameAddress=c->FrameAddress.QuadPart;
    c->Trace.FramePhysicalAddress=MmGetPhysicalAddress(c->Frame).QuadPart;
    c->Diag.Step=143;c->Diag.Region=CamSmmu;c->Diag.Offset=0x20;c->Diag.Mask=MAXULONG;
    c->Diag.Expected=id0;c->Diag.Observed=id0;c->Diag.Address=id1;c->Diag.Count=id2;
    c->Diag.Slot=MAXULONG;c->Diag.Phase=0;RtlCopyMemory(c->Diag.Resource,"appssmmu",8);
    ULONG slots=min(id0&255,128u);
    c->Trace.SmmuState[0]=CamRead(b,0);c->Trace.SmmuState[1]=slots;c->Trace.SmmuState[2]=id1&255;
    if(c->Trace.SmmuState[0]&1) {
        c->Diag.Offset=0;c->Diag.Mask=1;c->Diag.Expected=0;c->Diag.Observed=c->Trace.SmmuState[0];
        return STATUS_DEVICE_NOT_READY;
    }
    if(!(id0&(1u<<27))||(id0&0x100)||!slots)return STATUS_NOT_SUPPORTED;
    ULONG slot=MAXULONG,control=0;
    for(ULONG i=0;i<slots;i++) {
        ULONG smr=CamRead(b,0x800+4*i),s2cr=CamRead(b,0xc00+4*i);
        if(!(smr&0x80000000u))continue;
        if(((s2cr>>16)&3)==0)c->Trace.SmmuState[4]++;
        ULONG sid=smr&0x7fff,mask=(smr>>16)&0x7fff;
        if(((sid^0x1c00)&~mask)!=0)continue;
        if(mask||slot!=MAXULONG)return STATUS_DEVICE_BUSY;
        slot=i;control=s2cr;
    }
    c->Diag.Slot=slot;c->Diag.Phase=4;c->Trace.SmmuState[3]=slot;c->Trace.SmmuState[7]=control;
    if(slot==MAXULONG)return STATUS_NOT_SUPPORTED;
    c->Diag.Offset=0xc00+4*slot;c->Diag.Mask=0x30000;c->Diag.Expected=0x10000;c->Diag.Observed=control;
    if((c->PipelineLimit==14||c->PipelineLimit==15)&&c->IspFdCommonBuffer) {
        /* Windows owns the common-buffer DMA address and any adapter mapping.
           Keep its exact camera SID route; FdTranslate checks both plane ends
           against actual CPU physical pages before starting pixel DMA. */
        if(!c->FrameBuffer||((control>>16)&3)>1||(!(control&0x30000)&&(control&255)>=(id1&255)))return STATUS_NOT_SUPPORTED;
        c->Trace.FrameMapBank=control&255;return STATUS_SUCCESS;
    }
    if(((control>>16)&3)==1)return STATUS_SUCCESS;
    if((control&0x30000)||(id1&0x80000000u))return STATUS_NOT_SUPPORTED;
    ULONG bank=control&255,context=(1u<<(((id1>>28)&7)+1))*4096;
    if(bank>=(id1&255)||context+bank*4096>=0x100000)return STATUS_NOT_SUPPORTED;
    ULONG cbar=CamRead(b,0x1000+bank*4),sctlr=CamRead(b,context+bank*4096);
    c->Trace.SmmuState[5]=cbar;c->Trace.SmmuState[6]=sctlr;c->Diag.Phase=5;
    c->Diag.Offset=context+bank*4096;c->Diag.Mask=1;c->Diag.Expected=0;c->Diag.Observed=sctlr;c->Diag.Count=cbar;
    if(!CamDmaIdentity(control,cbar,sctlr))return STATUS_NOT_SUPPORTED;
    return c->PipelineLimit==12||c->PipelineLimit==14||c->PipelineLimit==15?FrameMap(c,slot,control):STATUS_SUCCESS;
}

VOID CamDmaFree(CAM *c) {
    if(c->DmaActive||c->RxActive||c->BusOn||c->ScmPending||c->CdmPending||c->TbuPending||c->FrameRouteOwned||c->FrameMapped||c->FrameContextOwned||c->IspFdOwned)return;
    if(c->IspFd){ExFreePoolWithTag(c->IspFd,'fIcQ');c->IspFd=NULL;}
    for(ULONG i=0;i<RTL_NUMBER_OF(c->PdafMdl);i++)if(c->PdafMdl[i]){IoFreeMdl(c->PdafMdl[i]);c->PdafMdl[i]=NULL;}
    if(c->PdafAllocation){MmFreeContiguousMemory(c->PdafAllocation);c->PdafAllocation=NULL;}
    if(c->PdafLatest){ExFreePoolWithTag(c->PdafLatest,'dPcQ');c->PdafLatest=NULL;}
    c->Pdaf=NULL;c->PdafAddress.QuadPart=0;
    if(c->FrameTables){MmFreeContiguousMemory(c->FrameTables);c->FrameTables=NULL;}
    if(c->CommandMdl){IoFreeMdl(c->CommandMdl);c->CommandMdl=NULL;}
    if(c->CommandBuffer){WdfObjectDelete(c->CommandBuffer);c->CommandBuffer=NULL;}
    if(c->CommandContiguous&&c->Command){MmFreeContiguousMemory(c->Command);c->CommandContiguous=FALSE;}
    c->Command=NULL;c->CommandAddress.QuadPart=0;
    for(ULONG i=0;i<RTL_NUMBER_OF(c->FrameMdl);i++)if(c->FrameMdl[i]){IoFreeMdl(c->FrameMdl[i]);c->FrameMdl[i]=NULL;}
    if(c->FrameGuardMdl){IoFreeMdl(c->FrameGuardMdl);c->FrameGuardMdl=NULL;}
    if(c->FrameBuffer){WdfObjectDelete(c->FrameBuffer);c->FrameBuffer=NULL;}
    if(c->FrameContiguous&&c->FrameAllocation){MmFreeContiguousMemory(c->FrameAllocation);c->FrameContiguous=FALSE;c->FrameAllocation=NULL;}
    c->Frame=NULL;
    c->FrameAllocation=NULL;
    c->FrameUncached=FALSE;
    c->FrameLow=FALSE;
    c->FrameAddress.QuadPart=0;
}

static VOID Table(CAM *c,const PHY_REG *table,ULONG count,ULONG settle) {
    for(ULONG i=0;i<count;i++) {
        ULONG value=table[i].Value;
        if(table[i].Kind==1&&settle)value=settle&255;
        if(table[i].Kind==2&&settle)value=(settle>>8)&255;
        if(table[i].Kind==3)continue;
        CamWrite(CamPhyBase(c),table[i].Address,value);
        if(table[i].Delay)CamDelay(table[i].Delay);
    }
}

static NTSTATUS Phy(CAM *c,const QCAM_CAPTURE_OPTIONS *o) {
    if(c->Sensor) {
        PUCHAR b=CamPhyBase(c);ULONG mask=c->Sensor==2?0x81:0xd5;
        CamWrite(b,0x1014,0);Table(c,PhyReset,RTL_NUMBER_OF(PhyReset),0);
        CamWrite(b,0x01e0,CamRead(b,0x01e0)&~0x101u);CamWrite(b,0x01e4,CamRead(b,0x01e4)&~0x101u);
        CamWrite(b,0x1084,0);CamWrite(b,0x108c,0);CamWrite(b,0x101c,0x7a);CamWrite(b,0x1018,1);
        Table(c,PhyDphy,RTL_NUMBER_OF(PhyDphy),o->SettleCount?o->SettleCount:0x19);
        CamWrite(b,0x1000,2);CamDelay(3000);
        for(ULONG i=0;i<22;i++)CamWrite(b,0x102c+4*i,0);
        CamWrite(b,0x1014,mask);c->PhyOn=TRUE;return STATUS_SUCCESS;
    }
    ULONG lanes=CamCphyLaneMask(0x210,3);
    if(!lanes)return STATUS_INVALID_PARAMETER;
    ULONG rate;
    for(rate=0;rate<RTL_NUMBER_OF(PhyRates);rate++)if(o->DataRate<=PhyRates[rate].Bandwidth)break;
    if(rate==RTL_NUMBER_OF(PhyRates))return STATUS_INVALID_PARAMETER;
    PUCHAR b=CamPhyBase(c);CamWrite(b,0x1014,0);
    Table(c,PhyReset,RTL_NUMBER_OF(PhyReset),0);
    CamWrite(b,0x1014,lanes);c->PhyOn=TRUE;
    CamWrite(b,0x01e0,CamRead(b,0x01e0)&~0x101u);CamWrite(b,0x01e4,CamRead(b,0x01e4)&~0x101u);
    Table(c,PhyCommon,RTL_NUMBER_OF(PhyCommon),0);
    Table(c,PhyRates[rate].Registers,PhyRates[rate].Count,o->SettleCount);
    Table(c,PhyCphy,RTL_NUMBER_OF(PhyCphy),o->SettleCount);
    Table(c,PhyResetExit,RTL_NUMBER_OF(PhyResetExit),0);
    for(ULONG i=0;i<22;i++)CamWrite(b,0x102c+4*i,0);
    CamWrite(b,0x1014,lanes);c->PhyOn=TRUE;return STATUS_SUCCESS;
}

VOID CamSnapshot(CAM *c) {
    if(!c->Powered)return;
    PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    c->Info.CsidIrq|=CamRead(s,0x84);c->Info.CsidRxIrq|=CamRead(s,0xb4);
    c->Info.TfeIrq|=CamRead(t,0x88);c->Info.TfeBusIrq|=CamRead(s,0xa4);
    c->Info.Packets=CamRead(s,0x440);c->Info.MeasuredSize=CamRead(s,0xe94);
}

static NTSTATUS ResetCsid(CAM *c) {
    PUCHAR s=c->Base[CamCsid];
    CamWrite(s,0x88,0);CamWrite(s,0x8c,1);CamWrite(s,0x14,1);
    CamWrite(s,0xc,0x11);CamWrite(s,0x10,1);
    NTSTATUS status=CamWait(s,0x84,1,1,50000);
    c->Diag.Step=144;c->Diag.Region=CamCsid;c->Diag.Offset=0x84;c->Diag.Mask=1;c->Diag.Expected=1;
    c->Diag.Observed=CamRead(s,0x84);
    if(NT_SUCCESS(status)){CamWrite(s,0x8c,1);CamWrite(s,0x14,1);}
    return status;
}

static VOID RawTbu(CAM *c,ULONGLONG values[2][4]) {
    const ULONG regions[]={CamHfTbu,CamSfTbu};
    for(ULONG n=0;n<2;n++) {
        PUCHAR base=c->Base[regions[n]];
        values[n][0]=CamRead(base,0xc00);values[n][1]=CamRead(base,0xc08);
        values[n][2]=CamRead64(base,0xc10);values[n][3]=CamRead64(base,0xc20);
    }
}

static NTSTATUS RawContext(CAM *c,ULONG values[4]) {
    ULONG bank=c->Trace.SmmuState[7]&255;
    if(bank>=0x53||(c->Trace.SmmuState[7]&0x30000))return STATUS_NOT_SUPPORTED;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    const ULONG offsets[]={0x58,0x60,0x64,0x68};
    for(ULONG i=0;i<4;i++)values[i]=CamRead(c->Base[CamSmmu],0x80000+bank*4096+offsets[i]);
    return PlatformLockRelease(&guard);
}

NTSTATUS CamCaptureStart(CAM *c,const QCAM_CAPTURE_OPTIONS *o) {
    if(c->DmaActive||c->RxActive)return STATUS_DEVICE_BUSY;
    if(((c->PipelineLimit>=6&&c->PipelineLimit<=12)||c->PipelineLimit==14||
        (c->PipelineLimit==15&&c->IspFdCommonBuffer))&&!c->ReceiverVerified)return STATUS_DEVICE_NOT_READY;
    c->PdafEnabled=!c->PipelineLimit&&!c->Sensor;
    NTSTATUS status=Allocate(c);
    if(!NT_SUCCESS(status)){CamDmaFree(c);return status;}
    ULONG exposure=c->Info.Exposure,gain=c->Info.Gain;
    status=CamPowerOn(c);if(!NT_SUCCESS(status))return status;
    if(exposure) {status=CamSensorExposure(c,min(exposure,c->Info.FrameLength-48),gain);if(!NT_SUCCESS(status))return status;}
    PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    c->Info.CsidVersion=CamRead(s,0);c->Info.TfeVersion=CamRead(t,0);
    c->Diag.Step=140;c->Diag.Region=CamCsid;c->Diag.Offset=0;c->Diag.Mask=MAXULONG;
    c->Diag.Expected=0x30000006;c->Diag.Observed=c->Info.CsidVersion;
    if(c->Info.CsidVersion!=0x30000006)return STATUS_REVISION_MISMATCH;
    c->Diag.Region=CamTfe;c->Diag.Expected=0x10000000;c->Diag.Observed=c->Info.TfeVersion;
    if(c->Info.TfeVersion!=0x10000000)return STATUS_REVISION_MISMATCH;
    if(!CamDmaResourcesIdle(s,t))return STATUS_DEVICE_BUSY;
    if(c->PipelineLimit==5) {
        status=CamCrmOff(c);if(!NT_SUCCESS(status))return status;
        c->Configured=FALSE;status=CamGdscCycle(c);
        if(!NT_SUCCESS(status))return status;
        status=CamCrmOn(c);if(!NT_SUCCESS(status))return status;
        status=CamCciInit(c);if(!NT_SUCCESS(status))return status;
    }
    if(c->PipelineLimit==4) {
        status=CamCrmOff(c);if(!NT_SUCCESS(status))return status;
        PUCHAR gcc=c->Base[CamGcc];ULONG original=CamRead(gcc,0x26000);
        if(original&1)return STATUS_DEVICE_BUSY;
        c->Diag.Step=155;c->Diag.Region=CamGcc;c->Diag.Offset=0x26000;
        c->Diag.Mask=1;c->Diag.Expected=0;
        status=CamSave(c,gcc,0x26000,original|1);
        if(!NT_SUCCESS(status))return status;
        CamDelay(1);CamWrite(gcc,0x26000,original);CamDelay(10);
        status=CamWait(gcc,0x26000,1,0,10000);c->Diag.Observed=CamRead(gcc,0x26000);
        if(!NT_SUCCESS(status))return status;
        c->Configured=FALSE;status=CamCrmOn(c);
        if(NT_SUCCESS(status))status=CamCciInit(c);
        if(!NT_SUCCESS(status))return status;
    }
    status=CamBusOn(c);if(!NT_SUCCESS(status))return status;
    if(!c->PipelineLimit||(c->PipelineLimit>=6&&c->PipelineLimit<=14)||
       (c->PipelineLimit==15&&c->IspFdCommonBuffer)) {
        status=CamEnsureStaticQos(c);if(!NT_SUCCESS(status))return status;
    }
    status=ResetCsid(c);if(!NT_SUCCESS(status))return status;
    status=Map(c);if(!NT_SUCCESS(status))return status;c->Info.Stage=7;
    if(c->PipelineLimit==11){status=CamFrameRoutePrepare(c);if(!NT_SUCCESS(status))return status;}
    if(c->PipelineLimit==1||c->PipelineLimit==4||c->PipelineLimit==5||c->PipelineLimit==15)return STATUS_SUCCESS;
    c->Info.Flags&=~QCAM_FRAME_VALID;c->Info.CsidIrq=0;c->Info.CsidRxIrq=0;c->Info.TfeIrq=0;c->Info.TfeBusIrq=0;
    if((c->PipelineLimit>=6&&c->PipelineLimit<=12)||c->PipelineLimit==14)RtlFillMemory(c->FrameAllocation,QCAM_ARENA_BYTES,0xa5);
    else RtlZeroMemory(c->FrameAllocation,QCAM_ARENA_BYTES);
    if(c->PipelineLimit==14)KeFlushIoBuffers(c->FrameGuardMdl,FALSE,TRUE);
    for(ULONG i=0;i<RTL_NUMBER_OF(c->FrameMdl);i++)KeFlushIoBuffers(c->FrameMdl[i],FALSE,TRUE);KeMemoryBarrier();
    RtlZeroMemory(&c->PdafInfo,sizeof(c->PdafInfo));c->PdafSlot=0;c->PdafStamp=0;c->PdafPending=FALSE;
    if(c->PdafEnabled) {
        RtlZeroMemory(c->PdafAllocation,QCAM_PDAF_BYTES*4+8192);
        for(ULONG i=0;i<RTL_NUMBER_OF(c->PdafMdl);i++)KeFlushIoBuffers(c->PdafMdl[i],FALSE,TRUE);
        KeMemoryBarrier();
    }
    if(c->PipelineLimit==14){status=CamIspFdConfigure(c,NULL);if(!NT_SUCCESS(status))return status;}
    status=Phy(c,o);if(!NT_SUCCESS(status))return status;
    if(c->PipelineLimit==2)return STATUS_SUCCESS;
    BOOLEAN diagnostic=c->PipelineLimit>=6&&c->PipelineLimit<=12;
    BOOLEAN frameMode=c->PipelineLimit>=7&&c->PipelineLimit<=11;
    BOOLEAN packed=c->PipelineLimit==12,dual=!diagnostic||packed;
    BOOLEAN pdaf=c->PipelineLimit==13;
    ULONG height=pdaf?QCAM_PDAF_HEIGHT:c->Info.Height,width=c->Info.Width;
    if(c->PipelineLimit!=3&&!diagnostic&&!(CamRead(c->Base[CamCpas],0xf0)&1)) {
        c->Diag.Step=146;c->Diag.Region=CamCpas;c->Diag.Offset=0xf0;
        c->Diag.Mask=1;c->Diag.Expected=1;c->Diag.Observed=CamRead(c->Base[CamCpas],0xf0);
        RtlCopyMemory(c->Diag.Resource,"cpasrt",7);c->Diag.Address=0x0ac040f0;
        c->Diag.Slot=MAXULONG;c->Diag.Phase=0;c->Diag.Count=1;
        return STATUS_DEVICE_NOT_READY;
    }
    if(diagnostic&&!CamDiagnosticDmaReady(c))return STATUS_DEVICE_NOT_READY;
    if(frameMode&&(c->Trace.Csid[14]&0xffff)!=QCAM_STRIDE)return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(diagnostic) {
        c->Trace.TfeTopCount=0;RtlZeroMemory(c->Trace.TfeTop,sizeof(c->Trace.TfeTop));
        c->Trace.NocFillReads=0;RtlZeroMemory(c->Trace.NocFillPeak,sizeof(c->Trace.NocFillPeak));
        RtlZeroMemory(c->Trace.RawOutstandingSeen,sizeof(c->Trace.RawOutstandingSeen));
        RawTbu(c,c->Trace.RawTbuBefore);c->Trace.RawContextStatus[0]=RawContext(c,c->Trace.RawContextBefore);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamNocFillRegs);i++)c->Trace.NocFillBefore[i]=CamRead(c->Base[CamNoc],CamNocFillRegs[i]);
        status=CamSave(c,t,0x1ec,3);if(!NT_SUCCESS(status))return status;
        if(CamRead(t,0x1ec)!=3)return STATUS_IO_DEVICE_ERROR;
    }
    if(c->PipelineLimit==8) {
        PUCHAR cpas=c->Base[CamCpas];status=CamSave(c,cpas,0xec,0);
        if(NT_SUCCESS(status))status=CamSave(c,cpas,0xf4,0);
        if(!NT_SUCCESS(status))return status;
        if((CamRead(cpas,0xec)|CamRead(cpas,0xf4))&1)return STATUS_IO_DEVICE_ERROR;
        CamDelay(1);CamBusSnapshot(c);
    }
    CamWrite(s,0x88,0);CamWrite(s,0x98,0);CamWrite(s,0xa8,0);CamWrite(s,0xb8,0);CamWrite(s,0xc8,0);CamWrite(s,0x118,0);
    CamWrite(s,0x8c,MAXULONG);CamWrite(s,0x9c,MAXULONG);CamWrite(s,0xac,MAXULONG);
    CamWrite(s,0xbc,MAXULONG);CamWrite(s,0xcc,MAXULONG);CamWrite(s,0x11c,MAXULONG);CamWrite(s,0x14,1);
    CamWrite(s,0x400,c->Sensor==1?0x00232103:c->Sensor==2?0x00400000:c->Sensor==3?0x00532103:0x01602102);CamWrite(s,0x404,0x2445);
    CamWrite(s,0x408,0x02b002b5);
    if(c->PipelineLimit==14)return CamIspFdStart(c);
    if(c->PipelineLimit==3) {
        CamWrite(s,0xe04,0);CamWrite(s,0xe00,CamRead(s,0xe00)&~0x80000000u);
        c->RxActive=TRUE;c->Info.Stage=8;
        status=CamSensorStream(c,TRUE);
        if(NT_SUCCESS(status))c->Info.Stage=9;
        return status;
    }
    CamWrite(s,0xe0c,0);CamWrite(s,0xe10,frameMode||dual?0x8011:0x1811);CamWrite(s,0xe14,0);
    CamWrite(s,0xe48,0);CamWrite(s,0xe4c,1);CamWrite(s,0xe50,1);CamWrite(s,0xe54,0);
    CamWrite(s,0xe58,(width-1)<<16);CamWrite(s,0xe5c,(height-1)<<16);
    CamWrite(s,0xe60,0);CamWrite(s,0xe64,0);CamWrite(s,0xe68,0);CamWrite(s,0xe6c,0);
    CamWrite(s,0xe88,3);CamWrite(s,0xe8c,(height<<16)|width);
    CamWrite(s,0xe00,pdaf?0x81b02260:0x802b2260);
    CamWrite(t,0x80,0);CamWrite(t,0x818,0);CamWrite(t,0x81c,0);
    CamWrite(t,0x84,MAXULONG);CamWrite(t,0x7c,1);CamWrite(t,0x820,MAXULONG);CamWrite(t,0x824,MAXULONG);CamWrite(t,0x830,1);
    CamWrite(s,0xa8,0x10000);
    CamWrite(t,0x808,0x7ffffff);CamWrite(t,0x924,0);CamWrite(t,0x858,0x1036);
    CamWrite(t,0x240c,frameMode?0xffff:(height<<16)|(dual?width*5/64:width));
    CamWrite(t,0x2410,0);CamWrite(t,0x2414,frameMode?0xffff:QCAM_STRIDE);
    CamWrite(t,0x2418,frameMode||dual?0:12);CamWrite(t,0x2408,0);CamWrite(t,0x2428,0);
    CamWrite(t,0x247c,11<<8);
    CamWrite(t,0x2430,0);CamWrite(t,0x2434,1);CamWrite(t,0x2438,0);CamWrite(t,0x243c,1);
    CamWrite(t,0x2460,!frameMode||c->PipelineLimit>=9?1:0);
    CamWrite(t,0x2464,!frameMode||c->PipelineLimit>=9?MAXULONG:QCAM_FRAME_BYTES);
    CamWrite(t,0x2468,0);CamWrite(t,0x2470,0);CamWrite(t,0x2478,0);
    if(c->PipelineLimit>=10&&c->PipelineLimit<=11) {
        c->Trace.TfePrimeTestBus=CamRead(t,0x928);
        status=CamSave(c,t,0x928,0);if(!NT_SUCCESS(status))return status;
        if(CamRead(t,0x928))return STATUS_IO_DEVICE_ERROR;
        CamWrite(t,0x2400,0x10001);
    }
    CamWrite(t,0x2404,CamDmaImageAddress(c->FrameAddress.QuadPart));CamWrite(t,0x2400,frameMode?0x10001:1);
    if(dual) {
        const ULONG csid[]={0xe0c,0xe10,0xe14,0xe48,0xe4c,0xe50,0xe54,0xe58,0xe5c,0xe60,0xe64,0xe68,0xe6c,0xe88,0xe8c,0xe00};
        const ULONG writer[]={0x2408,0x240c,0x2410,0x2414,0x2418,0x2428,0x2430,0x2434,0x2438,0x243c,0x2460,0x2464,0x2468,0x2470,0x2478,0x247c};
        for(ULONG i=0;i<RTL_NUMBER_OF(csid);i++)CamWrite(s,csid[i]+0x200,CamRead(s,csid[i]));
        CamWrite(s,0x1010,CamRead(s,0x1010)&~1u);
        for(ULONG i=0;i<RTL_NUMBER_OF(writer);i++)CamWrite(t,writer[i]+0x100,CamRead(t,writer[i]));
        CamWrite(t,0x2504,CamDmaImageAddress(c->FrameAddress.QuadPart+QCAM_FRAME_BYTES*2+256));
        CamWrite(t,0x2500,1);CamWrite(s,0xa8,0x30000);
        CamWrite(s,0x128,0);CamWrite(s,0x12c,MAXULONG);CamWrite(s,0x14,1);
    }
    if(c->PdafEnabled) {
        const ULONG csid[]={0x0c,0x10,0x14,0x48,0x4c,0x50,0x54,0x58,0x5c,0x60,0x64,0x68,0x6c,0x88,0x8c};
        const ULONG writer[]={0x08,0x0c,0x10,0x14,0x18,0x28,0x30,0x34,0x38,0x3c,0x60,0x64,0x68,0x70,0x78,0x7c};
        for(ULONG n=0;n<2;n++) {
            ULONG path=0x1200+n*0x200,wm=0x2600+n*0x100;
            for(ULONG i=0;i<RTL_NUMBER_OF(csid);i++)if(csid[i]!=0x14||!n)CamWrite(s,path+csid[i],CamRead(s,0x1000+csid[i]));
            CamWrite(s,path+0x5c,(QCAM_PDAF_HEIGHT-1)<<16);
            CamWrite(s,path+0x8c,(QCAM_PDAF_HEIGHT<<16)|QCAM_WIDTH);CamWrite(s,path,0x81b02260);
            for(ULONG i=0;i<RTL_NUMBER_OF(writer);i++)CamWrite(t,wm+writer[i],CamRead(t,0x2500+writer[i]));
            CamWrite(t,wm+0x0c,(QCAM_PDAF_HEIGHT<<16)|(QCAM_STRIDE/16));
            CamWrite(t,wm+4,CamDmaImageAddress(c->PdafAddress.QuadPart+(n?QCAM_PDAF_BYTES*2+256:0)));
            CamWrite(t,wm,1);
            CamWrite(s,0x138+n*0x10,0);CamWrite(s,0x13c+n*0x10,MAXULONG);
        }
        CamWrite(s,0xa8,0xf0000);CamWrite(s,0x14,1);
    }
    if(c->PipelineLimit>=10&&c->PipelineLimit<=11)for(ULONG i=0;i<4;i++)c->Trace.TfePrime[0][i]=CamRead(t,0x2490+i*4);
    c->DmaActive=TRUE;
    CamWrite(s,0x28,0x11);CamWrite(s,0xe04,1);
    if(dual)CamWrite(s,0x1004,1);
    if(c->PdafEnabled){CamWrite(s,0x1204,1);CamWrite(s,0x1404,1);}
    CamCsidCommit(s,c->PdafEnabled?0xf00:dual?0x300:0x100);
    if(c->PipelineLimit>=10&&c->PipelineLimit<=11) {
        CamDelay(1);
        for(ULONG i=0;i<4;i++)c->Trace.TfePrime[1][i]=CamRead(t,0x2490+i*4);
        CamWrite(t,0x2404,CamDmaImageAddress(c->FrameAddress.QuadPart));CamCsidCommit(s,0x100);
        CamDelay(1);
        for(ULONG i=0;i<4;i++)c->Trace.TfePrime[2][i]=CamRead(t,0x2490+i*4);
    }
    c->RxActive=TRUE;
    c->Info.Stage=8;status=CamSensorStream(c,TRUE);
    if(NT_SUCCESS(status))c->Info.Stage=9;
    return status;
}

static NTSTATUS PdafReady(CAM *c) {
    if(!c->PdafEnabled||c->PdafPending)return STATUS_SUCCESS;
    PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    if((CamRead(s,0x134)|CamRead(s,0x144))&CAM_RDI_FATAL_IRQ)return STATUS_DATA_ERROR;
    ULONGLONG sof=CamRead64(s,0x12a0),eof=CamRead64(s,0x12b0);
    if(!CamRdiPairReady(c->Info.TfeBusIrq,0xc0000,sof,CamRead64(s,0x14a0),eof,CamRead64(s,0x14b0),c->PdafStamp))return STATUS_SUCCESS;
    ULONG address=CamRead(t,0x2690),slot;
    if(address==CamDmaImageAddress(c->PdafAddress.QuadPart))slot=0;
    else if(address==CamDmaImageAddress(c->PdafAddress.QuadPart+QCAM_PDAF_BYTES))slot=1;
    else return STATUS_DATA_ERROR;
    if(CamRead(t,0x2790)!=CamDmaImageAddress(c->PdafAddress.QuadPart+(slot+2)*QCAM_PDAF_BYTES+256))return STATUS_DATA_ERROR;
    c->PdafCompletedId=CamRead(s,0x1304);c->PdafCompletedSlot=slot;c->PdafCompletedSof=sof;c->PdafCompletedEof=eof;c->PdafStamp=eof;
    CamWrite(s,0x13c,8);CamWrite(s,0x14c,8);CamWrite(s,0xac,0xc0000);CamWrite(s,0x14,1);c->Info.TfeBusIrq&=~0xc0000u;
    c->PdafSlot=slot^1;
    CamWrite(t,0x2604,CamDmaImageAddress(c->PdafAddress.QuadPart+c->PdafSlot*QCAM_PDAF_BYTES));
    CamWrite(t,0x2704,CamDmaImageAddress(c->PdafAddress.QuadPart+(c->PdafSlot+2)*QCAM_PDAF_BYTES+256));
    CamCsidCommit(s,0xc00);c->PdafPending=TRUE;return STATUS_SUCCESS;
}

NTSTATUS CamPdafFrame(CAM *c) {
    NTSTATUS status=PdafReady(c);if(!NT_SUCCESS(status)||!c->PdafPending)return status;
    PUCHAR source=(PUCHAR)c->Pdaf+c->PdafCompletedSlot*QCAM_PDAF_BYTES;
    c->PdafSource=source;c->PdafCopied=FALSE;c->PdafFocusCopied=FALSE;
    c->PdafInfo.Size=sizeof(c->PdafInfo);c->PdafInfo.Abi=QCAM_ABI;
    c->PdafInfo.Width=QCAM_WIDTH;c->PdafInfo.Height=QCAM_PDAF_HEIGHT;c->PdafInfo.Stride=QCAM_STRIDE;
    c->PdafInfo.FrameBytes=QCAM_PDAF_BYTES;c->PdafInfo.Sequence++;c->PdafInfo.FrameId=c->PdafCompletedId;
    c->PdafInfo.Sof=c->PdafCompletedSof;c->PdafInfo.Eof=c->PdafCompletedEof;c->PdafPending=FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS CamPdafPrepare(CAM *c) {
    if(!c->PdafSource||!c->PdafInfo.FrameBytes)return STATUS_DEVICE_NOT_READY;
    if(c->PdafCopied||c->PdafFocusCopied)return STATUS_SUCCESS;
    if(c->FocusRoi.Flags) {
        PUCHAR source=c->PdafSource+c->FocusRoiY*QCAM_STRIDE;
        CamDmaInvalidate(source,QCAM_STRIDE,QCAM_STRIDE*8,20,0);
        CamDmaInvalidate(source+QCAM_PDAF_BYTES*2+256,QCAM_STRIDE,QCAM_STRIDE*8,20,256);
        CamPdafGatherRows(c->PdafSource,c->PdafLatest,c->FocusRoiY);c->PdafFocusCopied=TRUE;return STATUS_SUCCESS;
    }
    ULONG begin=c->FocusWindowValid?c->FocusWindow/3:0,end=c->FocusWindowValid?begin+1:3;
    for(ULONG group=begin;group<end;group++) {
        PUCHAR source=c->PdafSource+(112+group*192)*QCAM_STRIDE;
        CamDmaInvalidate(source,QCAM_STRIDE,QCAM_STRIDE*8,20,0);
        CamDmaInvalidate(source+QCAM_PDAF_BYTES*2+256,QCAM_STRIDE,QCAM_STRIDE*8,20,256);
        CamPdafGather(c->PdafSource,c->PdafLatest,group);
    }
    c->PdafFocusCopied=TRUE;return STATUS_SUCCESS;
}

NTSTATUS CamPdafCopy(CAM *c) {
    if(!c->PdafSource||!c->PdafInfo.FrameBytes)return STATUS_DEVICE_NOT_READY;
    if(c->PdafCopied)return STATUS_SUCCESS;
    CamDmaInvalidate(c->PdafSource,QCAM_PDAF_BYTES,QCAM_PDAF_BYTES,1,0);
    CamDmaInvalidate(c->PdafSource+QCAM_PDAF_BYTES*2+256,QCAM_PDAF_BYTES,QCAM_PDAF_BYTES,1,256);
    PUCHAR destination=c->PdafLatest;
    for(ULONG x=0;x<QCAM_PDAF_BYTES;x+=512) {
        RtlCopyMemory(destination+x,c->PdafSource+x,256);
        RtlCopyMemory(destination+x+256,c->PdafSource+QCAM_PDAF_BYTES*2+512+x,256);
    }
    c->PdafCopied=TRUE;return STATUS_SUCCESS;
}

BOOLEAN CamCapturePoll(CAM *c,NTSTATUS *status) {
    c->Trace.FramePollCount++;
    if(c->PipelineLimit==14)return CamIspFdPoll(c,status);
    if(c->StreamFile&&(c->Sensor==1||c->Sensor==2)&&!c->PipelineLimit&&
       (CamRead(c->Base[CamCsid],0xa4)&0x30000)!=0x30000&&(c->Trace.FramePollCount&31))return FALSE;
    CamSnapshot(c);
    if(c->PipelineLimit>=6&&c->PipelineLimit<=12) {
        RawTbu(c,c->Trace.RawTbuAfter);
        for(ULONG i=0;i<4;i++)c->Trace.SmmuGlobalAfter[i]=CamRead(c->Base[CamSmmu],i?0x4c+i*4:0x48);
        for(ULONG n=0;n<2;n++)for(ULONG i=0;i<2;i++)c->Trace.RawOutstandingSeen[n][i]|=c->Trace.RawTbuAfter[n][i+2];
        c->Trace.NocFillReads++;
        for(ULONG i=0;i<RTL_NUMBER_OF(CamNocFillRegs);i++) {
            ULONG value=CamRead(c->Base[CamNoc],CamNocFillRegs[i]);c->Trace.NocFillAfter[i]=value;
            c->Trace.NocFillPeak[i][0]=max(c->Trace.NocFillPeak[i][0],value&0x7ff);
            c->Trace.NocFillPeak[i][1]=max(c->Trace.NocFillPeak[i][1],(value>>16)&0x7f);
        }
    }
    if(c->Info.CsidRxIrq&0x25fff000u) {*status=STATUS_DATA_ERROR;return TRUE;}
    ULONG overflow=CamRead(c->Base[CamTfe],0x868),size=CamRead(c->Base[CamTfe],0x870);
    if(overflow||size) {
        c->Diag.Step=145;c->Diag.Region=CamTfe;c->Diag.Offset=overflow?0x868:0x870;
        c->Diag.Mask=MAXULONG;c->Diag.Expected=0;c->Diag.Observed=overflow?overflow:size;
        c->Diag.Address=0xac86000+c->Diag.Offset;c->Diag.Count=1;c->Diag.Slot=23;c->Diag.Phase=0;
        RtlCopyMemory(c->Diag.Resource,"tfe-wm",7);
        *status=STATUS_DATA_ERROR;return TRUE;
    }
    NTSTATUS pdafStatus=PdafReady(c);if(!NT_SUCCESS(pdafStatus)){*status=pdafStatus;return TRUE;}
    ULONG rdiIrq=CamRead(c->Base[CamCsid],0x114);
    if(rdiIrq&CAM_RDI_FATAL_IRQ){*status=STATUS_DATA_ERROR;return TRUE;}
    if(CamRdiFrameCompleteHeight(c->Info.Packets,c->StreamFile?1u:c->PipelineLimit==13?QCAM_PDAF_HEIGHT:c->Info.Height,rdiIrq,c->Info.TfeBusIrq)) {
        if(!c->PipelineLimit||c->PipelineLimit>=12) {
            if(CamRead(c->Base[CamCsid],0x124)&CAM_RDI_FATAL_IRQ){*status=STATUS_DATA_ERROR;return TRUE;}
            ULONGLONG eof0=CamRead64(c->Base[CamCsid],0xeb0),eof1=CamRead64(c->Base[CamCsid],0x10b0);
            if((c->Info.TfeBusIrq&0x30000)!=0x30000||eof0!=eof1) {
                c->Trace.PairWaitCount++;c->Trace.PairEofGap=eof0>eof1?eof0-eof1:eof1-eof0;
                c->Trace.PairEofGapMax=max(c->Trace.PairEofGapMax,c->Trace.PairEofGap);return FALSE;
            }
        }
        ULONGLONG eof=CamRead64(c->Base[CamCsid],0xeb0),sof=CamRead64(c->Base[CamCsid],0xea0);
        ULONG stamp=(ULONG)eof;
        if(c->StreamFile&&stamp==c->FrameStamp)return FALSE;
        if(c->StreamFile&&sof>eof)return FALSE;
        if(c->StreamFile) {
            ULONG dropped=0;
            if(c->FrameTime.Sequence&&sof>c->FrameTime.Sof) {
                ULONGLONG elapsed=sof-c->FrameTime.Sof,period=QCAM_SENSOR_CLOCK/(c->Sensor==1||c->Sensor==2?30:60);
                if(elapsed>period*3/2)dropped=(ULONG)((elapsed+period/2)/period-1);
            }
            c->FrameTime.Size=sizeof(c->FrameTime);c->FrameTime.Abi=QCAM_ABI;c->FrameTime.ClockHz=QCAM_SENSOR_CLOCK;
            c->FrameTime.Sequence++;c->FrameTime.Dropped+=dropped;c->FrameTime.Sof=sof;c->FrameTime.Eof=eof;
            ULONGLONG qpc;c->FrameTime.Captured=KeQueryInterruptTimePrecise(&qpc);
        }
        c->FrameStamp=stamp;
        *status=STATUS_SUCCESS;
        return TRUE;
    }
    return FALSE;
}

NTSTATUS CamCaptureStop(CAM *c) {
    if(c->TbuPending)return STATUS_DEVICE_BUSY;
    if(c->ScmPending)return STATUS_DEVICE_BUSY;
    if(c->CdmPending){NTSTATUS aborted=CamCdmAbort(c);if(!NT_SUCCESS(aborted))return aborted;}
    NTSTATUS status=STATUS_SUCCESS;
    if(c->IspFdOwned) {
        status=(c->DmaActive||c->RxActive)?CamIspFdStop(c):CamIspFdRestore(c);
        if(!NT_SUCCESS(status)&&(status!=STATUS_DATA_ERROR||
           !CamIspFdReleased(c->IspFd,c->DmaActive,c->RxActive,c->IspFdOwned)))return status;
    }
    if(c->DmaActive||c->RxActive) {
        CamSnapshot(c);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamPhyTraceRegs);i++)c->Trace.Phy[i]=CamRead(CamPhyBase(c),CamPhyTraceRegs[i]);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamCsidTraceRegs);i++)c->Trace.Csid[i]=CamRead(c->Base[CamCsid],CamCsidTraceRegs[i]);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamWriterTraceRegs);i++)c->Trace.Writer[i]=CamRead(c->Base[CamTfe],CamWriterTraceRegs[i]);
        if(c->DmaActive&&c->PipelineLimit>=6&&c->PipelineLimit<=12) {
            for(ULONG i=0;i<RTL_NUMBER_OF(CamTfeTopTraceRegs);i++)c->Trace.TfeTop[i]=CamRead(c->Base[CamTfe],CamTfeTopTraceRegs[i]);
            c->Trace.TfeTopCount=RTL_NUMBER_OF(CamTfeTopTraceRegs);
            CamBusSnapshot(c);
            RawTbu(c,c->Trace.RawTbuAfter);c->Trace.RawContextStatus[1]=RawContext(c,c->Trace.RawContextAfter);
            for(ULONG i=0;i<RTL_NUMBER_OF(CamNocFillRegs);i++)c->Trace.NocFillAfter[i]=CamRead(c->Base[CamNoc],CamNocFillRegs[i]);
        }
        for(ULONG i=0;i<RTL_NUMBER_OF(CamNocTraceRegs);i++)c->Trace.Noc[i]=CamRead(c->Base[CamNoc],CamNocTraceRegs[i]);
        UCHAR value=0;
        if(c->Sensor)CamOtherSensorSnapshot(c);
        if(!c->Sensor&&NT_SUCCESS(CamCciRead(c,0x1a,0x0100,&value,1)))c->Trace.SensorStream=value;
        if(!c->Sensor&&NT_SUCCESS(CamCciRead(c,0x1a,0x0005,&value,1)))c->Trace.SensorCounter=value;
        status=CamSensorStream(c,FALSE);
        NTSTATUS reset=CamReset(c,FALSE);
        if(!NT_SUCCESS(status)&&!NT_SUCCESS(reset))return status;
        CamDelay(1000);
        PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
        CamWrite(s,0xe04,0);CamWrite(s,0xe00,CamRead(s,0xe00)&~0x80000000u);
        if(!c->PipelineLimit||c->PipelineLimit>=12){CamWrite(s,0x1004,0);CamWrite(s,0x1000,CamRead(s,0x1000)&~0x80000000u);}
        if(c->PdafEnabled)for(ULONG i=0;i<2;i++) {
            ULONG path=0x1200+i*0x200;CamWrite(s,path+4,0);CamWrite(s,path,CamRead(s,path)&~0x80000000u);
        }
        if(c->DmaActive) {
            if(c->PdafEnabled){CamWrite(t,0x2600,0);CamWrite(t,0x2700,0);}
            CamWrite(t,0x2400,0);
            if(!c->PipelineLimit||c->PipelineLimit>=12)CamWrite(t,0x2500,0);
            NTSTATUS disabled=CamWait(t,0x2400,1,0,50000);
            if(NT_SUCCESS(disabled)&&(!c->PipelineLimit||c->PipelineLimit>=12))disabled=CamWait(t,0x2500,1,0,50000);
            for(ULONG i=0;i<2&&c->PdafEnabled&&NT_SUCCESS(disabled);i++)disabled=CamWait(t,0x2600+i*0x100,1,0,50000);
            if(!NT_SUCCESS(disabled)){c->Faulted=TRUE;return disabled;}
        }
        CamWrite(CamPhyBase(c),0x1014,0);c->PhyOn=FALSE;
        NTSTATUS idle=CamWait(s,0xe74,1,1,50000);
        if(NT_SUCCESS(idle)&&(!c->PipelineLimit||c->PipelineLimit>=12))idle=CamWait(s,0x1074,1,1,50000);
        for(ULONG i=0;i<2&&c->PdafEnabled&&NT_SUCCESS(idle);i++)idle=CamWait(s,0x1274+i*0x200,1,1,50000);
        if(!NT_SUCCESS(idle))idle=ResetCsid(c);
        if(!NT_SUCCESS(idle)){c->Faulted=TRUE;return idle;}
        idle=CamBusOff(c);if(!NT_SUCCESS(idle))return idle;
        for(ULONG i=0;i<RTL_NUMBER_OF(c->FrameMdl);i++)KeFlushIoBuffers(c->FrameMdl[i],TRUE,TRUE);KeMemoryBarrier();
        for(ULONG i=0;i<RTL_NUMBER_OF(c->PdafMdl);i++)if(c->PdafMdl[i])KeFlushIoBuffers(c->PdafMdl[i],TRUE,TRUE);KeMemoryBarrier();
        if(!c->StreamFile) {
            RtlZeroMemory(c->Trace.FrameWrittenBlocks,sizeof(c->Trace.FrameWrittenBlocks));
            for(ULONG offset=0;offset<QCAM_FRAME_BYTES;offset+=256) {
                ULONG blockValue=*(volatile ULONG *)((PUCHAR)c->Frame+offset);
                if(blockValue!=0xa5a5a5a5)c->Trace.FrameWrittenBlocks[((c->FrameAddress.QuadPart+offset)>>8)&1]++;
            }
            RtlCopyMemory(c->Trace.FrameWords,c->Frame,sizeof(c->Trace.FrameWords));
            c->Trace.FrameSampleCount=0;c->Trace.FrameNonzeroSamples=0;
            for(ULONG offset=0;offset+sizeof(ULONG)<=QCAM_FRAME_BYTES;offset+=4096) {
                c->Trace.FrameSampleCount++;
                if(*(ULONG *)((PUCHAR)c->Frame+offset))c->Trace.FrameNonzeroSamples++;
            }
        }
        c->Trace.RawDrainStatus=CamDmaDrain(c->Base[CamCpas],s,t,c->Base[CamHfTbu],c->Base[CamSfTbu],50000);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamNocFillRegs);i++)c->Trace.NocFillStopped[i]=CamRead(c->Base[CamNoc],CamNocFillRegs[i]);
        if(c->DmaActive&&!NT_SUCCESS(c->Trace.RawDrainStatus)){c->Faulted=TRUE;return c->Trace.RawDrainStatus;}
        c->DmaActive=FALSE;c->RxActive=FALSE;c->Configured=FALSE;
        KeMemoryBarrier();
        RtlCopyMemory(c->Trace.FrameWords,c->Frame,sizeof(c->Trace.FrameWords));
    }
    if(c->PhyOn){CamWrite(CamPhyBase(c),0x1014,0);c->PhyOn=FALSE;}
    NTSTATUS bus=CamBusOff(c);if(!NT_SUCCESS(bus))return bus;
    NTSTATUS unmapped=FrameUnmap(c);if(!NT_SUCCESS(unmapped))return unmapped;
    if(c->FrameRouteOwned) {
        PLATFORM_LOCK_GUARD guard={0};NTSTATUS locked=PlatformLockAcquire(&guard,TRUE);
        if(locked!=STATUS_SUCCESS)return locked;
        NTSTATUS restored=CamFrameRouteRestore(c);c->Trace.FrameRouteStatus[1]=restored;
        NTSTATUS released=PlatformLockRelease(&guard);if(!NT_SUCCESS(restored))return restored;
        if(!NT_SUCCESS(released))return released;
    }
    if(NT_SUCCESS(status)){c->StreamFile=NULL;c->PdafEnabled=FALSE;c->PdafPending=FALSE;c->PdafInfo.FrameBytes=0;c->PdafSource=NULL;}
    return status;
}
