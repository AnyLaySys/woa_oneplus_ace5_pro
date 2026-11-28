#include "cam.h"
#include "isp-fd-check.h"
#include "dma-ready.h"
#include "tbu-translate.h"
#include "smmu-route-check.h"
#include "frame-map-table.h"
#include "isp-stop.h"
#include "isp-lut-read.h"
#include "isp-gamma-reference.h"
#include "isp-settle.h"
#include "isp-discard.h"
#include "../platform/lock.h"

static NTSTATUS FdLutRead(CAM *c) {
    QCAM_ISP_FD_STATE *s=c->IspFd;
    if(!s||!c->IspFdOwned||!c->Powered||!c->BusOn||c->DmaActive||c->RxActive||
       c->CdmPending||c->ScmPending||c->CrmPending||c->TbuPending||
       !CamDmaResourcesIdle(c->Base[CamCsid],c->Base[CamTfe]))return STATUS_DEVICE_BUSY;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,FALSE);
    if(status!=STATUS_SUCCESS)return status;
    PUCHAR t=c->Base[CamTfe];
    /* The bounded read restores these access registers before releasing
       this lock. Never put TFE DMI access in CamSave: CamPowerOff retires
       CRM performance votes before replaying that generic stack. */
    s->LutReadRestoreStatus=0;
    if(NT_SUCCESS(status)&&s->GammaLoaded) {
        CAM_ISP_GAMMA gamma;ULONG points[65];
        for(ULONG i=0;i<65;i++)points[i]=min(i*16,1023u);
        if(!(s->Profile>=15?CamIspGammaNativeReference(&gamma,s->GammaBank):
             CamIspGammaBuild(&gamma,points,s->GammaBank)))status=STATUS_INVALID_PARAMETER;
        if(NT_SUCCESS(status))for(ULONG channel=0;channel<3;channel++) {
            ULONG count=0,mismatch=0;NTSTATUS restored;
            NTSTATUS read=CamIspLutRead(t,FALSE,channel+1,s->GammaBank,gamma.Table,
                s->GammaRead[channel],&count,&mismatch,&restored);
            s->GammaReadCount+=count;s->GammaReadMismatch+=mismatch;
            if(!NT_SUCCESS(restored)){s->LutReadRestoreStatus=(ULONG)restored;status=restored;break;}
            if(!NT_SUCCESS(read))status=read;
            if(read!=STATUS_SUCCESS&&read!=STATUS_DATA_ERROR)break;
        }
    }
    if((NT_SUCCESS(status)||status==STATUS_DATA_ERROR)&&s->GtmLoaded) {
        CAM_ISP_GTM gtm;ULONG expected[512];
        if(!CamIspGtmNativeReference(&gtm,s->GtmBank))status=STATUS_INVALID_PARAMETER;
        else {
            for(ULONG i=0;i<256;i++){expected[i*2]=(ULONG)gtm.Table[i];expected[i*2+1]=(ULONG)(gtm.Table[i]>>32);}
            NTSTATUS restored;
            NTSTATUS read=CamIspLutRead(t,TRUE,1,s->GtmBank,expected,s->GtmRead,
                &s->GtmReadCount,&s->GtmReadMismatch,&restored);
            if(!NT_SUCCESS(restored)){s->LutReadRestoreStatus=(ULONG)restored;status=restored;}
            else if(!NT_SUCCESS(read))status=read;
        }
    }
    if(s->LutReadRestoreStatus)c->Faulted=TRUE;
    s->LutReadStatus=(ULONG)status;
    NTSTATUS released=PlatformLockRelease(&guard);
    if(NT_SUCCESS(status))status=released;
    return status;
}

static VOID FdFailure(CAM *c,NTSTATUS status) {
    QCAM_ISP_FD_STATE *state=c->IspFd;
    if(!state)return;
    state->Status=(ULONG)status;
    for(ULONG i=0;i<state->Count;i++)if(state->Configured[i]!=state->Expected[i]) {
        c->Diag.Step=191;c->Diag.Region=state->Region[i];c->Diag.Offset=state->Offset[i];
        c->Diag.Expected=state->Expected[i];c->Diag.Observed=state->Configured[i];c->Diag.Mask=MAXULONG;
        c->Diag.Address=(state->Region[i]==CamCsid?0x0ad2d000ULL:0x0aca6000ULL)+state->Offset[i];
        c->Diag.Count=i;c->Diag.Status=status;RtlCopyMemory(c->Diag.Resource,"isp-fd",7);break;
    }
}

