#pragma once

static NTSTATUS CamCrmStates(PUCHAR base,ULONG offset,const ULONG values[2]) {
    for(ULONG i=0;i<2;i++) {
        CamWrite(base,offset+4+i*4,values[i]);
        NTSTATUS status=CamWait(base,offset+4+i*4,0x3fffffffu,values[i]&0x3fffffffu,1000);
        if(!NT_SUCCESS(status))return status;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS CamCrmTransfer(PUCHAR base,ULONG offset,ULONG value) {
    ULONG irq=offset==0x21c?0x24c:offset+0x10;
    CamWrite(base,irq+4,1);
    NTSTATUS status=CamWait(base,irq,1,0,1000);
    if(!NT_SUCCESS(status))return status;
    CamWrite(base,offset,value);
    status=CamWait(base,irq,1,1,100000);
    if(NT_SUCCESS(status))CamWrite(base,irq+4,1);
    return status;
}

static NTSTATUS CamCrmCommand(PUCHAR base,ULONG value) {
    return CamCrmTransfer(base,0x21c,value|0xe0000000u);
}
