#include "cam.h"
#include "imx906-regs.h"

static NTSTATUS Set(CAM *c,USHORT address,UCHAR value) { return CamCciWrite(c,0x1a,address,&value,1); }
static NTSTATUS Set16(CAM *c,USHORT address,USHORT value) {
    UCHAR bytes[]={(UCHAR)(value>>8),(UCHAR)value};return CamCciWrite(c,0x1a,address,bytes,2);
}

static NTSTATUS Table(CAM *c,const SENSOR_REG *regs,ULONG count) {
    for(ULONG i=0;i<count;i++) {
        NTSTATUS status=Set(c,regs[i].Address,regs[i].Value);
        if(!NT_SUCCESS(status))return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CamSensorInit(CAM *c) {
    if(c->Sensor)return CamOtherSensorInit(c);
    UCHAR id[2];
    NTSTATUS status=CamCciRead(c,0x1a,0x0016,id,2);
    if(!NT_SUCCESS(status))return status;
    c->Info.SensorId=((ULONG)id[0]<<8)|id[1];
    if(c->Info.SensorId!=0x0906)return STATUS_NO_SUCH_DEVICE;
    c->Info.Stage=4;
    status=Set(c,0x0100,0);
    if(NT_SUCCESS(status))status=Table(c,Imx906Init,RTL_NUMBER_OF(Imx906Init));
    if(!NT_SUCCESS(status))return status;
    if(!c->MainQscValid) {
        BOOLEAN nonzero=FALSE,nonff=FALSE;
        for(ULONG offset=0;offset<sizeof(c->MainQsc)&&NT_SUCCESS(status);offset+=32) {
            status=CamCciRead(c,0x50,(USHORT)(0x2210+offset),c->MainQsc+offset,32);
            if(NT_SUCCESS(status))for(ULONG i=offset;i<offset+32;i++){nonzero|=c->MainQsc[i]!=0;nonff|=c->MainQsc[i]!=255;}
        }
        if(NT_SUCCESS(status)&&(!nonzero||!nonff))status=STATUS_DATA_ERROR;
        c->MainQscValid=NT_SUCCESS(status);
    }
    if(NT_SUCCESS(status))for(ULONG offset=0;offset<sizeof(c->MainQsc)&&NT_SUCCESS(status);offset+=8)
        status=CamCciWrite(c,0x1a,(USHORT)(0x1000+offset),c->MainQsc+offset,8);
    if(!NT_SUCCESS(status))return status;
    c->Info.Flags|=QCAM_CALIBRATED;c->Info.Stage=5;
    status=Table(c,Imx906Mode,RTL_NUMBER_OF(Imx906Mode));
    if(NT_SUCCESS(status)) {
        UCHAR geometry[4];status=CamCciRead(c,0x1a,0x034c,geometry,4);
        if(NT_SUCCESS(status)&&(((ULONG)geometry[0]<<8)|geometry[1])!=QCAM_WIDTH)status=STATUS_DATA_ERROR;
        if(NT_SUCCESS(status)&&(((ULONG)geometry[2]<<8)|geometry[3])!=QCAM_HEIGHT)status=STATUS_DATA_ERROR;
    }
    if(NT_SUCCESS(status)) {
        c->Info.Flags|=QCAM_INITIALIZED;c->Configured=TRUE;
        c->Info.Exposure=0x0e1c;c->Info.Gain=0x3f00;c->Info.FrameLength=0x0eb4;c->Info.Stage=6;
    }
    if(NT_SUCCESS(status))status=CamFocusInit(c);
    return status;
}

NTSTATUS CamSensorStream(CAM *c,BOOLEAN on) {
    if(c->Sensor)return CamOtherSensorStream(c,on);
    if(!c->Configured)return on?STATUS_INVALID_DEVICE_STATE:STATUS_SUCCESS;
    NTSTATUS status=Set(c,0x0100,on?1:0);
    if(NT_SUCCESS(status)) {
        UCHAR value;status=CamCciRead(c,0x1a,0x0100,&value,1);
        if(NT_SUCCESS(status)&&value!=(on?1:0))status=STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

NTSTATUS CamSensorExposure(CAM *c,ULONG lines,ULONG gain) {
    if(c->Sensor)return CamOtherSensorExposure(c,lines,gain);
    if(!c->Configured||(c->DmaActive&&!c->StreamFile))return STATUS_INVALID_DEVICE_STATE;
    if(!lines||lines>c->Info.FrameLength-48||gain>0x3fff)return STATUS_INVALID_PARAMETER;
    NTSTATUS status=Set(c,0x0104,1);
    if(!NT_SUCCESS(status))return status;
    status=Set16(c,0x0202,(USHORT)lines);
    if(NT_SUCCESS(status))status=Set16(c,0x0224,(USHORT)max(lines/2,1u));
    if(NT_SUCCESS(status))status=Set16(c,0x0204,(USHORT)gain);
    if(NT_SUCCESS(status))status=Set16(c,0x0216,(USHORT)gain);
    NTSTATUS release=Set(c,0x0104,0);
    if(NT_SUCCESS(status))status=release;
    if(NT_SUCCESS(status)){c->Info.Exposure=lines;c->Info.Gain=gain;}
    return status;
}
