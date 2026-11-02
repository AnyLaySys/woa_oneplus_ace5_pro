#pragma once
#define WCN_AUTH_OPEN 1u
#define WCN_AUTH_EAP 6u
#define WCN_AUTH_PSK 7u
#define WCN_AUTH_SAE 9u
#define WCN_AUTH_EAP_SHA256 11u
static __inline BOOLEAN WcnAuthSupported(ULONG A)
{return A==WCN_AUTH_OPEN||A==WCN_AUTH_EAP||A==WCN_AUTH_PSK||A==WCN_AUTH_SAE||A==WCN_AUTH_EAP_SHA256;}
static __inline UCHAR WcnAkm(ULONG A)
{return (UCHAR)(A==WCN_AUTH_SAE?8:A==WCN_AUTH_EAP_SHA256?5:A==WCN_AUTH_EAP?1:A==WCN_AUTH_PSK?2:0);}
static __inline BOOLEAN WcnAuthNeedsPmf(ULONG A){return A==WCN_AUTH_SAE||A==WCN_AUTH_EAP_SHA256;}
/* No WPA/TKIP/WEP fallback. The exact OS auth choice must appear in RSN.
 * Windows owns PSK/EAP/SAE derivation, certificate checks and four-way state. */
static __inline BOOLEAN WcnRsnAccept(const UCHAR *P,ULONG N,ULONG Auth,BOOLEAN Pmf)
{
    ULONG K,I;BOOLEAN Cipher=FALSE,Akm=FALSE;USHORT Caps=0;
    if(Auth==WCN_AUTH_OPEN||!WcnAuthSupported(Auth)||(WcnAuthNeedsPmf(Auth)&&!Pmf))return FALSE;
    if(N<8||P[0]!=1||P[1]||RtlCompareMemory(P+2,"\x00\x0f\xac\x04",4)!=4)return FALSE;
    P+=6;N-=6;K=P[0]|((ULONG)P[1]<<8);P+=2;N-=2;if(!K||K>N/4)return FALSE;
    for(I=0;I<K;I++)if(RtlCompareMemory(P+I*4,"\x00\x0f\xac\x04",4)==4)Cipher=TRUE;
    P+=K*4;N-=K*4;if(N<2)return FALSE;K=P[0]|((ULONG)P[1]<<8);P+=2;N-=2;if(!K||K>N/4)return FALSE;
    for(I=0;I<K;I++)if(RtlCompareMemory(P+I*4,"\x00\x0f\xac",3)==3&&P[I*4+3]==WcnAkm(Auth))Akm=TRUE;
    P+=K*4;N-=K*4;
    if(N){if(N<2)return FALSE;Caps=(USHORT)(P[0]|(P[1]<<8));P+=2;N-=2;}
    if((Caps&64)&&!Pmf)return FALSE;if(Pmf&&!(Caps&128))return FALSE;
    if(N){if(N<2)return FALSE;K=P[0]|((ULONG)P[1]<<8);P+=2;N-=2;if(K>N/16)return FALSE;P+=K*16;N-=K*16;}
    if(N&&N!=4)return FALSE;
    if(Pmf&&N&&RtlCompareMemory(P,"\x00\x0f\xac\x06",4)!=4)return FALSE; /* BIP-CMAC128 */
    return Cipher&&Akm;
}
static __inline ULONG WcnRsnBuild(UCHAR *P,ULONG Auth,BOOLEAN Pmf,BOOLEAN H2e)
{
    static const UCHAR Base[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    ULONG N=sizeof(Base);if(Auth==WCN_AUTH_OPEN||!WcnAuthSupported(Auth))return 0;
    RtlCopyMemory(P,Base,sizeof(Base));P[19]=WcnAkm(Auth);
    if(Pmf){P[20]=(UCHAR)(128|(WcnAuthNeedsPmf(Auth)?64:0));P[1]=26;P[22]=P[23]=0;RtlCopyMemory(P+24,"\x00\x0f\xac\x06",4);N=28;}
    if(Auth==WCN_AUTH_SAE&&H2e){P[N++]=244;P[N++]=1;P[N++]=0x20;} /* SAE H2E RSNXE */
    return N;
}
