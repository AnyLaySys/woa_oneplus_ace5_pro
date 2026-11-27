// Native buck bypass: disable battery charging before releasing USB input.
static BOOLEAN QchBypassFail(PSIA_LINK L,ULONGLONG Now,ULONG Error)
{
    QCH_LINK *C=&L->Charge;
    C->BypassError=(C->BypassState<<16)|Error;C->BypassResult=C->ReplyResult?C->ReplyResult:C->ReplyValue;
    C->Fault=39;C->Phase=92;C->BypassDeadline=Now+3000;C->Waiting=FALSE;
    QchRequest(L,0x33,5,0);return TRUE;
}
static BOOLEAN QchBypassPoll(PSIA_LINK L,ULONGLONG Now)
{
    // USB70 PLC status: 3=enabled, 2=disabled.
    static const ULONG Steps[][3]={{0x31,24,0},{0x30,24,0},{0x33,70,3},{0x33,5,MAXULONG},{0x32,5,0}};
    QCH_LINK *C=&L->Charge;
    BOOLEAN Start=C->Command==ChargeStart || C->Command==ChargeNormal || C->Command==ChargeBypass;
    BOOLEAN Detached=C->OnlineAt && Now-C->OnlineAt<=3000 && C->VoltageAt && Now-C->VoltageAt<=3000 &&
        !C->UsbOnline && C->UsbVoltage<1000000;
    BOOLEAN Safe=C->UsbVoltage>=4500000 && C->UsbVoltage<=5500000 &&
        C->OnlineAt && Now-C->OnlineAt<=3000 && C->VoltageAt && Now-C->VoltageAt<=3000 &&
        C->SourceAt && Now-C->SourceAt<=3000 && C->RoleAt && Now-C->RoleAt<=3000 &&
        C->MoistureAt && Now-C->MoistureAt<=3000 && !C->SourceEnabled && !C->SourceRole && !C->Moisture &&
        !C->UfcsStarted && !C->PeerRunning && !C->LoadMos && !C->CpActive;
    if(!C->BypassState && (Detached || C->Closing))return FALSE;
    if(((Start && C->BypassState==QchBypassReady) || Detached || C->Closing) && C->BypassState!=QchBypassExit){
        C->BypassState=QchBypassExit;C->BypassDeadline=Now+3000;C->ReplyReady=FALSE;
    }
    if(C->BypassState==QchBypassReady){
        if(!C->ReplyReady || C->ReplyOp!=0x30 || C->ReplyProp!=24 || !C->ReplyValue)return FALSE;
        C->BypassState=0;
    }
    if(C->BypassState!=QchBypassExit && C->Phase==92){if(Now<C->BypassDeadline)return FALSE;C->BypassState=0;C->Phase=91;}
    if(!C->BypassState){
        if(C->Waiting)return FALSE;
        if(!Safe)return FALSE;
        C->BypassState=1;C->BypassDeadline=Now+3000;C->ReplyReady=FALSE;
    }
    if(C->BypassState==QchBypassExit && !C->BypassDeadline)C->BypassDeadline=Now+3000;
    if(Now>=C->BypassDeadline)return QchBypassFail(L,Now,2);
    if(C->Waiting){if(Now-C->Last>900)return QchBypassFail(L,Now,3);return TRUE;}
    if(C->BypassState==QchBypassExit){
        if(C->ReplyReady && C->ReplyOp==0x33 && C->ReplyProp==70 && C->ExpectedValue==2){
            if(C->ReplyResult)return QchBypassFail(L,Now,4);
            C->BypassState=0;if(C->Fault==39){C->Phase=91;C->Fault=0;if(Start)return TRUE;}return FALSE;
        }
        QchRequest(L,0x33,70,2);return TRUE;
    }
    ULONG I=C->BypassState-1;
    if(C->ReplyReady){
        if(C->ReplyResult || C->ReplyOp!=Steps[I][0] || C->ReplyProp!=Steps[I][1] ||
           (I==1 && C->ReplyValue) || (I==4 && (LONG)C->UsbLimitUa<=0))return QchBypassFail(L,Now,4);
        C->ReplyReady=FALSE;
        if(++C->BypassState>ARRAYSIZE(Steps)){
            C->BypassState=QchBypassReady;C->BypassError=C->BypassResult=0;if(C->Fault==39)C->Fault=0;return FALSE;
        }
        I++;
    }
    if(I>=2 && !Safe)return QchBypassFail(L,Now,5);
    QchRequest(L,Steps[I][0],Steps[I][1],Steps[I][2]);return TRUE;
}
