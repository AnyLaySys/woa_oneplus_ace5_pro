#pragma once

static NTSTATUS CamGdscCycle(CAM *c) {
    if(!c->Powered)return STATUS_DEVICE_NOT_READY;
    if(c->DmaActive||c->RxActive||c->CrmPending||c->ScmPending)return STATUS_DEVICE_BUSY;
    PUCHAR base=c->Base[CamCc];
    const ULONG offsets[]={0x11004,0x1134c};
    ULONG active[2]={CamRead(base,offsets[0]),CamRead(base,offsets[1])},changed=0;
    NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++) {
        c->Diag.Step=156;c->Diag.Region=CamCc;c->Diag.Offset=offsets[i]+4;
        c->Diag.Mask=0x8000;c->Diag.Expected=0x8000;
        status=CamSave(c,base,offsets[i],(active[i]&~0x807u)|1);
        if(!NT_SUCCESS(status))break;
        changed|=1u<<i;CamDelay(1);
        status=CamWait(base,offsets[i]+4,0x8000,0x8000,20000);
        c->Diag.Observed=CamRead(base,offsets[i]+4);
        if(!NT_SUCCESS(status))break;
    }
    QCAM_DIAG diag=c->Diag;
    for(ULONG i=RTL_NUMBER_OF(offsets);i>0;i--) {
        ULONG slot=i-1;
        if(!(changed&(1u<<slot)))continue;
        NTSTATUS restored=CamSave(c,base,offsets[slot],active[slot]);
        if(NT_SUCCESS(restored))restored=CamWait(base,offsets[slot]+4,0x10000,0x10000,20000);
        if(!NT_SUCCESS(restored)) {
            c->Diag.Step=157;c->Diag.Region=CamCc;c->Diag.Offset=offsets[slot]+4;
            c->Diag.Mask=0x10000;c->Diag.Expected=0x10000;c->Diag.Observed=CamRead(base,offsets[slot]+4);
            c->Faulted=TRUE;return restored;
        }
    }
    c->Diag=diag;return status;
}
