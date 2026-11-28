#include "cam.h"
#include "dma-route.h"
#include "dma-ready.h"
#include "cdm-route.h"
#include "cdm-command.h"
#include "cdm-abort.h"
#include "trace-regs.h"
#include "smmu-fault.h"
#include "tbu-translate.h"
#include "scm-ready.h"
#include "scm-qos-payload.h"
#include "isp-dmi.h"
#include "isp-gtm-reference.h"
#include "isp-gamma-reference.h"
#include "dma-idle.h"
#include "smmu-route-check.h"
#include "../platform/lock.h"

VOID CamScmOnce(ULONGLONG *registers);
VOID CamScmSlow(ULONGLONG *registers);

static NTSTATUS NocFaults(CAM *c,ULONG function) {
    if(c->ScmPending||c->CdmPending||c->TbuPending||c->CrmPending||c->DmaActive||c->RxActive)return STATUS_DEVICE_BUSY;
    if(!c->Powered)return STATUS_DEVICE_NOT_READY;
    c->ScmNocReadable=FALSE;
    c->Trace.ScmIoFunction=function;
    RtlZeroMemory(c->Trace.NocErrors,sizeof(c->Trace.NocErrors));
    RtlZeroMemory(c->Trace.NocErrorCount,sizeof(c->Trace.NocErrorCount));
    for(ULONG n=0;n<2;n++)c->Trace.NocErrorStatus[n]=STATUS_NOT_SUPPORTED;
    if(!c->ScmIoKnown) {
        ULONGLONG registers[8]={0x42000601,1,0x02000501};
        c->Diag.Step=179;c->Diag.Region=MAXULONG;c->Diag.Address=(ULONG)registers[0];c->Diag.Offset=0x501;
        c->Diag.Phase=0;c->Diag.Count=1;c->Diag.Mask=MAXULONG;c->Diag.Expected=0;RtlCopyMemory(c->Diag.Resource,"scm-noc",8);
        c->ScmPending=TRUE;CamScmSlow(registers);
        c->Diag.Observed=(ULONG)registers[0];
        RtlCopyMemory(c->Trace.ScmIoCapability,registers,sizeof(registers));
        c->ScmPending=registers[0]==1||registers[0]==2||registers[0]==3;
        if(c->ScmPending)return STATUS_DEVICE_BUSY;
        if(registers[0])return STATUS_NOT_SUPPORTED;
        c->ScmIoKnown=TRUE;c->ScmIoAvailable=registers[1]==1;
    }
    if(!c->ScmIoAvailable)return STATUS_NOT_SUPPORTED;
    const ULONG bases[]={0xad90000,0xac62000},offsets[]={0x10,0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x3c};
    NTSTATUS result=STATUS_SUCCESS;
    for(ULONG n=0;n<2;n++)for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++) {
        ULONGLONG registers[8]={function,1,(ULONGLONG)bases[n]+offsets[i]};
        c->Diag.Step=180;c->Diag.Region=MAXULONG;c->Diag.Address=(ULONG)registers[2];c->Diag.Offset=offsets[i];
        c->Diag.Phase=n+1;c->Diag.Count=i;c->Diag.Mask=MAXULONG;c->Diag.Expected=0;RtlCopyMemory(c->Diag.Resource,"scm-noc",8);
        c->ScmPending=TRUE;CamScmSlow(registers);
        c->Diag.Observed=(ULONG)registers[0];
        RtlCopyMemory(c->Trace.ScmIoReturn,registers,sizeof(registers));
        c->ScmPending=registers[0]==1||registers[0]==2||registers[0]==3;
        NTSTATUS status=c->ScmPending?STATUS_DEVICE_BUSY:(LONG)registers[0]==-2?STATUS_INVALID_PARAMETER:
            registers[0]?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
        c->Trace.NocErrorStatus[n]=status;
        if(c->ScmPending)return status;
        if(!NT_SUCCESS(status)){result=status;break;}
        c->Trace.NocErrors[n][i]=(ULONG)registers[1];c->Trace.NocErrorCount[n]++;
        if(!i)c->ScmNocReadable=TRUE;
        if(!i&&!(registers[1]&1))break;
    }
    return result;
}

NTSTATUS CamScmNocFaults(CAM *c) {return NocFaults(c,c->Trace.ScmIoFunction?c->Trace.ScmIoFunction:0xc2000501);}
NTSTATUS CamScmNocFaults32(CAM *c) {return NocFaults(c,0x82000501);}

