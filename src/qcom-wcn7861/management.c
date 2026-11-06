/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * PMF: firmware verifies/decrypts unicast CCMP; group BIP-CMAC128 is checked
 * in software because ath12k explicitly leaves IGTK indices4/5 to the host.
 * BCrypt provides CMAC; this driver never derives passwords or SAE secrets. */
#include "wcn7861.h"
#include <bcrypt.h>
NTSTATUS WcnCmac(PVOID Algorithm,const UCHAR Key[16],const UCHAR *Data,ULONG Length,UCHAR Mac[16])
{
    BCRYPT_HASH_HANDLE H=NULL;NTSTATUS St;
    St=BCryptCreateHash((BCRYPT_ALG_HANDLE)Algorithm,&H,NULL,0,(PUCHAR)Key,16,0);
    if(NT_SUCCESS(St))St=BCryptHashData(H,(PUCHAR)Data,Length,0);
    if(NT_SUCCESS(St))St=BCryptFinishHash(H,Mac,16,0);
    if(H)BCryptDestroyHash(H);if(!NT_SUCCESS(St))RtlSecureZeroMemory(Mac,16);return St;
}
NTSTATUS WcnPmfInit(PWCN_DEVICE D,PWCN_JOIN J)
{
    UNREFERENCED_PARAMETER(D);
    if(J->PmfAlgorithm)return STATUS_SUCCESS;
    return BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&J->PmfAlgorithm,BCRYPT_AES_CMAC_ALGORITHM,NULL,0);
}
VOID WcnPmfClear(PWCN_DEVICE D,PWCN_JOIN J)
{
    UNREFERENCED_PARAMETER(D);
    RtlSecureZeroMemory(J->Igtk,sizeof(J->Igtk));J->MgmtRxPn=J->MgmtTxPn=0;J->SaQueryPending=FALSE;
}
static ULONG64 PmfPn(const UCHAR *P){ULONG I;ULONG64 V=0;for(I=0;I<6;I++)V|=(ULONG64)P[I]<<(8*I);return V;}
NTSTATUS WcnPmfInstallKey(WCN_IGTK Keys[2],UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6])
{
    WCN_IGTK *K;ULONG64 Pn;ULONG I;UCHAR Previous[16];BOOLEAN Had;if(Index<4||Index>5)return STATUS_INVALID_PARAMETER;
    K=&Keys[Index-4];Pn=PmfPn(Rsc);
    if((K->Known||K->Valid)&&RtlCompareMemory(K->Key,Key,16)==16){if(!K->Valid)return STATUS_INVALID_DEVICE_STATE;if(Pn>K->Pn)K->Pn=Pn;return STATUS_SUCCESS;}
    for(I=0;I<2;I++)if((I!=(ULONG)(Index-4)&&(Keys[I].Known||Keys[I].Valid)&&RtlCompareMemory(Keys[I].Key,Key,16)==16)||
        (Keys[I].PreviousKnown&&RtlCompareMemory(Keys[I].Previous,Key,16)==16))return STATUS_INVALID_PARAMETER;
    Had=K->Known||K->Valid;RtlCopyMemory(Previous,K->Key,16);RtlSecureZeroMemory(K,sizeof(*K));
    if(Had){RtlCopyMemory(K->Previous,Previous,16);K->PreviousKnown=TRUE;}RtlSecureZeroMemory(Previous,sizeof(Previous));
    RtlCopyMemory(K->Key,Key,16);K->Pn=Pn;K->Valid=K->Known=TRUE;return STATUS_SUCCESS;
}
NTSTATUS WcnPmfInstall(PWCN_DEVICE D,PWCN_JOIN J,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6])
{UNREFERENCED_PARAMETER(D);return J->Ap.Pmf&&J->PmfAlgorithm?WcnPmfInstallKey(J->Igtk,Index,Key,Rsc):STATUS_INVALID_PARAMETER;}
BOOLEAN WcnPmfReceiveFrame(PVOID Algorithm,WCN_IGTK Keys[2],ULONG64 *RxPn,const UCHAR *F,ULONG N,ULONG *BodyOffset)
{
    ULONG64 Pn;
    if(N<26||N>2048)return FALSE;
    if(!(F[4]&1)){
        /* WMI guarantees decrypted robust management with IV retained and
         * protected bit set; caller already rejected CRC/decrypt/MIC/keymiss.
         * Inspect CCMP header and an independent management PN stream. */
        if(!(F[1]&0x40)||N<34||F[26]||F[27]!=0x20)return FALSE;
        Pn=F[24]|((ULONG64)F[25]<<8)|((ULONG64)F[28]<<16)|((ULONG64)F[29]<<24)|((ULONG64)F[30]<<32)|((ULONG64)F[31]<<40);
        if(!Pn||Pn<=(*RxPn))return FALSE;(*RxPn)=Pn;*BodyOffset=32;return TRUE;
    }else{
        UCHAR Data[2048],Mac[16],Difference=0;const UCHAR *M;ULONG Key,I;NTSTATUS St;WCN_IGTK *K;
        if(N<44||!Algorithm)return FALSE;M=F+N-18;
        if(M[0]!=76||M[1]!=16)return FALSE;Key=M[2]|((ULONG)M[3]<<8);if(Key<4||Key>5)return FALSE;
        K=&Keys[Key-4];Pn=PmfPn(M+4);if(!K->Valid||!Pn||Pn<=K->Pn)return FALSE;
        /* 802.11 BIP AAD: FC with Retry/PwrMgt/MoreData masked, A1/A2/A3. */
        Data[0]=F[0];Data[1]=F[1]&~0x38;RtlCopyMemory(Data+2,F+4,18);RtlCopyMemory(Data+20,F+24,N-24);
        RtlZeroMemory(Data+N-4-8,8);St=WcnCmac(Algorithm,K->Key,Data,N-4,Mac);
        for(I=0;I<8;I++)Difference|=Mac[I]^M[10+I];
        RtlSecureZeroMemory(Data,sizeof(Data));RtlSecureZeroMemory(Mac,sizeof(Mac));
        if(!NT_SUCCESS(St)||Difference)return FALSE;K->Pn=Pn;*BodyOffset=24;return TRUE;
    }
}