static NTSTATUS FdTranslate(CAM *c) {
    QCAM_ISP_FD_STATE *state=c->IspFd;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    PUCHAR base=c->Base[CamSmmu];ULONG slot=MAXULONG,control=0;
    for(ULONG i=0;i<128;i++)if(CamRead(base,0x800+i*4)==0x80001c00) {
        if(slot!=MAXULONG){status=STATUS_DEVICE_BUSY;goto done;}
        slot=i;control=CamRead(base,0xc00+i*4);
    }
    if(!CamSmmuExactRoute(base,0x1c00,slot,control)){status=STATUS_DEVICE_BUSY;goto done;}
    /* No SMMU route is altered. Verify both ends of both planes through
       the HF TBU before a writer can own the allocation. */
    for(ULONG frame=0;frame<=state->WarmupFrames;frame++) {
        CAM_ISP_NV12_OUTPUT layout;ULONG displacement;
        if(!CamIspFdFrameLayout(&layout,state,frame,&displacement)){status=STATUS_INVALID_PARAMETER;goto done;}
        ULONG luma=state->Stride*state->Height;
        const ULONG offsets[]={0,luma-256,luma,state->ImageBytes-256};
        for(ULONG i=0;i<4;i++) {
            ULONGLONG address=c->FrameAddress.QuadPart+displacement+offsets[i];
            ULONGLONG *translation=frame<state->WarmupFrames?state->WarmupTranslation[frame][i]:state->Translation[i];
            c->Diag.Step=192;c->Diag.Region=CamHfTbu;c->Diag.Address=(ULONG)address;c->Diag.Count=frame*4+i;
            c->TbuPending=TRUE;
            status=CamTbuTranslateExpected(c->Base[CamHfTbu],0x1c00,address,
                MmGetPhysicalAddress((PUCHAR)c->Frame+displacement+offsets[i]).QuadPart,0x409,translation);
            if(status!=STATUS_DEVICE_BUSY)c->TbuPending=FALSE;
            if(!NT_SUCCESS(status))goto done;
            if(!CamSmmuExactRoute(base,0x1c00,slot,control)){status=STATUS_DEVICE_BUSY;goto done;}
        }
    }
    state->Stage|=4;
done:
    c->Diag.Status=status;
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}

