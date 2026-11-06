/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Firmware-scheduled TDLS, automatic LAN discovery and TPK handshake. Independent
 * peer keys/replay/REO queues; AP authentication and OS IP configuration stay
 * untouched. Failed optional peers fall back to the AP, never to cleartext.
 * A-MSDU and DBS reuse the existing data path; no software IP proxy/bridge. */
#include "wcn7861.h"
#include "stage7-util.h"
#include "stage7-wire.h"
#include "network-policy.h"
#include "security-policy.h"
#include "stage7-rx-protocol.h"
#include "tdls-wire.h"
#include "hotspot.h"
#include <bcrypt.h>
enum {Tfree,Tdiscover,Tdiscovery,Trequest,Tresponse,Treply,Tconfirm,TconfirmTx,Tsetup,Tup,Tclean,Tcool,Tquarantine};
typedef struct _TPEER {
    WCN_JOIN L;UCHAR State,Step,Token,Attempts,Reply,Errors;BOOLEAN Initiator,Secure,Notify,KeyReady,OffCap,BufferCap,PsmCap,OffRequested,OffTried,Buffering;
    ULONG64 Channels;ULONG Country,Qos,OffFrequency,OffCenter,OffWidth,OffClass;
    UCHAR Snonce[32],Anonce[32],Key[32],Rsn[80],Classes[32];ULONG RsnLength,Life,ClassCount,Aid;
    LONGLONG At,RetryAt,ReplyAt,LiveAt;UCHAR Restart[1038];ULONG RestartLength;
} TPEER;
typedef struct _TDLS {
    PVOID Sha,Hmac;TPEER Peer[WCN_TDLS_PEERS];BOOLEAN Enabled[2],Ps[2];UCHAR Options[2];ULONG Seen[2];LONGLONG BusyAt[2];UCHAR Token,ReplyVdev,ReplyMac[6],Reply[768];ULONG ReplyLength;LONGLONG ReplyAt,DiscoverAt;UCHAR Restart[1038];
} TDLS;
static TDLS *Ts(PWCN_DEVICE D){return (TDLS*)D->Tdls;}
static ULONG Ti(PWCN_JOIN J){return J->Vdev==2?1:0;}
static BOOLEAN Teligible(PWCN_DEVICE D,PWCN_JOIN J)
{return J&&J->Authorized&&(!(J->Vdev==0?D->Secondary.Started:D->Join.Started)||WcnService(D,363))&&!J->Failed&&!J->Resetting&&!J->CsaState&&!J->Ap.TdlsProhibited&&!WcnHotspotActive(D)&&WcnTdlsSupported(D);}
BOOLEAN WcnTdlsSupported(PWCN_DEVICE D)
{return D->Tdls&&WcnService(D,36)&&D->Join.PmfAlgorithm!=NULL;}
static TPEER *Tfind(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Mac)
{TDLS *T=Ts(D);ULONG I;if(!T)return NULL;for(I=0;I<WCN_TDLS_PEERS;I++)if(T->Peer[I].State&&T->Peer[I].L.Vdev==J->Vdev&&Jeq(T->Peer[I].L.Ap.Bssid,Mac,6))return &T->Peer[I];return NULL;}
static VOID Tclear(TPEER *P)
{WCN_DMA Reo=P->L.ReoQueue,Mgmt=P->L.MgmtDma;RtlSecureZeroMemory(P,sizeof(*P));P->L.ReoQueue=Reo;P->L.MgmtDma=Mgmt;}
static TPEER *Tnew(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Mac)
{
    TDLS *T=Ts(D);TPEER *P;ULONG I;P=Tfind(D,J,Mac);if(P){if(P->State!=Tcool||WcnNowMs()-P->At<30000)return NULL;}
    else{for(I=0;I<WCN_TDLS_PEERS;I++)if(T->Peer[I].State==Tfree||T->Peer[I].State==Tcool)break;if(I==WCN_TDLS_PEERS)return NULL;P=&T->Peer[I];}
    Tclear(P);P->L.Vdev=J->Vdev;P->L.Tdls=TRUE;P->L.Started=TRUE;P->L.MacId=J->MacId;P->Secure=J->Ap.Auth!=WCN_AUTH_OPEN;P->Life=43200;P->At=WcnNowMs();if(P->Secure)P->RsnLength=WcnTdlsRsn(P->Rsn);
    RtlCopyMemory(P->L.Mac,J->Mac,6);RtlCopyMemory(P->L.Infrastructure,J->Ap.Bssid,6);RtlCopyMemory(P->L.Ap.Bssid,Mac,6);return P;
}
static ULONG Tclass(const WCN_JOIN_AP *A)
{ULONG Ch=WcnFrequencyChannel(A->Frequency);if(A->Frequency<2500)return A->Width==40?(A->CenterFrequency>A->Frequency?83:84):81;
 if(A->Width>=80)return A->Width==160?129:128;if(Ch>=149)return A->Width==40?(A->CenterFrequency>A->Frequency?126:127):(Ch>161?125:124);return A->Width==40?(Ch<52?(A->CenterFrequency>A->Frequency?116:117):(A->CenterFrequency>A->Frequency?119:120)):(Ch<52?115:118);}
static VOID Tdrop(TPEER *P,BOOLEAN Notify);

/* Firmware owns channel-switch and PTI/PTR timing. Host policy supplies only
 * the peer/regulatory intersection; no guessed off-channel frame injection. */
static ULONG Toptions(PWCN_DEVICE D,PWCN_JOIN J)
{return (WcnService(D,56)&&!J->Ap.TdlsSwitchProhibited?1u:0u)|(WcnService(D,57)?2u:0u)|(WcnService(D,58)?4u:0u);}
ULONG WcnTdlsCapabilities(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *P)
{
    if(!WcnTdlsSupported(D)||J->Ap.TdlsProhibited)return 0;
    P[0]=127;P[1]=8;RtlZeroMemory(P+2,8);P[5]=(UCHAR)(((Toptions(D,J)&2)?16:0)|((Toptions(D,J)&4)?32:0)|((Toptions(D,J)&1)?64:0));P[6]=32;
    if(WcnService(D,342))P[9]=32;return 10;
}
VOID WcnTdlsObserve(PWCN_JOIN J,const UCHAR *P,ULONG N)
{if(!WcnTdlsBssLimits(P,N,&J->Ap.TdlsProhibited,&J->Ap.TdlsSwitchProhibited))J->Ap.TdlsProhibited=J->Ap.TdlsSwitchProhibited=TRUE;}
static ULONG Tgeometry(PWCN_DEVICE D,ULONG F,ULONG Limit,ULONG *Center)
{
    static const ULONG C80[]={5210,5290,5775};ULONG W,I,C,Ch=WcnFrequencyChannel(F);if(F<2500&&Limit>40)Limit=40;
    for(W=160;W>=20;W/=2){if(W>Limit)continue;
        if(W==160){C=5250;if(F+70>=C&&C+70>=F&&WcnWidthAllowed(D,C,W)){*Center=C;return W;}}
        else if(W==80){for(I=0;I<ARRAYSIZE(C80);I++){C=C80[I];if(F+30>=C&&C+30>=F&&WcnWidthAllowed(D,C,W)){*Center=C;return W;}}}
        else{C=W==20?F:F<2500?(Ch<=9?F+10:F-10):((((Ch-(Ch<149?36:149))/4)&1)?F-10:F+10);if(WcnWidthAllowed(D,C,W)){*Center=C;return W;}}
    }return 0;
}
static BOOLEAN TnonDfs(PWCN_DEVICE D,ULONG Center,ULONG Width)
{ULONG I;for(I=0;I<D->Scan.ChannelCount;I++)if(D->Scan.Channel[I].Frequency>=Center-Width/2+10&&D->Scan.Channel[I].Frequency<Center+Width/2&&((D->Scan.Channel[I].Flags&8)||(D->Scan.Channel[I].Frequency>=5260&&D->Scan.Channel[I].Frequency<=5320)))return FALSE;return TRUE;}

