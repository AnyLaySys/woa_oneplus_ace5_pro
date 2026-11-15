// UFCS bypass charging. Native USB39 MOS; SC8517 and ADSP watchdogs remain active.
// Request selection follows authenticated PDOs and fresh native battery FV/FCC/FCC_MAX.
// Native limits apply; UFCS thermal derating bounds requested input power only.
static BOOLEAN QchLoadPair(QCH_LINK *C,ULONG Mv,ULONG Ma)
{
    if(!C->AuthVerified || !Mv || !Ma)return FALSE;
    for(ULONG I=0;I<C->Caps.Count;I++){
        ULONGLONG P=C->Caps.Pdo[I];
        ULONG VMin=(ULONG)((P>>24)&65535)*10,VMax=(ULONG)((P>>40)&65535)*10;
        ULONG IMin=(ULONG)(P&255)*10,IMax=(ULONG)((P>>8)&65535)*10;
        ULONG Vs=(ULONG)(((P>>56)&1)+1)*10,Is=(ULONG)(((P>>57)&7)+1)*10;
        if(Mv>=VMin && Mv<=VMax && Ma>=IMin && Ma<=IMax && !((Mv-VMin)%Vs) && !((Ma-IMin)%Is))return TRUE;
    }
    return FALSE;
}
static ULONG QchLoadCurrent(const QCH_LINK *C)
{
    if((LONG)C->LoadFccUa<=0 || (LONG)C->LoadFccMaxUa<=0)return 0;
    ULONG Ma=min(C->LoadFccUa,C->LoadFccMaxUa)/1000;
    if(C->GaugeCcMa && C->GaugeCcMa!=65535)Ma=min(Ma,C->GaugeCcMa);
    // SBS FullyDischarged: retain the established startup current until gauge recovery.
    return (C->BatteryStatus&0x10)?min(Ma,500u):Ma;
}
static BOOLEAN QchLoadSelect(QCH_LINK *C)
{
    if(C->Temperature>=ALS_CHARGE_UFCS_DERATE_DK)C->UfcsDerated=TRUE;
    else if(C->Temperature<=ALS_CHARGE_TEMP_RESUME_DK)C->UfcsDerated=FALSE;
    ULONG Delta=max(C->Cell1Mv,C->Cell2Mv)-min(C->Cell1Mv,C->Cell2Mv);
    ULONG Target=C->LoadCv?C->LoadFvLimitMv-Delta-20:C->LoadFvLimitMv;
    ULONG PowerMw=C->Soc<45?29000u:C->Soc<81?26000u:23000u;
    ULONG Allowed=QchLoadCurrent(C),BestMa=0,BestMv=0;
    if(!C->AuthVerified || !Allowed)return FALSE;
    for(ULONG I=0;I<C->Caps.Count;I++){
        ULONGLONG P=C->Caps.Pdo[I];
        ULONG VMin=(ULONG)((P>>24)&65535)*10,VMax=(ULONG)((P>>40)&65535)*10;
        ULONG IMin=(ULONG)(P&255)*10,IMax=(ULONG)((P>>8)&65535)*10;
        ULONG Vs=(ULONG)(((P>>56)&1)+1)*10,Is=(ULONG)(((P>>57)&7)+1)*10;
        if(Target<VMin || Target>VMax || Allowed<IMin)continue;
        ULONG Mv=VMin+(Target-VMin)/Vs*Vs;
        ULONG MaxMa=min(Allowed,IMax);
        if(C->UfcsDerated)MaxMa=min(MaxMa,PowerMw*1000u/Mv);
        if(MaxMa<IMin)continue;
        ULONG Ma=IMin+(MaxMa-IMin)/Is*Is;
        if(Ma>BestMa || (Ma==BestMa && Mv>BestMv)){BestMa=Ma;BestMv=Mv;}
    }
    if(!BestMa)return FALSE;
    C->LoadReqMv=BestMv;C->LoadReqMa=BestMa;return TRUE;
}
static ULONG QchLoadFvLimit(ULONG Uv)
{
    ULONG V=Uv/1000;if(V>=3500 && V<=4600)V*=2;
    return V>=7500 && V<=9200?V:0;
}
static ULONG QchLoadTarget(const QCH_LINK *C){return ((C->PackVoltage+500+19)/20)*20;}
static VOID QchLoadExit(PSIA_LINK L,ULONGLONG Now)
{
    QCH_LINK *C=&L->Charge;
    if(Now-C->ExitAt>6000){C->Fault=10;C->Phase=92;return;}
    if(C->Waiting)return;
    if(C->CpTouched && !C->CpOffVerified){QchScStop(L);return;}
    if(C->OffCheck && C->OffCheck<=4){
        ULONG Op=C->OffCheck==1?0x32:0x30,Prop=C->OffCheck==1?39:C->OffCheck==2?24:C->OffCheck==3?34:56;
        if(!C->ReplyReady || C->ReplyResult || C->ReplyOp!=Op || C->ReplyProp!=Prop){C->Fault=33;C->Phase=92;return;}
        if(C->ReplyValue){if(Now-C->Last>=50)QchRequest(L,Op,Prop,0);return;}
    }
    if(C->OffCheck<4){if(QchRequest(L,C->OffCheck?0x30:0x32,C->OffCheck==0?39:C->OffCheck==1?24:C->OffCheck==2?34:56,0))C->OffCheck++;return;}
    if((C->VoltageAt>C->ExitAt && C->UsbVoltage>=4500000 && C->UsbVoltage<=5500000) ||
       (C->OnlineAt>C->ExitAt && !C->UsbOnline && C->UsbVoltage<1000000)){
        C->UfcsStarted=FALSE;C->Phase=91;return;
    }
    if(!C->Waiting || Now-C->Last>500)QchRequest(L,0x32,C->Query++%2,0);
    // Source reads are not enable readbacks. Do not revalidate the previous enable GET.
    C->OffCheck=5;
}
static VOID QchLoadPoll(PSIA_LINK L,ULONGLONG Now)
{
    QCH_LINK *C=&L->Charge;
    if(Now-C->GuardAt>=500)QchBatteryGuard(C);
    if(C->LoadFvLimitMv){
        ULONG Fv=QchLoadFvLimit(C->LoadFvUv);
        if(!Fv){C->Fault=28;QchOff(L);return;}
        C->LoadFvLimitMv=(!C->GaugeCvMv || C->GaugeCvMv==65535)?Fv:min(Fv,C->GaugeCvMv);
    }
    ULONG Limit=C->LoadFvLimitMv?C->LoadFvLimitMv:9200;
    ULONG CellMax=max(C->Cell1Mv,C->Cell2Mv);
    // The gauge owns calibrated full-charge detection. Never force SOC/FC or rewrite FV.
    BOOLEAN Full=C->CellValid && C->Soc>=99 && (C->BatteryStatus&0x20);
    BOOLEAN Steady=C->LoadAt && Now-C->LoadAt>=3000;
    BOOLEAN CurrentValid=QchLoadCurrent(C)!=0;
    if(Now>=C->Deadline || !C->AuthVerified || C->LoadBuckOff!=3 || !QchBatteryFresh(C,Now) || C->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK ||
       C->PackVoltage>=Limit || CellMax*2>=Limit ||
       (C->BatteryStatus&0x900f) || (!Full && (C->BatteryStatus&0x4000)) ||
       (C->GaugeCvMv && C->GaugeCvMv!=65535 && (C->GaugeCvMv<7500 || C->GaugeCvMv>9200)) ||
       !C->UsbOnline || C->UsbVoltage>11200000 || C->SourceEnabled || C->SourceRole || C->Moisture ||
       (Steady && (Now-C->OnlineAt>2500 || Now-C->VoltageAt>2500 || Now-C->SourceAt>2500 || Now-C->RoleAt>2500 || Now-C->MoistureAt>2500 || !C->LoadFvAt || Now-C->LoadFvAt>2500 || !C->LoadFccAt || Now-C->LoadFccAt>2500 || !C->LoadFccMaxAt || Now-C->LoadFccMaxAt>2500)) ||
       (!C->LoadAt && Now-C->LoadStartAt>15000)){
        C->LoadStopReason=Now>=C->Deadline?3:C->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK?ALS_CHARGE_STOP_TEMPERATURE:2;C->Fault=Now>=C->Deadline?0:27;QchOff(L);return;
    }
    if(C->LoadAt && !CurrentValid){C->Fault=37;QchOff(L);return;}
    if(Full){C->LoadStopReason=1;C->Fault=0;QchOff(L);return;}
    if(C->NotifyFlags&16){C->Fault=18;QchOff(L);return;}
    if(C->Waiting){if(Now-C->Last>1500){C->Fault=3;QchOff(L);}return;}
    if(C->ReplyReady && C->ReplyResult){C->Fault=4;QchOff(L);return;}
    if(C->Phase==40){if(QchRequest(L,0x30,8,0))C->Phase=41;return;}
    if(C->Phase==41){
        C->LoadFvLimitMv=QchLoadFvLimit(C->LoadFvUv);
        if(C->GaugeCvMv && C->GaugeCvMv!=65535)C->LoadFvLimitMv=min(C->LoadFvLimitMv,C->GaugeCvMv);
        if(!C->LoadFvLimitMv){C->Fault=28;QchOff(L);return;}
        if(C->PackVoltage>=C->LoadFvLimitMv || !QchLoadPair(C,QchLoadTarget(C),500)){C->Fault=29;QchOff(L);return;}
        if(QchRequest(L,0x30,10,0))C->Phase=50;
        return;
    }
    if(C->Phase==50){if(QchRequest(L,0x30,11,0))C->Phase=51;return;}
    if(C->Phase==51){
        if(!CurrentValid){C->Fault=37;QchOff(L);return;}
        if(!QchScPrepare(L)){C->Fault=36;QchOff(L);return;}
        if(QchRequest(L,0x32,39,0))C->Phase=42;
        return;
    }
    if(C->Phase==42){
        if(C->LoadMos){C->Fault=30;QchOff(L);return;}
        C->LoadReqMv=5000;C->LoadReqMa=500;C->LoadReturn=0;C->Phase=44;return;
    }
    if(C->Phase==44){
        if(!QchLoadPair(C,C->LoadReqMv,C->LoadReqMa)){C->Fault=29;QchOff(L);return;}
        if(QchRequest(L,0x33,55,C->LoadReqMv))C->Phase=45;
        return;
    }
    if(C->Phase==45){
        C->NotifyFlags&=~2u;
        if(QchRequest(L,0x33,56,C->LoadReqMa))C->Phase=46;
        return;
    }
    if(C->Phase==46){
        if(!(C->NotifyFlags&2)){if(Now-C->Last>1000){C->Fault=31;QchOff(L);}return;}
        if(C->LoadReturn){C->Phase=60;C->LoadNextAt=Now+350;return;}
        if(QchRequest(L,0x32,1,0))C->Phase=47;
        return;
    }
    if(C->Phase==47){
        ULONG Mv=C->UsbVoltage/1000;
        // Firmware's implicit first request can consume the first PS_READY while
        // ignoring an overlapping target. Finish5V initialization before precharge.
        if(C->LoadReqMv==5000){
            if(Mv<4500 || Mv>5500){C->Fault=31;QchOff(L);return;}
            C->LoadReqMv=QchLoadTarget(C);C->Phase=44;return;
        }
        if(Mv>C->LoadReqMv+200 || Mv>C->PackVoltage+850){C->Fault=31;QchOff(L);return;}
        // PS_READY confirms the request, not a freshly settled PMIC ADC cache.
        // Keep both switches OFF while reading actual voltage; startup deadline still applies.
        if(Mv+200<C->LoadReqMv || Mv<C->PackVoltage+350){if(Now-C->Last>=50)QchRequest(L,0x32,1,0);return;}
        if(QchRequest(L,0x33,39,1))C->Phase=48;
        return;
    }
    if(C->Phase==48){if(QchRequest(L,0x32,39,0))C->Phase=49;return;}
    if(C->Phase==49){
        if(C->LoadMos!=1){C->Fault=30;QchOff(L);return;}
        if(!QchScEnable(L)){C->Fault=36;QchOff(L);return;}
        C->LoadAt=Now;C->LoadNextAt=Now+350;C->Phase=60;return;
    }
    if(C->Phase==62){if(QchRequest(L,0x32,39,0))C->Phase=63;return;}
    if(C->Phase==63){if(C->LoadMos!=1){C->Fault=30;QchOff(L);return;}C->Phase=44;return;}
    if(C->Phase==61){if(C->ReplyOp==0x32 && C->ReplyProp==39 && C->LoadMos!=1){C->Fault=30;QchOff(L);return;}C->Phase=60;}
    if(C->Phase==60){
        if(Now>=C->LoadNextAt){
            if(!QchScKick(L)){C->Fault=36;QchOff(L);return;}
            // Native battery FV bounds source CV; PDO steps and native FCC bound requests.
            if(CellMax*2+200>=Limit)C->LoadCv=1;
            if(!QchLoadSelect(C)){C->Fault=37;QchOff(L);return;}
            C->LoadReturn=1;
            if(QchRequest(L,0x33,39,1))C->Phase=62;
            return;
        }
        if(Now-C->Last>=50){static const ULONG P[][2]={{0x32,20},{0x32,0},{0x32,1},{0x32,39},{0x32,24},{0x32,22},{0x32,9},{0x32,62},{0x30,8},{0x30,10},{0x30,11}};ULONG Q=C->LoadQuery%ARRAYSIZE(P);if(QchRequest(L,P[Q][0],P[Q][1],0)){C->LoadQuery++;C->Phase=61;}}
    }
}