NTSTATUS CamIspFdConfigure(CAM *c,QCAM_ISP_FD_STATE *output) {
    if(!c->Powered||!c->BusOn||!c->Frame||c->IspFdOwned||c->DmaActive||c->RxActive||
       c->StreamFile||c->CdmPending||c->ScmPending||c->TbuPending)return STATUS_DEVICE_BUSY;
    if(!c->IspFd)c->IspFd=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*c->IspFd),'fIcQ');
    if(!c->IspFd)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG bufferOffset=c->IspFdAddressOffset;
    if(bufferOffset!=0&&bufferOffset!=128&&bufferOffset!=256)return STATUS_INVALID_PARAMETER;
    ULONGLONG address=c->FrameAddress.QuadPart+bufferOffset;
    ULONG capacity=QCAM_FRAME_BYTES-bufferOffset;
    BOOLEAN built=c->IspFdPrecisionChain?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,c->IspFdBds2?23:22):c->IspFdLumaProfile?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,c->IspFdLumaProfile):c->IspFdCbLuma?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,17):c->IspFdInverseLuma?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,18):(c->IspFdGtm&&c->IspFdGamma)?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,c->IspFdBds2?16:15):c->IspFdGtm?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,14):c->IspFdBds2?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,13):c->IspFdLumaBias?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,12):c->IspFdBlsBypass?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,10):c->IspFdWbBypass?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,11):c->IspFdCcBypass?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,9):c->IspFdGamma?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,8):c->IspFdCompand?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,7):c->IspFdCstBias?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,6):c->IspFdChroma?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,5):c->IspFdConstant?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,4):c->IspFdByte?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,3):c->IspFdExact?
        CamIspFdBuildMode(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,2):c->IspFdHalf?
        CamIspFdBuildProfile(c->IspFd,c->Info.Width,c->Info.Height,address,capacity,TRUE):
        CamIspFdBuild(c->IspFd,c->Info.Width,c->Info.Height,address,capacity);
    if(!built)
        return STATUS_INVALID_PARAMETER;
    c->IspFd->WarmupFrames=c->IspFdSettled?3u:0u;
    c->IspFd->DiscardFrames=c->IspFdDiscard?3u:0u;
    c->IspFd->DiscardStatus=c->IspFdDiscard?(ULONG)STATUS_PENDING:0;
    c->IspFd->MemoryCached=!c->FrameUncached;
    c->IspFd->LowPhysical=c->FrameLow;
    c->IspFd->BufferOffset=bufferOffset;
    c->IspFd->PhysicalAddress=MmGetPhysicalAddress((PUCHAR)c->Frame+bufferOffset).QuadPart;
    c->IspFd->CpuMemoryAttribute=(ULONG)(c->Trace.FrameCpuPar>>56);
    if(c->IspFdCommonBuffer) {
        if(!c->FrameBuffer||c->FrameMapped||c->FrameContextOwned)return STATUS_DEVICE_NOT_READY;
        /* Framework-owned PTEs/MAIR are not read or rewritten. Zero fields
           mean unmeasured, not a claim about the adapter's mapping attributes. */
        c->IspFd->MappingLeafFlags=c->IspFd->MappingMair=c->IspFd->DeviceMemoryAttribute=0;
    } else {
        if(!c->FrameMapped||!c->FrameTables||c->FrameMapBank>=16)return STATUS_DEVICE_NOT_READY;
        c->IspFd->MappingLeafFlags=(ULONG)((PULONGLONG)c->FrameTables)[1025]&0xfff;
        c->IspFd->MappingMair=CamRead(c->Base[CamSmmu],0x80038+c->FrameMapBank*4096);
        ULONG memoryIndex=(c->IspFd->MappingLeafFlags>>2)&7;
        if(memoryIndex>1)return STATUS_DEVICE_CONFIGURATION_ERROR;
        c->IspFd->DeviceMemoryAttribute=(c->IspFd->MappingMair>>(memoryIndex*8))&255;
        if(c->IspFdCoherent&&!CamFrameMapCoherentReady(c->IspFd->MappingLeafFlags,c->IspFd->MappingMair,c->IspFd->CpuMemoryAttribute))return STATUS_DEVICE_NOT_READY;
    }
    NTSTATUS status=CamIspPixelClocksOn(c);
    if(!NT_SUCCESS(status)) {
        c->IspFd->Status=(ULONG)status;
        if(output)RtlCopyMemory(output,c->IspFd,sizeof(*output));
        return status;
    }
    status=CamIspFdApply(c->Base[CamCsid],c->Base[CamTfe],c->IspFd);
    c->IspFdOwned=(c->IspFd->Stage&2)!=0;
    if(NT_SUCCESS(status)&&!CamControlDmaReady(c))status=STATUS_DEVICE_NOT_READY;
    if(NT_SUCCESS(status))status=FdTranslate(c);
    if(NT_SUCCESS(status)&&c->IspFdGamma) {
        ULONG bank;
        status=CamIspFdGammaBank(c->IspFd,&bank)?CamCdmGammaLoad(c,bank):STATUS_NOT_SUPPORTED;
        if(NT_SUCCESS(status)&&!CamIspFdGammaConfigured(c->IspFd))status=STATUS_IO_DEVICE_ERROR;
    }
    if(NT_SUCCESS(status)&&c->IspFdGtm) {
        status=CamCdmGtmLoad(c,c->IspFd->GtmBank);
    }
    if(NT_SUCCESS(status)&&(c->IspFdGamma||c->IspFdGtm))status=FdLutRead(c);
    if(!NT_SUCCESS(status))FdFailure(c,status);
    if(output)RtlCopyMemory(output,c->IspFd,sizeof(*output));
    return status;
}

NTSTATUS CamIspFdRestore(CAM *c) {
    if(!c->IspFdOwned)return STATUS_SUCCESS;
    if(c->DmaActive||c->RxActive||c->CdmPending||c->ScmPending||c->TbuPending)return STATUS_DEVICE_BUSY;
    /* Retain ISP ownership and its clocks if immediate DMI restoration
       failed. Teardown cannot repair access state after dropping votes. */
    if(c->IspFd->LutReadRestoreStatus)return (NTSTATUS)c->IspFd->LutReadRestoreStatus;
    NTSTATUS status=CamIspFdRestoreRegisters(c->Base[CamCsid],c->Base[CamTfe],c->IspFd);
    if(NT_SUCCESS(status))c->IspFdOwned=FALSE;
    else c->Faulted=TRUE;
    return status;
}

