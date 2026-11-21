#include "cam.h"
#include "scm-ready.h"

VOID CamScmSlow(ULONGLONG *registers);
VOID CamScmOnce(ULONGLONG *registers);

static NTSTATUS Complete(CAM *c,ULONGLONG *registers,BOOLEAN fastWaitq) {
    ULONGLONG deadline=KeQueryInterruptTime()+20000000;
    for(ULONG resumed=0;resumed<64;resumed++) {
        RtlCopyMemory(c->Trace.ScmReturn,registers,8*sizeof(*registers));
        if(registers[0]!=1&&registers[0]!=2&&registers[0]!=3) {
            c->ScmPending=FALSE;
            return registers[0]?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
        }
        c->ScmPending=TRUE;
        if(registers[0]==1)return STATUS_DEVICE_BUSY;
        ULONG waitq=(ULONG)registers[1],context=(ULONG)registers[2];
        if(registers[0]==2) {
            BOOLEAN wake=FALSE;
            do {
                ULONGLONG query[8]={fastWaitq?0xc2002403:0x42002403};
                CamScmSlow(query);RtlCopyMemory(c->Trace.ScmWaitq,query,sizeof(query));
                if(query[0])return STATUS_DEVICE_BUSY;
                if((ULONG)query[1]==waitq&&((ULONG)query[2]&3)){wake=TRUE;break;}
                if(query[2]&&query[1]!=waitq)return STATUS_DEVICE_BUSY;
                CamDelay(1000);
            }while(KeQueryInterruptTime()<deadline);
            if(!wake)return STATUS_IO_TIMEOUT;
            RtlZeroMemory(registers,8*sizeof(*registers));registers[0]=0x42002402;
        } else {
            RtlZeroMemory(registers,8*sizeof(*registers));registers[0]=0x42002401;
        }
        registers[1]=1;registers[2]=context;CamScmSlow(registers);
    }
    RtlCopyMemory(c->Trace.ScmReturn,registers,8*sizeof(*registers));
    return STATUS_DEVICE_BUSY;
}

NTSTATUS CamScmProbe(CAM *c) {
    if(c->ScmPending)return STATUS_DEVICE_BUSY;
    ULONGLONG registers[8]={0x42000601,1,0x0200180a};
    c->Diag.Step=150;c->Diag.Region=MAXULONG;c->Diag.Offset=0;
    c->Diag.Address=(ULONG)registers[0];c->Diag.Count=1;c->Diag.Slot=MAXULONG;
    c->Diag.Phase=0;RtlCopyMemory(c->Diag.Resource,"scm-info",8);
    CamScmSlow(registers);
    RtlCopyMemory(c->Trace.ScmReturn,registers,sizeof(registers));
    c->Diag.Observed=(ULONG)registers[0];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;
    NTSTATUS status;
    if(registers[0]==1)status=STATUS_DEVICE_BUSY;
    else if((LONG)registers[0]<0)status=STATUS_NOT_SUPPORTED;
    else if(registers[0]||registers[1]!=1)status=STATUS_NOT_SUPPORTED;
    else status=STATUS_SUCCESS;
    c->ScmAvailable=NT_SUCCESS(status);c->Diag.Status=status;return status;
}

