/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#include "wcn7861.h"
#include "stage7-util.h"
#include "tdls-wire.h"
#include <bcrypt.h>
static NTSTATUS Thash(PVOID Algorithm,const UCHAR *Key,ULONG KeyLength,const UCHAR *P,ULONG N,UCHAR Out[32])
{
    BCRYPT_HASH_HANDLE H=NULL;NTSTATUS St=BCryptCreateHash((BCRYPT_ALG_HANDLE)Algorithm,&H,NULL,0,(PUCHAR)Key,KeyLength,0);
    if(NT_SUCCESS(St))St=BCryptHashData(H,(PUCHAR)P,N,0);if(NT_SUCCESS(St))St=BCryptFinishHash(H,Out,32,0);if(H)BCryptDestroyHash(H);if(!NT_SUCCESS(St))RtlSecureZeroMemory(Out,32);return St;
}
NTSTATUS WcnTdlsDerive(PVOID Sha,PVOID Hmac,const UCHAR S[32],const UCHAR A[32],const UCHAR Local[6],const UCHAR Peer[6],const UCHAR Bssid[6],UCHAR Key[32])
{
    UCHAR Nonces[64],Input[30],Hash[32];NTSTATUS St;BOOLEAN Less=WcnTdlsOrder(S,A,32)<0;
    RtlCopyMemory(Nonces,Less?S:A,32);RtlCopyMemory(Nonces+32,Less?A:S,32);St=Thash(Sha,NULL,0,Nonces,64,Hash);
    Jputle16(Input,1);RtlCopyMemory(Input+2,"TDLS PMK",8);Less=WcnTdlsOrder(Local,Peer,6)<0;
    RtlCopyMemory(Input+10,Less?Local:Peer,6);RtlCopyMemory(Input+16,Less?Peer:Local,6);RtlCopyMemory(Input+22,Bssid,6);Jputle16(Input+28,256);
    if(NT_SUCCESS(St))St=Thash(Hmac,Hash,32,Input,sizeof(Input),Key);else RtlSecureZeroMemory(Key,32);
    RtlSecureZeroMemory(Nonces,sizeof(Nonces));RtlSecureZeroMemory(Hash,sizeof(Hash));RtlSecureZeroMemory(Input,sizeof(Input));return St;
}
NTSTATUS WcnTdlsMic(PVOID Cmac,const UCHAR Key[16],UCHAR Sequence,const UCHAR *Link,const UCHAR *Rsn,ULONG RsnLength,const UCHAR *Life,const UCHAR *Ft,ULONG FtLength,UCHAR Mic[16])
{
    UCHAR B[397];ULONG N=0;NTSTATUS St;if(!Link||!Rsn||!Life||!Ft||RsnLength>80||FtLength<84||FtLength>257)return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(B,Link+8,12);N=12;B[N++]=Sequence;RtlCopyMemory(B+N,Link,20);N+=20;RtlCopyMemory(B+N,Rsn,RsnLength);N+=RsnLength;RtlCopyMemory(B+N,Life,7);N+=7;RtlCopyMemory(B+N,Ft,FtLength);RtlZeroMemory(B+N+4,16);N+=FtLength;
    St=WcnCmac(Cmac,Key,B,N,Mic);RtlSecureZeroMemory(B,sizeof(B));return St;
}
NTSTATUS WcnTdlsTeardownMic(PVOID Cmac,const UCHAR Key[16],USHORT Reason,UCHAR Token,const UCHAR *Link,const UCHAR *Ft,ULONG FtLength,UCHAR Mic[16])
{
    UCHAR B[281];ULONG N;NTSTATUS St;if(!Link||!Ft||FtLength<84||FtLength>257)return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(B,Link,20);Jputle16(B+20,Reason);B[22]=Token;B[23]=4;N=24;RtlCopyMemory(B+N,Ft,FtLength);RtlZeroMemory(B+N+4,16);N+=FtLength;
    St=WcnCmac(Cmac,Key,B,N,Mic);RtlSecureZeroMemory(B,sizeof(B));return St;
}
