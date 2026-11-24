#include "cam.h"

static NTSTATUS Position(CAM *c,ULONG position) {
    UCHAR data[]={(UCHAR)(position>>2),(UCHAR)(position<<6)};
    NTSTATUS status=CamCciWriteByte(c,0x0c,0,data,sizeof(data));
    if(NT_SUCCESS(status))c->FocusPosition=position;
    c->FocusStatus=status;c->FocusSequence=c->PdafInfo.Sequence;return status;
}

NTSTATUS CamFocusInit(CAM *c) {
    UCHAR id,state=0,data[2];
    NTSTATUS status=CamCciReadByte(c,0x0c,3,&id,1);
    if(NT_SUCCESS(status)&&id!=0x13)status=STATUS_NO_SUCH_DEVICE;
    if(NT_SUCCESS(status))status=CamCciReadByte(c,0x0c,0,data,1);
    if(NT_SUCCESS(status))status=CamCciReadByte(c,0x0c,1,data+1,1);
    if(NT_SUCCESS(status))status=CamCciWriteByte(c,0x0c,2,&state,1);
    if(NT_SUCCESS(status))CamDelay(10000);
    if(NT_SUCCESS(status)) {
        c->LensReady=TRUE;c->FocusPosition=((ULONG)data[0]<<2)|(data[1]>>6);
        while(c->FocusPosition<QCAM_FOCUS_MIN&&NT_SUCCESS(status)){status=Position(c,min(c->FocusPosition+32,QCAM_FOCUS_MIN));CamDelay(7000);}
        if(!c->MainFocusCalibration.Valid) {
            UCHAR calibration[1024];NTSTATUS calibrated=STATUS_SUCCESS;
            for(ULONG offset=0;offset<sizeof(calibration)&&NT_SUCCESS(calibrated);offset+=32)
                calibrated=CamCciRead(c,0x50,(USHORT)(0x530+offset),calibration+offset,32);
            if(NT_SUCCESS(calibrated))CamPdafCalibration(&c->MainFocusCalibration,calibration,sizeof(calibration));
        }
        c->FocusCalibration=c->MainFocusCalibration;
        c->FocusFlags=c->FocusCalibration.Valid?QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS:QCAM_FOCUS_MANUAL;
        c->FocusTarget=min(c->FocusPosition,QCAM_FOCUS_MAX);c->FocusWait=2;c->FocusStable=c->FocusMisses=c->FocusSamples=c->FocusSequence=0;c->FocusComplete=FALSE;c->FocusWindowValid=FALSE;RtlZeroMemory(&c->FocusResult,sizeof(c->FocusResult));
    }
    c->FocusStatus=status;return status;
}

NTSTATUS CamFocusStop(CAM *c) {
    NTSTATUS status=STATUS_SUCCESS;
    while(c->FocusPosition&&NT_SUCCESS(status)) {
        status=Position(c,c->FocusPosition>32?c->FocusPosition-32:0);CamDelay(7000);
    }
    UCHAR state=0x20;NTSTATUS standby=CamCciWriteByte(c,0x0c,2,&state,1);
    c->LensReady=FALSE;c->FocusWait=0;
    return NT_SUCCESS(status)?standby:status;
}

NTSTATUS CamFocusSet(CAM *c,const QCAM_FOCUS_OPTIONS *options) {
    if(!c->LensReady)return STATUS_DEVICE_NOT_READY;
    BOOLEAN automatic=options->Flags==QCAM_FOCUS_AUTO||options->Flags==(QCAM_FOCUS_AUTO|QCAM_FOCUS_CONTINUOUS)||options->Flags==(QCAM_FOCUS_AUTO|QCAM_FOCUS_LOCK);
    if(!automatic&&options->Flags!=QCAM_FOCUS_MANUAL&&options->Flags!=QCAM_FOCUS_LOCK)return STATUS_NOT_SUPPORTED;
    if(automatic&&!c->FocusCalibration.Valid)return STATUS_NOT_SUPPORTED;
    if(options->Flags==QCAM_FOCUS_MANUAL&&(options->Position<QCAM_FOCUS_MIN||options->Position>QCAM_FOCUS_MAX))return STATUS_INVALID_PARAMETER;
    c->FocusFlags=options->Flags;c->FocusStatus=STATUS_SUCCESS;
    c->FocusTarget=automatic||options->Flags==QCAM_FOCUS_LOCK?c->FocusPosition:options->Position;
    c->FocusStable=c->FocusMisses=c->FocusSamples=0;c->FocusComplete=FALSE;c->FocusWindowValid=FALSE;RtlZeroMemory(&c->FocusResult,sizeof(c->FocusResult));c->FocusSequence=c->PdafInfo.Sequence;
    c->FocusWait=automatic?2:c->FocusWait;
    return STATUS_SUCCESS;
}

