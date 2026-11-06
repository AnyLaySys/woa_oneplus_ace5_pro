/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Retain mode-specific firmware capabilities without diagnostic file copies.
 * Neither unknown tails nor generic chip features become advertised support. */
#include "wcn7861.h"
#include "stage7-util.h"
BOOLEAN WcnService(PWCN_DEVICE D,ULONG Service)
{return Service<512&&(D->Ce.Services[Service/32]&(1u<<(Service%32)))!=0;}
static VOID Cservices(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{
    ULONG L,T,I,Offset=0,Count;const UCHAR *V;
    /* Legacy service-ready packs FOUR bits per u32; service-available uses 32. */
    while(N>=4){L=Jle32(P)&65535;T=Jle32(P)>>16;if(L>N-4)return;V=P+4;
        if(Event==1&&T==16){
            Count=L/4;if(Count>32)Count=32;
            for(I=0;I<Count*4;I++)if(Jle32(V+(I/4)*4)&(1u<<(I%4)))D->Ce.Services[I/32]|=1u<<(I%32);
            return;
        }
        if(Event==3&&T==559&&L>=20){Offset=Jle32(V);if(Offset<128||Offset>384||(Offset&31))return;
            for(I=0;I<4;I++)D->Ce.Services[Offset/32+I]|=Jle32(V+4+I*4);Offset+=128;
        }else if(Event==3&&T==16&&Offset){Count=L/4;if(Count>(512-Offset)/32)Count=(512-Offset)/32;
            for(I=0;I<Count;I++)D->Ce.Services[Offset/32+I]|=Jle32(V+I*4);
        }
        P+=L+4;N-=L+4;
    }
}
const WCN_RADIO_CAP *WcnRadioCaps(PWCN_DEVICE D)
{
    ULONG I;const WCN_RADIO_CAP *Fallback=NULL;
    for(I=0;I<D->Ce.RadioCapCount;I++){
        const WCN_RADIO_CAP *C=&D->Ce.RadioCaps[I];if(C->Pdev!=1)continue;
        if(D->Ce.HwModeValidated&&C->Mode==D->Ce.HwModeIndex)return C;
        if(C->Mode==5)Fallback=C;else if(!Fallback)Fallback=C;
    }
    return Fallback;
}
ULONG WcnMaximumRate(PWCN_DEVICE D)
{
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);WCN_JOIN_AP A={0};ULONG B,I,M,R,Max=54000;
    if(!C||!D->Ce.NumRfChains||D->Ce.NumRfChains>2)return Max;
    A.He=TRUE;A.Nss=D->Ce.NumRfChains;
    for(B=0;B<2;B++){
        if(!(C->Bands&(1u<<B))||!C->HeMcs[B])continue;
        A.HeMcs[0]=(USHORT)C->HeMcs[B];A.HeMcs[1]=(USHORT)(C->HeMcs[B]>>16);
        A.Width=(UCHAR)(B?((C->WidthCode[1]>=3&&(C->HePhy[1][0]&8)&&(A.HeMcs[1]&3)!=3)?160:80):((C->HePhy[0][0]&2)?40:20));
        M=A.Width==160?1:0;A.Eht=C->EhtValid;
        for(I=0;I<3;I++){ULONG V=(C->EhtMcs[B][M+1]>>(I*8))&255,Count=(V&15)<(V>>4)?V&15:V>>4;if(Count>A.Nss)Count=A.Nss;A.EhtMcs[M][I]=(UCHAR)(Count|(Count<<4));}
        if(!A.EhtMcs[M][0])A.Eht=FALSE;R=WcnPhyAdvancedRate(&A);if(R>Max)Max=R;
    }
    return Max;
}
static WCN_RADIO_CAP *Cslot(PWCN_DEVICE D,const UCHAR *V,BOOLEAN Create)
{
    ULONG I,Mode=Jle32(V),Pdev=Jle32(V+4)&65535,Phy=Jle32(V+8);
    for(I=0;I<D->Ce.RadioCapCount;I++)if(D->Ce.RadioCaps[I].Mode==Mode&&D->Ce.RadioCaps[I].Pdev==Pdev&&D->Ce.RadioCaps[I].Phy==Phy)return &D->Ce.RadioCaps[I];
    if(!Create||I==WCN_MAX_RADIO_CAPS)return NULL;
    D->Ce.RadioCapCount++;D->Ce.RadioCaps[I].Mode=Mode;D->Ce.RadioCaps[I].Pdev=Pdev;D->Ce.RadioCaps[I].Phy=Phy;return &D->Ce.RadioCaps[I];
}
static VOID Cwalk(PWCN_DEVICE D,const UCHAR *P,ULONG N,ULONG Depth)
{
    ULONG H,L,T,I;const UCHAR *V;WCN_RADIO_CAP *C;
    if(Depth>3)return;
    while(N>=4){
        H=Jle32(P);L=H&65535;T=H>>16;if(L>N-4){WcnLog(D,"caps: truncated TLV tag=%u length=%u available=%u",T,L,N-4);return;}V=P+4;
        if(T==528&&L>=236){
            C=Cslot(D,V,TRUE);if(C){
                C->Bands=Jle32(V+16);C->WidthCode[0]=Jle32(V+24);C->WidthCode[1]=Jle32(V+56);C->NssRatio=Jle32(V+232);
                for(I=0;I<2;I++){
                    ULONG Base=I?72:40;C->HeMcs[I]=Jle32(V+Base+4);
                    RtlCopyMemory(C->HeMac[I],V+Base,4);RtlCopyMemory(C->HeMac[I]+4,V+200+I*4,2);
                    RtlCopyMemory(C->HePhy[I],V+88+I*12,11);RtlCopyMemory(C->HePpe[I],V+112+I*40,40);
                }
            }

        }else if(T==879&&L>=180){
            C=Cslot(D,V,FALSE);if(C){
                for(I=0;I<2;I++){RtlCopyMemory(C->EhtMac[I],V+16+I*8,2);RtlCopyMemory(C->EhtPhy[I],V+40+I*12,9);RtlCopyMemory(C->EhtPpe[I],V+64+I*40,40);}
                RtlCopyMemory(C->EhtMcs[0],V+148,8);RtlCopyMemory(C->EhtMcs[1],V+156,16);C->EhtValid=TRUE;
            }

        }else
        if(T==18)Cwalk(D,V,L,Depth+1);
        P+=L+4;N-=L+4;
    }
}
VOID WcnCapabilitiesEvent(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{
    if(Event==1||Event==3)Cservices(D,Event,P,N);
    if((Event==1||Event==0x4009||Event==0x4022)&&N&&N<=32768)Cwalk(D,P,N,0);
}
