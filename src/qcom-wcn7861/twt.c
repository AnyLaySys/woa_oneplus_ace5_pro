/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Individual announced STA TWT. This is D0 radio power saving, not WoWLAN.
 * Firmware owns the over-air negotiation; every dialog is ACK-gated. Busy
 * traffic, scans and shared-radio conflicts return to the awake path.
 * Wire layouts follow OnePlus SM8750 fw-api and wmi_unified_twt_tlv.c. */
#include "wcn7861.h"
#include "stage7-util.h"
#include "hotspot.h"
#include "twt-policy.h"
enum {Tidle,Tps,Tadd,Tadding,Tactive,Tdel,Tdeleting,Tawake,Tcooldown};

static NTSTATUS Tdialog(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Add)
{
    ULONG B[22]={0};B[0]=Jtlv(Add?727:728,Add?84:24);B[1]=J->Vdev;RtlCopyMemory(B+2,J->Ap.Bssid,6);
    if(Add){B[5]=65536;B[6]=256;B[7]=16384;} /* request, announced, unicast, dialog 0 */
    return WcnWmiSend(D,Add?0x3e003:0x3e004,B,Add?sizeof(B):28);
}
static VOID Tenable(PWCN_DEVICE D)
{
    ULONG I;NTSTATUS St;LONGLONG Now=WcnNowMs();
    if(!D->Ce.WmiReady||!D->Ce.HwModeValidated||!WcnService(D,152))return;
    for(I=0;I<2;I++){
        if(D->Ce.TwtEnable[I]==1){if(Now-D->Ce.TwtEnableAt[I]>3000)D->Ce.TwtEnable[I]=3;return;}
        if(D->Ce.TwtEnable[I])continue;
        {ULONG B[19]={0};B[0]=Jtlv(725,72);B[1]=I+1;B[18]=8; /* split config: individual requester, never AP responder */
        St=WcnWmiSend(D,0x3e001,B,sizeof(B));if(NT_SUCCESS(St)){D->Ce.TwtEnable[I]=1;D->Ce.TwtEnableAt[I]=Now;}
        else if(St!=STATUS_DEVICE_BUSY)D->Ce.TwtEnable[I]=3;return;}
    }
}
VOID WcnTwtService(PWCN_DEVICE D,PWCN_JOIN J)
{
    PWCN_JOIN Other=J->Vdev==0?&D->Secondary:&D->Join;const WCN_RADIO_CAP *C=WcnRadioCaps(D);
    ULONG Traffic=J->TxFrames+J->RxFrames;LONGLONG Now=WcnNowMs();NTSTATUS St=STATUS_SUCCESS;BOOLEAN Allow,Busy;
    if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed)return;
    if(J->Vdev==0)Tenable(D);
    if(!J->Started||J->Resetting||J->Failed||!J->Authorized||J->MacId>1)return;
    Busy=WcnTwtTrafficBusy(Traffic,&J->Traffic,Now,&J->TrafficAt,&J->TrafficBusyAt);
    Allow=D->Ce.TwtEnable[J->MacId]==2&&C&&(C->HeMac[J->Ap.Frequency<2500?0:1][0]&2)&&J->Ap.He&&(J->Ap.HeCap[0]&4)&&
        !D->Scan.RuntimeScan&&!J->CsaState&&!J->MgmtBusy&&!J->KeyHold&&!WcnHotspotActive(D)&&!WcnTdlsBusy(D,J)&&
        (!Other->Started||Other->Failed||Other->MacId!=J->MacId);
    switch(J->TwtState){
    case Tidle:
        if(!Allow||Busy)return;J->TwtState=Tps;J->TwtAt=Now;return;
    case Tps:
        if(!Allow||Busy){J->TwtState=Tidle;return;}
        St=WcnStationPower(D,J,TRUE);if(NT_SUCCESS(St))J->TwtState=Tadd;break;
    case Tadd:
        if(!Allow||Busy){J->TwtState=Tawake;break;}
        St=Tdialog(D,J,TRUE);if(NT_SUCCESS(St)){J->TwtState=Tadding;J->TwtAt=Now;}break;
    case Tadding:
        if(Now-J->TwtAt>5000){J->TwtState=Tdel;J->TwtAt=Now;}return;
    case Tactive:
        if(!Allow||Busy){J->TwtState=Tdel;J->TwtAt=Now;}return;
    case Tdel:
        St=Tdialog(D,J,FALSE);if(NT_SUCCESS(St)){J->TwtState=Tdeleting;}break;
    case Tdeleting:
        if(Now-J->TwtAt>3000)WcnStage7Fail(D,J,"TWT teardown completion timeout");return;
    case Tawake:
        St=WcnStationPower(D,J,FALSE);if(NT_SUCCESS(St)){J->TwtState=Tcooldown;J->TwtAt=Now;}break;
    case Tcooldown:
        if(Now-J->TwtAt>60000)J->TwtState=Tidle;return;
    default:WcnStage7Fail(D,J,"invalid TWT state");return;
    }
    if(St==STATUS_DEVICE_BUSY){if(Now-J->TwtAt>3000)WcnStage7Fail(D,J,"TWT command credit timeout");}
    else if(!NT_SUCCESS(St))WcnStage7Fail(D,J,"TWT command transport");
}
NTSTATUS WcnTwtStop(PWCN_DEVICE D,PWCN_JOIN J)
{
    NTSTATUS St;
    if(J->TwtState==Tidle||J->TwtState==Tcooldown)return STATUS_SUCCESS;
    if(J->TwtState==Tps||J->TwtState==Tadd)J->TwtState=Tawake;
    if(J->TwtState==Tadding||J->TwtState==Tactive){J->TwtState=Tdel;J->TwtAt=WcnNowMs();}
    if(J->TwtState==Tdel){St=Tdialog(D,J,FALSE);if(!NT_SUCCESS(St))return St;J->TwtState=Tdeleting;return STATUS_DEVICE_BUSY;}
    if(J->TwtState==Tdeleting)return WcnNowMs()-J->TwtAt>3000?STATUS_IO_TIMEOUT:STATUS_DEVICE_BUSY;
    St=WcnStationPower(D,J,FALSE);if(NT_SUCCESS(St)){J->TwtState=Tcooldown;J->TwtAt=WcnNowMs();}return St;
}
VOID WcnTwtEvent(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{
    ULONG L,Status,Vdev;const UCHAR *F;PWCN_JOIN J;
    if(Event<0x3e001||Event>0x3e004||!Jtlvs(P,N))return;
    F=Jfind(P,N,Event==0x3e001?731:Event==0x3e003?733:Event==0x3e004?734:732,&L);
    if(!F)return;
    if(Event==0x3e001){if(L<8)return;Vdev=Jle32(F);Status=Jle32(F+4);
        if(Vdev>=1&&Vdev<=2&&D->Ce.TwtEnable[Vdev-1]==1)D->Ce.TwtEnable[Vdev-1]=(UCHAR)(Status<=1?2:3);return;}
    if(L<20)return;J=WcnStation(D,Jle32(F));if(!J||!J->Started||!Jeq(F+4,J->Ap.Bssid,6)||Jle32(F+12)!=0)return;
    Status=Jle32(F+16);
    if(Event==0x3e003&&J->TwtState==Tadding){J->TwtState=(UCHAR)(Status==0?Tactive:Status==2?Tdel:Tawake);J->TwtAt=WcnNowMs();}
    if(Event==0x3e004&&(J->TwtState==Tactive||J->TwtState==Tadding||J->TwtState==Tdeleting)){
        if(Status==3){J->TwtState=Tdel;return;}
        if((Status>1&&Status<7)||Status>14){WcnStage7Fail(D,J,"TWT teardown rejected");return;}J->TwtState=Tawake;J->TwtAt=WcnNowMs();}
}