static NTSTATUS QosEntries(CAM *c,ULONG kind) {
    if(c->CdmPending||c->DmaActive||c->RxActive||c->ScmPending||c->CrmPending||c->TbuPending)return STATUS_DEVICE_BUSY;
    if(!c->Powered||!CamScmDiagnosticReady(c)||!c->Frame||!c->FrameMdl[0])return STATUS_DEVICE_NOT_READY;
    PHYSICAL_ADDRESS physical=MmGetPhysicalAddress(c->Frame);
    if(physical.QuadPart!=c->FrameAddress.QuadPart||!physical.QuadPart||
       (ULONGLONG)physical.QuadPart+CAM_SCM_RDI_BYTES-1>MAXULONG)return STATUS_DEVICE_CONFIGURATION_ERROR;
    NTSTATUS status=c->ScmAvailable?STATUS_SUCCESS:CamScmProbe(c);
    if(!NT_SUCCESS(status))return status;
    if(!c->ScmWaitqKnown) {
        status=CamScmWaitqProbe(c);
        if(!NT_SUCCESS(status)&&status!=STATUS_NOT_SUPPORTED)return status;
    }
    ULONG payloadBytes=kind==2?CAM_SCM_CDM_BYTES:kind==1?CAM_SCM_RDI_BYTES:CAM_SCM_SMART_BYTES;
    c->Info.Flags&=~QCAM_FRAME_VALID;RtlZeroMemory(c->Frame,CAM_SCM_RDI_BYTES);
    if(kind==2)CamScmCdmEncode(c->Frame);else if(kind==1)CamScmRdiEncode(c->Frame);else CamScmSmartEncode(c->Frame);
    for(ULONG i=0;i<RTL_NUMBER_OF(c->FrameMdl);i++)KeFlushIoBuffers(c->FrameMdl[i],FALSE,TRUE);KeMemoryBarrier();
    RtlCopyMemory(c->Trace.ScmSmartPayload,c->Frame,CAM_SCM_RDI_BYTES);c->Trace.ScmPayloadBytes=payloadBytes;
    c->Trace.ScmSmartAddress=physical.QuadPart;
    ULONGLONG registers[8]={0x4200180a,0x83,1,(ULONGLONG)physical.QuadPart,payloadBytes};
    c->Diag.Step=kind==2?181:kind==1?178:177;c->Diag.Region=MAXULONG;c->Diag.Address=(ULONG)registers[0];
    c->Diag.Offset=0;c->Diag.Count=3;c->Diag.Phase=1;RtlCopyMemory(c->Diag.Resource,"scm-smrt",8);
    RtlCopyMemory(c->ScmResumeArgs,registers,sizeof(registers));c->ScmResumeValid=TRUE;
    c->ScmPending=TRUE;CamScmOnce(registers);
    RtlCopyMemory(c->Trace.ScmReturn,registers,sizeof(registers));
    c->ScmPending=registers[0]==1||registers[0]==2||registers[0]==3;
    c->ScmResumeValid=c->ScmPending;
    status=c->ScmPending?STATUS_DEVICE_BUSY:registers[0]?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
    c->Diag.Observed=(ULONG)registers[0];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;c->Diag.Status=status;
    return status;
}

NTSTATUS CamScmQosSmart(CAM *c) {return QosEntries(c,FALSE);}
NTSTATUS CamScmQosRdi(CAM *c) {return QosEntries(c,TRUE);}
NTSTATUS CamScmQosCdm(CAM *c) {return QosEntries(c,2);}

static VOID ContextFaults(CAM *c,ULONG sample[2][4]) {
    const ULONG controls[]={c->Trace.SmmuState[7],c->Trace.CommandRoute[1]};
    const ULONG offsets[]={0x58,0x60,0x64,0x68};
    for(ULONG n=0;n<2;n++) {
        if((controls[n]&0x30000)||(controls[n]&255)>=0x53)continue;
        ULONG context=0x80000+(controls[n]&255)*4096;
        for(ULONG i=0;i<4;i++)sample[n][i]=CamRead(c->Base[CamSmmu],context+offsets[i]);
    }
}

static VOID Snapshot(CAM *c) {
    PUCHAR base=c->Base[CamCdm];
    for(ULONG i=0;i<RTL_NUMBER_OF(CamCdmTraceRegs);i++)c->Trace.Cdm[i]=CamRead(base,CamCdmTraceRegs[i]);
    ULONG original=CamRead(base,0xd8);
    for(ULONG i=0;i<16;i++) {
        CamWrite(base,0xd8,(original&~0xf1u)|1|(i<<4));
        c->Trace.CdmDebug[i]=CamRead(base,0xf4);
    }
    CamWrite(base,0xd8,original);
}

