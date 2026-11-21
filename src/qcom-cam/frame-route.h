#pragma once

static NTSTATUS CamFrameRouteBind(CAM *c) {
    if(c->FrameRouteOwned||c->DmaActive||c->RxActive||c->ScmPending||c->CdmPending||c->TbuPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    PUCHAR base=c->Base[CamSmmu];
    if(CamRead(base,0x20)!=0x4c017e84||CamRead(base,0x24)!=0x60000053||(CamRead(base,0)&1))return STATUS_NOT_SUPPORTED;
    ULONG slot=MAXULONG,alternate=MAXULONG,original=0;
    for(ULONG i=0;i<128;i++) {
        ULONG smr=CamRead(base,0x800+i*4);
        if(!(smr&0x80000000u))continue;
        ULONG mask=(smr>>16)&0x7fff,control=CamRead(base,0xc00+i*4);
        if(((smr^0x1c00)&~mask&0x7fff)==0) {
            if(smr!=0x80001c00||slot!=MAXULONG)return STATUS_DEVICE_BUSY;
            slot=i;original=control;
        }
    }
    if(slot==MAXULONG||(original&0x30000)||(original&255)>=83)return STATUS_NOT_SUPPORTED;
    ULONG originalBank=original&255;
    if(!CamDmaIdentity(original,CamRead(base,0x1000+originalBank*4),CamRead(base,0x80000+originalBank*4096))||
       CamSmmuFault(CamRead(base,0x80058+originalBank*4096)))return STATUS_NOT_SUPPORTED;
    for(ULONG i=0;i<128;i++) {
        ULONG smr=CamRead(base,0x800+i*4),control=CamRead(base,0xc00+i*4),bank=control&255;
        if(!(smr&0x80000000u)||(control&0x30000)||bank>=83||bank==(original&255))continue;
        ULONG cbar=CamRead(base,0x1000+bank*4),sctlr=CamRead(base,0x80000+bank*4096);
        if(CamDmaIdentity(control,cbar,sctlr)&&!(sctlr&0x0ff00000u)&&
           !CamSmmuFault(CamRead(base,0x80058+bank*4096))){alternate=control;break;}
    }
    if(alternate==MAXULONG)return STATUS_NOT_SUPPORTED;
    ULONG bank=alternate&255;
    c->FrameRoute[0]=slot;c->FrameRoute[1]=0x80001c00;c->FrameRoute[2]=original;c->FrameRoute[3]=alternate;
    c->Trace.FrameRoute[0]=slot;c->Trace.FrameRoute[1]=original;c->Trace.FrameRoute[2]=alternate;
    c->Trace.FrameRoute[3]=CamRead(base,0x1000+bank*4);c->Trace.FrameRoute[4]=CamRead(base,0x80000+bank*4096);
    c->FrameRouteOwned=TRUE;
    c->Trace.FrameRouteStatus[1]=STATUS_DEVICE_BUSY;
    CamWrite(base,0x800+slot*4,0);
    if(CamRead(base,0x800+slot*4))return STATUS_IO_DEVICE_ERROR;
    CamWrite(base,0xc00+slot*4,alternate);
    if(CamRead(base,0xc00+slot*4)!=alternate)return STATUS_IO_DEVICE_ERROR;
    CamWrite(base,0x800+slot*4,0x80001c00);
    if(CamRead(base,0x800+slot*4)!=0x80001c00)return STATUS_IO_DEVICE_ERROR;
    c->Trace.SmmuState[7]=alternate;c->Trace.SmmuState[5]=c->Trace.FrameRoute[3];c->Trace.SmmuState[6]=c->Trace.FrameRoute[4];
    return STATUS_SUCCESS;
}

static NTSTATUS CamFrameRouteRestore(CAM *c) {
    if(!c->FrameRouteOwned)return STATUS_SUCCESS;
    if(c->DmaActive||c->RxActive||c->ScmPending||c->CdmPending||c->TbuPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    PUCHAR base=c->Base[CamSmmu];ULONG slot=c->FrameRoute[0],original=c->FrameRoute[2];
    if(slot>=128||(original&0x30000)||(original&255)>=83)return STATUS_NOT_SUPPORTED;
    ULONG smr=CamRead(base,0x800+slot*4),control=CamRead(base,0xc00+slot*4);
    if((smr&&smr!=c->FrameRoute[1])||(control!=original&&control!=c->FrameRoute[3]))return STATUS_DEVICE_BUSY;
    CamWrite(base,0x800+slot*4,0);
    if(CamRead(base,0x800+slot*4))return STATUS_IO_DEVICE_ERROR;
    NTSTATUS status=CamSmmuRestoreControl(base,0xc00+slot*4,original,83);
    if(!NT_SUCCESS(status))return status;
    CamWrite(base,0x800+slot*4,c->FrameRoute[1]);
    if(CamRead(base,0x800+slot*4)!=c->FrameRoute[1])return STATUS_IO_DEVICE_ERROR;
    c->FrameRouteOwned=FALSE;return STATUS_SUCCESS;
}
