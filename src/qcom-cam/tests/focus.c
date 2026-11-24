#include "cam.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

NTSTATUS CamFocusInit(CAM *c);
NTSTATUS CamFocusStop(CAM *c);
NTSTATUS CamFocusSet(CAM *c,const QCAM_FOCUS_OPTIONS *options);
VOID CamFocusFrame(CAM *c);
static ULONG position,writes;
static BOOLEAN failed;
static UCHAR chip=0x13,state=0x20;
NTSTATUS CamCciReadByte(CAM *c,UCHAR slave,UCHAR address,PUCHAR buffer,ULONG bytes) {
    (void)c;assert(slave==0x0c&&bytes==1);
    *buffer=address==3?chip:address==2?state:address==0?(UCHAR)(position>>2):(UCHAR)(position<<6);return STATUS_SUCCESS;
}
NTSTATUS CamCciWriteByte(CAM *c,UCHAR slave,UCHAR address,const UCHAR *buffer,ULONG bytes) {
    (void)c;assert(slave==0x0c);
    if(failed)return STATUS_IO_DEVICE_ERROR;
    if(address==2){assert(bytes==1);state=buffer[0];}
    else {assert(address==0&&bytes==2);ULONG target=((ULONG)buffer[0]<<2)|(buffer[1]>>6);assert(abs((int)target-(int)position)<=32);position=target;writes++;}
    return STATUS_SUCCESS;
}
NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes) {
    (void)c;assert(slave==0x50&&address>=0x530&&address<0x930&&bytes==32);memset(buffer,0,bytes);return STATUS_SUCCESS;
}
NTSTATUS CamPdafPrepare(CAM *c){assert(c->PdafLatest&&c->PdafInfo.FrameBytes==QCAM_PDAF_BYTES);return STATUS_SUCCESS;}
VOID CamDelay(ULONG us){assert(us==7000||us==10000);}
int main(void) {
    CAM c={0};QCAM_FOCUS_OPTIONS options={sizeof(options),QCAM_ABI,480,QCAM_FOCUS_MANUAL,0};
    assert(CamFocusSet(&c,&options)==STATUS_DEVICE_NOT_READY);
    chip=0;assert(CamFocusInit(&c)==STATUS_NO_SUCH_DEVICE);chip=0x13;
    assert(CamFocusInit(&c)==STATUS_SUCCESS&&c.LensReady&&state==0);
    ULONG initial=position,initialWrites=writes;
    for(ULONG i=0;i<500;i++)CamFocusFrame(&c);
    assert(position==initial&&writes==initialWrites&&c.FocusFlags==QCAM_FOCUS_MANUAL);
    options.Position=QCAM_FOCUS_MAX+1;assert(CamFocusSet(&c,&options)==STATUS_INVALID_PARAMETER);
    options.Position=480;
    for(ULONG flags=0;flags<=7;flags++)if(flags!=QCAM_FOCUS_MANUAL&&flags!=QCAM_FOCUS_LOCK) {
        options.Flags=flags;assert(CamFocusSet(&c,&options)==STATUS_NOT_SUPPORTED);
        assert(c.FocusFlags==QCAM_FOCUS_MANUAL&&c.FocusTarget==initial);
    }
    options.Flags=QCAM_FOCUS_MANUAL;assert(CamFocusSet(&c,&options)==STATUS_SUCCESS);
    for(ULONG i=0;i<32;i++)CamFocusFrame(&c);
    assert(position==480&&writes==15);
    assert(c.FocusWait==0);
    for(ULONG i=0;i<500;i++)CamFocusFrame(&c);
    assert(position==480&&writes==15);
    options.Position=864;assert(CamFocusSet(&c,&options)==STATUS_SUCCESS);CamFocusFrame(&c);assert(position==512);
    options.Flags=QCAM_FOCUS_LOCK;assert(CamFocusSet(&c,&options)==STATUS_SUCCESS);
    ULONG locked=position,lockedWrites=writes;
    for(ULONG i=0;i<500;i++)CamFocusFrame(&c);
    assert(position==locked&&writes==lockedWrites&&c.FocusWait==0);
    options.Flags=QCAM_FOCUS_MANUAL;options.Position=64;assert(CamFocusSet(&c,&options)==STATUS_SUCCESS);
    failed=TRUE;ULONG before=position;CamFocusFrame(&c);assert(position==before&&c.FocusStatus==STATUS_IO_DEVICE_ERROR);
    failed=FALSE;for(ULONG i=0;i<32;i++)CamFocusFrame(&c);assert(position==64&&c.FocusStatus==STATUS_SUCCESS&&c.FocusWait==0);
    c.FocusCalibration.Valid=TRUE;c.FocusCalibration.QFormat=4;
    for(ULONG i=0;i<221;i++)c.FocusCalibration.Left[i]=c.FocusCalibration.Right[i]=256;
    for(ULONG i=0;i<48;i++)c.FocusCalibration.Dcc[i]=2880;
    c.PdafLatest=calloc(1,QCAM_PDAF_BYTES);assert(c.PdafLatest);c.PdafInfo.FrameBytes=QCAM_PDAF_BYTES;
    options.Flags=QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS;assert(CamFocusSet(&c,&options)==STATUS_SUCCESS);
    c.FocusWait=0;c.FocusWindow=0;c.FocusWindowValid=TRUE;c.FocusComplete=TRUE;
    for(ULONG i=0;i<2;i++){c.PdafInfo.Sequence++;CamFocusFrame(&c);}
    assert(!c.FocusWindowValid&&!c.FocusComplete&&c.FocusMisses==1);
    before=writes;
    for(ULONG i=0;i<150;i++){c.PdafInfo.Sequence++;CamFocusFrame(&c);}
    assert(writes==before&&position==64&&c.FocusMisses>=30);
    options.Flags=QCAM_FOCUS_LOCK;assert(CamFocusSet(&c,&options)==STATUS_SUCCESS);
    for(ULONG i=0;i<100;i++){c.PdafInfo.Sequence++;CamFocusFrame(&c);}
    assert(writes==before&&position==64);free(c.PdafLatest);c.PdafLatest=NULL;
    assert(CamFocusStop(&c)==STATUS_SUCCESS&&position==0&&state==0x20&&!c.LensReady);
    before=writes;CamFocusFrame(&c);assert(writes==before);
    puts("Lens initialization, manual control, bounded movement, phase confidence gating, lock and stop passed");return 0;
}