BOOLEAN WcnPmfReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N,ULONG *BodyOffset)
{UNREFERENCED_PARAMETER(D);return J->Ap.Pmf&&J->PairwiseInstalled&&!J->KeyHold&&WcnPmfReceiveFrame(J->PmfAlgorithm,J->Igtk,&J->MgmtRxPn,F,N,BodyOffset);}
NTSTATUS WcnPmfTxUnicast(UCHAR *F,ULONG *Length,ULONG Capacity,ULONG64 *Pn)
{
    ULONG N=*Length,Type=F[0]&0xfc;ULONG64 V;
    if(N<26||N>Capacity||Capacity-N<16||(F[4]&1)||(F[1]&0x40)||(Type!=0xa0&&Type!=0xc0&&Type!=0xd0))return STATUS_INVALID_PARAMETER;
    if(*Pn>=0xffffffffffffull)return STATUS_INTEGER_OVERFLOW;
    V=++*Pn;RtlMoveMemory(F+32,F+24,N-24);RtlZeroMemory(F+24,8);RtlZeroMemory(F+N+8,8);F[1]|=0x40;
    F[24]=(UCHAR)V;F[25]=(UCHAR)(V>>8);F[27]=0x20;F[28]=(UCHAR)(V>>16);F[29]=(UCHAR)(V>>24);F[30]=(UCHAR)(V>>32);F[31]=(UCHAR)(V>>40);*Length=N+16;return STATUS_SUCCESS;
}
NTSTATUS WcnPmfTxGroup(PVOID Algorithm,WCN_IGTK Keys[2],UCHAR Index,UCHAR *F,ULONG *Length,ULONG Capacity)
{
    ULONG N=*Length,I,Type=F[0]&0xfc;ULONG64 V;UCHAR Data[1024],Mac[16],*M;WCN_IGTK *K;NTSTATUS St;
    if(!Algorithm||Index<4||Index>5||N<26||N>Capacity||Capacity-N<18||N+14>sizeof(Data)||!(F[4]&1)||(F[1]&0x40)||(Type!=0xa0&&Type!=0xc0&&Type!=0xd0))return STATUS_INVALID_PARAMETER;
    K=&Keys[Index-4];if(!K->Valid||K->Pn>=0xffffffffffffull)return STATUS_INVALID_DEVICE_STATE;
    V=++K->Pn;M=F+N;RtlZeroMemory(M,18);M[0]=76;M[1]=16;M[2]=Index;for(I=0;I<6;I++)M[4+I]=(UCHAR)(V>>(8*I));N+=18;
    Data[0]=F[0];Data[1]=F[1]&~0x38;RtlCopyMemory(Data+2,F+4,18);RtlCopyMemory(Data+20,F+24,N-24);
    St=WcnCmac(Algorithm,K->Key,Data,N-4,Mac);if(NT_SUCCESS(St)){RtlCopyMemory(M+10,Mac,8);*Length=N;}
    RtlSecureZeroMemory(Data,sizeof(Data));RtlSecureZeroMemory(Mac,sizeof(Mac));return St;
}

/* Both vdevs share the firmware management descriptor namespace (bounded IDs).
 * An ID remains reserved until the matching completion, never by timer alone. */
ULONG WcnMgmtAllocate(PWCN_DEVICE D,ULONG Owner)
{
    ULONG I,Id;if(Owner>=ARRAYSIZE(D->MgmtInUse)||D->MgmtInUse[Owner])return 0;
    for(I=0;I<255;I++){Id=(++D->MgmtSequence)&255u;if(Id&&Id!=D->MgmtInUse[0]&&Id!=D->MgmtInUse[1]&&Id!=D->MgmtInUse[2]){D->MgmtInUse[Owner]=Id;return Id;}}return 0;
}