static VOID Tlocal(PWCN_DEVICE D,PWCN_JOIN J,PWCN_JOIN_AP A,BOOLEAN Wide)
{
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);ULONG I,B=J->Ap.Frequency<2500?0:1;*A=J->Ap;A->Tdls=TRUE;A->Eht=FALSE;A->Qos=TRUE;
    A->Ht=(D->Ce.HtCap&1)!=0; /* legacy rates stay from the associated band */
    A->Vht=B&&D->Ce.VhtCap!=0;A->He=C&&WcnService(D,288)&&C->HeMcs[B]!=0;A->Nss=J->NssLimit?(UCHAR)J->NssLimit:D->Ce.NumRfChains;
    if(!A->Nss||A->Nss>2)A->Nss=1;A->VhtMcs=65535;for(I=0;I<A->Nss;I++)A->VhtMcs=(USHORT)(A->VhtMcs&~(1u<<(I*2)));
    if(C){A->HeMcs[0]=(USHORT)C->HeMcs[B];A->HeMcs[1]=(USHORT)(C->HeMcs[B]>>16);}
    if(Wide&&WcnService(D,342)){ULONG Center,Limit=160,W;while((W=Tgeometry(D,A->Frequency,Limit,&Center))!=0){if((Center==J->Ap.CenterFrequency&&W<=J->Ap.Width)||TnonDfs(D,Center,W)){A->Width=(UCHAR)W;A->CenterFrequency=Center;break;}if(W==20)break;Limit=W/2;}}
}
NTSTATUS WcnTdlsAwake(PWCN_DEVICE D,PWCN_JOIN J)
{
    TDLS *T=Ts(D);ULONG I=Ti(J);NTSTATUS St;if(!T||!T->Ps[I])return STATUS_SUCCESS;
    St=WcnStationPower(D,J,FALSE);if(NT_SUCCESS(St)){T->Ps[I]=FALSE;T->BusyAt[I]=WcnNowMs();}return St;
}
static BOOLEAN Tshared(PWCN_DEVICE D,PWCN_JOIN J)
{PWCN_JOIN Other=J->Vdev==0?&D->Secondary:&D->Join;return Other->Started&&!Other->Failed&&Other->MacId==J->MacId;}
static BOOLEAN Tchannel(PWCN_DEVICE D,TPEER *P,ULONG Width,ULONG Center)
{
    ULONG X,I;for(X=Center-Width/2+10;X<Center+Width/2;X+=20){for(I=0;I<D->Scan.ChannelCount;I++)if(D->Scan.Channel[I].Frequency==X)break;if(I>=64||I==D->Scan.ChannelCount||!(P->Channels&((ULONG64)1<<I)))return FALSE;}
    return TRUE;
}
static VOID Ttarget(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P)
{
    ULONG I,K,F,Width,Center,Best=0,Score,Limit;WCN_JOIN_AP A=P->L.Ap;P->OffFrequency=0;if(!P->OffCap||J->Ap.TdlsSwitchProhibited)return;
    for(I=0;I<D->Scan.ChannelCount;I++){F=D->Scan.Channel[I].Frequency;if(F==J->Ap.Frequency||WcnIs2g(F)!=WcnIs2g(J->Ap.Frequency))continue;
        for(Limit=P->L.Ap.Width;Limit>=20;Limit=Width/2){Width=Tgeometry(D,F,Limit,&Center);if(!Width)break;if(!Tchannel(D,P,Width,Center)||!TnonDfs(D,Center,Width))continue;A.Frequency=F;A.Width=(UCHAR)Width;A.CenterFrequency=Center;
        for(K=0;K<P->ClassCount;K++)if(P->Classes[K]==Tclass(&A))break;if(K==P->ClassCount)continue;
        Score=Width*16384+(F>J->Ap.Frequency?F-J->Ap.Frequency:J->Ap.Frequency-F);if(Center+Width/2<=J->Ap.CenterFrequency-J->Ap.Width/2||Center-Width/2>=J->Ap.CenterFrequency+J->Ap.Width/2)Score+=8192;
        if(Score>Best){Best=Score;P->OffFrequency=F;P->OffCenter=Center;P->OffWidth=Width;P->OffClass=Tclass(&A);}
        }
    }
}
static NTSTATUS Toff(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P,BOOLEAN Enable)
{
    ULONG B[18]={0},I,Power=127,Gain=127;NTSTATUS St;B[0]=Jtlv(351,36);B[1]=J->Vdev;B[2]=Enable?0u:2u;RtlCopyMemory(B+3,P->L.Ap.Bssid,6);B[5]=P->Initiator?1:0;
    if(Enable){B[6]=WcnFrequencyChannel(P->OffFrequency);B[7]=P->OffWidth/20==8?15:P->OffWidth/20==4?7:P->OffWidth==40?3:1;B[8]=P->OffClass;B[9]=P->OffFrequency;
        B[10]=Jtlv(18,28);B[11]=Jtlv(80,24);B[12]=B[13]=P->OffFrequency;B[15]=WcnIs2g(P->OffFrequency)?1:0;
        for(I=0;I<D->Scan.ChannelCount;I++)if(D->Scan.Channel[I].Frequency>=P->OffCenter-P->OffWidth/2+10&&D->Scan.Channel[I].Frequency<P->OffCenter+P->OffWidth/2){if(D->Scan.Channel[I].Power*2<Power)Power=D->Scan.Channel[I].Power*2;if(D->Scan.Channel[I].Gain*2<Gain)Gain=D->Scan.Channel[I].Gain*2;}
        B[16]=(Power<<8)|(Power<<16);B[17]=Gain|(Power<<8);
    }else B[10]=Jtlv(18,0);
    St=WcnWmiSend(D,0x20003,B,Enable?sizeof(B):44);if(NT_SUCCESS(St))P->OffRequested=Enable;return St;
}
NTSTATUS WcnTdlsPause(PWCN_DEVICE D,PWCN_JOIN J)
{
    TDLS *T=Ts(D);ULONG I;BOOLEAN Busy=FALSE;NTSTATUS St=WcnTdlsAwake(D,J);if(!NT_SUCCESS(St)||!T)return St;
    for(I=0;I<WCN_TDLS_PEERS;I++){TPEER *P=&T->Peer[I];if(P->L.Vdev!=J->Vdev||!P->OffFrequency||!P->State)continue;if(P->State==Tquarantine&&P->OffRequested)return STATUS_IO_TIMEOUT;if(P->State>=Tcool)continue;Tdrop(P,TRUE);Busy=TRUE;}
    return Busy?STATUS_DEVICE_BUSY:STATUS_SUCCESS;
}
static VOID TpowerService(PWCN_DEVICE D,PWCN_JOIN J)
{
    TDLS *T=Ts(D);ULONG I,K=Ti(J);BOOLEAN Peer=FALSE,Busy;LONGLONG Now=WcnNowMs();NTSTATUS St;
    if(!T->BusyAt[K]||T->Seen[K]!=J->TxFrames+J->RxFrames){T->Seen[K]=J->TxFrames+J->RxFrames;T->BusyAt[K]=Now;}Busy=Now-T->BusyAt[K]<3000;
    if(Teligible(D,J)&&T->Enabled[K]&&T->Options[K]==Toptions(D,J)&&WcnService(D,58)&&!Tshared(D,J)&&!D->Scan.RuntimeScan&&!J->MgmtBusy){
        for(I=0;I<WCN_TDLS_PEERS;I++){TPEER *P=&T->Peer[I];if(P->L.Vdev!=J->Vdev||!P->State||P->State>=Tcool)continue;if(P->State!=Tup||!P->BufferCap||!P->PsmCap||P->OffRequested){Peer=FALSE;break;}Peer=TRUE;}
    }
    if(!Peer||Busy){WcnTdlsAwake(D,J);return;}if(!T->Ps[K]){St=WcnTwtStop(D,J);if(!NT_SUCCESS(St))return;St=WcnStationPower(D,J,TRUE);if(NT_SUCCESS(St))T->Ps[K]=TRUE;}
}

