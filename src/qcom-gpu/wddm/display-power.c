#include "adapter.h"
#define A830_BRIGHTNESS_DURATION_MS 180u
#define A830_BRIGHTNESS_INTERVAL_MS 9u
#define A830_BRIGHTNESS_MAX_ADVANCE_MS 18u

static ULONG Read(PUCHAR base,ULONG offset) {return READ_REGISTER_ULONG((PULONG)(base+offset));}
static VOID Write(PUCHAR base,ULONG offset,ULONG value) {WRITE_REGISTER_ULONG((PULONG)(base+offset),value);}
static NTSTATUS Command(PUCHAR base,const ULONG *words,ULONG bytes) {
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
    for(ULONG i=0;i<bytes/4;i++)Write(base,0x17c,words[i]);
    if((bytes/4)&1)Write(base,0x17c,0);
    Write(base,0x3c,(saved[2]&0x00ffffffu)|0x10000000u);
    Write(base,0x390,saved[8]&~2u);Write(base,0x4c,bytes);
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
static NTSTATUS Transfer(const ULONG *words,ULONG bytes) {
    PHYSICAL_ADDRESS address;address.QuadPart=0x0ae94000;
    PUCHAR base=MmMapIoSpaceEx(address,0x400,PAGE_READWRITE|PAGE_NOCACHE);
    if(!base)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=Command(base,words,bytes);MmUnmapIoSpace(base,0x400);return status;
}
static NTSTATUS Send(UCHAR command,USHORT brightness) {
    ULONG words[]={command==0x51?0xc0390003u:0x80050000u|command,
        0xff000051u|((ULONG)(brightness>>8)<<8)|((ULONG)(brightness&255)<<16)};
    return Transfer(words,command==0x51?8:4);
}
NTSTATUS A830PanelRefresh(ULONG hz) {
    if(hz!=60 && hz!=72 && hz!=90 && hz!=120)return STATUS_INVALID_PARAMETER;
    ULONG words[]={0x40390004,0x00a55aff,0x00150060|(hz==72?4u<<8:hz==90?2u<<8:0),
        0x40390004,0x02a55aff,0x40390005,0x000003c0,0,
        0x40390009,(hz==72?0x008300b0u:0x008000b0u),0x00000010,0,
        0x40390004,0x2da55aff,0x40390003,(hz==90?0x85u:0x83u)|(hz==60?1u<<8:0),
        0x001500f2|(hz==60?1u<<8:0),0xc0390004,0x00a55aff};
    return Transfer(words,sizeof(words));
}
static VOID Reference(PVOID context) {UNREFERENCED_PARAMETER(context);}
static NTSTATUS Levels(PVOID context,ULONG bytes,PUCHAR count,PUCHAR levels) {
    UNREFERENCED_PARAMETER(context);*count=101;if(bytes<101)return STATUS_BUFFER_TOO_SMALL;
    for(ULONG i=0;i<101;i++)levels[i]=(UCHAR)i;return STATUS_SUCCESS;
}
static NTSTATUS Get(PVOID context,PUCHAR brightness) {*brightness=((A830_ADAPTER *)context)->Brightness;return STATUS_SUCCESS;}
static NTSTATUS Adjust(A830_ADAPTER *a,BOOLEAN off) {
    LONGLONG position=0,origin=0;ULONGLONG tick=0;ULONG elapsed=0,failures=0;USHORT previous=MAXUSHORT;
    NTSTATUS status=STATUS_SUCCESS;
    PEX_TIMER timer=ExAllocateTimer(NULL,NULL,EX_TIMER_HIGH_RESOLUTION);
    for(;;) {
        USHORT target=off?0:A830BacklightLevel(a->Brightness);
        if(!a->PanelOn || (!off && (a->Stopping || !a->AdapterOn || !a->MonitorOn)) || a->PanelBrightness==target)break;
        ULONGLONG now=KeQueryInterruptTime();
        if(!tick)position=(LONGLONG)a->PanelBrightness*65536;
        if(previous!=target){origin=position;elapsed=0;previous=target;}
        ULONGLONG dt=tick?(now-tick)/10000:A830_BRIGHTNESS_INTERVAL_MS;tick=now;
        elapsed=(ULONG)min(elapsed+min(max(dt,1ULL),(ULONGLONG)A830_BRIGHTNESS_MAX_ADVANCE_MS),(ULONGLONG)A830_BRIGHTNESS_DURATION_MS);
        LONGLONG t=(LONGLONG)elapsed*65536/A830_BRIGHTNESS_DURATION_MS,s=t*t/65536*(196608-2*t)/65536;
        position=origin+((LONGLONG)target*65536-origin)*s/65536;
        USHORT next=(USHORT)((position+32768)/65536);
        if(next!=a->PanelBrightness) {
            status=Send(0x51,next);
            if(NT_SUCCESS(status)){a->PanelBrightness=next;failures=0;}
            else {
                if(status!=STATUS_DEVICE_BUSY || ++failures>=3)break;
                position=(LONGLONG)a->PanelBrightness*65536;previous=MAXUSHORT;
            }
        }
        if(!off)KeReleaseMutex(&a->DisplayPowerLock,FALSE);
        if(timer){ExSetTimer(timer,-(LONGLONG)A830_BRIGHTNESS_INTERVAL_MS*10000,0,NULL);KeWaitForSingleObject(timer,Executive,KernelMode,FALSE,NULL);}
        else {LARGE_INTEGER pause;pause.QuadPart=-(LONGLONG)A830_BRIGHTNESS_INTERVAL_MS*10000;KeDelayExecutionThread(KernelMode,FALSE,&pause);}
        if(!off) {
            KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
            if(a->PanelBrightness!=next){tick=0;previous=MAXUSHORT;}
        }
    }
    if(timer)ExDeleteTimer(timer,TRUE,TRUE,NULL);
    return status;
}
static VOID Fade(PDEVICE_OBJECT device,PVOID context) {
    UNREFERENCED_PARAMETER(device);A830_ADAPTER *a=context;
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    Adjust(a,FALSE);
    a->BrightnessQueued=FALSE;KeSetEvent(&a->BrightnessIdle,IO_NO_INCREMENT,FALSE);
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);
}
VOID A830BrightnessStop(A830_ADAPTER *a) {
    KeWaitForSingleObject(&a->BrightnessIdle,Executive,KernelMode,FALSE,NULL);
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);
}
static VOID QueueFade(A830_ADAPTER *a) {
    if(a->PanelOn && !a->Stopping && !a->BrightnessQueued && a->PanelBrightness!=A830BacklightLevel(a->Brightness)) {
        a->BrightnessQueued=TRUE;KeClearEvent(&a->BrightnessIdle);IoQueueWorkItem(a->BrightnessWork,Fade,DelayedWorkQueue,a);
    }
}
static NTSTATUS Set(PVOID context,UCHAR brightness) {
    A830_ADAPTER *a=context;if(brightness>100)return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&a->DisplayPowerLock,Executive,KernelMode,FALSE,NULL);
    a->Brightness=brightness;QueueFade(a);
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
        if(!on)status=Adjust(a,TRUE);
        if(NT_SUCCESS(status))status=Send(on?0x29:0x28,0);
        if(NT_SUCCESS(status)) {
            LARGE_INTEGER pause;pause.QuadPart=-200000;
            KeDelayExecutionThread(KernelMode,FALSE,&pause);a->PanelOn=on;
        }
    }
    if(on && NT_SUCCESS(status))QueueFade(a);
    KeReleaseMutex(&a->DisplayPowerLock,FALSE);return status;
}
