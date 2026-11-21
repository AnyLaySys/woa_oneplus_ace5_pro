#pragma once

static NTSTATUS CamCdmRoute(CAM *c) {
    PUCHAR b=c->Base[CamSmmu];
    if(CamRead(b,0x20)!=0x4c017e84||CamRead(b,0x24)!=0x60000053||
       (CamRead(b,0)&1))return STATUS_NOT_SUPPORTED;
    ULONG freeSlot=MAXULONG,existing=MAXULONG,identity=MAXULONG,existingControl=0;
    for(ULONG i=0;i<128;i++) {
        ULONG smr=CamRead(b,0x800+i*4),control=CamRead(b,0xc00+i*4);
        if(!(smr&0x80000000u)) {
            if(!smr&&freeSlot==MAXULONG)freeSlot=i;
            continue;
        }
        ULONG mask=(smr>>16)&0x7fff;
        if(((smr^0x1860)&~mask&0x7fff)==0) {
            if(smr!=0x80001860||existing!=MAXULONG)return STATUS_DEVICE_BUSY;
            existing=i;existingControl=control;
        }
        ULONG bank=control&255;
        if((control&0x30000)==0&&bank<0x53) {
            ULONG cbar=CamRead(b,0x1000+bank*4),sctlr=CamRead(b,0x80000+bank*4096);
            if(CamDmaIdentity(control,cbar,sctlr)) {
                if(identity==MAXULONG&&!(sctlr&0x0ff00000u))identity=control;
            } else if(existing==i)return STATUS_NOT_SUPPORTED;
        } else if(existing==i) {
            if(((control>>16)&3)!=1)return STATUS_NOT_SUPPORTED;
        }
    }
    if(existing!=MAXULONG) {
        c->Trace.CommandRoute[0]=existing;c->Trace.CommandRoute[1]=existingControl;
        if(!(existingControl&0x30000)) {
            c->Trace.CommandRoute[2]=CamRead(b,0x1000+(existingControl&255)*4);
            c->Trace.CommandRoute[3]=CamRead(b,0x80000+(existingControl&255)*4096);
        } else c->Trace.CommandRoute[2]=c->Trace.CommandRoute[3]=0;
        return ((existingControl>>16)&3)<=1?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
    }
    if(freeSlot==MAXULONG||identity==MAXULONG)return STATUS_NOT_SUPPORTED;
    if(CamRead(b,0x800+freeSlot*4))return STATUS_DEVICE_BUSY;
    ULONG control=identity;
    c->Diag.Region=CamSmmu;c->Diag.Slot=freeSlot;RtlCopyMemory(c->Diag.Resource,"cdm-sid",8);
    NTSTATUS status=CamSave(c,b,0xc00+freeSlot*4,control);
    if(!NT_SUCCESS(status))return status;
    c->Diag.Offset=0xc00+freeSlot*4;c->Diag.Mask=MAXULONG;c->Diag.Expected=control;
    c->Diag.Observed=CamRead(b,0xc00+freeSlot*4);
    if(c->Diag.Observed!=control)return STATUS_IO_DEVICE_ERROR;
    status=CamSave(c,b,0x800+freeSlot*4,0x80001860);
    if(!NT_SUCCESS(status))return status;
    c->Diag.Slot=freeSlot;
    c->Trace.CommandRoute[0]=freeSlot;c->Trace.CommandRoute[1]=control;
    c->Trace.CommandRoute[2]=CamRead(b,0x1000+(control&255)*4);
    c->Trace.CommandRoute[3]=CamRead(b,0x80000+(control&255)*4096);
    return CamRead(b,0x800+freeSlot*4)==0x80001860?STATUS_SUCCESS:STATUS_IO_DEVICE_ERROR;
}