NTSTATUS CamScmWaitqProbe(CAM *c) {
    if(c->ScmPending||c->CdmPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    ULONGLONG registers[8]={0xc2002404};
    c->Diag.Step=163;c->Diag.Region=MAXULONG;c->Diag.Address=(ULONG)registers[0];
    c->Diag.Count=0;c->Diag.Phase=0;RtlCopyMemory(c->Diag.Resource,"scm-wait",8);
    CamScmSlow(registers);RtlCopyMemory(c->Trace.ScmWaitq,registers,sizeof(registers));
    c->Diag.Observed=(ULONG)registers[0];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;
    if(registers[0]==1){c->ScmPending=TRUE;return STATUS_DEVICE_BUSY;}
    if(registers[0]==2||registers[0]==3){c->ScmPending=TRUE;return STATUS_DEVICE_BUSY;}
    c->ScmWaitqKnown=TRUE;c->ScmFastWaitq=registers[0]==0;
    NTSTATUS status=c->ScmFastWaitq?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
    c->Diag.Status=status;return status;
}

NTSTATUS CamScmWaitqContext(CAM *c) {
    if(!c->ScmWaitqKnown||!c->ScmFastWaitq)return STATUS_NOT_SUPPORTED;
    if(c->CdmPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    ULONGLONG registers[8]={0xc2002403};
    c->Diag.Step=165;c->Diag.Region=MAXULONG;c->Diag.Address=(ULONG)registers[0];
    c->Diag.Count=0;c->Diag.Phase=0;RtlCopyMemory(c->Diag.Resource,"scm-ctx",8);
    CamScmSlow(registers);RtlCopyMemory(c->Trace.ScmWaitq,registers,sizeof(registers));
    c->Diag.Observed=(ULONG)registers[0];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;
    NTSTATUS status=registers[0]==1?STATUS_DEVICE_BUSY:registers[0]?STATUS_NOT_SUPPORTED:STATUS_SUCCESS;
    c->Diag.Status=status;return status;
}

static NTSTATUS Qos(CAM *c,BOOLEAN diagnostic,BOOLEAN complete) {
    if(!c->Powered)return STATUS_DEVICE_NOT_READY;
    if(c->DmaActive||c->RxActive)return STATUS_DEVICE_BUSY;
    if(c->ScmPending||c->CrmPending||c->CdmPending)return STATUS_DEVICE_BUSY;
    ULONG rt=CamRead(c->Base[CamCpas],0xf0),nrt=CamRead(c->Base[CamCpas],0xf8);
    if(!(rt&1)||!(nrt&1)) {
        c->Diag.Step=186;c->Diag.Region=CamCpas;c->Diag.Offset=(rt&1)?0xf8:0xf0;
        c->Diag.Address=0xac04000+c->Diag.Offset;c->Diag.Mask=1;c->Diag.Expected=1;
        c->Diag.Observed=(rt&1)?nrt:rt;RtlCopyMemory(c->Diag.Resource,"cpas-qos",8);
        return STATUS_DEVICE_NOT_READY;
    }
    if(diagnostic&&!CamScmDiagnosticReady(c))return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=STATUS_SUCCESS;
    if(!c->ScmAvailable)status=CamScmProbe(c);
    if(!NT_SUCCESS(status))return status;
    if(!c->ScmWaitqKnown) {
        status=CamScmWaitqProbe(c);
        if(!NT_SUCCESS(status)&&status!=STATUS_NOT_SUPPORTED)return status;
    }
    ULONGLONG registers[8]={0x4200180a,0x83,0,0,0};
    c->Diag.Step=151;c->Diag.Region=MAXULONG;c->Diag.Offset=0;
    c->Diag.Address=(ULONG)registers[0];c->Diag.Count=3;c->Diag.Slot=MAXULONG;
    c->Diag.Phase=complete?1:2;RtlCopyMemory(c->Diag.Resource,"scm-qos",8);
    c->ScmPending=TRUE;
    RtlCopyMemory(c->ScmResumeArgs,registers,sizeof(registers));c->ScmResumeValid=TRUE;
    status=CamScmAudit(c,1,registers,STATUS_SUCCESS);
    if(!NT_SUCCESS(status)){c->ScmPending=FALSE;c->ScmResumeValid=FALSE;c->Diag.Status=status;return status;}
    if(complete)CamScmSlow(registers);else CamScmOnce(registers);
    RtlCopyMemory(c->Trace.ScmReturn,registers,sizeof(registers));
    CamScmAudit(c,2,registers,(NTSTATUS)registers[0]);
    if(complete)status=Complete(c,registers,c->ScmFastWaitq);
    else {
        c->ScmPending=registers[0]==1||registers[0]==2||registers[0]==3;
        status=c->ScmPending?STATUS_DEVICE_BUSY:registers[0]?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
    }
    c->Diag.Observed=(ULONG)registers[0];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;
    c->ScmResumeValid=c->ScmPending;c->Diag.Status=status;return status;
}

NTSTATUS CamScmContinue(CAM *c) {
    if(!c->ScmPending||!c->ScmResumeValid)return STATUS_DEVICE_NOT_READY;
    if(!c->Powered||c->DmaActive||c->RxActive||c->CdmPending||c->TbuPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    ULONGLONG registers[8];RtlCopyMemory(registers,c->Trace.ScmReturn,sizeof(registers));
    ULONG prior=(ULONG)registers[0];
    if(prior!=1&&prior!=2&&prior!=3)return STATUS_DEVICE_NOT_READY;
    c->Diag.Step=185;c->Diag.Region=MAXULONG;c->Diag.Address=(ULONG)c->ScmResumeArgs[0];
    c->Diag.Offset=0;c->Diag.Count=3;c->Diag.Slot=MAXULONG;c->Diag.Phase=prior;RtlCopyMemory(c->Diag.Resource,"scm-resm",8);
    if(prior==1) {
        ULONGLONG cookie=registers[6];RtlCopyMemory(registers,c->ScmResumeArgs,sizeof(registers));
        registers[0]=1;registers[6]=cookie;
    }
    NTSTATUS written=CamScmAudit(c,4,registers,STATUS_SUCCESS);if(!NT_SUCCESS(written))return written;
    if(prior==1){CamScmSlow(registers);CamScmAudit(c,5,registers,(NTSTATUS)registers[0]);}
    NTSTATUS status=Complete(c,registers,c->ScmFastWaitq);
    if(prior!=1)CamScmAudit(c,5,c->Trace.ScmReturn,status);
    c->ScmResumeValid=c->ScmPending;c->Diag.Observed=(ULONG)c->Trace.ScmReturn[0];
    c->Diag.Mask=MAXULONG;c->Diag.Expected=0;c->Diag.Status=status;return status;
}

NTSTATUS CamScmQos(CAM *c) {return Qos(c,FALSE,TRUE);}
NTSTATUS CamScmQosDiagnostic(CAM *c) {return Qos(c,TRUE,TRUE);}
NTSTATUS CamScmQosRaw(CAM *c) {return Qos(c,TRUE,FALSE);}

NTSTATUS CamScmIoProbe(CAM *c) {
    if(c->ScmPending)return STATUS_DEVICE_BUSY;
    if(!c->Powered)return STATUS_DEVICE_NOT_READY;
    if(c->DmaActive||c->RxActive)return STATUS_DEVICE_BUSY;
    ULONGLONG registers[8]={0xc2000501,1,0x0adcfd00};
    c->Diag.Step=152;c->Diag.Region=CamCrm;c->Diag.Offset=0x4d00;
    c->Diag.Address=(ULONG)registers[2];c->Diag.Count=1;c->Diag.Slot=MAXULONG;
    c->Diag.Phase=0;RtlCopyMemory(c->Diag.Resource,"scm-read",8);
    CamScmSlow(registers);
    RtlCopyMemory(c->Trace.ScmReturn,registers,sizeof(registers));
    c->Diag.Observed=(ULONG)registers[1];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;
    NTSTATUS status=registers[0]==1?STATUS_DEVICE_BUSY:registers[0]?STATUS_NOT_SUPPORTED:STATUS_SUCCESS;
    c->Diag.Status=status;return status;
}
