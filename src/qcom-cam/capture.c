#include "cam.h"
#include "gdsc-cycle.h"
#include "csiphy-regs.h"
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

static NTSTATUS Allocate(CAM *c) {
    if(c->Frame)return STATUS_SUCCESS;
    NTSTATUS status;
    if(!c->DmaEnabler) {
        WDF_DMA_ENABLER_CONFIG config;
        WDF_DMA_ENABLER_CONFIG_INIT(&config,WdfDmaProfileScatterGather64,QCAM_FRAME_BYTES);
        config.AddressWidthOverride=32;
        status=WdfDmaEnablerCreate(c->Device,&config,WDF_NO_OBJECT_ATTRIBUTES,&c->DmaEnabler);
        if(!NT_SUCCESS(status))return status;
    }
    PHYSICAL_ADDRESS lowest={0},highest,boundary={0};highest.QuadPart=MAXULONG;
    c->Frame=MmAllocateContiguousMemorySpecifyCache(QCAM_FRAME_BYTES,lowest,highest,boundary,MmNonCached);
    if(!c->Frame)return STATUS_INSUFFICIENT_RESOURCES;
    c->FrameContiguous=TRUE;c->FrameAddress=MmGetPhysicalAddress(c->Frame);
    c->FrameMdl=IoAllocateMdl(c->Frame,QCAM_FRAME_BYTES,FALSE,FALSE,NULL);
    if(!c->FrameMdl)return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(c->FrameMdl);
    if(!c->FrameAddress.QuadPart||(c->FrameAddress.QuadPart&255)||
       (ULONGLONG)c->FrameAddress.QuadPart+QCAM_FRAME_BYTES-1>MAXULONG)return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

static NTSTATUS Map(CAM *c) {
    PUCHAR b=c->Base[CamSmmu];ULONG id0=CamRead(b,0x20),id1=CamRead(b,0x24),id2=CamRead(b,0x28);
    c->Trace.Smmu[0]=id0;c->Trace.Smmu[1]=id1;c->Trace.Smmu[2]=id2;
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
    if(((control>>16)&3)==1)return STATUS_SUCCESS;
    if((control&0x30000)||(id1&0x80000000u))return STATUS_NOT_SUPPORTED;
    ULONG bank=control&255,context=(1u<<(((id1>>28)&7)+1))*4096;
    if(bank>=(id1&255)||context+bank*4096>=0x100000)return STATUS_NOT_SUPPORTED;
    ULONG cbar=CamRead(b,0x1000+bank*4),sctlr=CamRead(b,context+bank*4096);
    c->Trace.SmmuState[5]=cbar;c->Trace.SmmuState[6]=sctlr;c->Diag.Phase=5;
    c->Diag.Offset=context+bank*4096;c->Diag.Mask=1;c->Diag.Expected=0;c->Diag.Observed=sctlr;c->Diag.Count=cbar;
    return CamDmaIdentity(control,cbar,sctlr)?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
}

VOID CamDmaFree(CAM *c) {
    if(c->DmaActive||c->ScmPending||c->CdmPending||c->TbuPending||c->FrameRouteOwned)return;
    if(c->CommandMdl){IoFreeMdl(c->CommandMdl);c->CommandMdl=NULL;}
    if(c->CommandBuffer){WdfObjectDelete(c->CommandBuffer);c->CommandBuffer=NULL;}
    if(c->CommandContiguous&&c->Command){MmFreeContiguousMemory(c->Command);c->CommandContiguous=FALSE;}
    c->Command=NULL;c->CommandAddress.QuadPart=0;
    if(c->FrameMdl){IoFreeMdl(c->FrameMdl);c->FrameMdl=NULL;}
    if(c->FrameBuffer){WdfObjectDelete(c->FrameBuffer);c->FrameBuffer=NULL;}
    if(c->FrameContiguous&&c->Frame){MmFreeContiguousMemory(c->Frame);c->FrameContiguous=FALSE;}
    c->Frame=NULL;
    c->FrameAddress.QuadPart=0;
}

static VOID Table(CAM *c,const PHY_REG *table,ULONG count,ULONG settle) {
    for(ULONG i=0;i<count;i++) {
        ULONG value=table[i].Value;
        if(table[i].Kind==1&&settle)value=settle&255;
        if(table[i].Kind==2&&settle)value=(settle>>8)&255;
        if(table[i].Kind==3)continue;
        CamWrite(c->Base[CamPhy],table[i].Address,value);
        if(table[i].Delay)CamDelay(table[i].Delay);
    }
}

static NTSTATUS Phy(CAM *c,const QCAM_CAPTURE_OPTIONS *o) {
    ULONG lanes=CamCphyLaneMask(0x210,3);
    if(!lanes)return STATUS_INVALID_PARAMETER;
    ULONG rate;
    for(rate=0;rate<RTL_NUMBER_OF(PhyRates);rate++)if(o->DataRate<=PhyRates[rate].Bandwidth)break;
    if(rate==RTL_NUMBER_OF(PhyRates))return STATUS_INVALID_PARAMETER;
    PUCHAR b=c->Base[CamPhy];CamWrite(b,0x1014,0);
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
    c->Info.TfeIrq|=CamRead(t,0x88);c->Info.TfeBusIrq|=CamRead(t,0x828);
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
    if(c->PipelineLimit>=6&&c->PipelineLimit<=12&&!c->ReceiverVerified)return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=Allocate(c);
    if(!NT_SUCCESS(status)){CamDmaFree(c);return status;}
    ULONG exposure=c->Info.Exposure,gain=c->Info.Gain;
    status=CamPowerOn(c);if(!NT_SUCCESS(status))return status;
    if(exposure) {status=CamSensorExposure(c,exposure,gain);if(!NT_SUCCESS(status))return status;}
    PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
    c->Info.CsidVersion=CamRead(s,0);c->Info.TfeVersion=CamRead(t,0);
    c->Diag.Step=140;c->Diag.Region=CamCsid;c->Diag.Offset=0;c->Diag.Mask=MAXULONG;
    c->Diag.Expected=0x30000006;c->Diag.Observed=c->Info.CsidVersion;
    if(c->Info.CsidVersion!=0x30000006)return STATUS_REVISION_MISMATCH;
    c->Diag.Region=CamTfe;c->Diag.Expected=0x10000000;c->Diag.Observed=c->Info.TfeVersion;
    if(c->Info.TfeVersion!=0x10000000)return STATUS_REVISION_MISMATCH;
    for(ULONG wm=0;wm<28;wm++)if(CamRead(t,0xd00+wm*0x100)&1)return STATUS_DEVICE_BUSY;
    if(CamRead(s,0xe00)&0x80000000u)return STATUS_DEVICE_BUSY;
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
    if(!c->PipelineLimit||(c->PipelineLimit>=6&&c->PipelineLimit<=12)) {
        status=CamEnsureStaticQos(c);if(!NT_SUCCESS(status))return status;
    }
    status=ResetCsid(c);if(!NT_SUCCESS(status))return status;
    status=Map(c);if(!NT_SUCCESS(status))return status;c->Info.Stage=7;
    if(c->PipelineLimit==11){status=CamFrameRoutePrepare(c);if(!NT_SUCCESS(status))return status;}
    if(c->PipelineLimit==1||c->PipelineLimit==4||c->PipelineLimit==5)return STATUS_SUCCESS;
    c->Info.Flags&=~QCAM_FRAME_VALID;c->Info.CsidIrq=0;c->Info.CsidRxIrq=0;c->Info.TfeIrq=0;c->Info.TfeBusIrq=0;
    RtlZeroMemory(c->Frame,QCAM_FRAME_BYTES);KeFlushIoBuffers(c->FrameMdl,TRUE,TRUE);
    status=Phy(c,o);if(!NT_SUCCESS(status))return status;
    if(c->PipelineLimit==2)return STATUS_SUCCESS;
    BOOLEAN diagnostic=c->PipelineLimit>=6&&c->PipelineLimit<=12;
    BOOLEAN frameMode=c->PipelineLimit>=7&&c->PipelineLimit<=11;
    BOOLEAN packed=c->PipelineLimit==12;
    if(c->PipelineLimit!=3&&!diagnostic&&!(CamRead(c->Base[CamCpas],0xf0)&1)) {
        c->Diag.Step=146;c->Diag.Region=CamCpas;c->Diag.Offset=0xf0;
        c->Diag.Mask=1;c->Diag.Expected=1;c->Diag.Observed=CamRead(c->Base[CamCpas],0xf0);
        RtlCopyMemory(c->Diag.Resource,"cpasrt",7);c->Diag.Address=0x0ac040f0;
        c->Diag.Slot=MAXULONG;c->Diag.Phase=0;c->Diag.Count=1;
        return STATUS_DEVICE_NOT_READY;
    }
    if(diagnostic&&!CamDiagnosticDmaReady(c))return STATUS_DEVICE_NOT_READY;
    if(frameMode&&(c->Trace.Csid[14]&0xffffff)!=((0x2bu<<16)|QCAM_STRIDE))return STATUS_DEVICE_CONFIGURATION_ERROR;
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
    CamWrite(s,0x400,0x01602102);CamWrite(s,0x404,0x41);
    CamWrite(s,0x408,0x02b002b5);
    if(c->PipelineLimit==3) {
        CamWrite(s,0xe04,0);CamWrite(s,0xe00,CamRead(s,0xe00)&~0x80000000u);
        c->RxActive=TRUE;c->Info.Stage=8;
        status=CamSensorStream(c,TRUE);
        if(NT_SUCCESS(status))c->Info.Stage=9;
        return status;
    }
    CamWrite(s,0xe0c,0);CamWrite(s,0xe10,frameMode||packed?0x8011:0x1811);CamWrite(s,0xe14,0);
    CamWrite(s,0xe48,0);CamWrite(s,0xe4c,1);CamWrite(s,0xe50,1);CamWrite(s,0xe54,0);
    CamWrite(s,0xe58,(QCAM_WIDTH-1)<<16);CamWrite(s,0xe5c,(QCAM_HEIGHT-1)<<16);
    CamWrite(s,0xe60,0);CamWrite(s,0xe64,0);CamWrite(s,0xe68,0);CamWrite(s,0xe6c,0);
    CamWrite(s,0xe88,3);CamWrite(s,0xe8c,(QCAM_HEIGHT<<16)|QCAM_WIDTH);
    CamWrite(s,0xe00,0x802b2260);
    CamWrite(t,0x80,0);CamWrite(t,0x818,0);CamWrite(t,0x81c,0);
    CamWrite(t,0x84,MAXULONG);CamWrite(t,0x7c,1);CamWrite(t,0x820,MAXULONG);CamWrite(t,0x824,MAXULONG);CamWrite(t,0x830,1);
    CamWrite(t,0x818,0x10000);
    CamWrite(t,0x808,0x7ffffff);CamWrite(t,0x924,0);CamWrite(t,0x858,0x1026);
    CamWrite(t,0x240c,frameMode?0xffff:(QCAM_HEIGHT<<16)|(packed?QCAM_STRIDE/16:QCAM_WIDTH));
    CamWrite(t,0x2410,0);CamWrite(t,0x2414,frameMode?0xffff:QCAM_STRIDE);
    CamWrite(t,0x2418,frameMode||packed?0:12);CamWrite(t,0x2408,diagnostic?0:CamDmaImageAddress(QCAM_FRAME_BYTES));CamWrite(t,0x2428,0);
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
    if(c->PipelineLimit>=10&&c->PipelineLimit<=11)for(ULONG i=0;i<4;i++)c->Trace.TfePrime[0][i]=CamRead(t,0x2490+i*4);
    c->DmaActive=TRUE;
    ULONGLONG captureState[8]={c->PipelineLimit,c->FrameAddress.QuadPart,
        CamRead(t,0x240c),CamRead(t,0x2418),CamRead(t,0x2400),CamRead(c->Base[CamCpas],0xf0),
        CamRead(c->Base[CamCpas],0xf8),c->StaticQosReady};
    status=CamScmAudit(c,20,captureState,STATUS_SUCCESS);if(!NT_SUCCESS(status))return status;
    CamWrite(s,0x28,0x11);CamWrite(s,0xe04,1);
    CamCsidCommit(s,0x100);
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

BOOLEAN CamCapturePoll(CAM *c,NTSTATUS *status) {
    CamSnapshot(c);
    if(c->PipelineLimit>=6&&c->PipelineLimit<=12) {
        RawTbu(c,c->Trace.RawTbuAfter);
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
    ULONG rdiIrq=CamRead(c->Base[CamCsid],0x114);
    if(rdiIrq&CAM_RDI_FATAL_IRQ){*status=STATUS_DATA_ERROR;return TRUE;}
    if(CamRdiFrameComplete(c->Info.Packets,rdiIrq,c->Info.TfeBusIrq)) {
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
    if(c->DmaActive||c->RxActive) {
        CamSnapshot(c);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamPhyTraceRegs);i++)c->Trace.Phy[i]=CamRead(c->Base[CamPhy],CamPhyTraceRegs[i]);
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
        if(NT_SUCCESS(CamCciRead(c,0x1a,0x0100,&value,1)))c->Trace.SensorStream=value;
        if(NT_SUCCESS(CamCciRead(c,0x1a,0x0005,&value,1)))c->Trace.SensorCounter=value;
        status=CamSensorStream(c,FALSE);
        NTSTATUS reset=CamReset(c,FALSE);
        if(!NT_SUCCESS(status)&&!NT_SUCCESS(reset))return status;
        CamDelay(100000);
        PUCHAR s=c->Base[CamCsid],t=c->Base[CamTfe];
        CamWrite(s,0xe04,0);CamWrite(s,0xe00,CamRead(s,0xe00)&~0x80000000u);
        if(c->DmaActive) {
            CamWrite(t,0x2400,0);
            NTSTATUS disabled=CamWait(t,0x2400,1,0,50000);
            if(!NT_SUCCESS(disabled)){c->Faulted=TRUE;return disabled;}
        }
        CamWrite(c->Base[CamPhy],0x1014,0);c->PhyOn=FALSE;
        NTSTATUS idle=CamWait(s,0xe74,1,1,50000);
        if(!NT_SUCCESS(idle))idle=ResetCsid(c);
        if(!NT_SUCCESS(idle)){c->Faulted=TRUE;return idle;}
        idle=CamBusOff(c);if(!NT_SUCCESS(idle))return idle;
        KeFlushIoBuffers(c->FrameMdl,TRUE,TRUE);
        RtlCopyMemory(c->Trace.FrameWords,c->Frame,sizeof(c->Trace.FrameWords));
        c->Trace.FrameSampleCount=0;c->Trace.FrameNonzeroSamples=0;
        for(ULONG offset=0;offset+sizeof(ULONG)<=QCAM_FRAME_BYTES;offset+=4096) {
            c->Trace.FrameSampleCount++;
            if(*(ULONG *)((PUCHAR)c->Frame+offset))c->Trace.FrameNonzeroSamples++;
        }
        ULONGLONG stoppedState[8]={CamRead(c->Base[CamCpas],0xf0),CamRead(c->Base[CamCpas],0xf8),
            c->Trace.FrameNonzeroSamples,c->Trace.FrameSampleCount,c->Trace.FrameWords[0],
            CamRead64(c->Base[CamHfTbu],0xc10),CamRead64(c->Base[CamSfTbu],0xc10),
            CamRead(c->Base[CamNoc],0x5220)};
        CamScmAudit(c,21,stoppedState,STATUS_SUCCESS);
        c->Trace.RawDrainStatus=CamDmaDrain(c->Base[CamCpas],s,t,c->Base[CamHfTbu],c->Base[CamSfTbu],50000);
        for(ULONG i=0;i<RTL_NUMBER_OF(CamNocFillRegs);i++)c->Trace.NocFillStopped[i]=CamRead(c->Base[CamNoc],CamNocFillRegs[i]);
        if(c->DmaActive&&!NT_SUCCESS(c->Trace.RawDrainStatus)){c->Faulted=TRUE;return c->Trace.RawDrainStatus;}
        c->DmaActive=FALSE;c->RxActive=FALSE;c->Configured=FALSE;
        KeFlushIoBuffers(c->FrameMdl,TRUE,TRUE);
        RtlCopyMemory(c->Trace.FrameWords,c->Frame,sizeof(c->Trace.FrameWords));
    }
    if(c->PhyOn){CamWrite(c->Base[CamPhy],0x1014,0);c->PhyOn=FALSE;}
    NTSTATUS bus=CamBusOff(c);if(!NT_SUCCESS(bus))return bus;
    if(c->FrameRouteOwned) {
        PLATFORM_LOCK_GUARD guard={0};NTSTATUS locked=PlatformLockAcquire(&guard,TRUE);
        if(locked!=STATUS_SUCCESS)return locked;
        NTSTATUS restored=CamFrameRouteRestore(c);c->Trace.FrameRouteStatus[1]=restored;
        NTSTATUS released=PlatformLockRelease(&guard);if(!NT_SUCCESS(restored))return restored;
        if(!NT_SUCCESS(released))return released;
    }
    return status;
}
