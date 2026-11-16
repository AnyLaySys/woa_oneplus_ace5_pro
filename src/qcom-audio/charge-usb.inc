#include "..\qcom-battery\power.h"
#include "charge-policy.h"
static BOOLEAN QchSharedPoll(PSIA_LINK L,ULONGLONG Now)
{
    QCH_LINK *C=&L->Charge;SIA_USB *U=&L->Usb;
    C->PortOnline=U->Otg.Live[1];C->PortVoltage=U->Otg.Live[2];C->PortRole=U->Otg.Live[0];C->PortMoisture=U->Otg.Live[3];C->PortAt=U->OnlineAt;
    if(C->Bridge==0){C->UsbOnline=C->PortOnline;C->UsbVoltage=C->PortVoltage;C->OnlineAt=U->OnlineAt;}
    if(Now-C->GuardAt>=500)QchBatteryGuard(C);
    U->BatteryUntil=C->BatteryValid&&C->Temperature>=2832&&C->Temperature<=3182&&C->PackVoltage>=6500&&C->PackVoltage<=8600&&C->Soc>=5&&C->Soc<=100&&C->Remaining?C->SampleTimeMs+QCB_POWER_MAX_AGE_MS:0;
    C->OtgPhase=U->Otg.Phase;C->OtgFault=U->Otg.Fault;
    ULONG Action=QchPolicyNext(C,Now);
    if(Action==ChargeKey && !QchLoadAuth()){
        C->Policy.Normal=1;C->Policy.Attempts=0;C->Policy.RetryAt=Now+1000;
    }
    if(Action==ChargeStop || Action==ChargeStart || Action==ChargeNormal)C->Command=Action;
    if(Action==ChargeAcquire && C->Bridge==0){C->Bridge=1;C->Deadline=Now+6500;OtgShutdown(&U->Otg,Now);}
    if(Action==ChargeRelease && C->Bridge==2 && !C->Running && !C->OffMask && !C->UfcsStarted && C->Phase!=92 && !C->Waiting && !C->InputState){
        QchDiscardAuth();RtlZeroMemory(&U->Otg,sizeof(U->Otg));C->Bridge=0;
    }
    if(C->Bridge==1){
        if(!U->Otg.Touched&&!U->Otg.Pending&&U->Otg.Phase==OtgDone){C->Bridge=2;C->Waiting=FALSE;C->Last=0;C->Query=0;C->SourceAt=C->RoleAt=C->MoistureAt=C->OnlineAt=C->VoltageAt=0;C->AdapterType=0xffffffffu;C->CableOrientation=0xffffffffu;C->CableAt=0;}
        else if(Now>=C->Deadline){C->Bridge=3;}
    }
    C->OtgPhase=U->Otg.Phase;C->OtgFault=U->Otg.Fault;
    if(C->Bridge==2)QchPoll(L);
    QchPublish(C);
    return C->Bridge>=2;
}