static ULONG Tlink(PWCN_JOIN J,TPEER *P,UCHAR *Out)
{Out[0]=101;Out[1]=18;RtlCopyMemory(Out+2,P->L.Infrastructure,6);RtlCopyMemory(Out+8,P->Initiator?J->Mac:P->L.Ap.Bssid,6);RtlCopyMemory(Out+14,P->Initiator?P->L.Ap.Bssid:J->Mac,6);return 20;}
static ULONG Tcaps(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *Out)
{
    WCN_JOIN_AP A;ULONG N=0,I,K;static const UCHAR Classes[]={81,83,84,115,116,117,118,119,120,124,125,126,127,128,129};
    Tlocal(D,J,&A,TRUE);
    K=A.RateCount>8?8:A.RateCount;Out[N++]=1;Out[N++]=(UCHAR)K;RtlCopyMemory(Out+N,A.Rates,K);N+=K;
    if(A.RateCount>K){Out[N++]=50;Out[N++]=(UCHAR)(A.RateCount-K);RtlCopyMemory(Out+N,A.Rates+K,A.RateCount-K);N+=A.RateCount-K;}
    K=N;N+=WcnPhyBuildIes(D,&A,Out+N,FALSE);if(A.Qos&&WcnService(D,58))Out[K+8]=15;N+=WcnTdlsCapabilities(D,J,Out+N);
    Out[N++]=36;K=N++;for(I=0;I<D->Scan.ChannelCount;I++)if(!(D->Scan.Channel[I].Flags&(1u|2u|64u|(1u<<11)))){Out[N++]=(UCHAR)WcnFrequencyChannel(D->Scan.Channel[I].Frequency);Out[N++]=1;}Out[K]=(UCHAR)(N-K-1);
    Out[N++]=59;Out[N++]=1+sizeof(Classes);Out[N++]=(UCHAR)Tclass(&A);RtlCopyMemory(Out+N,Classes,sizeof(Classes));N+=sizeof(Classes);
    Out[N++]=197;Out[N++]=2;Jputle16(Out+N,J->Aid&0x3fff);return N+2;
}
static BOOLEAN Tphy(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P,const UCHAR *Ies,ULONG N,USHORT Capability)
{
    UCHAR B[1536];WCN_JOIN_AP A={0},Operation;WCN_TDLS_IES E;ULONG I,L,K=36,M;USHORT Aid=0;static const UCHAR Rates[]={2,4,11,22,12,18,24,36,48,72,96,108};
    if(N>1024||!WcnTdlsIes(Ies,N,&E))return FALSE;
    P->OffCap=WcnService(D,56)&&WcnTdlsBit(E.Ext,30);P->BufferCap=WcnTdlsBit(E.Ext,28);P->PsmCap=WcnTdlsBit(E.Ext,29);P->Qos=E.Wmm&&E.Wmm[6]==0?(E.Wmm[8]&15)|((E.Wmm[8]>>5&3)<<5):0;P->Country=D->Scan.CountryGeneration;P->Channels=0;
    if(E.Channels)for(I=2;I<E.Channels[1]+2u;I+=2){ULONG First=E.Channels[I],Count=E.Channels[I+1],X,Y,Step=First<=14?1:4;if(!First||!Count||First+(Count-1)*Step>196)return FALSE;
        for(X=0;X<Count;X++)for(Y=0;Y<D->Scan.ChannelCount&&Y<64;Y++)if(D->Scan.Channel[Y].Frequency==WcnChannelFrequency(First+X*Step))P->Channels|=(ULONG64)1<<Y;}
    Tlocal(D,J,&Operation,WcnService(D,342)&&WcnTdlsBit(E.Ext,61));RtlZeroMemory(B,36);B[0]=0x80;Operation.HeOps&=~((1u<<14)|(1u<<15)|(1u<<17));
    for(I=0;I+2<=N;I+=L){L=Ies[I+1]+2u;if(L>N-I)return FALSE;
        if(Ies[I]==1||Ies[I]==50){ULONG X,Y,Z;for(X=2;X<L;X++){UCHAR R=Ies[I+X]&127;for(Y=J->Ap.Frequency<2500?0:4;Y<sizeof(Rates);Y++)if(R==Rates[Y])break;if(Y==sizeof(Rates))continue;for(Z=0;Z<A.RateCount;Z++)if((A.Rates[Z]&127)==R)break;if(Z==A.RateCount){if(A.RateCount==16)return FALSE;A.Rates[A.RateCount++]=Ies[I+X];}}}
        if(Ies[I]==197&&L==4){Aid=Jle16(Ies+I+2)&0x3fff;if(Aid>2007)return FALSE;}
        if(Ies[I]==59&&L>=3){ULONG X,Y;static const UCHAR Allowed[]={81,83,84,115,116,117,118,119,120,124,125,126,127,128,129};P->ClassCount=0;for(X=2;X<L&&P->ClassCount<32;X++){for(Y=0;Y<sizeof(Allowed);Y++)if(Ies[I+X]==Allowed[Y])break;if(Y==sizeof(Allowed))continue;for(Y=0;Y<P->ClassCount;Y++)if(P->Classes[Y]==Ies[I+X])break;if(Y==P->ClassCount)P->Classes[P->ClassCount++]=Ies[I+X];}}
        if(Ies[I]==61||Ies[I]==192||(Ies[I]==255&&L>=3&&(Ies[I+2]==36||Ies[I+2]==106||Ies[I+2]==108||(!WcnService(D,288)&&Ies[I+2]==35))))continue;
        RtlCopyMemory(B+K,Ies+I,L);K+=L;
    }
    M=WcnPhyOperationIes(&Operation,B+K);K+=M;A.Frequency=J->Ap.Frequency;A.Power=J->Ap.Power;A.Gain=J->Ap.Gain;A.ChannelFlags=J->Ap.ChannelFlags;A.BeaconInterval=J->Ap.BeaconInterval;A.Capability=Capability;
    if(!A.RateCount||!WcnPhySelect(D,B,K,&A))return FALSE;
    A.Auth=P->Secure?WCN_AUTH_PSK:WCN_AUTH_OPEN;A.Tdls=TRUE;A.Pmf=FALSE;A.Valid=TRUE;A.Eht=FALSE;RtlCopyMemory(A.Bssid,P->L.Ap.Bssid,6);P->L.Ap=A;WcnPhyLimitNss(D,&P->L,J->NssLimit);P->Aid=Aid;Ttarget(D,J,P);return TRUE;
}
static BOOLEAN Tnonce(UCHAR *N)
{return NT_SUCCESS(BCryptGenRandom(NULL,N,32,BCRYPT_USE_SYSTEM_PREFERRED_RNG))&&WcnTdlsNonzero(N,32);}
static VOID Tdrop(TPEER *P,BOOLEAN Notify)
{if(P->State==Tfree||P->State==Tcool||P->State==Tquarantine||P->State==Tclean)return;P->Notify=Notify;P->State=Tclean;P->Step=0;P->L.Authorized=FALSE;P->L.Resetting=TRUE;P->At=WcnNowMs();P->Reply=0;}
static NTSTATUS Tsend(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P,UCHAR Action)
{
    UCHAR B[1024],*F=B+14,*Link,*Ft=NULL,*Life=NULL;ULONG N=3;NTSTATUS St;UCHAR Mic[16];
    if(J->KeyHold)return STATUS_DEVICE_BUSY;if(!J->Authorized||J->Failed||!J->VdevStarted)return STATUS_DEVICE_NOT_READY;St=WcnTdlsAwake(D,J);if(!NT_SUCCESS(St))return St;
    RtlCopyMemory(B,P->L.Ap.Bssid,6);RtlCopyMemory(B+6,J->Mac,6);B[12]=0x89;B[13]=0x0d;F[0]=2;F[1]=12;F[2]=Action;
    if(Action==3){Jputle16(F+N,25);N+=2;}else{if(Action==1||Action==2){Jputle16(F+N,0);N+=2;}F[N++]=P->Token;}
    if(Action==0||Action==1){Jputle16(F+N,(J->Ap.Capability&0x420u)|(P->Secure?16u:0u));N+=2;N+=Tcaps(D,J,F+N);}
    Link=F+N;N+=Tlink(J,P,F+N);
    if(P->Secure&&Action!=10){
        if(Action!=3){RtlCopyMemory(F+N,P->Rsn,P->RsnLength);N+=P->RsnLength;}
        Ft=F+N;RtlZeroMemory(Ft,84);Ft[0]=55;Ft[1]=82;RtlCopyMemory(Ft+20,P->Anonce,32);RtlCopyMemory(Ft+52,P->Snonce,32);N+=84;
        if(Action!=3){Life=F+N;Life[0]=56;Life[1]=5;Life[2]=2;Jputle32(Life+3,P->Life);N+=7;}
        if(Action==1||Action==2){St=WcnTdlsMic(J->PmfAlgorithm,P->Key,Action==1?2:3,Link,P->Rsn,P->RsnLength,Life,Ft,84,Mic);if(!NT_SUCCESS(St))return St;RtlCopyMemory(Ft+4,Mic,16);}
        if(Action==3){if(!P->KeyReady)return STATUS_INVALID_DEVICE_STATE;St=WcnTdlsTeardownMic(J->PmfAlgorithm,P->Key,25,P->Token,Link,Ft,84,Mic);if(!NT_SUCCESS(St))return St;RtlCopyMemory(Ft+4,Mic,16);}
    }
    St=WcnDataTx(D,B,N+14,FALSE,J->Vdev,J->MacId,J->Mac,J->Ap.Bssid,J->AstIndex,J->AstHash,FALSE);
    if(NT_SUCCESS(St)){J->TxFrames++;WcnStage7TxFlush(D);}RtlSecureZeroMemory(B,sizeof(B));RtlSecureZeroMemory(Mic,sizeof(Mic));return St;
}
static BOOLEAN Tmic(PWCN_JOIN J,TPEER *P,const WCN_TDLS_IES *E,UCHAR Sequence)
{UCHAR Mic[16];BOOLEAN Ok;if(!P->KeyReady||!E->Ft||!E->Rsn||!E->Life)return FALSE;Ok=NT_SUCCESS(WcnTdlsMic(J->PmfAlgorithm,P->Key,Sequence,E->Link,E->Rsn,E->RsnLength,E->Life,E->Ft,E->FtLength,Mic))&&Jeq(Mic,E->Ft+4,16);RtlSecureZeroMemory(Mic,sizeof(Mic));return Ok;}
static BOOLEAN Tderive(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P)
{TDLS *T=Ts(D);P->KeyReady=NT_SUCCESS(WcnTdlsDerive(T->Sha,T->Hmac,P->Snonce,P->Anonce,J->Mac,P->L.Ap.Bssid,P->L.Infrastructure,P->Key));return P->KeyReady;}

