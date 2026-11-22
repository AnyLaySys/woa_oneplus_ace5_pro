#pragma once

static BOOLEAN CamDmaStopped(PUCHAR cpas,PUCHAR csid,PUCHAR tfe,PUCHAR hf,PUCHAR sf) {
    if((CamRead(cpas,0xf0)|CamRead(cpas,0xf8))&1)return FALSE;
    if((CamRead(tfe,0x2400)&1)||CamRead(csid,0xe04))return FALSE;
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
