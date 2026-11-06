/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * SAE transport, not an SAE cryptographic implementation. Windows supplies
 * commit/confirm bytes, validates peer scalars/reflection and authorizes SAE
 * success. Only the active connection and selected BSSID/port can reach this state.
 * Each SAE SET command has its own transaction ID, separate from CONNECT. */
#include "stage8-internal.h"
extern "C" {
#include "security-policy.h"
#include "stage7-util.h"
}
#include "sae-wire.h"
NTSTATUS S8SaeIndicate(S8 *S,const WDI_MESSAGE_HEADER &Header,const UCHAR Peer[6],WDI_SAE_INDICATION_TYPE Type,const UCHAR *Body,ULONG Length)
{
    WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED_PARAMETERS P;RtlCopyMemory(P.BssId.Address,Peer,6);P.SAEIndicationType=Type;
    if(Type==WDI_SAE_INDICATION_TYPE_COMMIT_FRAME){P.Optional.SAECommitFrame_IsPresent=TRUE;P.SAECommitFrame.SimpleAssign((UCHAR*)Body,Length);}
    if(Type==WDI_SAE_INDICATION_TYPE_CONFIRM_FRAME){P.Optional.SAEConfirmFrame_IsPresent=TRUE;P.SAEConfirmFrame.SimpleAssign((UCHAR*)Body,Length);}
    if(Type==WDI_SAE_INDICATION_TYPE_ERROR){P.Optional.SAEStatus_IsPresent=TRUE;P.SAEStatus=WDI_SAE_STATUS_TIMEOUT_WAITING_FOR_STATUS;}
    UCHAR *Out=nullptr;ULONG N=0;if(GenerateWdiIndicationSaeAuthParamsNeeded(&P,0,&S->Tlv,&N,&Out)!=NDIS_STATUS_SUCCESS)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS St=S8Indicate(S,&Header,WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);return St;
}
static void SaeFail(S8 *S,const char *Reason,bool Notify=true){if(S->J->Failed)return;if(Notify)S8SaeIndicate(S,S->ConnectHeader,S->J->Ap.Bssid,WDI_SAE_INDICATION_TYPE_ERROR);RtlSecureZeroMemory(S->SaeTx,sizeof(S->SaeTx));S->SaeTxLength=0;WcnStage7Fail(S->D,S->J,Reason);}
NTSTATUS S8SaeCommand(S8 *S,const WDI_MESSAGE_HEADER *H,const UCHAR *Tlv,ULONG Length)
{
    if(!S->ConnectPending||S->J->State!=5||S->J->Ap.Auth!=WCN_AUTH_SAE||!S->SaeRequested||H->PortId!=S->ConnectHeader.PortId)return STATUS_INVALID_DEVICE_STATE;
    WDI_SET_SAE_AUTH_PARAMS_COMMAND P;
    if(ParseWdiSetSaeAuthParamsToIhv(Length,Tlv,&S->Tlv,&P)!=NDIS_STATUS_SUCCESS){CleanupParsedWdiSetSaeAuthParams(&P);return STATUS_INVALID_PARAMETER;}
    NTSTATUS St=STATUS_INVALID_PARAMETER;
    if(RtlCompareMemory(P.BssId.Address,S->J->Ap.Bssid,6)!=6)goto Done;
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_FAILURE){SaeFail(S,"Windows rejected SAE peer authentication",false);St=STATUS_SUCCESS;goto Done;}
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_SUCCESS){
        if(!S->SaePeerConfirm||!S->SaeConfirmSent||(P.Optional.SAEStatus_IsPresent&&P.SAEStatus!=WDI_SAE_STATUS_SUCCESS))goto Done;
        S->J->Authenticated=TRUE;S->SaeAwaitOs=FALSE;S->SaeTxLength=0;RtlSecureZeroMemory(S->SaeTx,sizeof(S->SaeTx));
        St=STATUS_SUCCESS;goto Done;
    }
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_PARAMS||P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS){
        bool H2e=P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS||P.SAECommitParams.StatusCode==126;
        if(S->J->Ap.H2eOnly&&!H2e)goto Done;
        S->SaeTxLength=S8SaeEncode(P,S->SaeTx,S->SaeGroup,false);if(!S->SaeTxLength)goto Done;
        S->SaeGroup=P.SAECommitParams.FiniteCyclicGroup;S->J->H2e=H2e;S->SaePeerConfirm=S->SaeConfirmSent=FALSE;
    }else{S->SaeTxLength=S8SaeEncode(P,S->SaeTx,S->SaeGroup,false);if(!S->SaeTxLength)goto Done;}
    S->SaeAttempts=0;S->SaeAwaitOs=FALSE;S->SaeAt=0;St=STATUS_SUCCESS;
Done:
    CleanupParsedWdiSetSaeAuthParams(&P);return St;
}
extern "C" VOID WcnStage8SaeReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Body,ULONG Length)
{
    auto S=S8Station(D,J);
    if(!S||S->Resetting||S->Fatal||!S->ConnectPending||!S->SaeRequested||Length<6||Length>488||Jle16(Body)!=3)return;
    ULONG Sequence=Jle16(Body+2);if(Sequence!=1&&Sequence!=2)return;
    if(Sequence==2&&(!S->SaeConfirmSent||Jle16(Body+4)!=0))return;
    if(Sequence==2)S->SaePeerConfirm=TRUE;
    S->SaeAwaitOs=TRUE;S->SaeAt=WcnNowMs();S->SaeTxLength=0;
    if(!NT_SUCCESS(S8SaeIndicate(S,S->ConnectHeader,S->J->Ap.Bssid,Sequence==1?WDI_SAE_INDICATION_TYPE_COMMIT_FRAME:WDI_SAE_INDICATION_TYPE_CONFIRM_FRAME,Body,Length)))SaeFail(S,"SAE frame indication allocation failed");
}
extern "C" VOID WcnStage8SaeService(PWCN_DEVICE D,PWCN_JOIN J)
{
    auto S=S8Station(D,J);if(!S||S->Fatal||S->Resetting)return;
    if(!S->SaeRequested){S->SaeRequested=S->SaeAwaitOs=TRUE;S->SaeAt=WcnNowMs();if(!NT_SUCCESS(S8SaeIndicate(S,S->ConnectHeader,S->J->Ap.Bssid,WDI_SAE_INDICATION_TYPE_COMMIT_REQUEST_PARAMS_NEEDED)))SaeFail(S,"SAE initial indication failed");return;}
    if(S->SaeAwaitOs){if(WcnNowMs()-S->SaeAt>4000)SaeFail(S,"Windows SAE response timeout");return;}
    if(!S->SaeTxLength||WcnNowMs()-S->SaeAt<700)return;
    if(S->SaeAttempts>=3){SaeFail(S,"SAE peer response timeout");return;}
    NTSTATUS St=WcnStage7AuthSend(D,S->J,S->SaeTx,S->SaeTxLength);if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St)){SaeFail(S,"SAE firmware transmit failed");return;}
    if(Jle16(S->SaeTx+2)==2)S->SaeConfirmSent=TRUE;S->SaeAt=WcnNowMs();S->SaeAttempts++;

}
