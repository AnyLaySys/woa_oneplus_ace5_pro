#include "cam.h"

VOID CamScmSlow(ULONGLONG *registers);

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

NTSTATUS CamScmQos(CAM *c) {
    if(!c->Powered)return STATUS_DEVICE_NOT_READY;
    if(c->DmaActive||c->RxActive)return STATUS_DEVICE_BUSY;
    if(c->ScmPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    if(!(CamRead(c->Base[CamCpas],0xf0)&1))return STATUS_DEVICE_NOT_READY;
    NTSTATUS status=STATUS_SUCCESS;
    if(!c->ScmAvailable)status=CamScmProbe(c);
    if(!NT_SUCCESS(status))return status;
    ULONGLONG waitq[8]={0xc2002404};CamScmSlow(waitq);
    RtlCopyMemory(c->Trace.ScmWaitq,waitq,sizeof(waitq));
    BOOLEAN fastWaitq=waitq[0]==0;
    ULONGLONG registers[8]={0x4200180a,0x83,0,0,0};
    c->Diag.Step=151;c->Diag.Region=MAXULONG;c->Diag.Offset=0;
    c->Diag.Address=(ULONG)registers[0];c->Diag.Count=3;c->Diag.Slot=MAXULONG;
    c->Diag.Phase=1;RtlCopyMemory(c->Diag.Resource,"scm-qos",8);
    CamScmSlow(registers);
    status=Complete(c,registers,fastWaitq);
    c->Diag.Observed=(ULONG)registers[0];c->Diag.Mask=MAXULONG;c->Diag.Expected=0;
    c->Diag.Status=status;return status;
}

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