static ULONGLONG Time(VOID) {
    ULONGLONG qpc;return KeQueryInterruptTimePrecise(&qpc);
}

static VOID Outstanding(CAM *c) {
    const ULONG regions[]={CamHfTbu,CamSfTbu};
    for(ULONG n=0;n<2;n++)for(ULONG port=0;port<2;port++)
        c->Trace.CdmOutstandingSeen[n][port]|=CamRead64(c->Base[regions[n]],0xc10+port*0x10);
}

static VOID Sample(CAM *c,ULONG phase,ULONGLONG start) {
    if(c->Trace.CdmSampleCount>=RTL_NUMBER_OF(c->Trace.CdmSamples))return;
    QCAM_CDM_SAMPLE *s=&c->Trace.CdmSamples[c->Trace.CdmSampleCount++];
    RtlZeroMemory(s,sizeof(*s));s->Phase=phase;
    s->ElapsedUs=phase?(ULONG)((Time()-start)/10):0;
    PUCHAR b=c->Base[CamCdm];
    s->Status=CamRead(b,0x28);s->Irq=CamRead(b,0x44);s->Pending=CamRead(b,0x6c);
    s->UserData=CamRead(b,0x80);s->CurrentBase=CamRead(b,0xe8);s->CurrentLength=CamRead(b,0xec);
    ULONG debug=CamRead(b,0xd8);CamWrite(b,0xd8,(debug&~0xf1u)|0xc1);
    s->Debug=CamRead(b,0xf4);CamWrite(b,0xd8,debug);
    s->Qchannel[0]=CamRead(c->Base[CamCpas],0xf0);s->Qchannel[1]=CamRead(c->Base[CamCpas],0xf8);
    const ULONG regions[]={CamHfTbu,CamSfTbu};
    for(ULONG n=0;n<2;n++) {
        PUCHAR tbu=c->Base[regions[n]];
        s->TbuStatus[n][0]=CamRead(tbu,0xc00);s->TbuStatus[n][1]=CamRead(tbu,0xc08);
        s->Outstanding[n][0]=CamRead64(tbu,0xc10);s->Outstanding[n][1]=CamRead64(tbu,0xc20);
    }
}

static NTSTATUS AllocateCommand(CAM *c) {
    if(c->Command&&c->CommandMdl)return STATUS_SUCCESS;
    if(!c->DmaEnabler&&!c->DmaEnabler32)return STATUS_DEVICE_NOT_READY;
    if(!c->Command) {
        PHYSICAL_ADDRESS lowest={0},highest,boundary={0};highest.QuadPart=MAXULONG;
        c->Command=MmAllocateContiguousMemorySpecifyCache(4096,lowest,highest,boundary,MmNonCached);
        if(!c->Command)return STATUS_INSUFFICIENT_RESOURCES;
        c->CommandContiguous=TRUE;c->CommandAddress=MmGetPhysicalAddress(c->Command);
    }
    if(!c->CommandAddress.QuadPart||(c->CommandAddress.QuadPart&255)||
       (ULONGLONG)c->CommandAddress.QuadPart+4095>MAXULONG||
       c->CommandAddress.QuadPart!=MmGetPhysicalAddress(c->Command).QuadPart)return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->CommandMdl=IoAllocateMdl(c->Command,4096,FALSE,FALSE,NULL);
    if(!c->CommandMdl)return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(c->CommandMdl);
    return STATUS_SUCCESS;
}

static NTSTATUS Submit(CAM *c,BOOLEAN fetch,const CAM_ISP_GAMMA *gamma,BOOLEAN lut,const CAM_ISP_GTM *gtm);