static VOID TdiscoveryReply(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR Peer[6],UCHAR Token)
{
    TDLS *T=Ts(D);UCHAR *B;ULONG N;if(T->ReplyLength)return;B=T->Reply;B[0]=4;B[1]=14;B[2]=Token;Jputle16(B+3,J->Ap.Capability&0x430u);N=5;N+=Tcaps(D,J,B+N);
    B[N]=101;B[N+1]=18;RtlCopyMemory(B+N+2,J->Ap.Bssid,6);RtlCopyMemory(B+N+8,Peer,6);RtlCopyMemory(B+N+14,J->Mac,6);N+=20;
    if(J->Ap.Auth!=WCN_AUTH_OPEN){N+=WcnTdlsRsn(B+N);B[N]=56;B[N+1]=5;B[N+2]=2;Jputle32(B+N+3,43200);N+=7;}
    T->ReplyLength=N;T->ReplyAt=WcnNowMs();T->ReplyVdev=(UCHAR)J->Vdev;RtlCopyMemory(T->ReplyMac,Peer,6);
}
BOOLEAN WcnTdlsTunnel(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N)
{
    TPEER *P;WCN_TDLS_IES E;const UCHAR *B,*Ies;ULONG L,Action,At,Life;UCHAR Token=0;USHORT Capability=0;
    if(N<14||Jbe16(F+12)!=0x890d)return FALSE;
    if(!Teligible(D,J)||N<18||N>1038||!Jeq(F,J->Mac,6)||(F[6]&1)||Jeq(F+6,J->Mac,6)||Jeq(F+6,J->Ap.Bssid,6))return TRUE;
    B=F+14;L=N-14;if(B[0]!=2||B[1]!=12)return TRUE;Action=B[2];
    if(Action==0){if(L<6)return TRUE;Token=B[3];Capability=Jle16(B+4);At=6;}
    else if(Action==1){if(L<8)return TRUE;Token=B[5];Capability=Jle16(B+6);At=8;}
    else if(Action==2){if(L<6)return TRUE;Token=B[5];At=6;}
    else if(Action==3){if(L<5)return TRUE;At=5;}
    else if(Action==10){if(L<4)return TRUE;Token=B[3];At=4;}else return TRUE;
    Ies=B+At;if(!WcnTdlsIes(Ies,L-At,&E)||!E.Link)return TRUE;
    P=Tfind(D,J,F+6);
    if(Action==10){if(Token&&WcnTdlsLink(E.Link,J->Ap.Bssid,F+6,J->Mac))TdiscoveryReply(D,J,F+6,Token);return TRUE;}
    if(Action==0){
        if(!Token||!WcnTdlsLink(E.Link,J->Ap.Bssid,F+6,J->Mac))return TRUE;
        if(J->Ap.Auth!=WCN_AUTH_OPEN){if(!WcnTdlsRsnValid(E.Rsn,E.RsnLength)||!E.Ft||!E.Life||!WcnTdlsNonzero(E.Ft+52,32)||WcnTdlsNonzero(E.Ft+20,32))return TRUE;Life=Jle32(E.Life+3);if(Life<300||Life>86400)return TRUE;}else{if(E.Rsn||E.Ft)return TRUE;Life=43200;}
        if(P&&P->State==Tquarantine)return TRUE;
        if(P&&P->State>=Tsetup&&P->State<=Tclean){
            if(P->State!=Tclean&&P->Secure&&P->Token==Token&&!P->Initiator&&Jeq(P->Snonce,E.Ft+52,32)){P->Reply=2;P->ReplyAt=WcnNowMs();return TRUE;}
            if(!P->RestartLength){RtlCopyMemory(P->Restart,F,N);P->RestartLength=N;}if(P->State!=Tclean)Tdrop(P,FALSE);return TRUE;
        }
        if(P&&P->Initiator&&(P->State==Trequest||P->State==Tresponse)&&WcnTdlsOrder(J->Mac,F+6,6)<0)return TRUE;
        if(P&&P->State==Tconfirm&&!P->Initiator&&P->Token==Token&&(!P->Secure||Jeq(P->Snonce,E.Ft+52,32))){P->State=Treply;return TRUE;}
        if(P){if(P->State!=Tcool&&P->State!=Tfree){Tclear(P);P->State=Tcool;P->At=WcnNowMs()-30001;}else if(P->State==Tcool&&WcnNowMs()-P->At<30000)return TRUE;}
        P=Tnew(D,J,F+6);if(!P)return TRUE;P->Initiator=FALSE;P->Token=Token;P->Life=Life;
        if(P->Secure){RtlCopyMemory(P->Snonce,E.Ft+52,32);if(!Tnonce(P->Anonce)||Jeq(P->Snonce,P->Anonce,32)||!Tderive(D,J,P)){Tclear(P);return TRUE;}RtlCopyMemory(P->Rsn,E.Rsn,E.RsnLength);P->RsnLength=E.RsnLength;}
        if(!Tphy(D,J,P,Ies,L-At,Capability)){Tclear(P);return TRUE;}P->State=Treply;return TRUE;
    }
    if(!P||P->State==Tcool||P->State==Tquarantine||!WcnTdlsLink(E.Link,P->L.Infrastructure,P->Initiator?J->Mac:F+6,P->Initiator?F+6:J->Mac))return TRUE;
    if(Action==1){
        if(!P->Initiator||Token!=P->Token)return TRUE;if(Jle16(B+3)){Tdrop(P,FALSE);return TRUE;}
        if(P->State!=Tresponse&&P->State!=TconfirmTx&&P->State!=Tsetup&&P->State!=Tup)return TRUE;
        if(P->Secure){
            if(!WcnTdlsRsnValid(E.Rsn,E.RsnLength)||!E.Ft||!E.Life||Jle32(E.Life+3)!=P->Life||!Jeq(E.Ft+52,P->Snonce,32)||!WcnTdlsNonzero(E.Ft+20,32)||Jeq(E.Ft+20,P->Snonce,32))return TRUE;
            if(P->State!=Tresponse){if(Jeq(P->Anonce,E.Ft+20,32)&&Tmic(J,P,&E,2)){P->Reply=3;P->ReplyAt=WcnNowMs();}return TRUE;}
            RtlCopyMemory(P->Anonce,E.Ft+20,32);if(!Tderive(D,J,P)||!Tmic(J,P,&E,2)){RtlSecureZeroMemory(P->Key,32);P->KeyReady=FALSE;if(++P->Errors>=8)Tdrop(P,FALSE);return TRUE;}
            RtlCopyMemory(P->Rsn,E.Rsn,E.RsnLength);P->RsnLength=E.RsnLength;
        }else{if(E.Rsn||E.Ft)return TRUE;if(P->State!=Tresponse){P->Reply=3;P->ReplyAt=WcnNowMs();return TRUE;}}
        if(!Tphy(D,J,P,Ies,L-At,Capability)){Tdrop(P,P->KeyReady);return TRUE;}P->State=TconfirmTx;P->Attempts=0;P->RetryAt=0;return TRUE;
    }
    if(Action==2){
        if(P->Initiator||Token!=P->Token||(P->State!=Tconfirm&&P->State!=Tsetup&&P->State!=Tup))return TRUE;
        if(Jle16(B+3)){Tdrop(P,FALSE);return TRUE;}
        if(P->Secure&&(!E.Ft||!E.Life||!E.Rsn||E.RsnLength!=P->RsnLength||!Jeq(E.Rsn,P->Rsn,P->RsnLength)||Jle32(E.Life+3)!=P->Life||!Jeq(E.Ft+20,P->Anonce,32)||!Jeq(E.Ft+52,P->Snonce,32)||!Tmic(J,P,&E,3)))return TRUE;
        if(!P->Secure&&(E.Rsn||E.Ft))return TRUE;if(P->State==Tconfirm){P->State=Tsetup;P->Step=0;P->At=WcnNowMs();}return TRUE;
    }
    if(Action==3){
        if(P->Secure){UCHAR Mic[16];BOOLEAN Ok=E.Ft&&P->KeyReady&&Jeq(E.Ft+20,P->Anonce,32)&&Jeq(E.Ft+52,P->Snonce,32)&&NT_SUCCESS(WcnTdlsTeardownMic(J->PmfAlgorithm,P->Key,Jle16(B+3),P->Token,E.Link,E.Ft,E.FtLength,Mic))&&Jeq(Mic,E.Ft+4,16);RtlSecureZeroMemory(Mic,16);if(!Ok)return TRUE;}
        Tdrop(P,FALSE);
    }return TRUE;
}
static VOID Tmanagement(PWCN_DEVICE D,const UCHAR *P,ULONG N)
{
    TDLS *T=Ts(D);const UCHAR *H,*F;ULONG L,Size,I,Offset;TPEER *Peer;PWCN_JOIN J;WCN_TDLS_IES E;USHORT Fc;
    H=Jfind(P,N,44,&L);if(!H||L<68||(Jle32(H+20)&0x39)||(Jle32(H+60)!=1&&Jle32(H+60)!=2))return;
    Size=Jle32(H+16);F=Jfind(P,N,17,&L);if(!F||Size>L||Size<26)return;Fc=Jle16(F);if((Fc&0x030f)||(Fc&0xfc)!=0xd0)return;
    for(I=0;I<WCN_TDLS_PEERS;I++){
        Peer=&T->Peer[I];if(!Peer->State||!Jeq(F+10,Peer->L.Ap.Bssid,6)||!Jeq(F+4,Peer->L.Mac,6)||!Jeq(F+16,Peer->L.Infrastructure,6))continue;
        J=WcnStation(D,Peer->L.Vdev);if(!Teligible(D,J))return;
        if(!(Fc&0x4000)&&F[24]==4&&F[25]==14&&Size>=29&&Peer->State==Tdiscovery&&Peer->Token==F[26]){
            if(!WcnTdlsIes(F+29,Size-29,&E)||!WcnTdlsLink(E.Link,J->Ap.Bssid,J->Mac,Peer->L.Ap.Bssid))return;
            if(Peer->Secure&&!WcnTdlsRsnValid(E.Rsn,E.RsnLength))return;
            if(E.Life&&Jle32(E.Life+3)>=300&&Jle32(E.Life+3)<Peer->Life)Peer->Life=Jle32(E.Life+3);
            if(Peer->Secure&&!Tnonce(Peer->Snonce)){Tdrop(Peer,FALSE);return;}Peer->State=Trequest;Peer->Attempts=0;Peer->RetryAt=0;Peer->At=WcnNowMs();return;
        }
        if(Peer->State==Tup){if(Peer->L.KeyHold)return;Offset=24;if(Fc&0x4000){if(!WcnPmfReceiveFrame(J->PmfAlgorithm,Peer->L.Igtk,&Peer->L.MgmtRxPn,F,Size,&Offset))return;}
            if(Size>=Offset+2&&F[Offset]==3)WcnAggregationReceivePeer(D,&Peer->L.Ap,Peer->L.Ba,F+Offset,Size-Offset);return;}
    }
}

