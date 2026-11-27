#pragma once

static BOOLEAN CamDmaResourcesIdle(PUCHAR csid,PUCHAR tfe) {
    for(ULONG writer=0;writer<28;writer++)if(CamRead(tfe,0xd00+writer*0x100)&1)return FALSE;
    for(ULONG path=0;path<9;path++) {
        ULONG base=0x600+path*0x200;
        if((CamRead(csid,base)&0x80000000u)||CamRead(csid,base+4))return FALSE;
    }
    return TRUE;
}

static BOOLEAN CamDmaStopped(PUCHAR cpas,PUCHAR csid,PUCHAR tfe,PUCHAR hf,PUCHAR sf) {
    if((CamRead(cpas,0xf0)|CamRead(cpas,0xf8))&1)return FALSE;
    if(!CamDmaResourcesIdle(csid,tfe))return FALSE;
    return !CamRead64(hf,0xc10)&&!CamRead64(hf,0xc20)&&!CamRead64(sf,0xc10)&&!CamRead64(sf,0xc20);
}

static NTSTATUS CamDmaDrain(PUCHAR cpas,PUCHAR csid,PUCHAR tfe,PUCHAR hf,PUCHAR sf,ULONG us) {
    ULONGLONG deadline=KeQueryInterruptTime()+(ULONGLONG)us*10;
    do {
        if(CamDmaStopped(cpas,csid,tfe,hf,sf))return STATUS_SUCCESS;
        CamDelay(1);
    }while(KeQueryInterruptTime()<deadline);
    return STATUS_IO_TIMEOUT;
}
