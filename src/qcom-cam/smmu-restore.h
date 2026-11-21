#pragma once

static NTSTATUS CamSmmuRestoreControl(PUCHAR base,ULONG offset,ULONG original,ULONG banks) {
    if(offset<0xc00||offset>=0xe00||(offset&3)||!banks||banks>255)return STATUS_INVALID_PARAMETER;
    ULONG slot=(offset-0xc00)/4,type=(original>>16)&3;
    if(CamRead(base,0x800+slot*4)&0x80000000u)return STATUS_DEVICE_BUSY;
    if(type==3||(type==0&&(original&255)>=banks))return STATUS_SUCCESS;
    CamWrite(base,offset,original);
    return CamRead(base,offset)==original?STATUS_SUCCESS:STATUS_IO_DEVICE_ERROR;
}