static NTSTATUS TpeerState(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P,ULONG State)
{
    ULONG *B=P->L.Wire,I,N=26,Count=0;RtlZeroMemory(B,sizeof(P->L.Wire));B[0]=Jtlv(201,16);B[1]=J->Vdev;RtlCopyMemory(B+2,P->L.Ap.Bssid,6);B[4]=State;
    B[5]=Jtlv(203,76);if(State!=1){B[25]=Jtlv(18,0);return WcnWmiSend(D,0x20002,B,104);}B[6]=P->Qos;B[7]=P->BufferCap;B[8]=P->OffCap&&!J->Ap.TdlsSwitchProhibited;B[9]=B[10]=Tclass(&J->Ap);B[12]=P->ClassCount;RtlCopyMemory(B+13,P->Classes,P->ClassCount);B[21]=P->Initiator?1:0;
    for(I=0;I<D->Scan.ChannelCount&&I<64;I++){WCN_SCAN_CHANNEL *C=&D->Scan.Channel[I];ULONG *Ch=B+N;
        if(C->Frequency!=J->Ap.Frequency&&(!B[8]||!(P->Channels&((ULONG64)1<<I))||!WcnConnectFrequency(C->Frequency)||WcnIs2g(C->Frequency)!=WcnIs2g(J->Ap.Frequency)||!TnonDfs(D,C->Frequency,20)||(C->Flags&(1u|2u|8u|64u|(1u<<11)))))continue;
        Ch[0]=Jtlv(80,24);Ch[1]=Ch[2]=C->Frequency;Ch[4]=WcnIs2g(C->Frequency)?1:0;Ch[5]=((C->Power*2&127)<<8)|((C->Power*2&127)<<16);Ch[6]=(C->Gain*2&127)|((C->Power*2&127)<<8);N+=7;Count++;
    }
    B[11]=Count;B[25]=Jtlv(18,Count*28);return WcnWmiSend(D,0x20002,B,N*4);
}

