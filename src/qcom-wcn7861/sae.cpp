/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * SAE transport, not an SAE cryptographic implementation. Windows supplies
 * commit/confirm bytes, validates peer scalars/reflection and authorizes SAE
 * success. Only the active connection and selected BSSID/port can reach this state.
 * Each SAE SET command has its own transaction ID, separate from CONNECT. */
#include "stage8-internal.h"
extern "C" {
#include "security-policy.h"
}
static ULONG Le16(const UCHAR *P){return P[0]|((ULONG)P[1]<<8);}
static void Put16(UCHAR *P,ULONG V){P[0]=(UCHAR)V;P[1]=(UCHAR)(V>>8);}
static NTSTATUS SaeIndicate(S8 *S,WDI_SAE_INDICATION_TYPE Type,const UCHAR *Body=nullptr,ULONG Length=0)
{
    WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED_PARAMETERS P;RtlCopyMemory(P.BssId.Address,S->D->Join.Ap.Bssid,6);P.SAEIndicationType=Type;
    if(Type==WDI_SAE_INDICATION_TYPE_COMMIT_FRAME){P.Optional.SAECommitFrame_IsPresent=TRUE;P.SAECommitFrame.SimpleAssign((UCHAR*)Body,Length);}
    if(Type==WDI_SAE_INDICATION_TYPE_CONFIRM_FRAME){P.Optional.SAEConfirmFrame_IsPresent=TRUE;P.SAEConfirmFrame.SimpleAssign((UCHAR*)Body,Length);}
    if(Type==WDI_SAE_INDICATION_TYPE_ERROR){P.Optional.SAEStatus_IsPresent=TRUE;P.SAEStatus=WDI_SAE_STATUS_TIMEOUT_WAITING_FOR_STATUS;}
    UCHAR *Out=nullptr;ULONG N=0;if(GenerateWdiIndicationSaeAuthParamsNeeded(&P,0,&S->Tlv,&N,&Out)!=NDIS_STATUS_SUCCESS)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS St=S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_SAE_AUTH_PARAMS_NEEDED,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);return St;
}
static void SaeFail(S8 *S,const char *Reason,bool Notify=true){if(S->D->Join.Failed)return;if(Notify)SaeIndicate(S,WDI_SAE_INDICATION_TYPE_ERROR);RtlSecureZeroMemory(S->SaeTx,sizeof(S->SaeTx));S->SaeTxLength=0;WcnStage7Fail(S->D,Reason);}
NTSTATUS S8SaeCommand(S8 *S,const WDI_MESSAGE_HEADER *H,const UCHAR *Tlv,ULONG Length)
{
    if(!S->ConnectPending||S->D->Join.State!=5||S->D->Join.Ap.Auth!=WCN_AUTH_SAE||!S->SaeRequested||H->PortId!=S->ConnectHeader.PortId)return STATUS_INVALID_DEVICE_STATE;
    WDI_SET_SAE_AUTH_PARAMS_COMMAND P;
    if(ParseWdiSetSaeAuthParamsToIhv(Length,Tlv,&S->Tlv,&P)!=NDIS_STATUS_SUCCESS){CleanupParsedWdiSetSaeAuthParams(&P);return STATUS_INVALID_PARAMETER;}
    NTSTATUS St=STATUS_INVALID_PARAMETER;
    if(RtlCompareMemory(P.BssId.Address,S->D->Join.Ap.Bssid,6)!=6)goto Done;
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_FAILURE){SaeFail(S,"Windows rejected SAE peer authentication",false);St=STATUS_SUCCESS;goto Done;}
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_SUCCESS){
        if(!S->SaePeerConfirm||!S->SaeConfirmSent||(P.Optional.SAEStatus_IsPresent&&P.SAEStatus!=WDI_SAE_STATUS_SUCCESS))goto Done;
        S->D->Join.Authenticated=TRUE;S->SaeAwaitOs=FALSE;S->SaeTxLength=0;RtlSecureZeroMemory(S->SaeTx,sizeof(S->SaeTx));
        WcnLog(S->D,"stage8: Windows validated SAE commit/confirm; association may proceed, controlled port remains closed");St=STATUS_SUCCESS;goto Done;
    }
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_PARAMS||P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS){
        auto C=&P.SAECommitParams;ULONG Size=C->FiniteCyclicGroup==19?32:C->FiniteCyclicGroup==20?48:C->FiniteCyclicGroup==21?66:0;
        ULONG Token=C->Optional.AntiCloggingToken_IsPresent?C->AntiCloggingToken.ElementCount:0,Rejected=C->Optional.RejectedGroups_IsPresent?C->RejectedGroups.ElementCount:0;
        bool H2e=P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS||C->StatusCode==126;
        if((S->D->Join.Ap.H2eOnly&&!H2e)||!P.Optional.SAECommitParams_IsPresent||!Size||!C->Optional.Scalar_IsPresent||!C->Optional.Element_IsPresent||C->Scalar.ElementCount!=Size||C->Element.ElementCount!=2*Size||Token>200||Rejected>64||(Rejected&1)||(!H2e&&Rejected)||(C->StatusCode!=0&&C->StatusCode!=126))goto Done;
        if(C->Optional.CipherAlgorithm_IsPresent&&C->CipherAlgorithm!=WDI_CIPHER_ALGO_CCMP)goto Done;
        ULONG N=8;auto B=S->SaeTx;RtlSecureZeroMemory(B,sizeof(S->SaeTx));Put16(B,3);Put16(B+2,1);Put16(B+4,H2e?126:0);Put16(B+6,C->FiniteCyclicGroup);
        if(!H2e&&Token){RtlCopyMemory(B+N,C->AntiCloggingToken.pElements,Token);N+=Token;}
        RtlCopyMemory(B+N,C->Scalar.pElements,Size);N+=Size;RtlCopyMemory(B+N,C->Element.pElements,2*Size);N+=2*Size;
        if(H2e&&Rejected){B[N++]=255;B[N++]=(UCHAR)(Rejected+1);B[N++]=92;RtlCopyMemory(B+N,C->RejectedGroups.pElements,Rejected);N+=Rejected;}
        if(H2e&&Token){B[N++]=255;B[N++]=(UCHAR)(Token+1);B[N++]=93;RtlCopyMemory(B+N,C->AntiCloggingToken.pElements,Token);N+=Token;}
        S->SaeTxLength=N;S->SaeGroup=C->FiniteCyclicGroup;S->D->Join.H2e=H2e;S->SaePeerConfirm=S->SaeConfirmSent=FALSE;
    }else if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_CONFIRM_PARAMS){
        auto C=&P.SAEConfirmParams;ULONG N=C->Confirm.ElementCount;
        if(!P.Optional.SAEConfirmParams_IsPresent||N!=(S->SaeGroup==19?32u:S->SaeGroup==20?48u:S->SaeGroup==21?64u:0u))goto Done;
        auto B=S->SaeTx;RtlSecureZeroMemory(B,sizeof(S->SaeTx));Put16(B,3);Put16(B+2,2);Put16(B+6,C->SendConfirm);RtlCopyMemory(B+8,C->Confirm.pElements,N);S->SaeTxLength=N+8;
    }else goto Done;
    S->SaeAttempts=0;S->SaeAwaitOs=FALSE;S->SaeAt=0;St=STATUS_SUCCESS;
