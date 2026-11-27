#pragma once

/* CSID980 stock full SW reset with immediate halt: cfg0x11, command2.
   Completion never authorizes freeing a frame; bus/TBU drain remains required. */
static NTSTATUS CamIspFdHalt(PUCHAR csid,PUCHAR tfe,ULONG *resetStatus) {
    NTSTATUS status=CamWait(csid,0x654,1,1,50000);
    if(NT_SUCCESS(status)){*resetStatus=STATUS_SUCCESS;return status;}
    for(ULONG i=0;i<28;i++)if(i!=7&&i!=8&&(CamRead(tfe,0xd00+i*0x100)&1))return STATUS_DEVICE_BUSY;
    for(ULONG i=1;i<9;i++)if((CamRead(csid,0x600+i*0x200)&0x80000000u)||CamRead(csid,0x604+i*0x200))return STATUS_DEVICE_BUSY;
    CamWrite(csid,0x8c,MAXULONG);CamWrite(csid,0x14,1);
    CamWrite(csid,0x0c,0x11);CamWrite(csid,0x10,2);
    status=CamWait(csid,0x84,1,1,50000);*resetStatus=(ULONG)status;
    return NT_SUCCESS(status)?CamWait(csid,0x654,1,1,50000):status;
}