static VOID TsetupService(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P)
{
    PWCN_JOIN L=&P->L;NTSTATUS St=STATUS_SUCCESS;UCHAR Rsc[6]={0};
    if(L->KeyUpdate.Slot)WcnStationKeysService(D,L);
    if(WcnNowMs()-P->At>6000){Tdrop(P,TRUE);return;}
    switch(P->Step){
    case 0:St=WcnStage7DpInit(D,L);if(NT_SUCCESS(St)&&!L->MgmtDma.Buffer)St=WcnDmaAllocAligned(D,1024,0xfff,&L->MgmtDma);break;
    case 1:{ULONG B[]={Jtlv(97,16),L->Vdev,0,0,2,Jtlv(18,0)};RtlCopyMemory(B+2,L->Ap.Bssid,6);St=WcnWmiSend(D,0x6001,B,sizeof(B));if(NT_SUCCESS(St))L->PeerCreated=TRUE;break;}
    case 2:if(!L->PeerMapped)return;St=WcnPeerParam(D,L->Vdev,L->Ap.Bssid,19,1u|((J->MacId+1)<<1));break;
    case 3:St=WcnStage7PeerQueues(D,L);if(St==STATUS_PENDING)return;break;
    case 4:St=TpeerState(D,J,P,0);break;
    case 5:St=WcnPhyPeer(D,&L->Ap,L->Ap.Bssid,L->Vdev,P->Aid,1,L->Wire);if(NT_SUCCESS(St))L->State=8;break;
    case 6:if(!L->AssocConfirmed)return;L->State=9;L->GroupInstalled=TRUE;if(P->Secure)St=WcnStage8Key(D,L,FALSE,0,P->Key+16,Rsc);else L->PairwiseInstalled=TRUE;break;
    case 7:if(!L->PairwiseInstalled)return;RtlSecureZeroMemory(P->Key+16,16);break;
    case 8:St=TpeerState(D,J,P,1);break;
    case 9:St=WcnPeerParam(D,L->Vdev,L->Ap.Bssid,3,1);if(NT_SUCCESS(St)){L->Authorized=TRUE;P->State=Tup;P->LiveAt=P->At=WcnNowMs();}return;
    default:Tdrop(P,TRUE);return;
    }
    if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St)){Tdrop(P,TRUE);return;}P->Step++;
}
static VOID Tfinish(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P)
{
    TDLS *T=Ts(D);ULONG N=P->RestartLength,Vdev=P->L.Vdev;UCHAR Mac[6];RtlCopyMemory(Mac,P->L.Ap.Bssid,6);
    if(N)RtlCopyMemory(T->Restart,P->Restart,N);Tclear(P);P->State=Tcool;P->At=WcnNowMs();P->L.Vdev=Vdev;RtlCopyMemory(P->L.Ap.Bssid,Mac,6);
    if(N&&Teligible(D,J)){P->State=Tfree;WcnTdlsTunnel(D,J,T->Restart,N);}RtlSecureZeroMemory(T->Restart,sizeof(T->Restart));
}
static VOID Tcleanup(PWCN_DEVICE D,PWCN_JOIN J,TPEER *P)
{
    NTSTATUS St=STATUS_SUCCESS;PWCN_JOIN L=&P->L;
    if(WcnNowMs()-P->At>8000){P->State=Tquarantine;RtlSecureZeroMemory(P->Key,sizeof(P->Key));RtlSecureZeroMemory(P->Snonce,sizeof(P->Snonce));RtlSecureZeroMemory(P->Anonce,sizeof(P->Anonce));P->RestartLength=0;RtlSecureZeroMemory(P->Restart,sizeof(P->Restart));WcnLog(D,"TDLS: peer cleanup unconfirmed; retaining its DMA, AP path remains enabled");return;}
    switch(P->Step){
    case 0:if(L->MgmtBusy)return;if(P->OffRequested){St=Toff(D,J,P,FALSE);if(!NT_SUCCESS(St))return;}if(P->Notify&&J&&J->Authorized&&!J->Failed&&(P->KeyReady||!P->Secure)){St=Tsend(D,J,P,3);if(St==STATUS_DEVICE_BUSY&&WcnNowMs()-P->At<500)return;}P->Notify=FALSE;St=STATUS_SUCCESS;break;
    case 1:if(L->PeerCreated)St=TpeerState(D,J,P,2);break;
    case 2:if(L->PeerCreated)St=WcnStage7TeardownCommand(D,L,1);break;
    case 3:if(L->PeerCreated&&(!L->PeerDeleteConfirmed||(L->PeerMapped&&!L->PeerUnmapped)))return;if(L->MgmtBusy)return;
        if(D->Runtime.ReoPending&&D->Runtime.ReoOwner==L->Vdev&&D->Runtime.ReoBase==L->ReoQueue.La){if(!D->Runtime.ReoDone||D->Runtime.ReoStatus)return;D->Runtime.ReoPending=D->Runtime.ReoDone=FALSE;}break;
    case 4:
        if(!L->PeerCreated)L->RuntimeReady=FALSE;
        if(L->RuntimeReady){St=WcnStage7ReoRelease(D,L);if(St==STATUS_PENDING||St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))return;}
        Tfinish(D,J,P);return;
    default:return;
    }
    if(St==STATUS_DEVICE_BUSY)return;if(NT_SUCCESS(St))P->Step++;
}
BOOLEAN WcnTdlsBusy(PWCN_DEVICE D,PWCN_JOIN J)
{TDLS *T=Ts(D);ULONG I;if(!T)return FALSE;for(I=0;I<WCN_TDLS_PEERS;I++)if(T->Peer[I].L.Vdev==J->Vdev&&T->Peer[I].State&&T->Peer[I].State<Tcool)return TRUE;return T->ReplyLength&&T->ReplyVdev==J->Vdev;}
PWCN_JOIN WcnTdlsPeer(PWCN_DEVICE D,ULONG Vdev,const UCHAR Peer[6])
{TDLS *T=Ts(D);ULONG I;if(!T)return NULL;for(I=0;I<WCN_TDLS_PEERS;I++)if(T->Peer[I].State&&T->Peer[I].L.Vdev==Vdev&&Jeq(T->Peer[I].L.Ap.Bssid,Peer,6))return &T->Peer[I].L;return NULL;}
NTSTATUS WcnTdlsStop(PWCN_DEVICE D,PWCN_JOIN J)
{
    TDLS *T=Ts(D);ULONG I;BOOLEAN Busy=FALSE;NTSTATUS St=WcnTdlsAwake(D,J);if(!NT_SUCCESS(St)||!T)return St;T->Enabled[Ti(J)]=FALSE;if(T->ReplyVdev==J->Vdev)T->ReplyLength=0;
    for(I=0;I<WCN_TDLS_PEERS;I++){TPEER *P=&T->Peer[I];if(P->L.Vdev!=J->Vdev)continue;
        P->RestartLength=0;if(P->State&&P->State<Tcool){Tdrop(P,TRUE);Busy=TRUE;}
        if(P->State==Tquarantine&&P->OffRequested)return STATUS_IO_TIMEOUT;
        if(P->State==Tquarantine&&P->L.MgmtBusy)Busy=TRUE;
    }if(Busy){KeSetEvent(&D->WorkEvent,IO_NO_INCREMENT,FALSE);return STATUS_DEVICE_BUSY;}return STATUS_SUCCESS;
}
static BOOLEAN Tlan(const UCHAR *F,ULONG N)
{
    ULONG Type;if(N<34||(F[0]&1))return FALSE;Type=Jbe16(F+12);
    if(Type==0x0800&&(F[14]>>4)==4){const UCHAR *P=F+30;return P[0]==10||(P[0]==172&&(P[1]&240)==16)||(P[0]==192&&P[1]==168)||(P[0]==169&&P[1]==254);}
    if(Type==0x86dd&&N>=54&&(F[14]>>4)==6)return (F[38]&254)==252||(F[38]==254&&(F[39]&192)==128);return FALSE;
}
NTSTATUS WcnTdlsTx(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N)
{
    TDLS *T=Ts(D);TPEER *P;if(!Teligible(D,J)||N<14||Jeq(F,J->Ap.Bssid,6))return STATUS_NOT_FOUND;P=Tfind(D,J,F);
    if(P&&P->State==Tup&&P->L.Authorized&&!P->L.Failed&&Jeq(P->L.Infrastructure,J->Ap.Bssid,6)&&P->L.Ap.Frequency==J->Ap.Frequency&&P->L.MacId==J->MacId){NTSTATUS St=WcnDataTx(D,F,N,FALSE,J->Vdev,J->MacId,J->Mac,P->L.Ap.Bssid,P->L.AstIndex,P->L.AstHash,TRUE);if(NT_SUCCESS(St))P->At=WcnNowMs();return St;}
    if((P&&P->State!=Tcool)||N<100||!Tlan(F,N)||D->Scan.RuntimeScan||!T->Enabled[Ti(J)]||(T->DiscoverAt&&WcnNowMs()-T->DiscoverAt<1000))return STATUS_NOT_FOUND;
    P=Tnew(D,J,F);if(P){P->Initiator=TRUE;P->Token=++T->Token;if(!P->Token)P->Token=++T->Token;P->State=Tdiscover;T->DiscoverAt=WcnNowMs();KeSetEvent(&D->WorkEvent,IO_NO_INCREMENT,FALSE);}return STATUS_NOT_FOUND;
}
BOOLEAN WcnTdlsRx(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *B,ULONG N,ULONG Pad,BOOLEAN Error,const ULONG *Desc)
{
    TDLS *T=Ts(D);ULONG I,Fc=Jle16(B+200),Meta=Error?Desc[4]:Desc[3],Mask=D->Ce.PeerMetadataVersion==1?0x3fff:0xffff;
    const UCHAR *F=B+392+Pad;if(!T)return FALSE;
    for(I=0;I<WCN_TDLS_PEERS;I++){TPEER *P=&T->Peer[I];PWCN_JOIN L=&P->L;
        if(!P->State||L->Vdev!=J->Vdev||!L->PeerMapped||(Meta&Mask)!=L->PeerId||!Jeq(B+210,L->Ap.Bssid,6)||!Jeq(L->Infrastructure,J->Ap.Bssid,6))continue;
        if(P->State!=Tup||!L->Authorized||L->Failed||!Teligible(D,J)||Error||(Fc&0x030c)!=8||!Jeq(F+6,L->Ap.Bssid,6))return TRUE;
        if(P->Secure){if(!(Fc&0x4000)||((Jle32(B+128)>>10)&7)||WcnStage7RxReplay(D,L,B,Desc,FALSE,Fc,F))return TRUE;}
        else if((Fc&0x4000)||((Jle32(B+128)>>10)&7)>1)return TRUE;
        L->RxFrames++;J->RxFrames++;P->At=WcnNowMs();WcnStage8DirectEthernet(D,J,F,N);return TRUE;
    }return FALSE;
}
VOID WcnTdlsService(PWCN_DEVICE D)
{
    TDLS *T=Ts(D);ULONG I;NTSTATUS St;LONGLONG Now=WcnNowMs();if(!T||D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed)return;
    for(I=0;I<2;I++){PWCN_JOIN J=I?&D->Secondary:&D->Join;if(Teligible(D,J)&&(!T->Enabled[I]||T->Options[I]!=Toptions(D,J))){ULONG B[17]={0};B[0]=Jtlv(200,64);B[1]=J->Vdev;B[2]=1;B[8]=Toptions(D,J);B[9]=2;B[10]=500;B[11]=15;B[12]=1000;B[13]=32;B[14]=5000;B[15]=32;B[16]=5000;St=WcnWmiSend(D,0x20001,B,sizeof(B));if(NT_SUCCESS(St)){T->Enabled[I]=TRUE;T->Options[I]=(UCHAR)B[8];}}}
    if(T->ReplyLength){PWCN_JOIN J=WcnStation(D,T->ReplyVdev);if(!Teligible(D,J)||Now-T->ReplyAt>2000)T->ReplyLength=0;
        else if(!D->Scan.RuntimeScan){St=WcnTwtStop(D,J);if(NT_SUCCESS(St))St=WcnStage7PublicAction(D,J,T->ReplyMac,T->Reply,T->ReplyLength);if(St!=STATUS_DEVICE_BUSY)T->ReplyLength=0;}}
    for(I=0;I<WCN_TDLS_PEERS;I++){
        TPEER *P=&T->Peer[I];PWCN_JOIN J=WcnStation(D,P->L.Vdev);if(!P->State||P->State==Tcool)continue;
        if(P->State==Tquarantine){if(P->L.PeerDeleteConfirmed&&(!P->L.PeerMapped||P->L.PeerUnmapped)&&!P->L.MgmtBusy){P->State=Tclean;P->Step=3;P->At=Now;}else continue;}
        if(P->L.MgmtBusy&&Now-P->L.MgmtAt>3000)P->L.Failed=TRUE;
        if((P->Country&&P->Country!=D->Scan.CountryGeneration)||P->L.Failed||!Teligible(D,J)||!Jeq(P->L.Infrastructure,J->Ap.Bssid,6)||P->L.MacId!=J->MacId)Tdrop(P,FALSE);
        if(P->State==Tclean){Tcleanup(D,J,P);continue;}
        if(P->State==Tsetup){TsetupService(D,J,P);continue;}
        if(P->State==Tup){
            if(Now-P->LiveAt>=(LONGLONG)P->Life*1000-30000||(!P->Buffering&&!T->Ps[Ti(J)]&&Now-P->At>120000)||
               (P->OffRequested&&(J->Ap.TdlsSwitchProhibited||Tshared(D,J)||D->Scan.RuntimeScan)))Tdrop(P,TRUE);
            else{
                WcnAggregationServicePeer(D,P->L.Vdev,&P->L.Ap,P->L.Ba,P->L.Ap.Bssid,P->L.ReoQueue.La,P->L.MgmtBusy,P->L.MgmtStatus);
                if(P->OffFrequency&&!P->OffTried&&!J->Ap.TdlsSwitchProhibited&&T->Enabled[Ti(J)]&&T->Options[Ti(J)]==Toptions(D,J)&&!D->Scan.RuntimeScan&&!J->MgmtBusy&&!P->L.MgmtBusy&&!Tshared(D,J)&&!T->Ps[Ti(J)]){
                    ULONG K;for(K=0;K<WCN_TDLS_PEERS;K++)if(T->Peer[K].OffRequested&&T->Peer[K].L.MacId==P->L.MacId)break;
                    if(K==WCN_TDLS_PEERS){St=Toff(D,J,P,TRUE);if(St!=STATUS_DEVICE_BUSY)P->OffTried=TRUE;}
                }
            }
        }
        if(P->Reply){if(Now-P->ReplyAt>2000)P->Reply=0;else if(!D->Scan.RuntimeScan){St=Tsend(D,J,P,P->Reply-1);if(St!=STATUS_DEVICE_BUSY)P->Reply=0;}}
        if(P->State==Tup)continue;
        if(Now-P->At>10000){Tdrop(P,FALSE);continue;}if(D->Scan.RuntimeScan)continue;
        if(P->State==Tdiscovery||P->State==Tresponse||P->State==Tconfirm){if(Now-P->RetryAt<700)continue;if(P->Attempts>=3){Tdrop(P,FALSE);continue;}P->State--;}
        if(P->State==Tdiscover||P->State==Trequest||P->State==Treply||P->State==TconfirmTx){
            St=WcnTwtStop(D,J);if(St==STATUS_DEVICE_BUSY)continue;if(NT_SUCCESS(St))St=Tsend(D,J,P,P->State==Tdiscover?10:P->State==Trequest?0:P->State==Treply?1:2);
            if(St==STATUS_DEVICE_BUSY)continue;if(!NT_SUCCESS(St)){Tdrop(P,FALSE);continue;}P->Attempts++;P->RetryAt=Now;
            if(P->State==TconfirmTx){P->State=Tsetup;P->Step=0;P->At=Now;}else P->State++;
        }
    }
    TpowerService(D,&D->Join);TpowerService(D,&D->Secondary);
}
VOID WcnTdlsWmi(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{
    TDLS *T=Ts(D);ULONG I,L;const UCHAR *F;if(!T)return;
    for(I=0;I<WCN_TDLS_PEERS;I++)if(T->Peer[I].State)WcnStage7PeerReceive(D,&T->Peer[I].L,Event,P,N);
    if(Event==0x7001&&Jtlvs(P,N))Tmanagement(D,P,N);
    if(Event==0x20001&&Jtlvs(P,N)){F=Jfind(P,N,202,&L);if(F&&L>=20){PWCN_JOIN J=WcnStation(D,Jle32(F+16));TPEER *Peer=J?Tfind(D,J,F):NULL;if(Peer){ULONG Status=Jle32(F+8),Reason=Jle32(F+12);if(Status==1)Tdrop(Peer,TRUE);else if(Status==2)Tdrop(Peer,FALSE);
        else if(Status==3&&(Reason==7||Reason==8)){Peer->Buffering=Reason==7;Peer->At=WcnNowMs();}}}}
}
VOID WcnTdlsHtt(PWCN_DEVICE D,const UCHAR *P,ULONG N)
{TDLS *T=Ts(D);ULONG I;if(T)for(I=0;I<WCN_TDLS_PEERS;I++)if(T->Peer[I].State)WcnStage7PeerHtt(D,&T->Peer[I].L,P,N);}
NTSTATUS WcnTdlsCreate(PWCN_DEVICE D)
{
    TDLS *T;NTSTATUS St;T=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*T),WCN_TAG);if(!T)return STATUS_INSUFFICIENT_RESOURCES;
    St=BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&T->Sha,BCRYPT_SHA256_ALGORITHM,NULL,0);
    if(NT_SUCCESS(St))St=BCryptOpenAlgorithmProvider((BCRYPT_ALG_HANDLE*)&T->Hmac,BCRYPT_SHA256_ALGORITHM,NULL,BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if(!NT_SUCCESS(St)){if(T->Sha)BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)T->Sha,0);RtlSecureZeroMemory(T,sizeof(*T));ExFreePoolWithTag(T,WCN_TAG);WcnLog(D,"TDLS: optional cryptographic providers unavailable");return STATUS_SUCCESS;}D->Tdls=T;return STATUS_SUCCESS;
}
VOID WcnTdlsPowerDown(PWCN_DEVICE D)
{
    TDLS *T=Ts(D);ULONG I;if(!T)return;for(I=0;I<WCN_TDLS_PEERS;I++){if(T->Peer[I].L.ReoQueue.Buffer)WdfObjectDelete(T->Peer[I].L.ReoQueue.Buffer);if(T->Peer[I].L.MgmtDma.Buffer)WdfObjectDelete(T->Peer[I].L.MgmtDma.Buffer);RtlSecureZeroMemory(&T->Peer[I],sizeof(T->Peer[I]));}T->Enabled[0]=T->Enabled[1]=T->Ps[0]=T->Ps[1]=FALSE;RtlZeroMemory(T->Options,sizeof(T->Options));RtlZeroMemory(T->Seen,sizeof(T->Seen));RtlZeroMemory(T->BusyAt,sizeof(T->BusyAt));T->ReplyLength=0;RtlSecureZeroMemory(T->Reply,sizeof(T->Reply));RtlSecureZeroMemory(T->Restart,sizeof(T->Restart));
}
VOID WcnTdlsDestroy(PWCN_DEVICE D)
{TDLS *T=Ts(D);if(!T)return;WcnTdlsPowerDown(D);if(T->Sha)BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)T->Sha,0);if(T->Hmac)BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)T->Hmac,0);RtlSecureZeroMemory(T,sizeof(*T));ExFreePoolWithTag(T,WCN_TAG);D->Tdls=NULL;}
