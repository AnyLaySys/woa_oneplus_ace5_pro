/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * WiFiCx owns RSNA handshakes. Validate a complete property before touching
 * hardware, serialize key operations, and complete only after matching ACKs.
 */
#include "stage8-internal.h"
extern "C" {
#include "stage7-util.h"
#include "security-policy.h"
}
template<bool Delete,class T>static NTSTATUS Kparse(S8 *S,const T &List)
{
    if(!List.ElementCount||List.ElementCount>ARRAYSIZE(S->Keys)||(!Delete&&S->J->Ap.Auth==WCN_AUTH_OPEN))return STATUS_NOT_SUPPORTED;
    for(ULONG I=0;I<List.ElementCount;I++){
        auto K=&List.pElements[I];auto To=&S->Keys[I];auto Type=K->CipherKeyTypeInfo.KeyType;auto Dir=K->CipherKeyTypeInfo.Direction;
        bool Pair=Type==WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY,Mgmt=Type==WDI_CIPHER_KEY_TYPE_IGTK;
        ULONG Index=K->Optional.CipherKeyID_IsPresent?K->CipherKeyID.CipherKeyID:0;
        if((!Pair&&!Mgmt&&Type!=WDI_CIPHER_KEY_TYPE_GROUP_KEY)||(K->Optional.LinkId_IsPresent&&K->LinkId)||
           (!Delete&&Dir!=WDI_CIPHER_KEY_DIRECTION_BOTH&&Dir!=WDI_CIPHER_KEY_DIRECTION_INBOUND)||(Pair&&(Index||(!Delete&&Dir!=WDI_CIPHER_KEY_DIRECTION_BOTH)))||
           (Mgmt?(Index<4||Index>5||!S->J->Ap.Pmf):Index>3))return STATUS_NOT_SUPPORTED;
        if(Pair&&(!K->Optional.PeerMacAddress_IsPresent||!Jeq(K->PeerMacAddress.Address,S->J->Ap.Bssid,6)))return STATUS_INVALID_PARAMETER;
        if(K->CipherKeyTypeInfo.CipherAlgorithm!=(Mgmt?WDI_CIPHER_ALGO_BIP:WDI_CIPHER_ALGO_CCMP))return STATUS_NOT_SUPPORTED;
        for(ULONG P=0;P<I;P++)if(S->Keys[P].Management==(BOOLEAN)Mgmt&&S->Keys[P].Group==(BOOLEAN)!Pair&&S->Keys[P].Index==Index)return STATUS_INVALID_PARAMETER;
        To->Management=Mgmt;To->Group=!Pair;To->Index=(UCHAR)Index;
        if constexpr(!Delete){
            auto &Bytes=Mgmt?K->BIPKey:K->CCMPKey;
            if(!(Mgmt?K->Optional.BIPKey_IsPresent:K->Optional.CCMPKey_IsPresent)||Bytes.ElementCount!=16)return STATUS_NOT_SUPPORTED;
            RtlCopyMemory(To->Bytes,Bytes.pElements,16);
            if(K->Optional.ReceiveSequenceCount_IsPresent)RtlCopyMemory(To->Rsc,K->ReceiveSequenceCount.ReceiveSequenceCount,6);
        }
    }
    // Firmware requires PTK before GTK on initial association, regardless of TLV order.
    if constexpr(!Delete)for(ULONG I=1;I<List.ElementCount;I++)if(!S->Keys[I].Group){S8_KEY Tmp=S->Keys[0];S->Keys[0]=S->Keys[I];S->Keys[I]=Tmp;RtlSecureZeroMemory(&Tmp,sizeof(Tmp));break;}
    S->KeyCount=List.ElementCount;S->KeyNext=0;S->KeyDelete=Delete;return STATUS_SUCCESS;
}
NTSTATUS S8ParseKeys(S8 *S,WIFIREQUEST R,const UCHAR *P,ULONG N,bool Delete)
{
    if(S->KeyRequest||!S->AssocIndicated||S->Resetting||S->J->Failed)return STATUS_INVALID_DEVICE_STATE;
    RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));NTSTATUS St;
    if(Delete){WDI_SET_DELETE_CIPHER_KEYS_PARAMETERS K;auto Ns=ParseWdiSetDeleteCipherKeysToIhv(N,P,&S->Tlv,&K);
        St=Ns==NDIS_STATUS_SUCCESS?Kparse<true>(S,K.CipherKeyInfo):STATUS_INVALID_PARAMETER;CleanupParsedWdiSetDeleteCipherKeys(&K);}
    else{WDI_SET_ADD_CIPHER_KEYS_PARAMETERS K;auto Ns=ParseWdiSetAddCipherKeysToIhv(N,P,&S->Tlv,&K);
        St=Ns==NDIS_STATUS_SUCCESS?Kparse<false>(S,K.SetCipherKey):STATUS_INVALID_PARAMETER;CleanupParsedWdiSetAddCipherKeys(&K);}
    if(!NT_SUCCESS(St)){RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));return St;}
    S->KeyRequest=R;S->KeyAt=WcnNowMs();return STATUS_PENDING;
}
void S8KeysService(S8 *S)
{
    if(!S->KeyRequest)return;auto J=S->J;NTSTATUS St=STATUS_SUCCESS;
    for(;S->KeyNext<S->KeyCount;S->KeyNext++){
        auto K=&S->Keys[S->KeyNext];
        if(K->Sent){St=WcnStationKeysService(S->D,J);if(St==STATUS_PENDING||St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))break;}
        else{
            if(K->Management){
                if(S->KeyDelete){J->Igtk[K->Index-4].Valid=FALSE;if(J->Ap.Pmf&&!J->Igtk[0].Valid&&!J->Igtk[1].Valid)J->Authorized=FALSE;}
                else St=WcnPmfInstall(S->D,J,K->Index,K->Bytes,K->Rsc);
            }else St=S->KeyDelete?WcnStage8DeleteKey(S->D,J,K->Group,K->Index):WcnStage8Key(S->D,J,K->Group,K->Index,K->Bytes,K->Rsc);
            if(St==STATUS_DEVICE_BUSY)break;
            if(St==STATUS_PENDING){K->Sent=TRUE;RtlSecureZeroMemory(K->Bytes,16);return;}
            if(!NT_SUCCESS(St))break;
        }
        RtlSecureZeroMemory(K,sizeof(*K));S->KeyAt=WcnNowMs();
    }
    if(J->Failed)St=STATUS_DEVICE_HARDWARE_ERROR;
    if(St==STATUS_DEVICE_BUSY&&WcnNowMs()-S->KeyAt<=5000)return;
    if(St==STATUS_DEVICE_BUSY){St=STATUS_IO_TIMEOUT;WcnStage7Fail(S->D,J,"key property credit deadline");}
    auto R=S->KeyRequest;S->KeyRequest=nullptr;S->KeyCount=S->KeyNext=0;S->KeyDelete=FALSE;RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));S8Complete(R,St);
}
