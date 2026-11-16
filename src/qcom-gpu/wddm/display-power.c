#include "adapter.h"

static ULONG Read(PUCHAR base,ULONG offset) {return READ_REGISTER_ULONG((PULONG)(base+offset));}
static VOID Write(PUCHAR base,ULONG offset,ULONG value) {WRITE_REGISTER_ULONG((PULONG)(base+offset),value);}
static NTSTATUS Command(PUCHAR base,UCHAR command) {
    static const ULONG offsets[]={0x11c,4,0x3c,0x84,0x100,0x104,0x110,0x15c,0x390,0x4c};
    ULONG saved[RTL_NUMBER_OF(offsets)];
    for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++)saved[i]=Read(base,offsets[i]);
    if(!(saved[1]&1) || saved[7] || !Read(base,0) || Read(base,0)==MAXULONG)return STATUS_DEVICE_NOT_READY;
    if(Read(base,8)&2)return STATUS_DEVICE_BUSY;
    Write(base,0x11c,saved[0]|0x200b3f);
    Write(base,4,saved[1]|5);
    Write(base,0x84,(saved[3]&~0xf000fu)|0x1004);
    Write(base,0x100,0);Write(base,0x104,0);
    Write(base,0x110,(saved[6]&0xaaaaaaa8u)|1);
    Write(base,0x15c,0x30006);
    Write(base,0x17c,0x80050000u|command);Write(base,0x17c,0);
    Write(base,0x3c,(saved[2]&0x00ffffffu)|0x10000000u);
    Write(base,0x390,saved[8]&~2u);Write(base,0x4c,4);
    Write(base,0x90,1);
    NTSTATUS status=STATUS_IO_TIMEOUT;
    LARGE_INTEGER pause;pause.QuadPart=-10000;
    for(ULONG i=0;i<200;i++) {
        if(Read(base,0x110)&1){status=STATUS_SUCCESS;break;}
        KeDelayExecutionThread(KernelMode,FALSE,&pause);
    }
    Write(base,0x15c,0);Write(base,0x1ec,1);KeStallExecutionProcessor(1);Write(base,0x1ec,0);
    Write(base,0x110,(saved[6]&0xaaaaaaaau)|1);
    for(ULONG i=RTL_NUMBER_OF(offsets);i--;)if(offsets[i]!=0x110)Write(base,offsets[i],saved[i]);
    return status;
}
NTSTATUS A830DisplayPower(A830_ADAPTER *a,ULONG uid,BOOLEAN on) {
    if(uid && uid!=DISPLAY_ADAPTER_HW_ID)return STATUS_SUCCESS;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_SUCCESS;
    if(uid)a->AdapterOn=on;else a->MonitorOn=on;
    on=a->AdapterOn && a->MonitorOn;
    if(a->PanelOn!=on) {
        PHYSICAL_ADDRESS address;address.QuadPart=0x0ae94000;
        PUCHAR base=MmMapIoSpaceEx(address,0x400,PAGE_READWRITE|PAGE_NOCACHE);
        if(!base)status=STATUS_INSUFFICIENT_RESOURCES;
        else {
            status=Command(base,on?0x29:0x28);
            MmUnmapIoSpace(base,0x400);
            if(NT_SUCCESS(status)) {
                LARGE_INTEGER pause;pause.QuadPart=-200000;
                KeDelayExecutionThread(KernelMode,FALSE,&pause);a->PanelOn=on;
            }
        }
    }
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);return status;
}
