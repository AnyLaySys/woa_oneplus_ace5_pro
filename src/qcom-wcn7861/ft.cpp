/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * WiFiCx FT-over-802.1X: Windows owns PMK-R0/R1, PTK derivation and MIC checks.
 * The driver transports OTA FT authentication and only associates after the
 * OS returns a successful, nonce/MDID-bound SET command. No PSK/SAE FT claim. */
#include "stage8-internal.h"
extern "C" {
#include "stage7-util.h"
#include "security-policy.h"
#include "ft-wire.h"
}
static void Ffail(S8 *S,const char *Reason)
{S->Ft.AwaitOs=FALSE;S->Ft.Ready=FALSE;WcnStage7Fail(S->D,S->J,Reason);}
bool S8FtCandidate(S8 *S,const WDI_CONNECT_BSS_ENTRY_CONTAINER &B,const UCHAR *Frame,ULONG N,ULONG Auth,UCHAR *Akm,UCHAR *Mode)
{
    bool Init=B.Optional.FTInitialAssocParameters_IsPresent,Reassoc=B.Optional.FTReAssocParameters_IsPresent;ULONG L;const UCHAR *M;
    UNREFERENCED_PARAMETER(S);*Mode=0;*Akm=WcnAkm(Auth);if(!Init&&!Reassoc)return true;
    if((Init&&Reassoc)||(Auth!=WCN_AUTH_EAP&&Auth!=WCN_AUTH_EAP_SHA256)||N<36)return false;
    auto &Md=Reassoc?B.FTReAssocParameters.MDE:B.FTInitialAssocParameters.MDE;
    if(!WcnFtElement(Md.pElements,Md.ElementCount,54,5,5))return false;
    M=WcnFtIe(Frame+36,N-36,54,&L);if(!M||L!=5||!Jeq(M+2,Md.pElements+2,2))return false;
    if(Reassoc){auto &Ft=B.FTReAssocParameters.FTE;if(!WcnFtElement(Ft.pElements,Ft.ElementCount,55,84,257)||!WcnFtNonce(Ft.pElements))return false;}
    *Akm=3;*Mode=(UCHAR)(Reassoc?2:1);return true;
}
NTSTATUS S8FtPrepare(S8 *S,const WDI_CONNECT_BSS_ENTRY_CONTAINER &B,const WCN_JOIN_AP &A)
{
    auto F=&S->Ft;RtlSecureZeroMemory(F,sizeof(*F));if(!A.FtMode)return STATUS_SUCCESS;
    auto &Md=A.FtMode==2?B.FTReAssocParameters.MDE:B.FTInitialAssocParameters.MDE;
    if((A.FtMode!=1&&A.FtMode!=2)||(A.Auth!=WCN_AUTH_EAP&&A.Auth!=WCN_AUTH_EAP_SHA256)||A.Akm!=3||!WcnFtElement(Md.pElements,Md.ElementCount,54,5,5))return STATUS_INVALID_PARAMETER;
    if(A.FtMode==2&&(!WcnFtElement(B.FTReAssocParameters.FTE.pElements,B.FTReAssocParameters.FTE.ElementCount,55,84,257)||!WcnFtNonce(B.FTReAssocParameters.FTE.pElements)))return STATUS_INVALID_PARAMETER;
    F->Enabled=TRUE;F->Reassoc=A.FtMode==2;RtlCopyMemory(F->Mde,Md.pElements,5);
    if(!F->Reassoc){F->AssocLength=WcnRsnBuild(F->AssocIes,A.Auth,A.Pmf,FALSE);F->AssocIes[19]=A.Akm;RtlCopyMemory(F->AssocIes+F->AssocLength,F->Mde,5);F->AssocLength+=5;F->Ready=TRUE;return STATUS_SUCCESS;}
    auto &Ft=B.FTReAssocParameters.FTE;UCHAR *P=F->Request;Jputle16(P,2);Jputle16(P+2,1);Jputle16(P+4,0);
    F->RequestLength=6+WcnRsnBuildCached(P+6,A.Auth,A.Pmf,FALSE,B.FTReAssocParameters.PMKR0Name.pmkname.Name);P[6+19]=A.Akm;
    RtlCopyMemory(P+F->RequestLength,F->Mde,5);F->RequestLength+=5;RtlCopyMemory(P+F->RequestLength,Ft.pElements,Ft.ElementCount);F->RequestLength+=Ft.ElementCount;
    if(F->RequestLength>488){RtlSecureZeroMemory(F,sizeof(*F));return STATUS_INVALID_BUFFER_SIZE;}return STATUS_SUCCESS;
}
NTSTATUS S8FtCommand(S8 *S,const WDI_MESSAGE_HEADER &H,const UCHAR *P,ULONG N)
{
    if(!S->ConnectPending||S->Resetting||S->J->State!=5||S->J->Ap.FtMode!=2||!S->Ft.AwaitOs||H.PortId!=S->ConnectHeader.PortId)return STATUS_INVALID_DEVICE_STATE;
    WDI_SET_FAST_BSS_TRANSITION_PARAMETERS_COMMAND K;auto Ns=ParseWdiSetFastBssTransitionParametersToIhv(N,P,&S->Tlv,&K);NTSTATUS St=STATUS_INVALID_PARAMETER;
    if(Ns!=NDIS_STATUS_SUCCESS){CleanupParsedWdiSetFastBssTransitionParameters(&K);return St;}
    if(K.status!=NDIS_STATUS_SUCCESS){Ffail(S,"Windows rejected FT authentication");St=STATUS_SUCCESS;goto Done;}
    if(K.Optional.RSNIE_IsPresent&&K.Optional.MDE_IsPresent&&K.Optional.FTE_IsPresent&&
       WcnFtElement(K.RSNIE.pElements,K.RSNIE.ElementCount,48,22,257)&&WcnFtElement(K.MDE.pElements,K.MDE.ElementCount,54,5,5)&&
       WcnFtElement(K.FTE.pElements,K.FTE.ElementCount,55,84,257)&&Jeq(K.MDE.pElements,S->Ft.Mde,5)){
        BOOLEAN Pmf=S->J->Ap.Pmf;ULONG L1,L2;auto Request=WcnFtIe(S->Ft.Request+6,S->Ft.RequestLength-6,55,&L1);auto Response=WcnFtIe(S->AuthResponse+30,S->AuthResponseSize-30,55,&L2);
        if(Request&&Response&&WcnRsnAcceptAkm(K.RSNIE.pElements+2,K.RSNIE.ElementCount-2,S->J->Ap.Auth,3,&Pmf)&&Pmf==S->J->Ap.Pmf&&
           Jeq(K.FTE.pElements+52,Request+52,32)&&Jeq(K.FTE.pElements+20,Response+20,32)&&
           K.RSNIE.ElementCount+K.MDE.ElementCount+K.FTE.ElementCount<=sizeof(S->Ft.AssocIes)){
            UCHAR *To=S->Ft.AssocIes;S->Ft.AssocLength=K.RSNIE.ElementCount;RtlCopyMemory(To,K.RSNIE.pElements,S->Ft.AssocLength);
            RtlCopyMemory(To+S->Ft.AssocLength,K.MDE.pElements,5);S->Ft.AssocLength+=5;RtlCopyMemory(To+S->Ft.AssocLength,K.FTE.pElements,K.FTE.ElementCount);S->Ft.AssocLength+=K.FTE.ElementCount;
            S->Ft.AwaitOs=FALSE;S->Ft.Ready=TRUE;S->J->Authenticated=TRUE;St=STATUS_SUCCESS;
        }
    }
Done:CleanupParsedWdiSetFastBssTransitionParameters(&K);return St;
}
extern "C" ULONG WcnStage8FtIes(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *Out)
{auto S=S8Station(D,J);if(!S||!S->Ft.Enabled||!S->Ft.Ready)return 0;RtlCopyMemory(Out,S->Ft.AssocIes,S->Ft.AssocLength);return S->Ft.AssocLength;}
extern "C" VOID WcnStage8FtService(PWCN_DEVICE D,PWCN_JOIN J)
{
    auto S=S8Station(D,J);if(!S||!S->Ft.Enabled||!S->Ft.Reassoc||S->Resetting||S->Fatal)return;
    if(S->Ft.AwaitOs){if(WcnNowMs()-S->Ft.At>4000)Ffail(S,"Windows FT parameters timeout");return;}
    if(S->Ft.Ready)return;if(S->Ft.Attempts&&WcnNowMs()-S->Ft.At<700)return;
    if(S->Ft.Attempts>=3){Ffail(S,"FT authentication response timeout");return;}
    NTSTATUS St=WcnStage7AuthSend(D,J,S->Ft.Request,S->Ft.RequestLength);if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St)){Ffail(S,"FT authentication transmit");return;}S->Ft.At=WcnNowMs();S->Ft.Attempts++;
}
extern "C" VOID WcnStage8FtReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Frame,ULONG N)
{
    auto S=S8Station(D,J);if(!S||!S->ConnectPending||!S->Ft.Enabled||!S->Ft.Reassoc||!S->Ft.Attempts||S->Ft.AwaitOs||S->Ft.Ready||N<30||N>sizeof(S->AuthResponse)||Jle16(Frame+24)!=2||Jle16(Frame+26)!=2)return;
    if(Jle16(Frame+28)){Ffail(S,"AP rejected FT authentication");return;}
    ULONG L1,L2,L3;auto Md=WcnFtIe(Frame+30,N-30,54,&L1);auto Ft=WcnFtIe(Frame+30,N-30,55,&L2);auto Request=WcnFtIe(S->Ft.Request+6,S->Ft.RequestLength-6,55,&L3);
    if(!Md||L1!=5||!Ft||L2<84||!Request||!Jeq(Md+2,S->Ft.Mde+2,2)||!Jeq(Ft+52,Request+52,32))return;
    RtlCopyMemory(S->AuthResponse,Frame,N);S->AuthResponseSize=N;
    WDI_INDICATION_FT_ASSOC_PARAMS_NEEDED_PARAMETERS K;RtlCopyMemory(K.BssId.Address,J->Ap.Bssid,6);K.AuthRequest.SimpleAssign(S->Ft.Request,S->Ft.RequestLength);K.AuthResponse.SimpleAssign(S->AuthResponse+24,N-24);
    UCHAR *Out=nullptr;ULONG Length=0;auto Ns=GenerateWdiIndicationFtAssocParamsNeededFromIhv(&K,0,&S->Tlv,&Length,&Out);
    if(Ns!=NDIS_STATUS_SUCCESS){Ffail(S,"FT indication allocation");return;}
    S->Ft.AwaitOs=TRUE;S->Ft.At=WcnNowMs();NTSTATUS St=S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_FT_ASSOC_PARAMS_NEEDED,STATUS_SUCCESS,Out,Length,true);FreeGenerated(Out);if(!NT_SUCCESS(St))Ffail(S,"FT indication delivery");
}