NTSTATUS CamIspFdStart(CAM *c) {
    QCAM_ISP_FD_STATE *state=c->IspFd;
    if(!c->IspFdOwned||!state||(state->Stage&7)!=7||!c->ReceiverVerified||
       !CamIspPixelDmaReady(c)||!CamDmaResourcesIdle(c->Base[CamCsid],c->Base[CamTfe]))return STATUS_DEVICE_NOT_READY;
    if((c->IspFdGamma&&(state->GammaReadCount!=192||state->GammaReadMismatch))||
       (c->IspFdGtm&&(state->GtmReadCount!=512||state->GtmReadMismatch))||
       ((c->IspFdGamma||c->IspFdGtm)&&(state->LutReadStatus||state->LutReadRestoreStatus)))return STATUS_DEVICE_NOT_READY;
    PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    /* Native source errors and FD composite completion are separate from
       RDI completion. Keep interrupts masked; bounded WaitFrame polls them. */
    CamWrite(s,0xd8,0);CamWrite(s,0xdc,MAXULONG);CamWrite(s,0x14,1);
    CamWrite(t,0x80,0);CamWrite(t,0x818,0);CamWrite(t,0x81c,0);
    CamWrite(t,0x84,MAXULONG);CamWrite(t,0x7c,1);
    CamWrite(t,0x820,MAXULONG);CamWrite(t,0x824,MAXULONG);CamWrite(t,0x830,1);
    /* TFE980 routes buffer-done through CSID, not its local bus-error IRQ.
       Keep top IRQs masked while polling the native FD composite bit3. */
    CamWrite(s,0xac,MAXULONG);CamWrite(s,0x14,1);CamWrite(s,0xa8,CAM_ISP_FD_DONE);
    CamWrite(t,0x808,0x7ffffff);
    CAM_ISP_NV12_OUTPUT layout;ULONG displacement;
    if(!CamIspFdFrameLayout(&layout,state,0,&displacement))return STATUS_INVALID_PARAMETER;
    c->DmaActive=TRUE;state->Stage|=8;
    state->StartSof=CamRead64(s,0x6a8);state->StartEof=CamRead64(s,0x6b8);
    state->DrainStatus=state->RestoreStatus=state->StopStatus=state->ResetStatus=(ULONG)STATUS_PENDING;
    for(ULONG plane=0;plane<2;plane++) {
        CamWrite(t,0x1404+plane*0x100,layout.ImageAddress[plane]);
        CamWrite(t,0x1400+plane*0x100,1);
    }
    if(CamRead(t,0x1400)!=1||CamRead(t,0x1500)!=1)return STATUS_IO_DEVICE_ERROR;
    CamWrite(s,0x600,CamRead(s,0x600)|0x80000000u);CamWrite(s,0x28,0x11);
    CamWrite(s,0x604,state->DiscardFrames?0u:1u);CamCsidCommit(s,1);c->RxActive=TRUE;
    c->Info.Stage=8;NTSTATUS status=CamSensorStream(c,TRUE);
    if(NT_SUCCESS(status))c->Info.Stage=9;
    return status;
}

