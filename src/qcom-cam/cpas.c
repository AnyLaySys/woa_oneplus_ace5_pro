#include "cam.h"
#include "trace-regs.h"
#include "crm-vote.h"

static const ULONG PerfDomains[]={0,6,7};

VOID CamBusSnapshot(CAM *c) {
    static const ULONG cpas[]={0,4,8,0xdc,0xec,0xf0,0xf4,0xf8};
    c->Trace.Step=c->Diag.Step;
    for(ULONG i=0;i<RTL_NUMBER_OF(cpas);i++)c->Trace.Cpas[i]=CamRead(c->Base[CamCpas],cpas[i]);
    for(ULONG i=0;i<RTL_NUMBER_OF(CamClockTraceRegs);i++)c->Trace.Clocks[i]=CamRead(c->Base[CamCc],CamClockTraceRegs[i]);
    for(ULONG i=0;i<RTL_NUMBER_OF(CamGccTraceRegs);i++)c->Trace.Gcc[i]=CamRead(c->Base[CamGcc],CamGccTraceRegs[i]);
    for(ULONG i=0;i<RTL_NUMBER_OF(CamNocTraceRegs);i++)c->Trace.Noc[i]=CamRead(c->Base[CamNoc],CamNocTraceRegs[i]);
    for(ULONG i=0;i<RTL_NUMBER_OF(CamCrmTraceRegs);i++)c->Trace.Crm[i]=CamRead(c->Base[CamCrm],CamCrmTraceRegs[i]);
}

NTSTATUS CamBusOn(CAM *c) {
    if(c->CrmPending)return STATUS_DEVICE_BUSY;
    if(c->BusOn)return STATUS_SUCCESS;
    PUCHAR crm=c->Base[CamCrm];
    if((CamRead(crm,0x4d00)>>16)!=2||!(CamRead(crm,0x4d0c)&1)||
       !((CamRead(crm,0x4d04)>>20)&15)||((CamRead(crm,0x4d08)>>26)&31)<2)return STATUS_REVISION_MISMATCH;
    if(CamRead(crm,0x254)&1)return STATUS_DEVICE_BUSY;
    c->CrmOriginal=CamRead(crm,0x21c);c->CrmOwned=TRUE;c->CrmPending=TRUE;c->CrmPendingIrq=0x24c;
    c->Diag.Step=153;c->Diag.Region=CamCrm;c->Diag.Offset=0x24c;c->Diag.Mask=1;c->Diag.Expected=1;
    NTSTATUS status=CamCrmCommand(crm,400);
    c->Diag.Observed=CamRead(crm,0x24c);CamBusSnapshot(c);
    if(!NT_SUCCESS(status))return status;
    c->CrmPending=FALSE;
    for(ULONG i=0;i<RTL_NUMBER_OF(PerfDomains);i++) {
        ULONG domain=PerfDomains[i],offset=0x11c+domain*0x20;
        ULONG cfg=CamRead(crm,0x2768+domain*0x210);
        if(cfg!=(i?0x108u:0x601u)||((CamRead(crm,0x4d04)>>8)&15)<=domain)return STATUS_REVISION_MISMATCH;
        if(CamRead(crm,offset+0x18)&1)return STATUS_DEVICE_BUSY;
        c->CrmPerfOriginal[i]=CamRead(crm,offset)&7;c->CrmPerfMask|=1u<<i;
        c->CrmPending=TRUE;c->CrmPendingIrq=offset+0x10;
        c->Diag.Step=154;c->Diag.Region=CamCrm;c->Diag.Offset=c->CrmPendingIrq;c->Diag.Mask=1;c->Diag.Expected=1;
        status=CamCrmTransfer(crm,offset,c->CrmPerfOriginal[i]?c->CrmPerfOriginal[i]:1);
        c->Diag.Observed=CamRead(crm,c->CrmPendingIrq);CamBusSnapshot(c);
        if(!NT_SUCCESS(status))return status;
        c->CrmPending=FALSE;
    }
    PUCHAR p=c->Base[CamCpas];
    status=CamSave(c,p,0xec,1);
    if(!NT_SUCCESS(status))return status;
    c->BusOn=TRUE;
    if(CamRead(p,0)!=0x00090800||CamRead(p,4)!=0x10000000)return STATUS_REVISION_MISMATCH;
    c->Diag.Step=141;c->Diag.Region=CamCpas;c->Diag.Offset=0xf0;c->Diag.Mask=1;c->Diag.Expected=1;
    status=CamWait(p,0xf0,1,1,20000);
    c->Diag.Observed=CamRead(p,0xf0);CamBusSnapshot(c);
    c->Info.Flags&=~QCAM_QCHANNEL_PENDING;
    if(status==STATUS_IO_TIMEOUT&&c->Trace.Cpas[0]==0x00090800&&c->Trace.Cpas[1]==0x10000000&&
       (c->Trace.Cpas[4]&1)&&!(c->Trace.Clocks[3]&0x80000000)&&!(c->Trace.Clocks[4]&0x80000000)) {
        c->Info.Flags|=QCAM_QCHANNEL_PENDING;return STATUS_SUCCESS;
    }
    return status;
}

NTSTATUS CamCrmOff(CAM *c) {
    if(!c->CrmOwned)return STATUS_SUCCESS;
    PUCHAR crm=c->Base[CamCrm];
    if(c->CrmPending) {
        NTSTATUS status=CamWait(crm,c->CrmPendingIrq,1,1,100000);
        if(!NT_SUCCESS(status))return status;
        CamWrite(crm,c->CrmPendingIrq+4,1);c->CrmPending=FALSE;
    }
    for(ULONG i=RTL_NUMBER_OF(PerfDomains);i>0;i--) {
        ULONG slot=i-1,offset=0x11c+PerfDomains[slot]*0x20;
        if(!(c->CrmPerfMask&(1u<<slot)))continue;
        c->CrmPending=TRUE;c->CrmPendingIrq=offset+0x10;
        NTSTATUS restored=CamCrmTransfer(crm,offset,c->CrmPerfOriginal[slot]);
        if(!NT_SUCCESS(restored))return restored;
        c->CrmPending=FALSE;c->CrmPerfMask&=~(1u<<slot);
    }
    c->CrmPending=TRUE;c->CrmPendingIrq=0x24c;
    NTSTATUS status=CamCrmCommand(crm,c->CrmOriginal&0x1fffffffu);
    if(NT_SUCCESS(status)){c->CrmPending=FALSE;c->CrmOwned=FALSE;}
    return status;
}

NTSTATUS CamBusOff(CAM *c) {
    if(!c->BusOn)return STATUS_SUCCESS;
    CamWrite(c->Base[CamCpas],0xec,0);
    NTSTATUS status=CamWait(c->Base[CamCpas],0xf0,1,0,50000);
    if(!NT_SUCCESS(status)) {
        c->Diag.Step=142;c->Diag.Region=CamCpas;c->Diag.Offset=0xf0;c->Diag.Mask=1;c->Diag.Expected=0;
        c->Diag.Observed=CamRead(c->Base[CamCpas],0xf0);
    }
    if(NT_SUCCESS(status))c->BusOn=FALSE;else c->Faulted=TRUE;
    return status;
}
