/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Windows owns BSS selection/authentication. Forward verified associated-peer
 * action frames (RRM/WNM) and firmware roam triggers, never invent a PMK or
 * perform a firmware-authenticated handover behind the OS's controlled port. */
#include "stage8-internal.h"
extern "C" {
#include "stage7-util.h"
#include "network-policy.h"
}
static void Rfinish(S8 *S,NTSTATUS St)
{
    if(!S->ActionPending)return;S->ActionPending=FALSE;
    S8Indicate(S,&S->ActionHeader,S->ActionKind==WDI_TASK_SEND_REQUEST_ACTION_FRAME?WDI_INDICATION_SEND_REQUEST_ACTION_FRAME_COMPLETE:WDI_INDICATION_SEND_RESPONSE_ACTION_FRAME_COMPLETE,St);
    S->ActionSent=FALSE;S->ActionLength=0;RtlSecureZeroMemory(S->ActionBody,sizeof(S->ActionBody));
}
void S8ActionCancel(S8 *S,NTSTATUS St){Rfinish(S,St);}
bool S8ActionCommand(S8 *S,WIFIREQUEST R,const WDI_MESSAGE_HEADER &H,UINT16 Id,const UCHAR *P,ULONG N)
{
    if(Id==WDI_GET_NEXT_ACTION_FRAME_DIALOG_TOKEN){
        WDI_GET_NEXT_ACTION_FRAME_DIALOG_TOKEN_PARAMETERS K;UCHAR *B=nullptr;ULONG L=0;UINT In,Out;
        K.NextDialogToken=++S->ActionDialog;if(!K.NextDialogToken)K.NextDialogToken=++S->ActionDialog;
        auto Ns=GenerateWdiGetNextActionFrameDialogTokenFromIhv(&K,0,&S->Tlv,&L,&B);auto Buffer=(UCHAR*)WifiRequestGetInOutBuffer(R,&In,&Out);
        NTSTATUS St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INSUFFICIENT_RESOURCES;
        if(NT_SUCCESS(St)){if(!Buffer||Out<sizeof(H)+L)St=STATUS_BUFFER_TOO_SMALL;else RtlCopyMemory(Buffer+sizeof(H),B,L);}
        if(B)FreeGenerated(B);S8Complete(R,St,NT_SUCCESS(St)?(ULONG)sizeof(H)+L:(ULONG)sizeof(H));return true;
    }
    if(Id!=WDI_TASK_SEND_REQUEST_ACTION_FRAME&&Id!=WDI_TASK_SEND_RESPONSE_ACTION_FRAME)return false;
    WDI_TASK_SEND_REQUEST_ACTION_FRAME_PARAMETERS A;WDI_TASK_SEND_RESPONSE_ACTION_FRAME_PARAMETERS B;
    auto Ns=Id==WDI_TASK_SEND_REQUEST_ACTION_FRAME?ParseWdiTaskSendRequestActionFrameToIhv(N,P,&S->Tlv,&A):ParseWdiTaskSendResponseActionFrameToIhv(N,P,&S->Tlv,&B);
    auto &Q=Id==WDI_TASK_SEND_REQUEST_ACTION_FRAME?A.RequestParams:B.ResponseParams;auto &Body=Id==WDI_TASK_SEND_REQUEST_ACTION_FRAME?A.ActionFrameBody:B.ActionFrameBody;
    NTSTATUS St=STATUS_INVALID_PARAMETER;
    if(Ns==NDIS_STATUS_SUCCESS){
        if(!S->Adapter||H.PortId!=S->Port)St=STATUS_NOT_SUPPORTED;
        else if(S->ActionPending||!S->J->Authorized||S->Resetting||S->J->CsaState)St=STATUS_DEVICE_BUSY;
        else if(Body.ElementCount<2||Body.ElementCount>sizeof(S->ActionBody)||
            (Body.pElements[0]!=5&&Body.pElements[0]!=10)||Q.ChannelNumber!=WcnFrequencyChannel(S->J->Ap.Frequency)||
            Q.BandId!=(S->J->Ap.Frequency<2500?WDI_BAND_ID_2400:WDI_BAND_ID_5000)||!Jeq(Q.DestinationAddress.Address,S->J->Ap.Bssid,6)||Q.PostACKDwellTime)St=STATUS_NOT_SUPPORTED;
        else{S->ActionPending=TRUE;S->ActionSent=FALSE;S->ActionKind=Id;S->ActionHeader=H;S->ActionAt=WcnNowMs();
            S->ActionTimeout=Q.SendTimeout&&Q.SendTimeout<5000?Q.SendTimeout:5000;S->ActionLength=Body.ElementCount;RtlCopyMemory(S->ActionBody,Body.pElements,S->ActionLength);St=STATUS_SUCCESS;}
    }
    if(Id==WDI_TASK_SEND_REQUEST_ACTION_FRAME)CleanupParsedWdiTaskSendRequestActionFrame(&A);else CleanupParsedWdiTaskSendResponseActionFrame(&B);
    S8Complete(R,St);if(!NT_SUCCESS(St))S8Indicate(S,&H,Id==WDI_TASK_SEND_REQUEST_ACTION_FRAME?WDI_INDICATION_SEND_REQUEST_ACTION_FRAME_COMPLETE:WDI_INDICATION_SEND_RESPONSE_ACTION_FRAME_COMPLETE,St);return true;
}
void S8ActionService(S8 *S)
{
    if(!S->ActionPending)return;
    if(S->Stopped||S->Resetting||S->J->Failed||!S->J->Authorized){Rfinish(S,STATUS_CONNECTION_DISCONNECTED);return;}
    if(WcnNowMs()-S->ActionAt>S->ActionTimeout){Rfinish(S,STATUS_IO_TIMEOUT);return;}
    if(S->J->MgmtBusy)return;
    if(S->ActionSent){Rfinish(S,S->J->MgmtStatus?STATUS_UNSUCCESSFUL:STATUS_SUCCESS);return;}
    if(S->J->CsaState||S->D->Scan.RuntimeScan)return;
    NTSTATUS St=WcnTdlsPause(S->D,S->J);if(NT_SUCCESS(St))St=WcnTwtStop(S->D,S->J);if(NT_SUCCESS(St))St=WcnStage7ActionSend(S->D,S->J,S->ActionBody,S->ActionLength);
    if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St)){Rfinish(S,St);return;}S->ActionSent=TRUE;
}
extern "C" VOID WcnStage8ActionReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Body,ULONG N)
{
    auto S=S8Station(D,J);if(!S||!S->AssocIndicated||N<2||N>2048||(Body[0]!=5&&Body[0]!=10))return;
    WDI_INDICATION_ACTION_FRAME_RECEIVED_PARAMETERS K;RtlCopyMemory(K.SourceAddress.Address,J->Ap.Bssid,6);
    K.ChannelInfo.ChannelNumber=WcnFrequencyChannel(J->Ap.Frequency);K.ChannelInfo.BandId=J->Ap.Frequency<2500?WDI_BAND_ID_2400:WDI_BAND_ID_5000;K.ActionFrameBody.SimpleAssign(const_cast<UCHAR*>(Body),N);
    UCHAR *B=nullptr;ULONG L=0;if(GenerateWdiIndicationActionFrameReceivedFromIhv(&K,0,&S->Tlv,&L,&B)==NDIS_STATUS_SUCCESS){S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_ACTION_FRAME_RECEIVED,STATUS_SUCCESS,B,L,true);FreeGenerated(B);}
}
extern "C" VOID WcnStage8RoamEvent(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{
    if(Event!=0xb001||!Jtlvs(P,N))return;ULONG L;auto F=Jfind(P,N,48,&L);if(!F||L<8)return;
    auto J=WcnStation(D,Jle32(F));auto S=S8Station(D,J);ULONG Reason=Jle32(F+4);LONGLONG Now=WcnNowMs();
    if(!J||!S||!S->LinkUp||!J->Authorized||S->Resetting||S->Roaming||S->RoamPending||Reason<1||Reason>3||(S->RoamNotifyAt&&Now-S->RoamNotifyAt<10000))return;
    WDI_INDICATION_ROAMING_NEEDED_PARAMETERS K;K.RoamingReason=Reason==1?WDI_ASSOC_STATUS_ROAMING_ASSOCIATION_LOST:Reason==2?WDI_ASSOC_STATUS_ROAMING_LOW_LINK_QUALITY:WDI_ASSOC_STATUS_ROAMING_BETTER_AP_FOUND;
    UCHAR *B=nullptr;if(GenerateWdiIndicationRoamingNeededFromIhv(&K,0,&S->Tlv,&L,&B)==NDIS_STATUS_SUCCESS){if(NT_SUCCESS(S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_ROAMING_NEEDED,STATUS_SUCCESS,B,L,true)))S->RoamNotifyAt=Now;FreeGenerated(B);}
}