static VOID FdSnapshot(CAM *c) {
    QCAM_ISP_FD_STATE *state=c->IspFd;PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    /* Original Apollo980 PDPC/ABF/LSC/FCG module emitters provide these
       read-only control/context addresses. No access to their DMI ports. */
    static const ULONG nativeModules[]={0xd458,0xd45c,0xd460,0xd858,0xd85c,0xd860,
        0xdc58,0xdc5c,0xdc60,0xde58,0xde5c,0xde60,0xd5f4,0xd9f4,0xddf4,0xdff4};
    for(ULONG i=0;i<RTL_NUMBER_OF(nativeModules);i++)state->NativeModuleControls[i]=CamRead(t,nativeModules[i]);
    state->BusIrq=CamRead(s,0xa4);state->BusErrorIrq=CamRead(t,0x828);
    state->IppIrq=CamRead(s,0xd4);state->TopIrq=CamRead(t,0x88);
    state->RxIrq=CamRead(s,0xb4);state->YAddress=CamRead(t,0x1490);state->CAddress=CamRead(t,0x1590);
    state->Overflow=CamRead(t,0x868);state->SizeViolation=CamRead(t,0x870);
    state->PixelViolation=CamRead(t,0x90);state->BayerViolation=CamRead(t,0xc024);
    state->Sof=CamRead64(s,0x6a8);state->Eof=CamRead64(s,0x6b8);
    state->PreviousSof=CamRead64(s,0x6b0);state->PreviousEof=CamRead64(s,0x6c0);
    const ULONG registers[]={0x600,0x604,0x638,0x648,0x64c,0x650,0x654,0x690,0x694,0x69c,0x6a0,0x714};
    for(ULONG i=0;i<12;i++)state->Snapshot[i]=CamRead(s,registers[i]);
    for(ULONG i=0;i<17;i++)state->Snapshot[12+i]=CamRead(t,0x190+i*4);
    state->Snapshot[29]=CamRead(t,0xa0);state->Snapshot[30]=CamRead(t,0x1480);state->Snapshot[31]=CamRead(t,0x1580);
    state->WmConstraints[0]=CamRead(t,0x1484);state->WmConstraints[1]=CamRead(t,0x1584);
    for(ULONG i=0;i<4;i++)state->Idle[i]=CamRead(t,0x1d4+i*4);
    for(ULONG plane=0;plane<2;plane++)for(ULONG i=0;i<4;i++)
        state->WriterAddressStatus[plane][i]=CamRead(t,0x1490+plane*0x100+i*4);
    if(c->FrameMapped&&c->FrameMapBank<16) {
        PUCHAR mmu=c->Base[CamSmmu];ULONG context=0x80000+c->FrameMapBank*4096;
        const ULONG offsets[]={0x58,0x60,0x64,0x68,0x6c};
        for(ULONG i=0;i<5;i++)state->SmmuFault[i]=CamRead(mmu,context+offsets[i]);
        state->SmmuFault[5]=CamRead(mmu,0xc00+c->FrameRoute[0]*4);
        state->SmmuFault[6]=CamRead(mmu,0x800+c->FrameRoute[0]*4);
        state->SmmuFault[7]=c->FrameMapBank;
    }
}

