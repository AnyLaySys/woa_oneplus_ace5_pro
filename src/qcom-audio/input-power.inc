// OEM input_suspend vote:0=suspend,UINT_MAX=release. Confirm after bounded settling.
static BOOLEAN QchInputFail(QCH_LINK *C,ULONG Error)
{
    C->InputError=(C->InputState<<16)|Error;
    C->InputResult=C->ReplyResult?C->ReplyResult:C->ReplyValue;
    C->Fault=39;C->Phase=92;return TRUE;
}
static BOOLEAN QchInputPoll(PSIA_LINK L,ULONGLONG Now)
{
    QCH_LINK *C=&L->Charge;
    BOOLEAN Cool=QchBatteryFresh(C,Now) && C->Temperature<=ALS_CHARGE_TEMP_RESUME_DK;
    BOOLEAN Detached=C->CableAt && Now-C->CableAt<=2500 && !C->CableOrientation &&
        Now-C->OnlineAt<=2500 && Now-C->VoltageAt<=2500 && !C->UsbOnline && C->UsbVoltage<1000000;
    BOOLEAN Release=Cool||Detached;
    if(C->Phase==92){
        if(!Release)return FALSE;
        C->InputState=4;C->InputDeadline=Now+3000;C->Phase=91;C->Waiting=FALSE;C->ReplyReady=FALSE;
    }
    if(!C->InputState && QchInputSaved()){C->InputState=1;C->InputDeadline=Now+3000;}
    if(!C->InputState){
        if(!QchBatteryFresh(C,Now) || C->Temperature<ALS_CHARGE_TEMP_LIMIT_DK)return FALSE;
        if(!NT_SUCCESS(QchInputSave(TRUE)))return QchInputFail(C,1);
        C->InputState=1;C->InputDeadline=Now+3000;
        C->LoadStopReason=ALS_CHARGE_STOP_TEMPERATURE;C->Phase=91;
    }
    if(C->InputState==7){
        if(!Release)return FALSE;
        C->InputState=4;C->InputDeadline=Now+3000;
    }
    if(Now>=C->InputDeadline)return QchInputFail(C,2);
    if(C->Waiting){if(Now-C->Last>1500)return QchInputFail(C,3);return TRUE;}
    if(C->InputState==2 || C->InputState==5){
        if(!C->ReplyReady || C->ReplyResult || C->ReplyOp!=0x33 || C->ReplyProp!=5)return QchInputFail(C,4);
        if(QchRequest(L,0x32,5,0))C->InputState++;
        return TRUE;
    }
    if(C->InputState==6 || C->InputState==3){
        BOOLEAN Released=C->InputState==6;
        if(!C->ReplyReady || C->ReplyResult || C->ReplyOp!=0x32 || C->ReplyProp!=5)return QchInputFail(C,5);
        if(Released?((LONG)C->UsbLimitUa<=0 && !Detached):C->UsbLimitUa!=0){
            if(Now-C->Last>=50)QchRequest(L,0x32,5,0);
            return TRUE;
        }
        if(Released){
            if(!NT_SUCCESS(QchInputSave(FALSE)))return QchInputFail(C,6);
            C->InputState=0;C->Fault=0;return FALSE;
        }
        C->InputState=7;return FALSE;
    }
    if(QchRequest(L,0x33,5,C->InputState==1?0:MAXULONG))C->InputState++;
    return TRUE;
}
