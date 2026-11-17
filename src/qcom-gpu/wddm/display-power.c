#include "adapter.h"

static ULONG Read(PUCHAR base,ULONG offset) {return READ_REGISTER_ULONG((PULONG)(base+offset));}
static VOID Write(PUCHAR base,ULONG offset,ULONG value) {WRITE_REGISTER_ULONG((PULONG)(base+offset),value);}
static NTSTATUS Command(PUCHAR base,UCHAR command,USHORT brightness) {
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
    Write(base,0x17c,command==0x51?0xC0390003u:0x80050000u|command);
    Write(base,0x17c,command==0x51?0xFF000051u|((ULONG)(brightness>>8)<<8)|((ULONG)(brightness&0xFF)<<16):0);
    Write(base,0x3c,(saved[2]&0x00ffffffu)|0x10000000u);
    Write(base,0x390,saved[8]&~2u);Write(base,0x4c,command==0x51?8:4);
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
static NTSTATUS Send(UCHAR command,USHORT brightness) {
    PHYSICAL_ADDRESS address;address.QuadPart=0x0ae94000;
    PUCHAR base=MmMapIoSpaceEx(address,0x400,PAGE_READWRITE|PAGE_NOCACHE);
    if(!base)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=Command(base,command,brightness);MmUnmapIoSpace(base,0x400);return status;
}
static VOID Reference(PVOID context) {UNREFERENCED_PARAMETER(context);}
static NTSTATUS Levels(PVOID context,ULONG bytes,PUCHAR count,PUCHAR levels) {
    UNREFERENCED_PARAMETER(context);*count=101;if(bytes<101)return STATUS_BUFFER_TOO_SMALL;
    for(ULONG i=0;i<101;i++)levels[i]=(UCHAR)i;return STATUS_SUCCESS;
}
static NTSTATUS Get(PVOID context,PUCHAR brightness) {*brightness=((A830_ADAPTER *)context)->Brightness;return STATUS_SUCCESS;}
static VOID Fade(PDEVICE_OBJECT device,PVOID context) {
    UNREFERENCED_PARAMETER(device);A830_ADAPTER *a=context;
    for(;;) {
        KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
        LONG delta=(a->Brightness*0xDBBu+50)/100-a->PanelBrightness;
        NTSTATUS status=STATUS_SUCCESS;
        if(delta && a->PanelOn && !a->Stopping) {
            USHORT next=(USHORT)(a->PanelBrightness+(delta>0?(delta+3)/4:(delta-3)/4));
            status=Send(0x51,next);if(NT_SUCCESS(status))a->PanelBrightness=next;
        }
        if(!delta || !a->PanelOn || a->Stopping || !NT_SUCCESS(status)) {
            a->BrightnessQueued=FALSE;KeSetEvent(&a->BrightnessIdle,IO_NO_INCREMENT,FALSE);
            KeReleaseMutex(&a->DisplayPowerLock,FALSE);return;
        }
        KeReleaseMutex(&a->DisplayPowerLock,FALSE);
        LARGE_INTEGER pause;pause.QuadPart=-100000;KeDelayExecutionThread(KernelMode,FALSE,&pause);
    }
}
VOID A830BrightnessStop(A830_ADAPTER *a) {
    KeWaitForSingleObject(&a->BrightnessIdle,Executive,KernelMode,FALSE,NULL);
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);
}
static NTSTATUS Set(PVOID context,UCHAR brightness) {
    A830_ADAPTER *a=context;if(brightness>100)return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    a->Brightness=brightness;
    if(a->PanelOn && !a->Stopping && !a->BrightnessQueued && a->PanelBrightness!=(brightness*0xDBBu+50)/100) {
        a->BrightnessQueued=TRUE;KeClearEvent(&a->BrightnessIdle);IoQueueWorkItem(a->BrightnessWork,Fade,DelayedWorkQueue,a);
    }
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);return STATUS_SUCCESS;
}
NTSTATUS A830BrightnessInterface(PVOID context,PQUERY_INTERFACE query) {
    static const GUID guid={0xfde5bba4,0xb3f9,0x46fb,{0xbd,0xaa,0x07,0x28,0xce,0x31,0x00,0xb4}};
    if(!RtlEqualMemory(query->InterfaceType,&guid,sizeof(guid)) || query->Version!=1)return STATUS_NOT_SUPPORTED;
    if(query->Size<sizeof(DXGK_BRIGHTNESS_INTERFACE) || !query->Interface)return STATUS_INVALID_PARAMETER;
    DXGK_BRIGHTNESS_INTERFACE value={sizeof(value),1,context,Reference,Reference,Levels,Set,Get};
    *(DXGK_BRIGHTNESS_INTERFACE *)query->Interface=value;return STATUS_SUCCESS;
}
NTSTATUS A830DisplayPower(A830_ADAPTER *a,ULONG uid,BOOLEAN on) {
    if(uid && uid!=DISPLAY_ADAPTER_HW_ID)return STATUS_SUCCESS;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=STATUS_SUCCESS;
    if(uid)a->AdapterOn=on;else a->MonitorOn=on;
    on=a->AdapterOn && a->MonitorOn;
    if(a->PanelOn!=on) {
        status=Send(on?0x29:0x28,0);
        if(NT_SUCCESS(status)) {
            LARGE_INTEGER pause;pause.QuadPart=-200000;
            KeDelayExecutionThread(KernelMode,FALSE,&pause);a->PanelOn=on;
            if(on){USHORT brightness=(USHORT)((a->Brightness*0xDBBu+50)/100);status=Send(0x51,brightness);if(NT_SUCCESS(status))a->PanelBrightness=brightness;}
        }
    }
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);return status;
}
