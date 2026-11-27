#pragma once
#include "usb.h"
#include "charge.h"

static ULONG QchPolicyNext(QCH_LINK *C,ULONGLONG Now)
{
    QCH_POLICY *P=&C->Policy;
    if(C->Closing || Now<P->PollAt)return ChargeNone;
    P->PollAt=Now+300;
    if(C->Bridge==3 || C->Phase==92)return ChargeNone;
    if(C->Running || C->Phase==90){P->Active=1;return ChargeNone;}
    if(C->Command || C->Waiting || (C->BypassState && C->BypassState!=QchBypassReady) || C->Bridge==1)return ChargeNone;
    if(C->Bridge==2 && QchThermalHeld())P->Hold=3;
    if(QchBatteryFresh(C,Now)){
        if((P->Hold==2 && C->Soc<=96 && !(C->BatteryStatus&0x20) && C->Cell1Mv<4350 && C->Cell2Mv<4350) ||
           (P->Hold==3 && C->Temperature<=ALS_CHARGE_TEMP_RESUME_DK)){P->Hold=0;P->AcquireAt=Now;}
    }
    if(C->Bridge==2){
        if(C->OnlineAt && Now-C->OnlineAt<=3000){
            if(!C->UsbOnline && C->VoltageAt && Now-C->VoltageAt<=3000 && C->UsbVoltage<1000000){
                if(!P->OfflineAt)P->OfflineAt=Now;
                return Now-P->OfflineAt>=900?ChargeRelease:ChargeNone;
            }
            P->OfflineAt=0;
        }
        if(P->Active && (C->Phase==91 || C->Phase==99)){
            P->Active=0;
            if(C->LoadStopReason==1){P->Hold=2;P->Attempts=0;}
            else if(C->LoadStopReason==ALS_CHARGE_STOP_TEMPERATURE){P->Hold=3;P->Attempts=0;P->RetryAt=0;}
            else if(!C->BypassMa)P->RetryAt=Now+(P->Attempts>=2?9000:3000);
            if(!P->Normal && !C->AuthVerified && !C->LoadReqMv &&
               ((C->Fault>=2 && C->Fault<=5) || C->Fault==18 || C->Fault==21 || (C->Fault>=23 && C->Fault<=26))){
                P->Normal=1;P->Attempts=0;P->RetryAt=0;
            }
            P->AcquireAt=Now;return ChargeNone;
        }
        if(P->Hold || Now<P->RetryAt){
            if(C->BypassState!=QchBypassReady || C->AdapterType!=2 || !QchSupplySafe(C,Now) || C->Moisture || C->SourceRole || C->SourceEnabled ||
               !C->InputAt || Now-C->InputAt>3000 || (LONG)C->UsbLimitUa<500000)return ChargeNone;
            if(!QchAuthAvailable())return ChargeKey;
            P->Active=1;return ChargeBypass;
        }
        if(P->Attempts>=3){if(!P->RetryAt){P->RetryAt=Now+9000;return ChargeNone;}P->Attempts=0;}
        if(P->RetryAt){P->RetryAt=0;P->AcquireAt=Now;}
        if(Now-P->AcquireAt>9000){P->Attempts++;P->RetryAt=Now+9000;return ChargeNone;}
        if(!C->GuardGood || !QchBatteryFresh(C,Now)){
            if(C->Temperature>=ALS_CHARGE_TEMP_LIMIT_DK){P->Hold=3;P->Attempts=0;P->RetryAt=0;}
            return ChargeNone;
        }
        if(C->Soc>=99 && (C->BatteryStatus&0x20)){P->Hold=2;return ChargeNone;}
        if(!C->OnlineAt || Now-C->OnlineAt>3000 || !C->VoltageAt || Now-C->VoltageAt>3000 ||
           !C->SourceAt || Now-C->SourceAt>3000 || !C->RoleAt || Now-C->RoleAt>3000 ||
           !C->MoistureAt || Now-C->MoistureAt>3000)return ChargeNone;
        if(!C->UsbOnline || C->UsbVoltage<4500000 || C->UsbVoltage>5500000 || C->SourceEnabled || C->SourceRole || C->Moisture)return ChargeNone;
        if(C->AdapterType<1 || C->AdapterType>3)return ChargeNone;
        if(C->LoadMos || C->PeerRunning)return ChargeStop;
        if(P->Normal || C->AdapterType!=2){P->Attempts++;P->Active=1;return ChargeNormal;}
        if(!QchAuthAvailable())return ChargeKey;
        P->Attempts++;P->Active=1;return ChargeStart;
    }
    if(!C->PortAt || Now-C->PortAt>3000)return ChargeNone;
    if(!C->PortOnline){
        P->StableAt=0;
        if(!P->OfflineAt)P->OfflineAt=Now;
        if(Now-P->OfflineAt>=900){P->Hold=P->Attempts=P->Active=P->Normal=0;P->RetryAt=0;}
        return ChargeNone;
    }
    P->OfflineAt=0;
    if(P->Hold || P->Attempts>=3 || Now<P->RetryAt || !QchBatteryFresh(C,Now) ||
       (!C->GuardGood && C->Temperature<ALS_CHARGE_TEMP_LIMIT_DK) || C->PortRole || C->PortMoisture ||
       C->PortVoltage<4500000 || C->PortVoltage>5500000 || C->OtgPhase!=OtgWatch){P->StableAt=0;return ChargeNone;}
    if(!P->StableAt)P->StableAt=Now;
    if(Now-P->StableAt<900)return ChargeNone;
    P->AcquireAt=Now;P->StableAt=0;return ChargeAcquire;
}
