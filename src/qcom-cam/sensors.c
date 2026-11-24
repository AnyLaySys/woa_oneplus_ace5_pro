#include "cam.h"
#include "wide-regs.h"
#include "micro-regs.h"
#include "front-regs.h"

static VOID Line(CAM *c,ULONG pin,BOOLEAN high) {
    PUCHAR b=c->Base[CamTlmm]+pin*0x1000;
    CamWrite(b,0,(CamRead(b,0)&~0x200u)|(high?0:0x200u));KeStallExecutionProcessor(5);
}

static NTSTATUS Clock(CAM *c,BOOLEAN high) {
    Line(c,5,high);
    if(high)for(ULONG i=0;!(CamRead(c->Base[CamTlmm],0x5004)&1);i++) {
        if(i==1000)return STATUS_IO_TIMEOUT;KeStallExecutionProcessor(5);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Byte(CAM *c,UCHAR *value,BOOLEAN read,BOOLEAN last) {
    NTSTATUS status=STATUS_SUCCESS;UCHAR result=0;
    for(ULONG i=0;i<8&&NT_SUCCESS(status);i++) {
        Line(c,4,read||((*value&(0x80u>>i))!=0));status=Clock(c,TRUE);
        if(read)result=(UCHAR)((result<<1)|(CamRead(c->Base[CamTlmm],0x4004)&1));Clock(c,FALSE);
    }
    Line(c,4,read?last:TRUE);
    if(NT_SUCCESS(status))status=Clock(c,TRUE);
    if(NT_SUCCESS(status)&&!read&&((CamRead(c->Base[CamTlmm],0x4004)&1)!=0))status=STATUS_NO_SUCH_DEVICE;
    Clock(c,FALSE);Line(c,4,TRUE);if(read)*value=result;return status;
}

static NTSTATUS Transfer(CAM *c,USHORT address,UCHAR *data,ULONG count,BOOLEAN read) {
    Line(c,4,TRUE);NTSTATUS status=Clock(c,TRUE);
    if(NT_SUCCESS(status)&&!(CamRead(c->Base[CamTlmm],0x4004)&1))status=STATUS_DEVICE_BUSY;
    Line(c,4,FALSE);Clock(c,FALSE);UCHAR value=0x20;
    if(NT_SUCCESS(status))status=Byte(c,&value,FALSE,FALSE);
    value=(UCHAR)(address>>8);if(NT_SUCCESS(status))status=Byte(c,&value,FALSE,FALSE);
    value=(UCHAR)address;if(NT_SUCCESS(status))status=Byte(c,&value,FALSE,FALSE);
    if(read&&NT_SUCCESS(status)) {
        Line(c,4,TRUE);status=Clock(c,TRUE);Line(c,4,FALSE);Clock(c,FALSE);
        value=0x21;if(NT_SUCCESS(status))status=Byte(c,&value,FALSE,FALSE);
    }
    for(ULONG i=0;i<count&&NT_SUCCESS(status);i++)status=Byte(c,data+i,read,i+1==count);
    Line(c,4,FALSE);Clock(c,TRUE);Line(c,4,TRUE);return status;
}

static NTSTATUS Read(CAM *c,USHORT reg,UCHAR *value,ULONG bytes) {
    if(c->Sensor==3)return Transfer(c,reg,value,bytes,TRUE);
    return c->Sensor==2?CamCciReadByte(c,0x3d,(UCHAR)reg,value,bytes):CamCciRead(c,0x1a,reg,value,bytes);
}

static NTSTATUS Write(CAM *c,USHORT reg,UCHAR value) {
    if(c->Sensor==3)return Transfer(c,reg,&value,1,FALSE);
    return c->Sensor==2?CamCciWriteByte(c,0x3d,(UCHAR)reg,&value,1):CamCciWrite(c,0x1a,reg,&value,1);
}

static NTSTATUS Table(CAM *c,const CAM_REG *regs,ULONG count) {
    for(ULONG i=0;i<count;i++) {
        NTSTATUS status=Write(c,(USHORT)regs[i].Address,(UCHAR)regs[i].Value);
        if(!NT_SUCCESS(status))return status;if(regs[i].Delay)CamDelay(regs[i].Delay*1000);
    }
    return STATUS_SUCCESS;
}

NTSTATUS CamOtherSensorInit(CAM *c) {
    NTSTATUS status=STATUS_SUCCESS;
    if(c->Sensor==3)for(ULONG pin=4;pin<=5&&NT_SUCCESS(status);pin++) {
        status=CamSave(c,c->Base[CamTlmm],pin*0x1000+4,0);
        if(NT_SUCCESS(status))status=CamSave(c,c->Base[CamTlmm],pin*0x1000,0x1003);
    }
    UCHAR id[2]={0};if(c->Sensor==2&&NT_SUCCESS(status))status=Write(c,0xfd,0);
    if(NT_SUCCESS(status))status=Read(c,c->Sensor==2?2:0x16,id,2);
    if(!NT_SUCCESS(status))return status;
    c->Info.SensorId=((ULONG)id[0]<<8)|id[1];
    if(c->Info.SensorId!=(c->Sensor==1?0x355u:c->Sensor==2?0x2bu:0x471u))return STATUS_NO_SUCH_DEVICE;
    c->Info.Stage=4;status=Write(c,c->Sensor==2?0xfd:0x0100,0);
    if(NT_SUCCESS(status))status=c->Sensor==1?Table(c,WideInit,RTL_NUMBER_OF(WideInit)):c->Sensor==2?Table(c,MicroInit,RTL_NUMBER_OF(MicroInit)):Table(c,FrontInit,RTL_NUMBER_OF(FrontInit));
    if(NT_SUCCESS(status))status=c->Sensor==1?Table(c,WideMode,RTL_NUMBER_OF(WideMode)):c->Sensor==2?Table(c,MicroMode,RTL_NUMBER_OF(MicroMode)):Table(c,FrontMode,RTL_NUMBER_OF(FrontMode));
    if(NT_SUCCESS(status)) {
        c->Configured=TRUE;c->Info.Flags|=QCAM_INITIALIZED;c->Info.Stage=6;
        c->Info.FrameLength=c->Sensor==1?2532:c->Sensor==2?1221:3762;
        c->Info.Exposure=min(1000u,c->Info.FrameLength-48);c->Info.Gain=0;
        c->FocusFlags=QCAM_FOCUS_LOCK;c->FocusComplete=TRUE;c->FocusCalibration.Valid=FALSE;
    }
    return status;
}

NTSTATUS CamOtherSensorStream(CAM *c,BOOLEAN on) {
    if(!c->Configured)return on?STATUS_INVALID_DEVICE_STATE:STATUS_SUCCESS;
    NTSTATUS status=STATUS_SUCCESS;
    if(c->Sensor==2)status=Write(c,0xfd,3);
    if(NT_SUCCESS(status))status=Write(c,c->Sensor==2?0xc2:0x100,on?1:0);
    return status;
}

NTSTATUS CamOtherSensorExposure(CAM *c,ULONG lines,ULONG gain) {
    if(!c->Configured||!lines||lines>c->Info.FrameLength-48||gain>0x3fff)return STATUS_INVALID_PARAMETER;
    NTSTATUS status=Write(c,c->Sensor==2?0xfd:0x104,1);
    if(NT_SUCCESS(status))status=Write(c,c->Sensor==2?0xe:0x202,(UCHAR)(lines>>8));
    if(NT_SUCCESS(status))status=Write(c,c->Sensor==2?0xf:0x203,(UCHAR)lines);
    ULONG mapped=c->Sensor==2?min(255u,16u*16384/(16384-gain)):1024u-1024u*(16384-gain)/16384;
    if(c->Sensor!=2&&NT_SUCCESS(status))status=Write(c,0x204,(UCHAR)(mapped>>8));
    if(NT_SUCCESS(status))status=Write(c,c->Sensor==2?0x22:0x205,(UCHAR)mapped);
    NTSTATUS released=Write(c,c->Sensor==2?0xfe:0x104,c->Sensor==2?2:0);
    if(NT_SUCCESS(status))status=released;
    if(NT_SUCCESS(status)){c->Info.Exposure=lines;c->Info.Gain=gain;}return status;
}

VOID CamOtherSensorSnapshot(CAM *c) {
    UCHAR value=0;
    if(c->Sensor==2)Write(c,0xfd,3);
    if(NT_SUCCESS(Read(c,c->Sensor==2?0xc2:0x100,&value,1)))c->Trace.SensorStream=value;
    if(c->Sensor!=2&&NT_SUCCESS(Read(c,5,&value,1)))c->Trace.SensorCounter=value;
    static const ULONG regs[]={0xc0,0xc4,0xc8,0xcc,0x4c0,0x8c0,0xcc0,0xec0,0xec4,0xec8,0xecc,0x1184};
    for(ULONG i=0;i<RTL_NUMBER_OF(regs);i++)c->Trace.Dphy[i]=CamRead(CamPhyBase(c),regs[i]);
}