static NTSTATUS Configure(CAM *c,ULONG limit,BOOLEAN fetch) {
    if(c->CdmPending||c->DmaActive||c->RxActive||c->ScmPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    /* Command/LUT DMA does not require a running sensor receiver. All clock
       and interconnect gates remain required; image DMA keeps its RX guard. */
    if(!c->Powered||!CamControlDmaReady(c))return STATUS_DEVICE_NOT_READY;
    PUCHAR b=c->Base[CamCdm];
    c->Diag.Step=161;c->Diag.Region=CamCdm;c->Diag.Address=0x1860;c->Diag.Slot=MAXULONG;
    if(CamRead(b,0)!=0x20020000)return STATUS_REVISION_MISMATCH;
    if(CamRead(b,0x1c)!=1||(CamRead(b,0x6c)&0x01ff01ff)||
       (CamRead(b,0x70)&0x01ff01ff)||(CamRead(b,0x44)&0x70000))return STATUS_DEVICE_BUSY;
    NTSTATUS status=CamSave(c,b,0x1c,3);
    if(NT_SUCCESS(status))status=CamSave(c,b,0x30,0);
    if(NT_SUCCESS(status))status=CamSave(c,b,0x5c,CamRead(b,0x5c));
    if(NT_SUCCESS(status))status=CamSave(c,b,0xd8,CamRead(b,0xd8));
    if(NT_SUCCESS(status))status=CamSave(c,b,0x18,CamRead(b,0x18));
    if(!NT_SUCCESS(status))return status;
    c->Diag.Step=162;c->Diag.Offset=0x44;c->Diag.Mask=1;c->Diag.Expected=1;
    c->CdmPending=TRUE;
    status=CamCdmAbortCore(b);c->Diag.Observed=CamRead(b,0x44);
    if(!NT_SUCCESS(status)) {
        Snapshot(c);
        c->Faulted=TRUE;return status;
    }
    c->CdmPending=FALSE;
    CamWrite(b,0x18,0x11f);CamWrite(b,0xd8,0x100);CamWrite(b,0x5c,0x00400000);
    CamWrite(b,0x1c,1);
    if(CamRead(b,0x18)!=0x11f||!(CamRead(b,0xd8)&0x100)||
       CamRead(b,0x5c)!=0x00400000||CamRead(b,0x1c)!=1)return STATUS_IO_DEVICE_ERROR;
    if(limit==1){Snapshot(c);c->Diag.Step=171;return STATUS_SUCCESS;}
    status=AllocateCommand(c);if(!NT_SUCCESS(status))return status;
    c->Trace.CommandAddress=c->CommandAddress.QuadPart;
    if(limit==2){Snapshot(c);c->Diag.Step=172;return STATUS_SUCCESS;}
    c->Diag.Step=161;
    PLATFORM_LOCK_GUARD routeGuard={0};status=PlatformLockAcquire(&routeGuard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    status=CamCdmRoute(c);
    if(NT_SUCCESS(status)&&limit>=4) {
        ContextFaults(c,c->Trace.ContextBefore);
        for(ULONG n=0;n<2;n++)if(CamSmmuFault(c->Trace.ContextBefore[n][0])) {
            ULONG control=n?c->Trace.CommandRoute[1]:c->Trace.SmmuState[7];
            c->Diag.Step=168;c->Diag.Region=CamSmmu;c->Diag.Offset=0x80058+(control&255)*4096;
            c->Diag.Mask=CAM_SMMU_FSR_FAULT;c->Diag.Expected=0;c->Diag.Observed=c->Trace.ContextBefore[n][0];
            status=STATUS_DEVICE_BUSY;break;
        }
    }
    NTSTATUS routeReleased=PlatformLockRelease(&routeGuard);
    if(!NT_SUCCESS(status))return status;
    if(!NT_SUCCESS(routeReleased))return routeReleased;
    c->Diag.Region=CamCdm;c->Diag.Address=0xac81000;RtlCopyMemory(c->Diag.Resource,"cam-cdm",8);
    if(limit<5) {
        Snapshot(c);
        c->Diag.Step=169;return STATUS_SUCCESS;
    }
    return Submit(c,fetch,NULL,FALSE,NULL);
}

static NTSTATUS Submit(CAM *c,BOOLEAN fetch,const CAM_ISP_GAMMA *gamma,BOOLEAN lut,const CAM_ISP_GTM *gtm) {
    if(c->CdmPending||c->DmaActive||c->RxActive||!c->Command||!c->CommandMdl)return STATUS_DEVICE_BUSY;
    PUCHAR b=c->Base[CamCdm];NTSTATUS status;
    if(CamRead(b,0x1c)!=1||(CamRead(b,0x6c)&0x01ff01ff)||
       (CamRead(b,0x70)&0x01ff01ff))return STATUS_DEVICE_BUSY;
    ULONG irqMask=CamRead(b,0x30),marker=(CamRead(b,0x80)+1)&255;
    status=CamSave(c,b,0x30,0);if(!NT_SUCCESS(status))return status;
    CamWrite(b,0x30,0);
    if(CamRead(b,0x30))return STATUS_IO_DEVICE_ERROR;
    CamWrite(b,0x34,MAXULONG);CamWrite(b,0x38,1);
    if(CamRead(b,0x44)&0x70000){CamWrite(b,0x30,irqMask);return STATUS_IO_DEVICE_ERROR;}
    ULONG commandBytes=gtm?CAM_ISP_GTM_CDM_WORDS*4:gamma?(lut?CAM_ISP_GAMMA_CDM_WORDS:CAM_ISP_GAMMA_REG_WORDS)*4:
        fetch?CAM_CDM_FETCH_BYTES:CAM_CDM_COMMAND_BYTES;
    RtlZeroMemory(c->Command,4096);
    if(gtm) {
        if(!CamIspGtmDmiBuildFormat(c->Command,4096,c->CommandAddress.QuadPart,CAM_CDM_TFE2_BASE,marker,gtm,c->IspFdDmi64))return STATUS_INVALID_PARAMETER;
    } else if(gamma) {
        if(!CamIspGammaCommandBuild(c->Command,4096,c->CommandAddress.QuadPart,CAM_CDM_TFE2_BASE,marker,gamma,lut))return STATUS_INVALID_PARAMETER;
    } else if(fetch)CamCdmEncodeFetch(c->Command,marker);else CamCdmEncode(c->Command,marker);
    KeFlushIoBuffers(c->CommandMdl,FALSE,TRUE);
    KeMemoryBarrier();
    RtlCopyMemory(c->Trace.CommandWords,c->Command,CAM_CDM_COMMAND_BYTES);
    c->Trace.CommandAddress=c->CommandAddress.QuadPart;c->Trace.CommandMarker=marker;
    c->Trace.CdmSampleCount=0;RtlZeroMemory(c->Trace.CdmOutstandingSeen,sizeof(c->Trace.CdmOutstandingSeen));
    Sample(c,0,0);
    c->CdmPending=TRUE;
    CamWrite(b,0x50,(ULONG)c->CommandAddress.QuadPart);
    CamWrite(b,0x54,commandBytes-1);
    ULONGLONG start=Time();CamWrite(b,0x58,1);
    ULONGLONG deadline=start+1000000;
    const ULONG sampleUs[]={0,10,100,1000,10000,50000};ULONG nextSample=0;
    status=STATUS_IO_TIMEOUT;
    do {
        Outstanding(c);
        if(nextSample<RTL_NUMBER_OF(sampleUs)&&Time()-start>=(ULONGLONG)sampleUs[nextSample]*10) {
            Sample(c,1,start);nextSample++;
        }
        if(fetch||gamma||gtm?CamCdmFetched(b,marker):CamCdmCompleted(b,marker)){status=STATUS_SUCCESS;break;}
        if(CamRead(b,0x44)&0x70000){status=STATUS_IO_DEVICE_ERROR;break;}
        CamDelay(Time()-start<10000?10:100);
    } while(Time()<deadline);
    Sample(c,2,start);
    Snapshot(c);
    ContextFaults(c,c->Trace.ContextAfter);
    const ULONG tbuRegions[]={CamHfTbu,CamSfTbu};
    for(ULONG n=0;n<2;n++)for(ULONG i=0;i<RTL_NUMBER_OF(CamTbuTraceRegs);i++)
        c->Trace.Tbu[n][i]=i?CamRead64(c->Base[tbuRegions[n]],CamTbuTraceRegs[i]):CamRead(c->Base[tbuRegions[n]],0);
    c->Diag.Offset=0x80;c->Diag.Mask=MAXULONG;c->Diag.Expected=marker;c->Diag.Observed=CamRead(b,0x80);
    if(NT_SUCCESS(status)) {
        c->CdmPending=FALSE;
        CamWrite(b,0x34,MAXULONG);CamWrite(b,0x38,1);CamWrite(b,0x30,irqMask);
        if(!fetch&&!gamma&&!gtm)c->Trace.Csid[20]=CamRead(c->Base[CamCsid],0xe10);
    } else c->Faulted=TRUE;
    return status;
}

NTSTATUS CamCdmPrepare(CAM *c,ULONG limit) { return limit>=1&&limit<=4?Configure(c,limit,FALSE):STATUS_INVALID_PARAMETER; }
NTSTATUS CamCdmConfigure(CAM *c) { return Configure(c,5,FALSE); }
NTSTATUS CamCdmFetch(CAM *c) { return Configure(c,5,TRUE); }

NTSTATUS CamCdmTranslate(CAM *c) {
    NTSTATUS status=CamCdmPrepare(c,4);if(!NT_SUCCESS(status))return status;
    PLATFORM_LOCK_GUARD guard={0};status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    PUCHAR base=c->Base[CamSmmu];ULONG slot=c->Trace.CommandRoute[0],control=c->Trace.CommandRoute[1];
    if(slot>=128||(control&0x30000)||(c->Trace.CommandRoute[3]&1)||
       CamRead(base,0x800+slot*4)!=0x80001860||CamRead(base,0xc00+slot*4)!=control)status=STATUS_DEVICE_BUSY;
    else {
        /* Stock sf_qtb has SID range0x1800..0x1bff, and ECATS selects
           sid&0x3ff. Other SIDs within that TBU are distinct clients, not
           aliases of0x1860. Keep exact/masked/duplicate SID conflicts gated. */
        if(!CamSmmuExactRoute(base,0x1860,slot,control))status=STATUS_DEVICE_BUSY;
        if(NT_SUCCESS(status)) {
            c->Diag.Step=174;c->Diag.Region=CamSfTbu;c->Diag.Address=0x17d1000;
            c->Diag.Offset=0x458;c->Diag.Mask=MAXULONG;c->Diag.Expected=(ULONG)c->CommandAddress.QuadPart;
            status=CamTbuTranslatePage(c->Base[CamSfTbu],0x1860,c->CommandAddress.QuadPart,c->Trace.TbuTranslation);
            c->Diag.Observed=(ULONG)(c->Trace.TbuTranslation[5]>>12);ContextFaults(c,c->Trace.ContextAfter);
            if(c->Trace.TbuTranslation[6]||(c->Trace.TbuTranslation[7]&1)||CamSmmuFault(c->Trace.ContextAfter[1][0]))
                {c->TbuPending=c->Faulted=TRUE;if(NT_SUCCESS(status))status=STATUS_DATA_ERROR;}
        }
    }
    NTSTATUS released=PlatformLockRelease(&guard);
    return NT_SUCCESS(status)?released:status;
}

NTSTATUS CamCdmGamma(CAM *c,QCAM_ISP_GAMMA_STATE *state) {
    if(!state)return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(state,sizeof(*state));state->Size=sizeof(*state);state->Abi=QCAM_ABI;
    if(!c->Powered||!c->BusOn||c->DmaActive||c->RxActive||c->StreamFile||
       !CamDmaResourcesIdle(c->Base[CamCsid],c->Base[CamTfe]))return STATUS_DEVICE_BUSY;
    PUCHAR t=c->Base[CamTfe];
    if(CamRead(t,0)!=0x10000000)return STATUS_REVISION_MISMATCH;
    for(ULONG i=0;i<3;i++)state->Before[i]=CamRead(t,0x6b58+i*4);
    if(state->Before[0]>1||state->Before[1]!=state->Before[0]||state->Before[2]>1)return STATUS_NOT_SUPPORTED;
    /* Standby transfer test uses an explicit linear reference curve in the
       inactive bank. Existing selected LUTs are retained and controls restored. */
    CAM_ISP_GAMMA gamma;ULONG points[65];
    for(ULONG i=0;i<65;i++)points[i]=min(i*16,1023u);
    if(!CamIspGammaBuild(&gamma,points,state->Before[0]^1u))return STATUS_INVALID_PARAMETER;
    /* Distinguish MMIO/bank access from command transport. Image DMA is off
       throughout this probe, and the selected bank is restored before CDM. */
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<3;i++)CamWrite(t,0x6b58+i*4,gamma.Config[i]);
    for(ULONG i=0;i<3;i++) {
        state->CpuConfigured[i]=CamRead(t,0x6b58+i*4);
        if(state->CpuConfigured[i]!=gamma.Config[i]) {
            status=STATUS_IO_DEVICE_ERROR;c->Diag.Step=192;c->Diag.Region=CamTfe;
            c->Diag.Offset=0x6b58+i*4;c->Diag.Address=0x0aca6000+c->Diag.Offset;
            c->Diag.Expected=gamma.Config[i];c->Diag.Observed=state->CpuConfigured[i];
        }
    }
    for(ULONG i=0;i<3;i++)CamWrite(t,0x6b58+i*4,state->Before[i]);
    if(NT_SUCCESS(status))status=CamEnsureStaticQos(c);
    if(NT_SUCCESS(status))status=CamCdmTranslate(c);
    /* Verify only the register command before introducing DMI. A completed
       inline IRQ cannot stand in for a successful target register write. */
    if(NT_SUCCESS(status))status=Submit(c,FALSE,&gamma,FALSE,NULL);
    state->RegisterStatus=(ULONG)status;
    state->RegisterAhbAddress=CamRead(c->Base[CamCdm],0xd0);
    state->RegisterAhbData=CamRead(c->Base[CamCdm],0xd4);
    for(ULONG i=0;i<3;i++) {
        state->RegisterConfigured[i]=CamRead(t,0x6b58+i*4);
        if(NT_SUCCESS(status)&&state->RegisterConfigured[i]!=gamma.Config[i])status=STATUS_IO_DEVICE_ERROR;
    }
    if(NT_SUCCESS(status)) {
        for(ULONG i=0;i<3;i++)CamWrite(t,0x6b58+i*4,state->Before[i]);
        status=Submit(c,FALSE,&gamma,TRUE,NULL);
    }
    state->CdmStatus=(ULONG)status;state->CommandAddress=c->CommandAddress.QuadPart;
    state->Marker=c->Trace.CommandMarker;
    for(ULONG i=0;i<3;i++) {
        state->Configured[i]=CamRead(t,0x6b58+i*4);
        if(NT_SUCCESS(status)&&state->Configured[i]!=gamma.Config[i])status=STATUS_IO_DEVICE_ERROR;
    }
    state->Loaded=NT_SUCCESS(status);
    /* Abort/drain while the camera interconnect is still on. On failed abort
       retain command memory and the hardware ownership flags. */
    NTSTATUS aborted=CamCdmAbort(c);
    if(!NT_SUCCESS(aborted)){state->Status=(ULONG)aborted;return aborted;}
    for(ULONG i=0;i<3;i++)CamWrite(t,0x6b58+i*4,state->Before[i]);
    for(ULONG i=0;i<3;i++) {
        state->Restored[i]=CamRead(t,0x6b58+i*4);
        if(state->Restored[i]!=state->Before[i])status=STATUS_IO_DEVICE_ERROR;
    }
    if(!CamDmaResourcesIdle(c->Base[CamCsid],t))status=STATUS_DEVICE_BUSY;
    state->Status=(ULONG)status;return status;
}

/* FD owns/restores the three controls and original active LUT selection.
   Profile8 retains the verified standalone linear curve; combined profiles
   use the original mode11 reference. Command memory/routes remain owned
   through frame teardown. */
NTSTATUS CamCdmGammaLoad(CAM *c,ULONG bank) {
    if(!c->IspFdOwned||!c->IspFd||(c->IspFd->Profile!=8&&(c->IspFd->Profile<15||c->IspFd->Profile>33))||bank>1||
       !c->Powered||!c->BusOn||c->DmaActive||c->RxActive||c->CdmPending||c->StreamFile||
       !CamDmaResourcesIdle(c->Base[CamCsid],c->Base[CamTfe]))return STATUS_DEVICE_BUSY;
    QCAM_ISP_FD_STATE *state=c->IspFd;
    CAM_ISP_GAMMA gamma;ULONG points[65];
    for(ULONG i=0;i<65;i++)points[i]=min(i*16,1023u);
    if(!(state->Profile>=15?CamIspGammaNativeReference(&gamma,bank):CamIspGammaBuild(&gamma,points,bank)))return STATUS_INVALID_PARAMETER;
    state->GammaLoaded=0;state->GammaBank=bank;
    NTSTATUS status=CamEnsureStaticQos(c);
    if(NT_SUCCESS(status))status=CamCdmTranslate(c);
    if(NT_SUCCESS(status))status=Submit(c,FALSE,&gamma,TRUE,NULL);
    state->GammaAddress=c->CommandAddress.QuadPart;state->GammaMarker=c->Trace.CommandMarker;
    for(ULONG i=0;i<3;i++) {
        state->GammaConfig[i]=CamRead(c->Base[CamTfe],0x6b58+i*4);
        if(NT_SUCCESS(status)&&state->GammaConfig[i]!=gamma.Config[i])status=STATUS_IO_DEVICE_ERROR;
    }
    if(!NT_SUCCESS(status)) {
        NTSTATUS aborted=CamCdmAbort(c);
        if(!NT_SUCCESS(aborted))status=aborted;
    }
    state->GammaStatus=(ULONG)status;state->GammaLoaded=NT_SUCCESS(status);
    return status;
}

NTSTATUS CamCdmGtmLoad(CAM *c,ULONG bank) {
    if(!c->IspFdOwned||!c->IspFd||(c->IspFd->Profile<14||c->IspFd->Profile>33)||bank>1||bank!=c->IspFd->GtmBank||
       !c->Powered||!c->BusOn||c->DmaActive||c->RxActive||c->CdmPending||c->StreamFile||
       !CamDmaResourcesIdle(c->Base[CamCsid],c->Base[CamTfe]))return STATUS_DEVICE_BUSY;
    QCAM_ISP_FD_STATE *state=c->IspFd;
    CAM_ISP_GTM *gtm=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*gtm),'gIcQ');
    if(!gtm)return STATUS_INSUFFICIENT_RESOURCES;
    state->GtmLoaded=0;
    NTSTATUS status=CamIspGtmNativeReference(gtm,bank)?CamEnsureStaticQos(c):STATUS_INVALID_PARAMETER;
    if(NT_SUCCESS(status))status=CamCdmTranslate(c);
    if(NT_SUCCESS(status))status=Submit(c,FALSE,NULL,TRUE,gtm);
    state->GtmAddress=c->CommandAddress.QuadPart;state->GtmMarker=c->Trace.CommandMarker;
    for(ULONG i=0;i<7;i++) {
        state->GtmConfig[i]=CamRead(c->Base[CamTfe],0x6658+i*4);
        if(NT_SUCCESS(status)&&state->GtmConfig[i]!=gtm->Registers[i])status=STATUS_IO_DEVICE_ERROR;
    }
    if(!NT_SUCCESS(status)) {
        NTSTATUS aborted=CamCdmAbort(c);
        if(!NT_SUCCESS(aborted))status=aborted;
    }
    ExFreePoolWithTag(gtm,'gIcQ');
    state->GtmStatus=(ULONG)status;state->GtmLoaded=NT_SUCCESS(status);
    return status;
}

