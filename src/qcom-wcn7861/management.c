/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * PMF: firmware verifies/decrypts unicast CCMP; group BIP-CMAC128 is checked
 * in software because ath12k explicitly leaves IGTK indices4/5 to the host.
 * BCrypt provides CMAC; this driver never derives passwords or SAE secrets. */
#include "wcn7861.h"
#include <bcrypt.h>
static NTSTATUS PmfCmac(PVOID Algorithm,const UCHAR Key[16],const UCHAR *Data,ULONG Length,UCHAR Mac[16])
{
    BCRYPT_HASH_HANDLE H=NULL;NTSTATUS St;
    St=BCryptCreateHash((BCRYPT_ALG_HANDLE)Algorithm,&H,NULL,0,(PUCHAR)Key,16,0);
    if(NT_SUCCESS(St))St=BCryptHashData(H,(PUCHAR)Data,Length,0);
    if(NT_SUCCESS(St))St=BCryptFinishHash(H,Mac,16,0);
    if(H)BCryptDestroyHash(H);if(!NT_SUCCESS(St))RtlSecureZeroMemory(Mac,16);return St;
}
NTSTATUS WcnPmfInit(PWCN_DEVICE D)
{
    if(D->Join.PmfAlgorithm)return STATUS_SUCCESS;
    return BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&D->Join.PmfAlgorithm,BCRYPT_AES_CMAC_ALGORITHM,NULL,0);
}
VOID WcnPmfClear(PWCN_DEVICE D)
{
    RtlSecureZeroMemory(D->Join.Igtk,sizeof(D->Join.Igtk));D->Join.MgmtRxPn=D->Join.MgmtTxPn=0;D->Join.SaQueryPending=FALSE;
}
static ULONG64 PmfPn(const UCHAR *P){ULONG I;ULONG64 V=0;for(I=0;I<6;I++)V|=(ULONG64)P[I]<<(8*I);return V;}
NTSTATUS WcnPmfInstall(PWCN_DEVICE D,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6])
{
    WCN_IGTK *K;ULONG64 Pn;
    if(!D->Join.Ap.Pmf||!D->Join.PmfAlgorithm||Index<4||Index>5)return STATUS_INVALID_PARAMETER;
    K=&D->Join.Igtk[Index-4];Pn=PmfPn(Rsc);
    /* Duplicate install must not reset replay state (including GTK rekeys). */
    if(K->Valid&&RtlCompareMemory(K->Key,Key,16)==16){if(Pn>K->Pn)K->Pn=Pn;return STATUS_SUCCESS;}
    RtlSecureZeroMemory(K,sizeof(*K));RtlCopyMemory(K->Key,Key,16);K->Pn=Pn;K->Valid=TRUE;return STATUS_SUCCESS;
}
BOOLEAN WcnPmfReceive(PWCN_DEVICE D,const UCHAR *F,ULONG N,ULONG *BodyOffset)
{
    PWCN_JOIN J=&D->Join;ULONG64 Pn;
    if(N<26||N>2048||!J->Ap.Pmf||!J->PairwiseInstalled)return FALSE;
    if(!(F[4]&1)){
        /* WMI guarantees decrypted robust management with IV retained and
         * protected bit set; caller already rejected CRC/decrypt/MIC/keymiss.
         * Inspect CCMP header and an independent management PN stream. */
        if(!(F[1]&0x40)||N<34||F[26]||F[27]!=0x20)return FALSE;
        Pn=F[24]|((ULONG64)F[25]<<8)|((ULONG64)F[28]<<16)|((ULONG64)F[29]<<24)|((ULONG64)F[30]<<32)|((ULONG64)F[31]<<40);
        if(!Pn||Pn<=J->MgmtRxPn)return FALSE;J->MgmtRxPn=Pn;*BodyOffset=32;return TRUE;
    }else{
        UCHAR Data[2048],Mac[16],Difference=0;const UCHAR *M;ULONG Key,I;NTSTATUS St;WCN_IGTK *K;
        if(N<44||!J->PmfAlgorithm)return FALSE;M=F+N-18;
        if(M[0]!=76||M[1]!=16)return FALSE;Key=M[2]|((ULONG)M[3]<<8);if(Key<4||Key>5)return FALSE;
        K=&J->Igtk[Key-4];Pn=PmfPn(M+4);if(!K->Valid||!Pn||Pn<=K->Pn)return FALSE;
        /* 802.11 BIP AAD: FC with Retry/PwrMgt/MoreData masked, A1/A2/A3. */
        Data[0]=F[0];Data[1]=F[1]&~0x38;RtlCopyMemory(Data+2,F+4,18);RtlCopyMemory(Data+20,F+24,N-24);
        RtlZeroMemory(Data+N-4-8,8);St=PmfCmac(J->PmfAlgorithm,K->Key,Data,N-4,Mac);
        for(I=0;I<8;I++)Difference|=Mac[I]^M[10+I];
        RtlSecureZeroMemory(Data,sizeof(Data));RtlSecureZeroMemory(Mac,sizeof(Mac));
        if(!NT_SUCCESS(St)||Difference)return FALSE;K->Pn=Pn;*BodyOffset=24;return TRUE;
    }
}
