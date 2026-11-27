// Native5V charging. No input-current/FCC votes or UFCS/SC8517 enabling.
static VOID QchNormalExit(PSIA_LINK L,ULONGLONG Now)
{
    static const ULONG P[][2]={{0x30,24},{0x30,34},{0x32,39},{0x30,56}};
    QCH_LINK *C=&L->Charge;
    if(Now-C->ExitAt>3000){C->Fault=10;C->Phase=92;return;}
    if(C->Waiting)return;
    if(C->OffCheck){
        ULONG I=C->OffCheck-1;
        if(!C->ReplyReady || C->ReplyResult || C->ReplyOp!=P[I][0] || C->ReplyProp!=P[I][1] || C->ReplyValue){
            C->Fault=33;C->Phase=92;return;
        }
    }
    if(C->OffCheck==ARRAYSIZE(P)){C->Phase=91;return;}
    ULONG I=C->OffCheck;if(QchRequest(L,P[I][0],P[I][1],0))C->OffCheck++;
}
static VOID QchNormalPoll(PSIA_LINK L,ULONGLONG Now)
{
    static const ULONG Start[][3]={
        {0x31,24,0},{0x31,34,0},{0x30,24,0},{0x30,34,0},{0x32,39,0},{0x30,56,0},
        {0x30,8,0},{0x30,10,0},{0x30,11,0},{0x32,5,0},
        {0x32,0,0},{0x32,1,0},{0x32,24,0},{0x32,22,0},{0x32,9,0},{0x31,24,1},{0x30,24,0}
    };
    static const ULONG Poll[][2]={
        {0x32,20},{0x32,0},{0x32,1},{0x32,5},{0x32,24},{0x32,22},{0x32,9},
        {0x30,8},{0x30,10},{0x30,11},{0x30,24},{0x30,34},{0x32,39},{0x30,56}
    };
    QCH_LINK *C=&L->Charge;
    if(Now-C->GuardAt>=300)QchBatteryGuard(C);
    ULONG Limit=QchLoadFvLimit(C->LoadFvUv);
    if(Limit && C->GaugeCvMv && C->GaugeCvMv!=65535)Limit=min(Limit,C->GaugeCvMv);
    BOOLEAN Full=C->CellValid && C->Soc>=99 && (C->BatteryStatus&0x20);
    if((!C->LoadAt && Now>=C->Deadline) || !QchBatteryFresh(C,Now) || C->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK ||
       C->PackVoltage>=9200 || (C->BatteryStatus&0x900f) || (!Full && (C->BatteryStatus&0x4000)) ||
       (C->GaugeCvMv && C->GaugeCvMv!=65535 && (C->GaugeCvMv<7500 || C->GaugeCvMv>9200)) ||
       !C->UsbOnline || C->UsbVoltage<4500000 || C->UsbVoltage>5500000 ||
       C->SourceEnabled || C->SourceRole || C->Moisture || C->UfcsStarted || C->PeerRunning || C->LoadMos || C->CpActive ||
       Now-C->OnlineAt>3000 || Now-C->VoltageAt>3000 || Now-C->SourceAt>3000 || Now-C->RoleAt>3000 || Now-C->MoistureAt>3000 ||
       (Limit && (Limit<7500 || Limit>9200 || C->PackVoltage>=Limit || max(C->Cell1Mv,C->Cell2Mv)*2>=Limit)) ||
       (C->LoadAt && (!Limit || !QchLoadCurrent(C) || Now-C->LoadFvAt>3000 || Now-C->LoadFccAt>3000 ||
        Now-C->LoadFccMaxAt>3000 || Now-C->InputAt>3000 || (LONG)C->UsbLimitUa<=0))){
        C->LoadStopReason=(!C->LoadAt && Now>=C->Deadline)?3:C->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK?ALS_CHARGE_STOP_TEMPERATURE:2;
        C->Fault=(!C->LoadAt && Now>=C->Deadline)?0:27;QchOff(L);return;
    }
    if(Full){C->LoadStopReason=1;C->Fault=0;QchOff(L);return;}
    if(C->Waiting){if(Now-C->Last>900){C->Fault=3;QchOff(L);}return;}
    if(C->ReplyReady && C->ReplyResult){C->Fault=4;QchOff(L);return;}
    if(C->Phase<120){
        ULONG Step=C->Phase-100;
        if(Step>ARRAYSIZE(Start)){C->Fault=38;QchOff(L);return;}
        if(Step){
            ULONG I=Step-1;
            if(!C->ReplyReady || C->ReplyOp!=Start[I][0] || C->ReplyProp!=Start[I][1]){C->Fault=38;QchOff(L);return;}
            if((I>=2 && I<=5 && C->ReplyValue) || (I==16 && C->ReplyValue!=1)){C->Fault=38;QchOff(L);return;}
            if(I==6 && !Limit){C->Fault=28;QchOff(L);return;}
            if(I==8 && !QchLoadCurrent(C)){C->Fault=37;QchOff(L);return;}
            if(I==9){if((LONG)C->UsbLimitUa<=0){C->Fault=38;QchOff(L);return;}C->LoadReqMv=5000;}
        }
        if(Step==ARRAYSIZE(Start)){
            C->Phase=120;C->LoadAt=Now;C->LoadFvLimitMv=Limit;return;
        }
        if(QchRequest(L,Start[Step][0],Start[Step][1],Start[Step][2]))C->Phase++;
        return;
    }
    if(C->ReplyReady && C->ReplyOp==0x30 &&
       ((C->ReplyProp==24 && C->ReplyValue!=1) || (C->ReplyProp==34 && C->ReplyValue))){C->Fault=38;QchOff(L);return;}
    C->LoadFvLimitMv=Limit;
    if(Now-C->Last>=30){ULONG I=C->LoadQuery%ARRAYSIZE(Poll);if(QchRequest(L,Poll[I][0],Poll[I][1],0))C->LoadQuery++;}
}
