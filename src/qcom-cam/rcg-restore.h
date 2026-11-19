#pragma once

static NTSTATUS CamRcgRestore(PUCHAR base,ULONG offset,ULONG config,ULONG command) {
    CamWrite(base,offset,CamRead(base,offset)|2);
    CamWrite(base,offset+4,config);
    CamWrite(base,offset,CamRead(base,offset)|1);
    NTSTATUS status=CamWait(base,offset,1,0,10000);
    if(NT_SUCCESS(status))CamWrite(base,offset,command);
    return status;
}