BOOLEAN CamIspFdPoll(CAM *c,NTSTATUS *status) {
    QCAM_ISP_FD_STATE *state=c->IspFd;
    if(!state||!c->DmaActive){*status=STATUS_DEVICE_NOT_READY;return TRUE;}
    FdSnapshot(c);
    if((state->IppIrq&CAM_ISP_IPP_FATAL)||(state->RxIrq&0x25fff000u)||
       (state->TopIrq&0xf000004)||state->Overflow||state->SizeViolation) {
        c->Diag.Step=193;c->Diag.Region=state->IppIrq&CAM_ISP_IPP_FATAL?CamCsid:CamTfe;
        c->Diag.Offset=c->Diag.Region==CamCsid?0xd4:state->SizeViolation?0x870:state->Overflow?0x868:0x90;
        c->Diag.Observed=CamRead(c->Base[c->Diag.Region],c->Diag.Offset);c->Diag.Expected=0;c->Diag.Mask=MAXULONG;
        c->Diag.Address=(c->Diag.Region==CamCsid?0x0ad2d000u:0x0aca6000u)+c->Diag.Offset;
        c->Diag.Count=state->PixelViolation;RtlCopyMemory(c->Diag.Resource,"isp-fd",7);
        *status=STATUS_DATA_ERROR;state->Status=(ULONG)*status;return TRUE;
    }
    if(state->DiscardFrames&&state->DiscardSofCount<state->DiscardFrames) {
        if(!c->IspFdOwned||!c->RxActive||c->CdmPending||c->ScmPending||c->TbuPending||c->CrmPending) {
            *status=STATUS_DEVICE_NOT_READY;state->Status=(ULONG)*status;return TRUE;
        }
        NTSTATUS discarded=CamIspFdDiscardPoll(c->Base[CamCsid],c->Base[CamTfe],state,KeQueryInterruptTime());
        state->DiscardStatus=(ULONG)discarded;
        if(!NT_SUCCESS(discarded)){*status=discarded;state->Status=(ULONG)*status;return TRUE;}
        if(discarded==STATUS_SUCCESS) {
            state->StartSof=CamRead64(c->Base[CamCsid],0x6a8);
            state->StartEof=CamRead64(c->Base[CamCsid],0x6b8);
        }
        return FALSE;
    }
    CAM_ISP_NV12_OUTPUT layout;ULONG displacement;
    if(!CamIspFdFrameLayout(&layout,state,state->CompletedFrames,&displacement)) {
        *status=STATUS_INVALID_PARAMETER;return TRUE;
    }
    ULONGLONG sof=CamIspFdFrameSof(state->Sof,state->PreviousSof,state->Eof,state->StartSof,state->StartEof);
    if(!CamIspFdFrameReady(state->BusIrq,state->IppIrq,sof,state->Eof,state->StartEof,state->YAddress,state->CAddress,&layout))return FALSE;
    state->FrameSof=sof;state->FrameEof=state->Eof;
    state->Stage|=16;
    if(!CamIspFdFrameRegisters(c->Base[CamCsid],c->Base[CamTfe],state)||state->AtFrameMismatch) {
        *status=STATUS_DATA_ERROR;state->Status=(ULONG)*status;return TRUE;
    }
    if(state->CompletedFrames<state->WarmupFrames) {
        /* Requeue only after fresh paired completion/EOF and exact live
           controls. No CPU access to any active or discarded pixel buffer. */
        if(!c->IspFdOwned||!c->RxActive||c->CdmPending||c->ScmPending||c->TbuPending||c->CrmPending) {
            *status=STATUS_DEVICE_NOT_READY;state->Status=(ULONG)*status;return TRUE;
        }
        NTSTATUS queued=CamIspFdRequeueWarmup(c->Base[CamCsid],c->Base[CamTfe],state);
        if(!NT_SUCCESS(queued)) {
            *status=queued;state->Status=(ULONG)*status;return TRUE;
        }
        return FALSE;
    }
    state->CompletedFrames++;
    /* Bounded raw observations before sensor/bus shutdown. Only the
       explicitly uncached diagnostic allocation has this visibility claim;
       the production path gains no CPU pixel processing or cache operation. */
    if(c->FrameUncached) {
        KeMemoryBarrier();
        for(ULONG plane=0;plane<2;plane++)for(ULONG block=0;block<2;block++) {
            volatile ULONG *sample=(volatile ULONG *)((PUCHAR)c->Frame+state->BufferOffset+
                (plane?state->Stride*state->Height:0)+block*256);
            for(ULONG i=0;i<8;i++){state->AtFrameSamples[plane][block][i]=sample[i];state->AtFrameSampleCount++;}
        }
    }
    *status=STATUS_SUCCESS;return TRUE;
}