VOID CamFocusFrame(CAM *c) {
    if(!c->LensReady)return;
    if(c->FocusPosition!=c->FocusTarget) {
        LONG delta=(LONG)c->FocusTarget-(LONG)c->FocusPosition;
        ULONG position=(ULONG)((LONG)c->FocusPosition+(delta>32?32:delta<-32?-32:delta));
        if(!NT_SUCCESS(Position(c,position)))return;
        c->FocusWait=2;return;
    }
    if(c->FocusWait){c->FocusWait--;return;}
    if(!(c->FocusFlags&QCAM_FOCUS_AUTO)||(!((c->FocusFlags&QCAM_FOCUS_CONTINUOUS))&&c->FocusComplete))return;
    if(!c->PdafInfo.FrameBytes||c->PdafInfo.Sequence==c->FocusSequence)return;
    if(++c->FocusSamples<2)return;
    c->FocusSamples=0;c->FocusSequence=c->PdafInfo.Sequence;
    if(c->FocusRoi.Flags)CamPdafRoiOrigin(&c->FocusRoi,c->VideoZoom?c->VideoZoom:QCAM_ZOOM_MIN,&c->FocusRoiX,&c->FocusRoiY);
    else if(c->VideoZoom>QCAM_ZOOM_MIN){c->FocusWindow=4;c->FocusWindowValid=TRUE;}
    else if(c->FocusWindowValid&&c->FocusWindow!=4&&(c->PdafInfo.Sequence&15)<2)c->FocusWindowValid=FALSE;
    if(!NT_SUCCESS(CamPdafPrepare(c)))return;
    CAM_PDAF_RESULT phase=c->FocusRoi.Flags?CamPdafRegion(c->PdafLatest,&c->FocusCalibration,c->FocusRoiX,c->FocusRoiY,9,c->FocusWorkspace):c->FocusWindowValid?
        CamPdafWindow(c->PdafLatest,&c->FocusCalibration,c->FocusWindow,c->FocusWorkspace):
        CamPdafEstimate(c->PdafLatest,c->PdafInfo.FrameBytes,&c->FocusCalibration,c->FocusWorkspace);
    c->FocusResult=phase;
    if(phase.Confidence){c->FocusWindow=phase.Window;c->FocusWindowValid=TRUE;}
    if(!phase.Confidence){c->FocusStable=0;c->FocusMisses++;c->FocusComplete=FALSE;c->FocusWindowValid=FALSE;return;}
    c->FocusMisses=0;
    LONG magnitude=phase.Phase<0?-phase.Phase:phase.Phase;
    if(magnitude<=32) {
        if(++c->FocusStable>=3)c->FocusComplete=TRUE;
        return;
    }
    if(c->FocusComplete&&magnitude<64)return;
    c->FocusStable=0;c->FocusComplete=FALSE;
    LONG delta=max(-128,min(phase.Defocus,128));
    if(!delta)delta=phase.Phase>0?-1:1;
    LONG target=max((LONG)QCAM_FOCUS_MIN,min((LONG)c->FocusPosition+delta,(LONG)QCAM_FOCUS_MAX));
    if(target==(LONG)c->FocusPosition){c->FocusMisses=30;return;}
    c->FocusTarget=(ULONG)target;
}

ULONG CamFocusState(CAM *c) {
    if(!c->LensReady)return 0;
    if(!NT_SUCCESS(c->FocusStatus)||c->FocusMisses>=30)return 4;
    if(c->FocusWait||c->FocusPosition!=c->FocusTarget)return 2;
    if(c->FocusFlags&QCAM_FOCUS_AUTO)return c->FocusComplete?3:2;
    return 3;
}
