#include "ufcs-wire.h"
static VOID QchReceive(PSIA_LINK L,const UCHAR *Data,ULONG Length)
{
    QCH_LINK *C=&L->Charge;
    if(C->Bridge!=2)return;
            if(Length>=12 && QchLe32(Data)==32778) {
                ULONG Type=QchLe32(Data+4),Op=QchLe32(Data+8);
                if((Type==1 || Type==2) && Op==4 && C->Running && C->Phase==4) C->NotifyAcked=TRUE;
                if(Type==2 && Op==7 && Length>=16) {
                    ULONG N=QchLe32(Data+12);
                    if(N==0x71)C->NotifyFlags|=1;
                    if(N==0x70)C->NotifyFlags|=2;
                    if(N==0x74)C->NotifyFlags|=4;
                    if(N==0x7e)C->NotifyFlags|=8;
                    if(N==0x73)C->NotifyFlags|=16;
                    if(N==0x75)C->NotifyFlags|=32;
                }
                if(Type==1 && Op==0x10005) {
                    if(Length>=152 && QchLe32(Data+148)==3) {
                        ULONG Size=QchLe32(Data+144),Result=Data[16];
                        BOOLEAN Valid=Length==152 && Size>=1 && Size<=128 && Result<=1;
                        if(C->Waiting && C->ExpectedOp==0x10005 && C->ExpectedProp==3){
                            C->Waiting=FALSE;C->ReplyReady=TRUE;C->ReplyResult=Valid?0:1;
                            C->AuthReplyValid=Valid;C->AuthResult=Valid?Result:0;
                        }
                        return;
                    }
                    QCH_UFCS_CAPS Caps;
                    C->CapResponse=QchUfcsCaps(Data,Length,&Caps)?1:2;
                    if(C->CapResponse==1) {C->Caps=Caps;}
                    if(C->Waiting && C->ExpectedOp==0x10005 && C->ExpectedProp==0) {
                        C->Waiting=FALSE;C->ReplyReady=TRUE;C->ReplyResult=C->CapResponse==1?0:1;
                    }
                }
            }
            if(Length>=24 && *(ULONG UNALIGNED*)Data==32778 && *(ULONG UNALIGNED*)(Data+4)==1) {
                ULONG Op=*(ULONG UNALIGNED*)(Data+8),Prop=*(ULONG UNALIGNED*)(Data+12),Value=*(ULONG UNALIGNED*)(Data+16),Result=*(ULONG UNALIGNED*)(Data+20);
                if(Op==0x32 && Result==0) {
                    ULONGLONG Now=KeQueryInterruptTime()/10000;
                    if(Prop==0) {
                        C->UsbOnline=Value==1;C->OnlineAt=Value<=1?Now:0;
                        if(!C->UsbOnline){C->UfcsStarted=FALSE;C->NotifyFlags=0;C->CapResponse=0;RtlZeroMemory(&C->Caps,sizeof(C->Caps));}
                    }
                    if(Prop==1) {C->UsbVoltage=Value;C->VoltageAt=Now;}
                    if(Prop==5){C->UsbLimitUa=Value;C->InputAt=Now;}
                    if(Prop==20){C->CableOrientation=Value;C->CableAt=Value<=2?Now:0;}
                    if(Prop==16)C->VoocEnable=Value;
                    if(Prop==39)C->LoadMos=Value;
                    if(Prop==24){C->SourceEnabled=Value;C->SourceAt=Value<=1?Now:0;}
                    if(Prop==22){C->SourceRole=Value;C->RoleAt=Value<=1?Now:0;}
                    if(Prop==9){C->Moisture=Value;C->MoistureAt=Now;}
                    if(Prop==7) C->AdapterType=Value;
                    if(Prop==62){C->SourceLo=Value;C->InfoAt=Now;}
                }
                if(Op==0x30 && Result==0) {if(Prop==8){C->LoadFvUv=Value;C->LoadFvAt=KeQueryInterruptTime()/10000;}if(Prop==10){C->LoadFccUa=Value;C->LoadFccAt=KeQueryInterruptTime()/10000;}if(Prop==11){C->LoadFccMaxUa=Value;C->LoadFccMaxAt=KeQueryInterruptTime()/10000;}if(Prop==56)C->PeerRunning=Value;}
                if(C->Waiting && Op==C->ExpectedOp && Prop==C->ExpectedProp && Op!=0x10005) {
                    if(C->Running && C->UfcsStarted){
                        ULONG Bit=Op==0x32&&Prop==58?1u:Op==0x32&&Prop==59?2u:Op==0x30&&Prop==56?4u:Op==0x32&&Prop==57?8u:0u;
                        if(Bit){
                            if(!Result){
                                C->SessionValid|=Bit;
                                if(Bit==2)C->SessionDevHi=Value;
                                if(Bit==4)C->SessionRunning=Value;
                                if(Bit==8)C->SessionStatus=Value;
                            }
                        }
                    }
                    C->Waiting=FALSE;C->ReplyReady=TRUE;C->ReplyOp=Op;C->ReplyProp=Prop;C->ReplyValue=Value;C->ReplyResult=Result;
                    if(Op==0x31 && C->ExpectedValue==0 && Result==0) {
                        if(Prop==24) C->OffMask&=~1u;
                        if(Prop==34) C->OffMask&=~2u;
                    }
                    if(Op==0x33 && Result==0 && Prop==39 && C->ExpectedValue==0)C->OffMask&=~8u;
                    if(Op==0x33 && Result==0 && Prop==61 && C->ExpectedValue==1) C->OffMask&=~4u;
                }
            }
}
static BOOLEAN QchRequest(PSIA_LINK L,ULONG Op,ULONG Prop,ULONG Value)
{
    QCH_LINK *C=&L->Charge;SIA_USB *U=&L->Usb;ULONG Slot;
    for(Slot=0;Slot<U->Count && !(U->Free[Slot] && U->Size[Slot]>=24);Slot++);
    if(Slot==U->Count) return FALSE;
    ULONG Msg[]={GLINK_CMD_TX_DATA|(SIA_USB_LCID<<16),U->Iid[Slot],24,0,32778,1,Op,0,Prop,Value};
    if(!SiaGlinkSend(L,(UCHAR*)Msg,sizeof(Msg))) return FALSE;
    U->Free[Slot]=FALSE;C->Waiting=TRUE;C->ReplyReady=FALSE;C->ExpectedOp=Op;C->ExpectedProp=Prop;C->ExpectedValue=Value;
    C->Last=KeQueryInterruptTime()/10000;return TRUE;
}
static BOOLEAN QchCapsRequest(PSIA_LINK L)
{
    QCH_LINK *C=&L->Charge;SIA_USB *U=&L->Usb;ULONG Slot;
    for(Slot=0;Slot<U->Count && !(U->Free[Slot] && U->Size[Slot]>=20);Slot++);
    if(Slot==U->Count) return FALSE;
    // Standard output-mode list only; no private VDM power table.
    ULONG M[]={GLINK_CMD_TX_DATA|(SIA_USB_LCID<<16),U->Iid[Slot],20,0,32778,1,0x10005,128,0};
    if(!SiaGlinkSend(L,(UCHAR*)M,sizeof(M)))return FALSE;
    U->Free[Slot]=FALSE;C->Waiting=TRUE;C->ReplyReady=FALSE;C->ExpectedOp=0x10005;C->ExpectedProp=0;
    C->Last=KeQueryInterruptTime()/10000;C->CapResponse=0;C->PdoRequests++;RtlZeroMemory(&C->Caps,sizeof(C->Caps));return TRUE;
}
static BOOLEAN QchAuthRequest(PSIA_LINK L,BOOLEAN Provision)
{
    QCH_LINK *C=&L->Charge;SIA_USB *U=&L->Usb;ULONG Slot;
    for(Slot=0;Slot<U->Count && !(U->Free[Slot] && U->Size[Slot]>=44);Slot++);
    if(Slot==U->Count)return FALSE;
    // Native44-byte UFCS_ADAPTER_VERIFY. Key index1,16 bytes only for provisioning.
    // Once provisioned firmware ignores payload and returns the cached REAL result.
    ULONG M[]={GLINK_CMD_TX_DATA|(SIA_USB_LCID<<16),U->Iid[Slot],44,0,32778,1,0x10005,0,3,1,Provision?16u:0u,0,0,0,0};
    if(Provision && !QchTakeAuth((UCHAR*)(M+11)))return FALSE;
    BOOLEAN Sent=SiaGlinkSend(L,(UCHAR*)M,sizeof(M));RtlSecureZeroMemory(M,sizeof(M));
    if(!Sent)return FALSE;
    U->Free[Slot]=FALSE;C->Waiting=TRUE;C->ReplyReady=FALSE;C->ExpectedOp=0x10005;C->ExpectedProp=3;
    C->AuthReplyValid=0;C->AuthResult=0;C->Last=KeQueryInterruptTime()/10000;
    return TRUE;
}
#include "sc8517.inc"
static VOID QchOff(PSIA_LINK L)
{
    QCH_LINK *C=&L->Charge;
    if(C->CpTouched)QchScStop(L);
    QchDiscardAuth();
    C->Running=FALSE;C->Phase=90;C->OffMask=11|((C->UfcsStarted||C->PeerRunning)?4:0);
    C->Waiting=FALSE;C->ReplyReady=FALSE;C->OffRetries=C->OffCheck=0;C->ExitAt=KeQueryInterruptTime()/10000;
}
#include "ufcs-load.inc"
#include "normal-charge.inc"
#include "input-power.inc"
static VOID QchPoll(PSIA_LINK L)
{
    static const ULONG Queries[][2]={{0x32,20},{0x32,5},{0x32,0},{0x32,1},{0x32,24},{0x32,22},{0x32,9},{0x32,7},{0x32,39},{0x30,24},{0x30,34},{0x30,56}};
    QCH_LINK *C=&L->Charge;SIA_USB *U=&L->Usb;ULONGLONG Now=KeQueryInterruptTime()/10000;
    if(!U->Acked||!U->Rcid)return;
    if(!C->Running && !C->OffMask && C->Phase!=90 && (C->Phase!=92 || C->Fault==39) && QchInputPoll(L,Now))return;
    ULONG Action=QchTakeCommand();
    if((Action==ChargeStart||Action==ChargeNormal) && C->Waiting && !C->Running && !C->OffMask && !C->UfcsStarted && C->Phase!=92){QchDeferCommand(Action);Action=0xffffffffu;}
    if(Action==ChargeStop){QchOff(L);return;}
    if((Action==ChargeStart||Action==ChargeNormal) && C->InputState){QchDeferCommand(Action);Action=0xffffffffu;}
    if(Action==ChargeStart||Action==ChargeNormal){
        if(C->Running||C->OffMask){C->Fault=6;QchOff(L);return;}
        if(Action==ChargeStart && !QchAuthAvailable()){C->Fault=23;return;}
        if(C->UfcsStarted||C->PeerRunning||C->Phase==92){QchDiscardAuth();C->Fault=11;return;}
        if(C->Waiting||!C->SourceAt||Now-C->SourceAt>2500||C->SourceEnabled||
           !C->RoleAt||Now-C->RoleAt>2500||C->SourceRole||!C->MoistureAt||Now-C->MoistureAt>2500||C->Moisture||
           !C->UsbOnline||Now-C->OnlineAt>2500||C->UsbVoltage<4500000||C->UsbVoltage>5500000||Now-C->VoltageAt>2500||C->AdapterType!=2||!QchBatteryGuard(C)||!C->CellValid){
            QchDiscardAuth();C->Phase=99;C->Fault=1;return;
        }
        C->Normal=Action==ChargeNormal;C->UsbLimitUa=0;C->InputAt=0;
        if(C->Normal)QchDiscardAuth();
        C->Running=TRUE;C->Phase=C->Normal?100:10;C->Deadline=Now+15000;C->PdoRequests=0;
        C->SessionValid=C->SessionDevHi=C->SessionRunning=C->SessionStatus=0;
        C->AuthVerified=C->AuthResult=C->AuthReplyValid=0;
        C->LoadCv=C->LoadStopReason=C->LoadFvUv=C->LoadFvLimitMv=C->LoadReqMv=C->LoadReqMa=C->LoadMos=C->LoadReturn=C->LoadQuery=C->LoadBuckOff=0;
        C->LoadFccUa=C->LoadFccMaxUa=0;C->LoadStartAt=C->LoadAt=C->LoadNextAt=C->LoadFvAt=C->LoadFccAt=C->LoadFccMaxAt=0;
        C->CpError=C->CpPrepared=C->CpActive=C->CpTouched=C->CpOffVerified=0;
        C->NotifyFlags=0;C->NotifyAcked=FALSE;C->UfcsStarted=FALSE;C->ReplyReady=FALSE;C->Fault=0;C->CapResponse=0;RtlZeroMemory(&C->Caps,sizeof(C->Caps));
    }
    if(C->OffMask){
        if(C->Waiting && Now-C->Last<500)return;
        if(++C->OffRetries>30){C->Fault=9;C->Phase=92;C->OffMask=0;return;}
        if(C->OffMask&8)QchRequest(L,0x33,39,0);
        else if(C->OffMask&1)QchRequest(L,0x31,24,0);
        else if(C->OffMask&2)QchRequest(L,0x31,34,0);
        else QchRequest(L,0x33,61,1);
        return;
    }
    if(C->Phase==90){if(C->Normal)QchNormalExit(L,Now);else QchLoadExit(L,Now);return;}
    if(C->Running && C->Normal){QchNormalPoll(L,Now);return;}
    if(C->Running && C->Phase>=40){QchLoadPoll(L,Now);return;}
    if(C->Running) {
        if(C->UfcsStarted && (C->NotifyFlags&16)){C->Fault=18;QchOff(L);return;}
        if(Now>=C->Deadline || !C->UsbOnline ||
           ((C->Phase!=10 && C->Phase!=11) && Now-C->OnlineAt>5000) ||
           C->UsbVoltage>5800000 || (Now-C->GuardAt>=500 && !QchBatteryGuard(C))) {C->Fault=2;QchOff(L);return;}
        if(C->Waiting){if(Now-C->Last>1500){C->Fault=3;QchOff(L);}return;}
        if(C->ReplyReady && C->ReplyResult){C->Fault=4;QchOff(L);return;}
        if(C->Phase==10){if(QchRequest(L,0x32,0,0))C->Phase=11;return;}
        if(C->Phase==11){if(QchRequest(L,0x32,1,0))C->Phase=1;return;}
        if(C->Phase==1){if(QchRequest(L,0x31,24,0))C->Phase=12;return;}
        // Match native DPDM_SWITCH_TO_UFCS: explicitly disable the competing VOOC PHY.
        // This is not the SSDev setting performed internally by USB54.
        if(C->Phase==12){
            if(QchRequest(L,0x31,34,0))C->Phase=32;
            return;
        }
        if(C->Phase==32){if(QchRequest(L,0x30,24,0))C->Phase=33;return;}
        if(C->Phase==33){
            if(!C->ReplyReady || C->ReplyOp!=0x30 || C->ReplyProp!=24 || C->ReplyValue){C->Fault=35;QchOff(L);return;}
            C->LoadBuckOff|=1;if(QchRequest(L,0x30,34,0))C->Phase=34;return;
        }
        if(C->Phase==34){
            if(!C->ReplyReady || C->ReplyOp!=0x30 || C->ReplyProp!=34 || C->ReplyValue){C->Fault=35;QchOff(L);return;}
            C->LoadBuckOff|=2;if(QchRequest(L,0x33,16,0))C->Phase=13;return;
        }
        if(C->Phase==13){if(QchRequest(L,0x32,16,0))C->Phase=14;return;}
        if(C->Phase==14){
            if(C->VoocEnable){C->Fault=22;QchOff(L);return;}
            if(QchAuthRequest(L,TRUE))C->Phase=30;
            else {C->Fault=23;QchOff(L);}
            return;
        }
        if(C->Phase==30){
            if(!C->AuthReplyValid){C->Fault=24;QchOff(L);return;}
            // Before handshake cached result0 is normal, NOT authentication failure.
            C->Phase=3;
        }
        if(C->Phase==3){
            ULONG M[]={GLINK_CMD_TX_DATA|(SIA_USB_LCID<<16),0,28,0,32778,2,4,0,0,0,0};
            ULONG Slot;for(Slot=0;Slot<U->Count && !(U->Free[Slot]&&U->Size[Slot]>=28);Slot++);
            if(Slot==U->Count)return;
            M[1]=U->Iid[Slot];
            if(SiaGlinkSend(L,(UCHAR*)M,sizeof(M))){U->Free[Slot]=FALSE;C->Phase=4;C->Last=Now;C->ReplyReady=FALSE;}
            return;
        }
        if(C->Phase==4){
            if(!C->NotifyAcked){if(Now-C->Last>1000){C->Fault=5;QchOff(L);}return;}
            // Material is staged once per session by the protected broker.
            C->NotifyFlags&=~16u; // Only pre-session notices, not active-session failures.
            if(QchRequest(L,0x33,54,1)){C->UfcsStarted=TRUE;C->NotifyFlags=0;C->Phase=5;}
            return;
        }
        if(C->Phase==5){
            // Wait for fresh session handshake, then real identity/authentication/PDO.
            if(C->NotifyFlags&1){if(QchRequest(L,0x32,58,0))C->Phase=15;return;}
            if(Now-C->Last>=500)QchRequest(L,0x32,C->Query++%2,0);
            return;
        }
        // Native host reads device info immediately after handshake; snapshot survives peer drop/cache clearing.
        // These are fixed read-only fields, not VDM/authentication requests or protocol state overrides.
        if(C->Phase==15){if(QchRequest(L,0x32,59,0))C->Phase=16;return;}
        if(C->Phase==16){if(QchRequest(L,0x30,56,0))C->Phase=17;return;}
        if(C->Phase==17){if(QchRequest(L,0x32,57,0))C->Phase=18;return;}
        if(C->Phase==18){
            if(C->SessionValid!=15 || (C->SessionDevHi>>16)!=0x22d9 || !C->SessionRunning || !C->SessionStatus){C->Fault=25;QchOff(L);return;}
            if(!(C->NotifyFlags&32))return;
            if(QchAuthRequest(L,FALSE))C->Phase=31;
            return;
        }
        if(C->Phase==31){
            if(!C->AuthReplyValid || C->AuthResult!=1){C->Fault=26;QchOff(L);return;}
            C->AuthVerified=1;if(QchCapsRequest(L))C->Phase=6;return;
        }
        if(C->Phase==6){
            if(C->CapResponse==1 && C->Caps.Count){
                C->LoadStartAt=Now;C->Deadline=Now+14888000;C->Phase=40;
                return;
            }
            if(C->CapResponse==2){C->Fault=21;QchOff(L);return;}
            // A correctly framed empty cache is not evidence of absent adapter capability.
            if(C->PdoRequests<3 && Now-C->Last>=100)QchCapsRequest(L);
            else if(Now-C->Last>=500)QchRequest(L,0x32,C->Query++%2,0);
            return;
        }
    }

    if(C->Waiting){if(Now-C->Last>1500){C->Waiting=FALSE;C->Last=Now;}return;}
    if(Now-C->Last>=100){ULONG I=C->Query%ARRAYSIZE(Queries);if(QchRequest(L,Queries[I][0],Queries[I][1],0))C->Query++;}
}
