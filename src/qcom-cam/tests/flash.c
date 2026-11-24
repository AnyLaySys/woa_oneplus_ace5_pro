#include "cam.h"
#include <assert.h>
#include <stdio.h>

NTSTATUS CamFlashSet(CAM *c,const QCAM_FLASH_OPTIONS *options);
VOID CamFlashFrame(CAM *c);
static UCHAR registers[256],original[256];
static ULONG operations,fail;
static ULONGLONG clockValue;
ULONGLONG KeQueryInterruptTime(void){return clockValue;}
NTSTATUS CamPmic(CAM *c,ULONG ppid,UCHAR reg,UCHAR *value,BOOLEAN write) {
    (void)c;assert(ppid==0x1ee);
    if(++operations==fail)return STATUS_IO_DEVICE_ERROR;
    if(write)registers[reg]=*value;else *value=registers[reg];return STATUS_SUCCESS;
}
int main(void) {
    CAM c={0};registers[4]=0x18;registers[5]=7;registers[0xed]=10;memcpy(original,registers,256);
    QCAM_FLASH_OPTIONS options={sizeof(options),QCAM_ABI,QCAM_FLASH_TORCH,40,0,0,0};
    assert(CamFlashSet(&c,&options)==STATUS_SUCCESS&&c.FlashAvailable&&c.FlashOwned);
    assert(registers[0x46]==0x80&&registers[0x4e]==9&&registers[0x42]==9&&registers[0x45]==9&&registers[0x49]==9);
    assert(!registers[0x3e]&&!registers[0x41]&&!c.FlashDeadline);
    assert(CamFlashStop(&c)==STATUS_SUCCESS&&!c.FlashOwned&&!memcmp(original,registers,256));
    options.Mode=QCAM_FLASH_STROBE;options.Power=100;options.TimeoutMs=100;
    assert(CamFlashSet(&c,&options)==STATUS_SUCCESS&&c.FlashDeadline&&registers[0x42]==39&&registers[0x49]==0);
    clockValue=c.FlashDeadline-1;CamFlashFrame(&c);assert(c.FlashOwned);
    clockValue++;CamFlashFrame(&c);assert(!c.FlashOwned&&!memcmp(original,registers,256));
    options.Power=101;assert(CamFlashSet(&c,&options)==STATUS_INVALID_PARAMETER);options.Power=40;
    for(ULONG i=14;i<26;i++) {
        operations=0;fail=i;
        assert(CamFlashSet(&c,&options)==STATUS_IO_DEVICE_ERROR&&!c.FlashOwned&&!memcmp(original,registers,256));
    }
    fail=0;registers[7]=1;assert(CamFlashSet(&c,&options)==STATUS_DEVICE_HARDWARE_ERROR&&!c.FlashOwned);registers[7]=0;
    registers[0x4e]=4;assert(CamFlashSet(&c,&options)==STATUS_DEVICE_BUSY&&!c.FlashOwned);registers[0x4e]=0;
    puts("Flash channel routing, torch current, strobe timer, fault gating and failed-write restoration passed");return 0;
}