static NTSTATUS FdStop(CAM *c) {
    QCAM_ISP_FD_STATE *state=c->IspFd;
    if(!state||!c->IspFdOwned)return STATUS_DEVICE_NOT_READY;
    PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    if(!(state->Stage&16))FdSnapshot(c);
    CamWrite(s,0xa8,0);
    NTSTATUS sensor=CamSensorStream(c,FALSE),reset=CamReset(c,FALSE);
    if(!NT_SUCCESS(sensor)&&!NT_SUCCESS(reset))return sensor;
    CamDelay(1000);CamWrite(s,0x604,0);CamWrite(s,0x600,CamRead(s,0x600)&~0x80000000u);
    CamWrite(t,0x1400,0);CamWrite(t,0x1500,0);
    NTSTATUS status=CamWait(t,0x1400,1,0,50000);
    if(NT_SUCCESS(status))status=CamWait(t,0x1500,1,0,50000);
    if(!NT_SUCCESS(status)){c->Faulted=TRUE;return status;}
    CamWrite(CamPhyBase(c),0x1014,0);c->PhyOn=FALSE;
    status=CamIspFdHalt(s,t,&state->ResetStatus);
    if(!NT_SUCCESS(status)){c->Faulted=TRUE;return status;}
    status=CamBusOff(c);if(!NT_SUCCESS(status))return status;
    state->DrainStatus=(ULONG)CamDmaDrain(c->Base[CamCpas],s,t,c->Base[CamHfTbu],c->Base[CamSfTbu],50000);
    if(!NT_SUCCESS((NTSTATUS)state->DrainStatus)){c->Faulted=TRUE;return (NTSTATUS)state->DrainStatus;}
    state->Stage|=64;
    KeFlushIoBuffers(c->FrameGuardMdl,TRUE,TRUE);
    for(ULONG i=0;i<RTL_NUMBER_OF(c->FrameMdl);i++)KeFlushIoBuffers(c->FrameMdl[i],TRUE,TRUE);
    KeMemoryBarrier();
    state->GuardChanged=state->ImageChanged=0;
    for(ULONG i=0;i<4096;i++)if(((PUCHAR)c->FrameAllocation)[i]!=0xa5)state->GuardChanged++;
    CAM_ISP_NV12_COVERAGE coverage;
    if(!CamIspNv12Coverage(&coverage,(PUCHAR)c->Frame+state->BufferOffset,state->ImageBytes,state->Stride,state->Height))return STATUS_INVALID_PARAMETER;
    state->ImageChanged=coverage.Changed[0]+coverage.Changed[1];
    RtlCopyMemory(state->SignalMinimum,coverage.WrittenMinimum,sizeof(coverage.WrittenMinimum));
    RtlCopyMemory(state->SignalMaximum,coverage.WrittenMaximum,sizeof(coverage.WrittenMaximum));
    RtlCopyMemory(state->PlaneChanged,coverage.Changed,sizeof(coverage.Changed));
    RtlCopyMemory(state->FirstChangedRow,coverage.FirstRow,sizeof(coverage.FirstRow));
    RtlCopyMemory(state->LastChangedRow,coverage.LastRow,sizeof(coverage.LastRow));
    RtlCopyMemory(state->ChangedBands,coverage.Bands,sizeof(coverage.Bands));
    RtlCopyMemory(state->ChangedBlockParity,coverage.BlockParity,sizeof(coverage.BlockParity));
    for(ULONG i=0;i<state->BufferOffset;i++)if(((PUCHAR)c->Frame)[i]!=0xa5)state->GuardChanged++;
    for(ULONG i=0;i<4096;i++)if(((PUCHAR)c->Frame)[state->BufferOffset+state->ImageBytes+i]!=0xa5)state->GuardChanged++;
    for(ULONG frame=0;frame<state->WarmupFrames;frame++) {
        CAM_ISP_NV12_OUTPUT layout;ULONG displacement;
        if(!CamIspFdFrameLayout(&layout,state,frame,&displacement))return STATUS_INVALID_PARAMETER;
        for(ULONG i=0;i<4096;i++) {
            if(((PUCHAR)c->Frame)[displacement-4096+i]!=0xa5)state->GuardChanged++;
            if(((PUCHAR)c->Frame)[displacement+state->ImageBytes+i]!=0xa5)state->GuardChanged++;
        }
    }
    c->DmaActive=FALSE;c->RxActive=FALSE;c->Configured=FALSE;
    status=CamIspFdRestore(c);
    if(NT_SUCCESS(status)&&state->GuardChanged)status=STATUS_DATA_ERROR;
    if(NT_SUCCESS(status)&&(state->Stage&16)&&!state->ImageChanged)status=STATUS_DATA_ERROR;
    if(NT_SUCCESS(status)&&(state->Stage&16)&&state->CompletedFrames!=state->WarmupFrames+1)status=STATUS_DATA_ERROR;
    if(NT_SUCCESS(status)&&(state->Stage&16)&&state->DiscardFrames&&
       (state->DiscardSofCount!=state->DiscardFrames||state->DiscardStatus))status=STATUS_DATA_ERROR;
    if(NT_SUCCESS(status)&&(state->Stage&16)&&!CamIspNv12CoverageValid(&coverage))status=STATUS_DATA_ERROR;
    if(NT_SUCCESS(status)&&(state->Stage&16))for(ULONG plane=0;plane<2;plane++)
        if(!state->ChangedBlockParity[plane][0]||!state->ChangedBlockParity[plane][1])status=STATUS_DATA_ERROR;
    if(NT_SUCCESS(status)&&(state->Stage&16)&&!CamIspFdSignalValid(state->Profile,state->SignalMinimum[0],state->SignalMaximum[0]))status=STATUS_DATA_ERROR;
    return status;
}

NTSTATUS CamIspFdStop(CAM *c) {
    NTSTATUS status=FdStop(c);
    if(c->IspFd)c->IspFd->StopStatus=(ULONG)status;
    return status;
}
