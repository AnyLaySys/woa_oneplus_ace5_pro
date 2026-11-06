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
static __inline BOOLEAN WcnRsnAcceptAkm(const UCHAR *P,ULONG N,ULONG Auth,UCHAR Wanted,BOOLEAN *Pmf)
{
    ULONG K,I;BOOLEAN Cipher=FALSE,Akm=FALSE,UsePmf;USHORT Caps=0;
    if(Auth==WCN_AUTH_OPEN||!WcnAuthSupported(Auth)||(WcnAuthNeedsPmf(Auth)&&!*Pmf))return FALSE;
    if(N<8||P[0]!=1||P[1]||RtlCompareMemory(P+2,"\x00\x0f\xac\x04",4)!=4)return FALSE;
    P+=6;N-=6;K=P[0]|((ULONG)P[1]<<8);P+=2;N-=2;if(!K||K>N/4)return FALSE;
    for(I=0;I<K;I++)if(RtlCompareMemory(P+I*4,"\x00\x0f\xac\x04",4)==4)Cipher=TRUE;
    P+=K*4;N-=K*4;if(N<2)return FALSE;K=P[0]|((ULONG)P[1]<<8);P+=2;N-=2;if(!K||K>N/4)return FALSE;
    for(I=0;I<K;I++)if(RtlCompareMemory(P+I*4,"\x00\x0f\xac",3)==3&&P[I*4+3]==Wanted)Akm=TRUE;
    P+=K*4;N-=K*4;
    if(N){if(N<2)return FALSE;Caps=(USHORT)(P[0]|(P[1]<<8));P+=2;N-=2;}
    /* WDI MFPEnabled permits advertising PMF; negotiate with this AP.
     * WPA3 and an AP's MFPR bit still require PMF, never fall back. */
    UsePmf=*Pmf&&(Caps&128)!=0;
    if(((Caps&64)||WcnAuthNeedsPmf(Auth))&&!UsePmf)return FALSE;
    if(N){if(N<2)return FALSE;K=P[0]|((ULONG)P[1]<<8);P+=2;N-=2;if(K>N/16)return FALSE;P+=K*16;N-=K*16;}
    if(N&&N!=4)return FALSE;
    if(UsePmf&&N&&RtlCompareMemory(P,"\x00\x0f\xac\x06",4)!=4)return FALSE; /* BIP-CMAC128 */
    if(!Cipher||!Akm)return FALSE;*Pmf=UsePmf;return TRUE;
}
static __inline BOOLEAN WcnRsnAccept(const UCHAR *P,ULONG N,ULONG Auth,BOOLEAN *Pmf)
{return WcnRsnAcceptAkm(P,N,Auth,WcnAkm(Auth),Pmf);}
static __inline ULONG WcnRsnBuild(UCHAR *P,ULONG Auth,BOOLEAN Pmf,BOOLEAN H2e)
{
    static const UCHAR Base[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
    ULONG N=sizeof(Base);if(Auth==WCN_AUTH_OPEN||!WcnAuthSupported(Auth))return 0;
    RtlCopyMemory(P,Base,sizeof(Base));P[19]=WcnAkm(Auth);
    if(Pmf){P[20]=(UCHAR)(128|(WcnAuthNeedsPmf(Auth)?64:0));P[1]=26;P[22]=P[23]=0;RtlCopyMemory(P+24,"\x00\x0f\xac\x06",4);N=28;}
    if(Auth==WCN_AUTH_SAE&&H2e){P[N++]=244;P[N++]=1;P[N++]=0x20;} /* SAE H2E RSNXE */
    return N;
}

/* PMKID is an OS-supplied cache identifier, never a PMK/password. Keep the
 * management cipher and SAE RSNXE after the inserted PMKID list intact. */
static __inline ULONG WcnRsnBuildCached(UCHAR *P,ULONG Auth,BOOLEAN Pmf,BOOLEAN H2e,const UCHAR *Pmkid)
{
    ULONG N=WcnRsnBuild(P,Auth,Pmf,H2e),At;if(!N||!Pmkid)return N;
    if(Pmf){RtlMoveMemory(P+40,P+24,N-24);P[22]=1;P[23]=0;RtlCopyMemory(P+24,Pmkid,16);P[1]+=16;return N+16;}
    At=P[1]+2;RtlMoveMemory(P+At+18,P+At,N-At);P[At]=1;P[At+1]=0;RtlCopyMemory(P+At+2,Pmkid,16);P[1]+=18;return N+18;
}

static __inline BOOLEAN WcnRsnCacheRejected(ULONG Status,BOOLEAN *HasPmkid,UCHAR Pmkid[16])
{if(Status!=53||!*HasPmkid)return FALSE;*HasPmkid=FALSE;RtlZeroMemory(Pmkid,16);return TRUE;}

/* AP policy comes from the OS auth list, not from an SSID or password. */
static __inline ULONG WcnRsnBuildAp(UCHAR *P,BOOLEAN Psk,BOOLEAN Sae)
{
    ULONG N;if(!Psk&&!Sae)return 0;
    N=WcnRsnBuild(P,Sae?WCN_AUTH_SAE:WCN_AUTH_PSK,Sae,FALSE);
    if(Sae&&Psk){RtlMoveMemory(P+24,P+20,N-20);P[14]=2;RtlCopyMemory(P+20,"\x00\x0f\xac\x02",4);N+=4;P[1]+=4;P[24]=128;}
    if(Sae){P[N++]=244;P[N++]=1;P[N++]=0x20;}
    return N;
}

static __inline BOOLEAN WcnRobustManagement(const UCHAR *F,ULONG N)
{
    ULONG Type;if(N<25)return FALSE;Type=F[0]&0xfc;
    if(Type==0xa0||Type==0xc0)return TRUE;if(Type!=0xd0)return FALSE;
    return (F[1]&0x40)||(F[24]!=4&&F[24]!=7&&F[24]!=11&&F[24]!=15&&F[24]!=20&&F[24]!=21&&F[24]!=22&&F[24]!=127);
}