Done:
    CleanupParsedWdiSetSaeAuthParams(&P);return St;
}
extern "C" VOID WcnStage8SaeReceive(PWCN_DEVICE D,const UCHAR *Body,ULONG Length)
{
    auto S=(S8*)D->Stage8;
    if(!S||S->Resetting||S->Fatal||!S->ConnectPending||!S->SaeRequested||Length<6||Length>488||Le16(Body)!=3)return;
    ULONG Sequence=Le16(Body+2);if(Sequence!=1&&Sequence!=2)return;
    if(Sequence==2&&(!S->SaeConfirmSent||Le16(Body+4)!=0))return;
    if(Sequence==2)S->SaePeerConfirm=TRUE;
    S->SaeAwaitOs=TRUE;S->SaeAt=WcnNowMs();S->SaeTxLength=0;
    if(!NT_SUCCESS(SaeIndicate(S,Sequence==1?WDI_SAE_INDICATION_TYPE_COMMIT_FRAME:WDI_SAE_INDICATION_TYPE_CONFIRM_FRAME,Body,Length)))SaeFail(S,"SAE frame indication allocation failed");
}
extern "C" VOID WcnStage8SaeService(PWCN_DEVICE D)
{
    auto S=(S8*)D->Stage8;if(!S||S->Fatal||S->Resetting)return;
    if(!S->SaeRequested){S->SaeRequested=S->SaeAwaitOs=TRUE;S->SaeAt=WcnNowMs();if(!NT_SUCCESS(SaeIndicate(S,WDI_SAE_INDICATION_TYPE_COMMIT_REQUEST_PARAMS_NEEDED)))SaeFail(S,"SAE initial indication failed");return;}
    if(S->SaeAwaitOs){if(WcnNowMs()-S->SaeAt>4000)SaeFail(S,"Windows SAE response timeout");return;}
    if(!S->SaeTxLength||WcnNowMs()-S->SaeAt<700)return;
    if(S->SaeAttempts>=3){SaeFail(S,"SAE peer response timeout");return;}
    NTSTATUS St=WcnStage7AuthSend(D,S->SaeTx,S->SaeTxLength);if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St)){SaeFail(S,"SAE firmware transmit failed");return;}
    if(Le16(S->SaeTx+2)==2)S->SaeConfirmSent=TRUE;S->SaeAt=WcnNowMs();S->SaeAttempts++;
    WcnLog(D,"stage8: SAE auth frame submitted sequence=%u attempt=%u (public exchange and credentials not logged)",Le16(S->SaeTx+2),S->SaeAttempts);
}