NTSTATUS CamCdmAbort(CAM *c) {
    if(!c->CdmPending)return STATUS_SUCCESS;
    NTSTATUS status=CamCdmAbortCore(c->Base[CamCdm]);
    c->Trace.CommandAbortStatus=status;
    if(NT_SUCCESS(status)){c->CdmPending=FALSE;c->Faulted=FALSE;}
    return status;
}

NTSTATUS CamFrameTranslate(CAM *c) {
    RtlZeroMemory(c->Trace.FrameTranslation,sizeof(c->Trace.FrameTranslation));
    if(c->CdmPending||c->DmaActive||c->RxActive||c->ScmPending||c->CrmPending||c->TbuPending)return STATUS_DEVICE_BUSY;
    if(!c->Powered||!CamDiagnosticDmaReady(c)||!c->Frame)return STATUS_DEVICE_NOT_READY;
    ULONGLONG address=c->FrameAddress.QuadPart;
    if(!address||(address&4095)||address+QCAM_FRAME_BYTES-1>MAXULONG)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG i=0;i<QCAM_FRAME_BYTES;i+=4096)
        if((ULONGLONG)MmGetPhysicalAddress((PUCHAR)c->Frame+i).QuadPart!=address+i)return STATUS_DEVICE_CONFIGURATION_ERROR;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    ULONG slot=c->Trace.SmmuState[3],control=c->Trace.SmmuState[7];PUCHAR smmu=c->Base[CamSmmu];
    if(slot>=128||(control&0x30000)||(control&255)>=0x53||
       CamRead(smmu,0x800+slot*4)!=0x80001c00||CamRead(smmu,0xc00+slot*4)!=control||
       !CamDmaIdentity(control,CamRead(smmu,0x1000+(control&255)*4),CamRead(smmu,0x80000+(control&255)*4096)))status=STATUS_DEVICE_BUSY;
    else {
        for(ULONG i=0;i<128;i++) {
            ULONG smr=CamRead(smmu,0x800+i*4),mask=(smr>>16)&0x7fff;
            if((smr&0x80000000u)&&((smr^0x1c00)&~(mask|0x3ffu)&0x7fff)==0&&smr!=0x80001c00) {
                status=STATUS_DEVICE_BUSY;break;
            }
        }
        if(NT_SUCCESS(status)) {
            ContextFaults(c,c->Trace.ContextBefore);
            if(CamSmmuFault(c->Trace.ContextBefore[0][0]))status=STATUS_DEVICE_BUSY;
            else for(ULONG i=0;i<2;i++) {
                ULONGLONG page=address+(i?256:0);
                c->Diag.Step=175;c->Diag.Region=CamHfTbu;c->Diag.Address=(ULONG)page;
                c->Diag.Offset=0x458;c->Diag.Mask=MAXULONG;c->Diag.Expected=(ULONG)page;c->Diag.Phase=i;
                status=CamTbuTranslateAccess(c->Base[CamHfTbu],0x1c00,page,0x409,c->Trace.FrameTranslation[i]);
                c->Diag.Observed=(ULONG)(c->Trace.FrameTranslation[i][5]>>12);ContextFaults(c,c->Trace.ContextAfter);
                if(c->Trace.FrameTranslation[i][6]||(c->Trace.FrameTranslation[i][7]&1)||CamSmmuFault(c->Trace.ContextAfter[0][0]))
                    {c->TbuPending=c->Faulted=TRUE;if(NT_SUCCESS(status))status=STATUS_DATA_ERROR;}
                if(!NT_SUCCESS(status)||c->TbuPending)break;
            }
        }
    }
    NTSTATUS released=PlatformLockRelease(&guard);
    return NT_SUCCESS(status)?released:status;
}
