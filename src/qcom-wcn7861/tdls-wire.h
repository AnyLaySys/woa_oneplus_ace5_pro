/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * IEEE 802.11 TDLS TPK wire rules. MAC/nonce ordering and MIC input order
 * checked against the BSD-licensed AOSP wpa_supplicant TDLS implementation. */
#pragma once
#define WCN_TDLS_PEERS 8
static __inline int WcnTdlsOrder(const UCHAR *A,const UCHAR *B,ULONG N)
{ULONG I;for(I=0;I<N;I++)if(A[I]!=B[I])return A[I]<B[I]?-1:1;return 0;}
static __inline BOOLEAN WcnTdlsNonzero(const UCHAR *P,ULONG N)
{ULONG I;UCHAR V=0;for(I=0;I<N;I++)V|=P[I];return V!=0;}
typedef struct _WCN_TDLS_IES {const UCHAR *Link,*Rsn,*Ft,*Life,*Ext,*Channels,*Classes,*Wmm;ULONG RsnLength,FtLength;} WCN_TDLS_IES;
static __inline BOOLEAN WcnTdlsIes(const UCHAR *P,ULONG N,WCN_TDLS_IES *E)
{
    ULONG L,I;RtlZeroMemory(E,sizeof(*E));if(N&&!P)return FALSE;
    while(N){if(P[0]==0){for(I=0;I<N;I++)if(P[I])break;if(I==N)return TRUE;}
        if(N<2||(L=P[1]+2u)>N)return FALSE;
        if(P[0]==101){if(E->Link||L!=20)return FALSE;E->Link=P;}
        if(P[0]==48){if(E->Rsn||L>80)return FALSE;E->Rsn=P;E->RsnLength=L;}
        if(P[0]==55){if(E->Ft||L<84)return FALSE;E->Ft=P;E->FtLength=L;}
        if(P[0]==56){if(E->Life||L!=7||P[2]!=2)return FALSE;E->Life=P;}
        if(P[0]==127){if(E->Ext)return FALSE;E->Ext=P;}
        if(P[0]==36){if(E->Channels||L<4||(L&1))return FALSE;E->Channels=P;}
        if(P[0]==59){if(E->Classes||L<3)return FALSE;E->Classes=P;}
        if(P[0]==221&&L>=8&&Jeq(P+2,(const UCHAR*)"\x00\x50\xf2\x02",4)){if(E->Wmm||((L!=9||P[6]!=0)&&(L!=26||P[6]!=1))||P[7]!=1)return FALSE;E->Wmm=P;}
        P+=L;N-=L;
    }return TRUE;
}
static __inline BOOLEAN WcnTdlsBit(const UCHAR *Ext,ULONG Bit)
{return Ext&&Ext[1]>Bit/8&&(Ext[2+Bit/8]&(1u<<(Bit&7)))!=0;}
static __inline BOOLEAN WcnTdlsBssLimits(const UCHAR *P,ULONG N,BOOLEAN *Prohibit,BOOLEAN *NoSwitch)
{
    const UCHAR *Ext=NULL;ULONG L;*Prohibit=*NoSwitch=FALSE;
    while(N){if(N<2||(L=P[1]+2u)>N)return FALSE;if(P[0]==127){if(Ext)return FALSE;Ext=P;}P+=L;N-=L;}
    *Prohibit=WcnTdlsBit(Ext,38);*NoSwitch=WcnTdlsBit(Ext,39);return TRUE;
}
static __inline ULONG WcnTdlsRsn(UCHAR *P)
{static const UCHAR B[]={48,20,1,0,0,15,172,7,1,0,0,15,172,4,1,0,0,15,172,7,12,2};RtlCopyMemory(P,B,sizeof(B));return sizeof(B);}
static __inline BOOLEAN WcnTdlsRsnValid(const UCHAR *P,ULONG N)
{
    UCHAR Canon[22];WcnTdlsRsn(Canon);if(!P||(N!=22&&N!=24)||P[1]+2u!=N)return FALSE;
    if(!Jeq(P+2,Canon+2,18)||(Jle16(P+20)&0x2c2)!=0x200)return FALSE;
    return N==22||Jle16(P+22)==0;
}
static __inline BOOLEAN WcnTdlsLink(const UCHAR *P,const UCHAR Bssid[6],const UCHAR Init[6],const UCHAR Resp[6])
{return P&&Jeq(P+2,Bssid,6)&&Jeq(P+8,Init,6)&&Jeq(P+14,Resp,6);}
NTSTATUS WcnTdlsDerive(PVOID Sha,PVOID Hmac,const UCHAR Snonce[32],const UCHAR Anonce[32],const UCHAR Local[6],const UCHAR Peer[6],const UCHAR Bssid[6],UCHAR Key[32]);
NTSTATUS WcnTdlsMic(PVOID Cmac,const UCHAR Key[16],UCHAR Sequence,const UCHAR *Link,const UCHAR *Rsn,ULONG RsnLength,const UCHAR *Life,const UCHAR *Ft,ULONG FtLength,UCHAR Mic[16]);
NTSTATUS WcnTdlsTeardownMic(PVOID Cmac,const UCHAR Key[16],USHORT Reason,UCHAR Token,const UCHAR *Link,const UCHAR *Ft,ULONG FtLength,UCHAR Mic[16]);
