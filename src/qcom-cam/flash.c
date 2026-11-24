#include "cam.h"

static const UCHAR Registers[]={0x3e,0x41,0x42,0x45,0x46,0x49,0x4a,0x4d,0x4e,0xed};

static NTSTATUS Read(CAM *c,UCHAR reg,UCHAR *value) {return CamPmic(c,0x1ee,reg,value,FALSE);}
static NTSTATUS Write(CAM *c,UCHAR reg,UCHAR value) {return CamPmic(c,0x1ee,reg,&value,TRUE);}

NTSTATUS CamFlashStop(CAM *c) {
    if(!c->FlashOwned){c->FlashMode=QCAM_FLASH_OFF;return STATUS_SUCCESS;}
    NTSTATUS status=Write(c,0x4e,0);
    if(NT_SUCCESS(status))status=Write(c,0x46,0);
    for(ULONG i=RTL_NUMBER_OF(Registers);i&&NT_SUCCESS(status);i--)status=Write(c,Registers[i-1],c->FlashSaved[i-1]);
    if(NT_SUCCESS(status)){c->FlashOwned=FALSE;c->FlashMode=QCAM_FLASH_OFF;c->FlashDeadline=0;}
    return status;
}

NTSTATUS CamFlashSet(CAM *c,const QCAM_FLASH_OPTIONS *options) {
    if(options->Mode>QCAM_FLASH_STROBE||options->Power>100||options->TimeoutMs>1280)return STATUS_INVALID_PARAMETER;
    if(options->Mode==QCAM_FLASH_OFF)return CamFlashStop(c);
    if(!c->FlashOwned) {
        UCHAR type=0,subtype=0;NTSTATUS status=Read(c,4,&type);
        if(NT_SUCCESS(status))status=Read(c,5,&subtype);
        if(!NT_SUCCESS(status))return status;
        if(type!=0x18||subtype!=7)return STATUS_NOT_SUPPORTED;
        c->FlashAvailable=TRUE;
        for(ULONG i=0;i<RTL_NUMBER_OF(Registers);i++) {
            status=Read(c,Registers[i],c->FlashSaved+i);if(!NT_SUCCESS(status))return status;
        }
        if(c->FlashSaved[8]&15)return STATUS_DEVICE_BUSY;
        c->FlashOwned=TRUE;
    }
    UCHAR shorted=0,faults=0;NTSTATUS status=Read(c,6,&shorted);
    if(NT_SUCCESS(status))status=Read(c,7,&faults);
    if(NT_SUCCESS(status)&&((shorted&0x41)||(faults&0x7f)))status=STATUS_DEVICE_HARDWARE_ERROR;
    ULONG power=options->Power?options->Power:40;
    ULONG step=options->Mode==QCAM_FLASH_TORCH?5000:12500;
    ULONG current=(options->Mode==QCAM_FLASH_TORCH?250000:1000000)*power/100/2;
    UCHAR target=(UCHAR)(max(current/step,1u)-1),timer=options->Mode==QCAM_FLASH_TORCH?0:(UCHAR)(0x80|(min(max(((options->TimeoutMs?options->TimeoutMs:100)+9)/10,1u),128u)-1));
    if(NT_SUCCESS(status))status=Write(c,0x4e,0);
    if(NT_SUCCESS(status))status=Write(c,0x46,0);
    if(NT_SUCCESS(status))status=Write(c,0x3e,timer);
    if(NT_SUCCESS(status))status=Write(c,0x41,timer);
    if(NT_SUCCESS(status))status=Write(c,0x42,target);
    if(NT_SUCCESS(status))status=Write(c,0x45,target);
    if(NT_SUCCESS(status))status=Write(c,0x49,(UCHAR)((c->FlashSaved[5]&~9u)|(options->Mode==QCAM_FLASH_TORCH?9:0)));
    if(NT_SUCCESS(status))status=Write(c,0x4a,1);
    if(NT_SUCCESS(status))status=Write(c,0x4d,1);
    if(NT_SUCCESS(status))status=Write(c,0xed,24);
    if(NT_SUCCESS(status))status=Write(c,0x46,0x80);
    if(NT_SUCCESS(status))status=Write(c,0x4e,9);
    if(!NT_SUCCESS(status)){CamFlashStop(c);return status;}
    if(options->Mode==QCAM_FLASH_STROBE){c->Trace.StrobeCount++;c->Trace.StrobeStarted=KeQueryInterruptTime();}
    c->FlashMode=options->Mode;c->FlashPower=power;c->FlashTimeout=timer?((timer&127)+1)*10:0;
    c->FlashDeadline=timer?KeQueryInterruptTime()+(ULONGLONG)c->FlashTimeout*10000:0;return STATUS_SUCCESS;
}

VOID CamFlashFrame(CAM *c) {
    if(c->FlashDeadline&&KeQueryInterruptTime()>=c->FlashDeadline)CamFlashStop(c);
}
