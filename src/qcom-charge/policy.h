#pragma once
// Pure broker policy. The kernel remains the only electrical actuator/safety owner.
enum {ChargeNone=0,ChargeAbort=5,ChargeKey=6};
typedef struct {ULONG Attempts,Hold,Active,Normal;unsigned long long StableAt,OfflineAt,RetryAt,AcquireAt;} CHARGE_POLICY;
static int ChargeNext(CHARGE_POLICY *P,const ALS_CHARGE_STATUS_DATA *S,unsigned long long Now)
{
    if(S->Abi!=ALS_CHARGE_ABI||S->Bridge==3||S->Phase==92)return ChargeNone;
    if(S->Running||S->Phase==90){P->Active=1;return ChargeNone;}
    if(S->Pending||S->Bridge==1)return ChargeNone;
    if(S->BatteryAge<=6000 && S->CellValid){
        if((P->Hold==2 && S->Soc<=95 && !(S->BatteryStatus&0x20) && S->Cell1<4350 && S->Cell2<4350) ||
           (P->Hold==3 && S->Temperature<=ALS_CHARGE_TEMP_RESUME_DK)){P->Hold=0;P->AcquireAt=Now;}
    }
    if(S->Bridge==2){
        // Retain sink ownership while plugged in; only real detach returns the port to OTG.
        if(S->OnlineAge<=2500){
            if(!S->Online){
                if(!P->OfflineAt)P->OfflineAt=Now;
                return Now-P->OfflineAt>=500?ChargeRelease:ChargeNone;
            }
            P->OfflineAt=0;
        }
        if(P->Active&&(S->Phase==91||S->Phase==99)){
            P->Active=0;
            if(S->StopReason==1){P->Hold=2;P->Attempts=0;}
            else if(S->StopReason==ALS_CHARGE_STOP_TEMPERATURE){P->Hold=3;P->Attempts=0;P->RetryAt=0;}
            else P->RetryAt=Now+(P->Attempts>=2?30000:5000);
            if(!P->Normal && !S->AuthVerified && !S->RequestedMv &&
               ((S->Fault>=2 && S->Fault<=5) || S->Fault==18 || S->Fault==21 || (S->Fault>=23 && S->Fault<=26))){
                P->Normal=1;P->Attempts=0;P->RetryAt=0;
            }
            P->AcquireAt=Now;return ChargeNone;
        }
        if(P->Hold||Now<P->RetryAt||P->Attempts>=3)return ChargeNone;
        if(P->RetryAt){P->RetryAt=0;P->AcquireAt=Now;}
        if(Now-P->AcquireAt>15000){P->Attempts++;P->RetryAt=Now+30000;return ChargeNone;}
        if(!S->Guard||!S->CellValid||S->BatteryAge>6000){
            if(S->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK){P->Hold=3;P->Attempts=0;P->RetryAt=0;return ChargeNone;}
            return ChargeNone;
        }
        if(S->Soc>=99&&(S->BatteryStatus&0x20)){P->Hold=2;return ChargeNone;}
        if(S->OnlineAge>2500||S->VoltageAge>2500||S->SourceAge>2500||S->RoleAge>2500||S->MoistureAge>2500)return ChargeNone;
        if(!S->Online||S->UsbMv<4500||S->UsbMv>5500||S->SourceEnabled||S->SourceRole||S->Moisture)return ChargeNone;
        if(S->Adapter==0xffffffffu)return ChargeNone;
        if(S->Adapter!=2){P->Hold=1;return ChargeNone;}
        if(S->Mos||S->PeerRunning)return ChargeAbort;
        if(P->Normal){P->Attempts++;P->Active=1;return ChargeNormal;}
        if(!S->AuthReady)return ChargeKey;
        P->Attempts++;P->Active=1;return ChargeStart;
    }
    if(S->PortAge>2500)return ChargeNone;
    if(!S->PortOnline){
        P->StableAt=0;
        if(!P->OfflineAt)P->OfflineAt=Now;
        if(Now-P->OfflineAt>=500){P->Hold=P->Attempts=P->Active=P->Normal=0;P->RetryAt=0;}
        return ChargeNone;
    }
    P->OfflineAt=0;
    if(P->Hold||P->Attempts>=3||Now<P->RetryAt||(!S->Guard && !(S->CellValid && S->BatteryAge<=6000 && S->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK))||!S->CellValid||S->PortRole||S->PortMoisture||S->PortMv<4500||S->PortMv>5500||S->OtgPhase!=3){P->StableAt=0;return ChargeNone;}
    if(!P->StableAt)P->StableAt=Now;
    if(Now-P->StableAt<500)return ChargeNone;
    P->AcquireAt=Now;P->StableAt=0;return ChargeAcquire;
}
