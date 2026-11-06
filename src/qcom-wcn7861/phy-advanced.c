/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * HE/EHT single-link negotiation. IEEE maps and PPE fields are intersected
 * with mode-specific firmware data. MLO/concurrent ports are not advertised
 * until their independent key, peer and queue ownership paths exist. */
#include "wcn7861.h"
#include "stage7-wire.h"
#include "stage6-protocol.h"
#include "stage7-util.h"
#include "network-policy.h"
static USHORT Amap(USHORT Rx,USHORT Tx,USHORT Hw,ULONG Nss)
{
    ULONG I,R,T,H;USHORT Out=65535;
    for(I=0;I<Nss;I++){R=(Rx>>(I*2))&3;T=(Tx>>(I*2))&3;H=(Hw>>(I*2))&3;
        if(R==3||T==3||H==3)break;if(T<R)R=T;if(H<R)R=H;Out=(USHORT)((Out&~(3u<<(I*2)))|(R<<(I*2)));}
    return Out;
}
static ULONG Abits(const UCHAR *P,ULONG Bit,ULONG Count)
{ULONG I,V=0;for(I=0;I<Count;I++)V|=((P[(Bit+I)/8]>>((Bit+I)&7))&1u)<<I;return V;}
static VOID Aput(UCHAR *P,ULONG Bit,ULONG Count,ULONG Value)
{ULONG I;for(I=0;I<Count;I++)P[(Bit+I)/8]|=(UCHAR)(((Value>>I)&1u)<<((Bit+I)&7));}
static ULONG Appesize(const UCHAR *P,ULONG N,BOOLEAN Eht)
{
    ULONG Nbits=Eht?4:3,Rbits=Eht?5:4,Ss,Ru,I,K=0,Bits;
    if(N*8<Nbits+Rbits)return 0;Ss=Abits(P,0,Nbits)+1;Ru=Abits(P,Nbits,Rbits);
    if(!Ru)return 0;for(I=0;I<Rbits;I++)if(Ru&(1u<<I))K++;
    Bits=Nbits+Rbits+Ss*K*6;return (Bits+7)/8<=N?(Bits+7)/8:0;
}
static ULONG Appack(UCHAR *P,const ULONG *W,ULONG Nss,BOOLEAN Eht)
{
    ULONG Nbits=Eht?4:3,Rbits=Eht?5:4,I,R,Bit=Nbits+Rbits,Ss=W[0]+1,Ru=W[1]&((1u<<Rbits)-1);
    if(!Ss||Ss>8||!Ru)return 0;if(Ss>Nss)Ss=Nss;RtlZeroMemory(P,32);Aput(P,0,Nbits,Ss-1);Aput(P,Nbits,Rbits,Ru);
    for(I=0;I<Ss;I++)for(R=0;R<Rbits;R++)if(Ru&(1u<<R)){Aput(P,Bit,6,(W[I+2]>>(R*6))&63);Bit+=6;}
    return (Bit+7)/8;
}
static VOID Apunpack(ULONG *W,const UCHAR *P,ULONG N,BOOLEAN Eht,ULONG Nss)
{
    ULONG Nbits=Eht?4:3,Rbits=Eht?5:4,I,R,Bit=Nbits+Rbits,Ss,Ru;
    if(!Appesize(P,N,Eht))return;Ss=Abits(P,0,Nbits)+1;Ru=Abits(P,Nbits,Rbits);if(Ss>Nss)Ss=Nss;if(Ss>8)Ss=8;
    W[0]=Ss-1;W[1]=Ru;for(I=0;I<Ss;I++)for(R=0;R<Rbits;R++)if(Ru&(1u<<R)){W[I+2]|=Abits(P,Bit,6)<<(R*6);Bit+=6;}
}
static ULONG Ahelen(const UCHAR *P,ULONG N)
{
    ULONG L=21;if(N<L)return 0;if(P[6]&8)L+=4;if(P[6]&16)L+=4;if(N<L)return 0;
    if(P[12]&128){ULONG K=Appesize(P+L,N-L,FALSE);if(!K)return 0;L+=K;}return L==N?L:0;
}
static ULONG Aehtmaps(const UCHAR *He,const UCHAR *Eht)
{ULONG N=0;if(He[6]&2)return 3;if(He[6]&4)N+=3;if(He[6]&8)N+=3;if(Eht[2]&2)N+=3;return N?N:3;}
static UCHAR Acount(UCHAR Peer,UCHAR Own,ULONG Nss)
{
    ULONG Rx=Peer&15,Tx=Peer>>4,Hr=Own&15,Ht=Own>>4,M;
    if(!Rx||!Tx||!Hr||!Ht)return 0;M=Rx<Tx?Rx:Tx;if(Hr<M)M=Hr;if(Ht<M)M=Ht;if(Nss<M)M=Nss;return (UCHAR)(M|(M<<4));
}
static BOOLEAN Abasic(USHORT Map,USHORT Basic)
{
    ULONG I,R,M;if(!Basic)return TRUE; /* interoperable all-zero AP quirk, as mac80211 */
    for(I=0;I<8;I++){R=(Basic>>(I*2))&3;M=(Map>>(I*2))&3;if(R!=3&&(M==3||M<R))return FALSE;}return TRUE;
}
static BOOLEAN Apunct(ULONG Width,ULONG Bitmap,ULONG Primary)
{
    ULONG I;if(Bitmap&~((1u<<(Width/20))-1u))return FALSE;
    if(!Bitmap)return TRUE;if(Width<80||(Bitmap&(1u<<Primary)))return FALSE;
    if(!(Bitmap&(Bitmap-1)))return TRUE;
    if(Width==160)for(I=0;I<8;I+=2)if(Bitmap==(3u<<I))return TRUE;return FALSE;
}
BOOLEAN WcnPhyAdvancedSelect(PWCN_DEVICE D,const UCHAR *F,ULONG N,PWCN_JOIN_AP A)
{
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);const UCHAR *He=NULL,*Ho=NULL,*Eht=NULL,*Eo=NULL;
    ULONG I,L,Hn=0,En=0,Hon=0,Eon=0,B=WcnIs2g(A->Frequency)?0:1,M,MapBytes,Center,Width,Pos;
    if(!A->Ht||!C||!(C->HeMcs[B]&65535)||(D->Scan.PhyBitmap&32))return TRUE;
    for(I=36;I<N;I+=L+2){L=F[I+1];if(F[I]!=255||!L)continue;
        if(F[I+2]==35){if(He)return FALSE;He=F+I+3;Hn=L-1;}
        if(F[I+2]==36){if(Ho)return FALSE;Ho=F+I+3;Hon=L-1;}
        if(F[I+2]==108){if(Eht)return FALSE;Eht=F+I+3;En=L-1;}
        if(F[I+2]==106){if(Eo)return FALSE;Eo=F+I+3;Eon=L-1;}
    }
    if(!He||!Ho)return TRUE;
    if(Hn>sizeof(A->HeCap)||!Ahelen(He,Hn)||Hon<6)return FALSE;
    {ULONG Ops=Jle32(Ho)&0xffffff,Need=6+((Ops&0x4000)?3:0)+((Ops&0x8000)?1:0)+((Ops&0x20000)?5:0);if(Need!=Hon)return FALSE;}
    A->HeMcs[0]=Amap(Jle16(He+17),Jle16(He+19),(USHORT)C->HeMcs[B],A->Nss);
    if((A->HeMcs[0]&3)==3||!Abasic(A->HeMcs[0],Jle16(Ho+4)))return TRUE;
    A->HeMcs[1]=65535;
    if((He[6]&8)&&(C->HePhy[B][0]&8)&&B)A->HeMcs[1]=Amap(Jle16(He+21),Jle16(He+23),(USHORT)(C->HeMcs[B]>>16),A->Nss);
    if(A->Width==160&&((A->HeMcs[1]&3)==3||!Abasic(A->HeMcs[1],Jle16(Ho+4)))){A->CenterFrequency+=A->Frequency<A->CenterFrequency?-40:40;A->Width=80;}
    A->He=TRUE;A->HeLength=(UCHAR)Hn;RtlCopyMemory(A->HeCap,He,Hn);A->HeOps=Jle32(Ho);
    if(Eht&&Eo&&C->EhtValid&&!(D->Scan.PhyBitmap&64)){
        if(En<14||En>sizeof(A->EhtCap)||Eon<5)return FALSE;
        MapBytes=Aehtmaps(He,Eht);if(11+MapBytes>En)return FALSE;
        if(Eht[7]&8){ULONG Ppe=Appesize(Eht+11+MapBytes,En-11-MapBytes,TRUE);if(!Ppe||11+MapBytes+Ppe!=En)return FALSE;}
        else if(11+MapBytes!=En)return FALSE;
        for(M=0;M<2;M++)for(I=0;I<3;I++){
            Pos=11+M*3;
            A->EhtMcs[M][I]=(Pos+3<=11+MapBytes)?Acount(Eht[Pos+I],(UCHAR)(C->EhtMcs[B][M+1]>>(I*8)),A->Nss):0;
        }
        if(Eo[0]&1){
            ULONG Low,Primary,Bitmap=0,W,Punct=0;
            if(Eon<8)return FALSE;Width=20u<<(Eo[5]&7);if(Width>160||(!B&&Width>40))return FALSE;
            Center=WcnChannelFrequency(Width==160?Eo[7]:Eo[6]);if(Center<Width/2)return FALSE;Low=Center-Width/2+10;
            if(A->Frequency<Low||A->Frequency>=Low+Width||(A->Frequency-Low)%20)return FALSE;Primary=(A->Frequency-Low)/20;
            if(Eo[0]&2){if(Eon!=10)return FALSE;Bitmap=Jle16(Eo+8);}else if(Eon!=8)return FALSE;
            if(!Apunct(Width,Bitmap,Primary))return FALSE;
            for(W=Width;W>=20;W/=2){
                Pos=Primary/(W/20)*(W/20);Center=Low-10+Pos*20+W/2;Punct=(Bitmap>>Pos)&((1u<<(W/20))-1u);
                if(W==160&&((A->HeMcs[1]&3)==3||!Abasic(A->HeMcs[1],Jle16(Ho+4))||(A->EhtMcs[0][0]&&!A->EhtMcs[1][0])))continue;
                if(WcnWidthAllowed(D,Center,W)&&Apunct(W,Punct,Primary-Pos))break;
            }
            if(W<20)return FALSE;A->Width=(UCHAR)W;A->CenterFrequency=Center;A->Punctured=(USHORT)Punct;
        }else if(Eon!=5||(Eo[0]&2))return FALSE;
        M=A->Width==160?1:0;A->Eht=A->EhtMcs[M][0]!=0;
        for(I=0;I<4;I++){ULONG G=I<2?0:I-1,V=A->EhtMcs[M][G],R=Eo[I+1];if((R&15)>(V&15)||(R>>4)>(V>>4))A->Eht=FALSE;}
        if(!A->Eht&&A->Punctured)return FALSE; /* HE fallback must not transmit in disabled subchannels. */
        if(A->Eht){A->EhtLength=(UCHAR)En;RtlCopyMemory(A->EhtCap,Eht,En);}
    }
    M=A->Width==160?1:0;if(A->Nss==2&&((A->HeMcs[M]>>2)&3)==3)A->Nss=1;
    if(A->Width==160&&(C->NssRatio&1)){ULONG Ratio=(C->NssRatio>>1)&15;if(Ratio==0)A->Nss=(UCHAR)((A->Nss+1)/2);else if(Ratio==1)A->Nss=1;}
    if(A->Eht&&(A->EhtMcs[M][0]&15)<A->Nss)A->Nss=A->EhtMcs[M][0]&15;
    A->PhyMode=WcnPhyMode(A);
    return TRUE;
}
ULONG WcnPhyAdvancedRate(const WCN_JOIN_AP *A)
{
    static const ULONG Eff[]={3,6,9,12,18,24,27,30,36,40,45,50,54,60};
    ULONG I,M,Rate=0,Nd=A->Width==160?1960:A->Width==80?980:A->Width==40?468:234,Map=A->Width==160?1:0;
    for(I=0;I<A->Nss;I++){
        if(A->Eht){for(M=14;M;M--){ULONG G=(M-1)<=9?0:(M-1)<=11?1:2;if((A->EhtMcs[Map][G]&15u)>I)break;}if(!M)continue;M--;}
        else{M=(A->HeMcs[Map]>>(I*2))&3;if(M==3)break;M=7+M*2;}
        {ULONG K=(ULONG)((ULONG64)Nd*Eff[M]*(I+1)*10000/(6*136));if(K>Rate)Rate=K;}
    }
    return Rate;
}
ULONG WcnPhyAdvancedIes(PWCN_DEVICE D,PWCN_JOIN_AP A,UCHAR *Out)
{
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);ULONG B=WcnIs2g(A->Frequency)?0:1,N=0,K,I,Maps;UCHAR *P,*Phy;
    if(!A->He||!C)return 0;
    P=Out;P[0]=255;P[2]=35;RtlCopyMemory(P+3,C->HeMac[B],6);RtlCopyMemory(P+9,C->HePhy[B],11);Phy=P+9;
    P[3]&=(UCHAR)(1|(D->Ce.TwtEnable[0]==2&&D->Ce.TwtEnable[1]==2?2:0));P[4]&=12;P[5]&=~16;P[6]&=~96; /* requester TWT only after firmware ACK; no fragmentation claim */
    Phy[0]&=B?12:2;
    {ULONG Lo=Phy[5]&7,Hi=(Phy[5]>>3)&7;if(Lo>=A->Nss)Lo=A->Nss-1;if(Hi>=A->Nss)Hi=A->Nss-1;Phy[5]=(UCHAR)((Phy[5]&~63u)|Lo|(Hi<<3));}
    if(B&&(C->WidthCode[1]<3||(A->HeMcs[1]&3)==3))Phy[0]&=~8;
    K=20;Jputle16(P+K,Amap(A->HeMcs[0],A->HeMcs[0],65530,A->Nss));Jputle16(P+K+2,Amap(A->HeMcs[0],A->HeMcs[0],65530,A->Nss));K+=4;
    if(Phy[0]&8){Jputle16(P+K,Amap(A->HeMcs[1],A->HeMcs[1],65530,A->Nss));Jputle16(P+K+2,Amap(A->HeMcs[1],A->HeMcs[1],65530,A->Nss));K+=4;}
    if(Phy[6]&128){I=Appack(P+K,C->HePpe[B],A->Nss,FALSE);if(!I){Phy[6]&=~128;Phy[9]=(Phy[9]&63)|128;}else K+=I;}
    P[1]=(UCHAR)(K-2);N=K;
    if(A->Eht){
        P=Out+N;P[0]=255;P[2]=108;RtlCopyMemory(P+3,C->EhtMac[B],2);P[3]&=14;
        RtlCopyMemory(P+5,C->EhtPhy[B],9);Phy=P+5;Phy[0]&=~2;Phy[6]&=~0xf8;Phy[7]&=~0x78;
        Maps=B&&(C->HePhy[B][0]&8)&&C->WidthCode[1]>=3&&(A->HeMcs[1]&3)!=3?2:1;K=14;
        for(I=0;I<Maps;I++){ULONG J;for(J=0;J<3;J++)P[K++]=Acount(A->EhtMcs[I][J],A->EhtMcs[I][J],A->Nss);}
        if(Phy[5]&8){I=Appack(P+K,C->EhtPpe[B],A->Nss,TRUE);if(!I){Phy[5]&=~8;Phy[5]=(Phy[5]&~48u)|48u;}else K+=I;}
        P[1]=(UCHAR)(K-2);N+=K;
    }
    return N;
}
VOID WcnPhyAdvancedPeer(PWCN_DEVICE D,PWCN_JOIN_AP A,struct wmi_peer_assoc_complete_cmd *C)
{
    ULONG L;const UCHAR *P;UNREFERENCED_PARAMETER(D);
    if(!A->He)return;C->peer_he_mcs=A->Width==160?2:1;C->peer_flags|=0x400u;C->peer_he_cap_info=Jle32(A->HeCap);C->peer_he_cap_info_ext=Jle16(A->HeCap+4);
    RtlCopyMemory(C->peer_he_cap_phy,A->HeCap+6,11);C->peer_he_ops=A->HeOps&0xffffff;
    {ULONG Ext=(A->HeCap[3]>>3)&3;if(Ext)C->peer_max_mpdu=(1u<<((A->Vht?20:16)+Ext))-1u;}
    L=21+((A->HeCap[6]&8)?4:0)+((A->HeCap[6]&16)?4:0);
    if(A->HeCap[12]&128)Apunpack((ULONG*)&C->peer_ppet,A->HeCap+L,A->HeLength-L,FALSE,A->Nss);
    if(A->Eht){C->peer_flags_ext|=1;RtlCopyMemory(C->peer_eht_cap_mac,A->EhtCap,2);RtlCopyMemory(C->peer_eht_cap_phy,A->EhtCap+2,9);
        C->punct_bitmap=~(ULONG)A->Punctured;L=11+Aehtmaps(A->HeCap,A->EhtCap);P=A->EhtCap+L;
        if(A->EhtCap[7]&8)Apunpack((ULONG*)&C->peer_eht_ppet,P,A->EhtLength-L,TRUE,A->Nss);}
}
ULONG WcnPhyAdvancedRates(PWCN_DEVICE D,PWCN_JOIN_AP A,ULONG *Out)
{
    ULONG N=0,I,Count=A->Width==160?2:1,M;
    UNREFERENCED_PARAMETER(D);Out[N++]=Jtlv(18,A->He?Count*12:0);
    if(A->He)for(I=0;I<Count;I++){M=Amap(A->HeMcs[I],A->HeMcs[I],65530,A->Nss);Out[N++]=Jtlv(645,8);Out[N++]=M;Out[N++]=M;}
    Out[N++]=Jtlv(18,0); /* no MLO partner/key ownership advertised */
    Out[N++]=Jtlv(18,A->Eht?Count*12:0);
    if(A->Eht)for(I=0;I<Count;I++){
        ULONG J,Rx=0,Tx=0;Out[N++]=Jtlv(964,8);
        for(J=0;J<4;J++){ULONG G=J<2?0:J-1,V=A->EhtMcs[I][G],R=V&15,T=V>>4;if(R>A->Nss)R=A->Nss;if(T>A->Nss)T=A->Nss;Rx|=R<<(J*4);Tx|=T<<(J*4);}
        Out[N++]=Rx;Out[N++]=Tx;
    }
    Out[N++]=Jtlv(18,0);return N*4;
}
NTSTATUS WcnPhyBeamform(PWCN_DEVICE D,PWCN_JOIN_AP A,ULONG Vdev)
{
    ULONG Cap=D->Ce.VhtCap,Value=0,Nsts=(Cap>>13)&7,Dim=(Cap>>16)&7,Nss=A->Nss;
    if(!A->Ht)return STATUS_SUCCESS;if(Dim>=Nss)Dim=Nss-1;Value=(Nsts<<4)|(Dim<<8);
    if(Cap&(1u<<12)){Value|=1;if(Cap&(1u<<20))Value|=2;}if(Cap&(1u<<11))Value|=4;
    return WcnVdevParam(D,Vdev,45,Value);
}

