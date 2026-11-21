#pragma once
/* Windows owns SAE. Encode only its public authentication parameters. */
static __inline ULONG S8SaeEncode(const WDI_SET_SAE_AUTH_PARAMS_COMMAND &P,UCHAR Out[488],ULONG Group,bool Ap)
{
    if(P.SAERequestType==WDI_SAE_REQUEST_TYPE_CONFIRM_PARAMS){
        ULONG Size=Group==19?32u:Group==20?48u:Group==21?64u:0u;
        if(!P.Optional.SAEConfirmParams_IsPresent||!Size||(Ap&&Group!=19)||P.SAEConfirmParams.Confirm.ElementCount!=Size)return 0;
        RtlZeroMemory(Out,488);Jputle16(Out,3);Jputle16(Out+2,2);Jputle16(Out+6,P.SAEConfirmParams.SendConfirm);
        RtlCopyMemory(Out+8,P.SAEConfirmParams.Confirm.pElements,Size);return Size+8;
    }
    if(P.SAERequestType!=WDI_SAE_REQUEST_TYPE_COMMIT_PARAMS&&P.SAERequestType!=WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS)return 0;
    if(!P.Optional.SAECommitParams_IsPresent)return 0;auto C=&P.SAECommitParams;
    ULONG Size=C->FiniteCyclicGroup==19?32u:C->FiniteCyclicGroup==20?48u:C->FiniteCyclicGroup==21?66u:0u;
    ULONG Token=C->Optional.AntiCloggingToken_IsPresent?C->AntiCloggingToken.ElementCount:0,Rejected=C->Optional.RejectedGroups_IsPresent?C->RejectedGroups.ElementCount:0;
    bool H2e=P.SAERequestType==WDI_SAE_REQUEST_TYPE_COMMIT_H2E_PARAMS||C->StatusCode==126;
    if(Token>200||Rejected>64||(Rejected&1)||(!H2e&&Rejected)||(C->Optional.CipherAlgorithm_IsPresent&&C->CipherAlgorithm!=WDI_CIPHER_ALGO_CCMP))return 0;
    if(C->Optional.RsnaAkmSuite_IsPresent&&(C->RsnaAkmSuite.ElementCount!=1||C->RsnaAkmSuite.pElements[0]!=rsna_akm_sae_pmk256))return 0;
    bool Challenge=Ap&&C->StatusCode==76,Reject=Ap&&C->StatusCode==77;
    if(!Challenge&&!Reject&&((C->StatusCode!=0&&C->StatusCode!=126)||!Size||(Ap&&C->FiniteCyclicGroup!=19)||!C->Optional.Scalar_IsPresent||!C->Optional.Element_IsPresent||C->Scalar.ElementCount!=Size||C->Element.ElementCount!=2*Size))return 0;
    if(Challenge&&(!Token||C->Optional.Scalar_IsPresent||C->Optional.Element_IsPresent))return 0;
    RtlZeroMemory(Out,488);Jputle16(Out,3);Jputle16(Out+2,1);Jputle16(Out+4,H2e&&C->StatusCode==0?126:C->StatusCode);Jputle16(Out+6,C->FiniteCyclicGroup);ULONG N=8;
    if(Reject)return N;
    if(Challenge){if(H2e){Out[N++]=255;Out[N++]=(UCHAR)(Token+1);Out[N++]=93;}RtlCopyMemory(Out+N,C->AntiCloggingToken.pElements,Token);return N+Token;}
    if(!H2e&&Token){RtlCopyMemory(Out+N,C->AntiCloggingToken.pElements,Token);N+=Token;}
    RtlCopyMemory(Out+N,C->Scalar.pElements,Size);N+=Size;RtlCopyMemory(Out+N,C->Element.pElements,2*Size);N+=2*Size;
    if(H2e&&Rejected){Out[N++]=255;Out[N++]=(UCHAR)(Rejected+1);Out[N++]=92;RtlCopyMemory(Out+N,C->RejectedGroups.pElements,Rejected);N+=Rejected;}
    if(H2e&&Token){Out[N++]=255;Out[N++]=(UCHAR)(Token+1);Out[N++]=93;RtlCopyMemory(Out+N,C->AntiCloggingToken.pElements,Token);N+=Token;}
    return N;
}
