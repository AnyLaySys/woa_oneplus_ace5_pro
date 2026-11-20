#pragma once

static NTSTATUS CamCdmAbortCore(PUCHAR base) {
    CamWrite(base,0x30,0);
    if(CamRead(base,0x30))return STATUS_IO_DEVICE_ERROR;
    CamWrite(base,0x1c,3);
    CamWait(base,0x28,2,2,20000);
    CamWrite(base,0x34,MAXULONG);CamWrite(base,0x38,1);
    if(CamRead(base,0x44)&1)return STATUS_IO_DEVICE_ERROR;
    CamWrite(base,0x10,1);
    NTSTATUS status=CamWait(base,0x44,1,1,50000);
    if(!NT_SUCCESS(status))return status;
    CamWrite(base,0x1c,0);
    if(CamRead(base,0x1c)||(CamRead(base,0x6c)&0x01ff01ff)||
       (CamRead(base,0x70)&0x01ff01ff))return STATUS_DEVICE_BUSY;
    CamWrite(base,0x34,MAXULONG);CamWrite(base,0x38,1);
    return STATUS_SUCCESS;
}
