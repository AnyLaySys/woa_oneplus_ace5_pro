/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Radio geometry is an intersection of AP, firmware, board and CN rules.
 * 5GHz adjacent regulatory rules combine as ath12k/cfg80211 AUTO_BW does;
 * width flags and the firmware's band-wide limit remain mandatory. */
#include "wcn7861.h"
#include "stage7-wire.h"
#include "stage7-util.h"
#include "network-policy.h"
#include "security-policy.h"
BOOLEAN WcnWidthAllowed(PWCN_DEVICE D,ULONG Center,ULONG Width)
{
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);ULONG F,I,J,Limit=Center<2500?D->Scan.MaxBw2g:D->Scan.MaxBw5g;
    if(Width!=20&&Width!=40&&Width!=80&&Width!=160)return FALSE;
    if(Width>Limit||!C||Width>(20u<<(C->WidthCode[Center<2500?0:1]>3?3:C->WidthCode[Center<2500?0:1])))return FALSE;
    for(F=Center-Width/2+10;F<Center+Width/2;F+=20){
        if(!WcnConnectFrequency(F))return FALSE;
        for(I=0;I<D->Scan.ChannelCount;I++)if(D->Scan.Channel[I].Frequency==F&&!(D->Scan.Channel[I].Flags&(1u|2u|64u|(1u<<11))))break;
        if(I==D->Scan.ChannelCount)return FALSE;
        if((Width>=40&&(D->Scan.Channel[I].Flags&16))||(Width>=80&&(D->Scan.Channel[I].Flags&128))||(Width>=160&&(D->Scan.Channel[I].Flags&256)))return FALSE;
        for(J=0;J<D->Scan.RuleCount;J++)if(D->Scan.Rule[J].Start<=F-10&&D->Scan.Rule[J].End>=F+10)break;
        if(J==D->Scan.RuleCount)return FALSE;
        if(Center<2500&&D->Scan.Rule[J].MaxBw<Width)return FALSE;
    }
    return TRUE;
}
ULONG WcnPhyMode(const WCN_JOIN_AP *A)
{
    BOOLEAN B=!WcnIs2g(A->Frequency);
    if(A->Eht)return B?(A->Width==160?28:A->Width==80?26:A->Width==40?25:24):(A->Width==40?32:31);
    if(A->He)return B?(A->Width==160?20:A->Width==80?18:A->Width==40?17:16):(A->Width==40?22:21);
    if(A->Vht)return A->Width==160?15:A->Width==80?10:A->Width==40?9:8;
    return A->Ht?(B?(A->Width==40?6:4):(A->Width==40?7:5)):(B?0:1);
}
ULONG WcnPhyRate(PWCN_JOIN_AP A)
{
    static const ULONG H20[]={6500,13000,19500,26000,39000,52000,58500,65000};
    static const ULONG H40[]={13500,27000,40500,54000,81000,108000,121500,135000};
    static const ULONG V80[]={29250,58500,87750,117000,175500,234000,263250,292500,351000,390000};
    ULONG I,Rate=0,One,Max;
    if(A->He)return WcnPhyAdvancedRate(A);
    if(!A->Ht){for(I=0;I<A->RateCount;I++)if((A->Rates[I]&127)*500u>Rate)Rate=(A->Rates[I]&127)*500u;return Rate;}
    if(A->Vht){for(I=0;I<A->Nss;I++){
        Max=(A->VhtMcs>>(I*2))&3;if(Max==3)break;Max+=7;
        if(A->Width>=80)One=V80[Max]*(A->Width==160?2:1);
        else if(Max<=7)One=A->Width==40?H40[Max]:H20[Max];
        else if(Max==8)One=A->Width==40?162000:78000;else One=A->Width==40?180000:78000;
        if(One*(I+1)>Rate)Rate=One*(I+1);
    }}else for(I=0;I<A->HtRateCount;I++){Max=A->HtRates[I];One=(A->Width==40?H40[Max&7]:H20[Max&7])*((Max/8)+1);if(One>Rate)Rate=One;}
    return A->Sgi?(Rate*10+4)/9:Rate;
}
static VOID Pmaps(PWCN_JOIN_AP A)
{
    ULONG I,K=0;USHORT Map=65535;
    for(I=0;I<A->Nss*8u;I++)if(A->HtCap[3+I/8]&(1u<<(I&7)))A->HtRates[K++]=(UCHAR)I;
    A->HtRateCount=(UCHAR)K;
    if(A->Vht)for(I=0;I<A->Nss;I++){
        ULONG Rx=(Jle16(A->VhtCap+4)>>(I*2))&3,Tx=(Jle16(A->VhtCap+8)>>(I*2))&3,M=Rx<Tx?Rx:Tx;
        if(Rx==3||Tx==3)M=3;Map=(USHORT)((Map&~(3u<<(I*2)))|(M<<(I*2)));
    }
    A->VhtMcs=Map;A->RateKbps=WcnPhyRate(A);
}
BOOLEAN WcnPhySelect(PWCN_DEVICE D,const UCHAR *F,ULONG N,PWCN_JOIN_AP A)
{
    const UCHAR *Ht=NULL,*Op=NULL,*Vht=NULL,*Vo=NULL;ULONG I,L,Sec,Center,Wide=0;
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);
    static const UCHAR DefaultEdca[]={3,0xa4,0,0,0x27,0xa4,0,0,0x42,0x43,94,0,0x62,0x32,47,0};
    A->Width=20;A->Nss=1;A->CenterFrequency=A->Frequency;A->PhyMode=WcnIs2g(A->Frequency)?1:0;A->RateKbps=WcnPhyRate(A);
    if(WcnIs2g(A->Frequency)&&A->RateKbps<=11000)A->PhyMode=2;
    RtlCopyMemory(A->Edca,DefaultEdca,16);
    for(I=36;I<N;I+=L+2){
        if(N-I<2||(L=F[I+1])>N-I-2)return FALSE;
        if(F[I]==45){if(L!=26||Ht)return FALSE;Ht=F+I+2;}
        if(F[I]==61){if(L!=22||Op)return FALSE;Op=F+I+2;}
        if(F[I]==191){if(L!=12||Vht)return FALSE;Vht=F+I+2;}
        if(F[I]==192){if(L!=5||Vo)return FALSE;Vo=F+I+2;}
        if(F[I]==221&&L>=7&&Jeq(F+I+2,(const UCHAR*)"\x00\x50\xf2\x02",4)&&F[I+7]==1){
            A->Qos=TRUE;if(F[I+6]==1&&L==24){ULONG K,Mask=0;for(K=0;K<4;K++){
                const UCHAR *E=F+I+10+K*4;ULONG Ac=(E[0]>>5)&3;
                if((Mask&(1u<<Ac))||!(E[0]&15)||(E[1]&15)>(E[1]>>4))return FALSE;Mask|=1u<<Ac;RtlCopyMemory(A->Edca+Ac*4,E,4);
            }}
        }
    }
    if(!A->Qos||!Ht||!Op||!(D->Ce.HtCap&1))return TRUE;
    if(!Ht[3]||!D->Ce.NumRfChains||D->Ce.NumRfChains>2)return FALSE;
    A->Ht=TRUE;RtlCopyMemory(A->HtCap,Ht,26);A->Nss=D->Ce.NumRfChains;if(A->Nss==2&&!Ht[4])A->Nss=1;
    Sec=Op[1]&3;
    if((Jle16(Ht)&2)&&(Op[1]&4)&&(Sec==1||Sec==3)){
        Center=Sec==1?A->Frequency+10:A->Frequency-10;
        if(WcnWidthAllowed(D,Center,40)){A->Width=40;A->CenterFrequency=Center;}
    }
    A->Sgi=!!(Jle16(Ht)&(A->Width==40?64:32))&&!!(D->Ce.HtCap&(A->Width==40?0x800:2));
    A->PhyMode=WcnIs2g(A->Frequency)?(A->Width==40?7:5):(A->Width==40?6:4);
    if(!WcnIs2g(A->Frequency)&&Vht&&Vo&&D->Ce.VhtCap){
        ULONG Rx=Jle16(Vht+4),Tx=Jle16(Vht+8);
        if((Rx&3)!=3&&(Tx&3)!=3){
            A->Vht=TRUE;RtlCopyMemory(A->VhtCap,Vht,12);if(A->Nss==2&&(((Rx>>2)&3)==3||((Tx>>2)&3)==3))A->Nss=1;
            Center=WcnChannelFrequency(Vo[1]);
            if(Vo[0]==2){Wide=Center;Center=A->Frequency<Wide?Wide-40:Wide+40;}
            else if(Vo[0]==1&&Vo[2]&&(Vo[1]+8==Vo[2]||Vo[2]+8==Vo[1]))Wide=WcnChannelFrequency(Vo[2]);
            if(Vo[0]>=1&&Vo[0]<=3&&Vo[1]&&A->Frequency+30>=Center&&Center+30>=A->Frequency&&WcnWidthAllowed(D,Center,80)){A->Width=80;A->CenterFrequency=Center;}
            if(Wide&&C&&C->WidthCode[1]>=3&&(Jle32(Vht)&12)&&(D->Ce.VhtCap&12)&&A->Frequency+70>=Wide&&Wide+70>=A->Frequency&&WcnWidthAllowed(D,Wide,160)){A->Width=160;A->CenterFrequency=Wide;}
            A->PhyMode=A->Width==160?15:A->Width==80?10:A->Width==40?9:8;
            A->Sgi=A->Width>=80?!!((Jle32(Vht)&D->Ce.VhtCap)&(A->Width==160?64:32)):A->Sgi;
        }
    }
    if(!WcnPhyAdvancedSelect(D,F,N,A))return FALSE;
    for(I=A->CenterFrequency-A->Width/2+10;I<A->CenterFrequency+A->Width/2;I+=20){ULONG K;
        for(K=0;K<D->Scan.ChannelCount;K++)if(D->Scan.Channel[K].Frequency==I){
            if(D->Scan.Channel[K].Power<A->Power)A->Power=D->Scan.Channel[K].Power;if(D->Scan.Channel[K].Gain<A->Gain)A->Gain=D->Scan.Channel[K].Gain;
            if(D->Scan.Channel[K].Flags&8)A->Dfs=TRUE;break;
        }
    }
    Pmaps(A);return A->HtRateCount!=0&&A->RateKbps!=0;
}
VOID WcnPhyLimitNss(PWCN_DEVICE D,PWCN_JOIN J,ULONG Nss)
{PWCN_JOIN_AP A=&J->Ap;if(Nss)J->NssLimit=Nss<D->Ce.NumRfChains?Nss:D->Ce.NumRfChains;if(Nss&&Nss<A->Nss){A->Nss=(UCHAR)Nss;J->BeamformSet=FALSE;Pmaps(A);}}
ULONG WcnPhyScore(const WCN_JOIN_AP *A)
{LONG Signal=A->Rssi+90;if(Signal<1)Signal=1;if(Signal>40)Signal=40;return A->RateKbps*(ULONG)Signal/40;}
ULONG WcnPhyBuildIes(PWCN_DEVICE D,PWCN_JOIN_AP A,UCHAR *Out,BOOLEAN Ap)
{
    const WCN_RADIO_CAP *C=WcnRadioCaps(D);ULONG N=0,I,Cap;UCHAR *P;
    if(A->Qos){if(Ap){RtlCopyMemory(Out,"\xdd\x18\x00\x50\xf2\x02\x01\x01\x00\x00",10);RtlCopyMemory(Out+10,A->Edca,16);N=26;}else{RtlCopyMemory(Out,"\xdd\x07\x00\x50\xf2\x02\x00\x01\x00",9);N=9;}}if(!A->Ht)return N;
    P=Out+N;RtlZeroMemory(P,28);P[0]=45;P[1]=26;Cap=12;
    if(A->Width>=40)Cap|=2;if(WcnIs2g(A->Frequency))Cap|=0x1000;
    if((D->Ce.VhtCap&3)!=0&&(D->Ce.VhtCap&3)!=3)Cap|=0x800;
    if(D->Ce.HtCap&2)Cap|=32;if(D->Ce.HtCap&0x800)Cap|=64;if(D->Ce.HtCap&0x1000)Cap|=1;
    Jputle16(P+2,Cap);P[4]=3;for(I=0;I<A->Nss;I++)P[5+I]=255;P[17]=1;N+=28;
    if(A->Vht){P=Out+N;RtlZeroMemory(P,14);P[0]=191;P[1]=12;
        Cap=D->Ce.VhtCap&0x303ff8f3u;if((Cap&3)==3)Cap&=~3u;Cap&=~(1u<<19); /* STA is not a MU beamformer. */
        if(C&&C->WidthCode[1]>=3)Cap|=D->Ce.VhtCap&12;
        Cap&=~(7u<<16);Cap|=(ULONG)(A->Nss-1)<<16;if(A->Nss<2)Cap&=~128u;
        Cap|=D->Ce.VhtCap&(7u<<23);Jputle32(P+2,Cap);Jputle16(P+6,A->VhtMcs);Jputle16(P+10,A->VhtMcs);N+=14;
    }
    I=WcnPhyAdvancedIes(D,A,Out+N);if((Ap||A->Tdls)&&I)Out[N+3]&=~6u; /* AP responder scheduling is not enabled by the STA role. */
    return N+I;
}
NTSTATUS WcnPhyWmm(PWCN_DEVICE D,PWCN_JOIN_AP A,ULONG Vdev)
{
    ULONG B[31]={0},I;UCHAR *E;
    if(!A->Qos)return STATUS_SUCCESS;B[0]=Jtlv(0xc7,sizeof(B)-4);B[1]=Vdev;
    for(I=0;I<4;I++){ULONG *P=B+2+I*7;E=A->Edca+I*4;P[0]=Jtlv(0xc7,24);P[1]=(1u<<(E[1]&15))-1;P[2]=(1u<<(E[1]>>4))-1;P[3]=E[0]&15;P[4]=Jle16(E+2);P[5]=(E[0]>>4)&1;}
    return WcnWmiSend(D,0x500d,B,sizeof(B));
}

NTSTATUS WcnPhyPeer(PWCN_DEVICE Dev,PWCN_JOIN_AP A,const UCHAR Peer[6],ULONG Vdev,ULONG Aid,ULONG Listen,ULONG *Wire)
{
    struct wmi_peer_assoc_complete_cmd *C=(void*)Wire;
    struct ath12k_wmi_vht_rate_set_params *V;ULONG Offset,I,N=(A->RateCount+3)&~3u;
    RtlZeroMemory(Wire,512*sizeof(ULONG));JHDR(C,S7_WMI_TAG_PEER_ASSOC_COMPLETE_CMD);
    C->vdev_id=Vdev;RtlCopyMemory(C->peer_macaddr.addr,Peer,6);C->peer_new_assoc=1;C->peer_associd=Aid;
    C->peer_flags=(A->Auth==WCN_AUTH_OPEN||A->Tdls||A->FtMode==2?0:S7_WMI_PEER_NEED_PTK_4_WAY)|(A->Pmf?0x08000000u:0); /* GTK two-way flag is WPA1-only, not RSN. */
    C->peer_caps=A->Capability;C->peer_listen_intval=Listen;C->peer_nss=A->Nss;C->peer_phymode=A->PhyMode;C->num_peer_legacy_rates=A->RateCount;
    if(A->Qos)C->peer_flags|=2;
    if(A->Ht){
        ULONG Caps=Jle16(A->HtCap),Density=(A->HtCap[2]>>2)&7;
        C->peer_flags|=0x1000u|(A->Width>=40?0x2000u:0);C->peer_ht_caps=Caps;
        C->peer_max_mpdu=(1u<<(13+(A->HtCap[2]&3)))-1;
        C->peer_mpdu_density=Density<=3?!!Density:(1u<<(Density-3));
        C->peer_rate_caps=8u|(A->Width>=40?2u:0)|(A->Sgi?4u:0)|(A->Nss>1?1u:0);
        if(Caps&1)C->peer_flags|=0x10000u;
        if((Caps&12)==0)C->peer_flags|=0x40000u;else if((Caps&12)==4)C->peer_flags|=0x20000u;else C->peer_flags|=0x200000u;
        C->num_peer_ht_rates=A->HtRateCount;
    }
    if(A->Vht){ULONG Max=(1u<<(13+((Jle32(A->VhtCap)>>23)&7)))-1;C->peer_flags|=0x02000000u|(A->Width>=80?0x04000000u:0)|(A->Width==160?0x40000000u:0);C->peer_vht_caps=Jle32(A->VhtCap);if(Max>C->peer_max_mpdu)C->peer_max_mpdu=Max;}

    WcnPhyAdvancedPeer(Dev,A,C);
    if(A->Width==160)C->peer_bw_rxnss_override=0x80000000u|(A->Nss-1);
    Offset=sizeof(*C)/4;Wire[Offset++]=Jtlv(17,N);
    for(I=0;I<A->RateCount;I++)((UCHAR*)&Wire[Offset])[I]=A->Rates[I]&127;
    Offset+=N/4;N=(A->HtRateCount+3)&~3u;Wire[Offset++]=Jtlv(17,N);
    RtlCopyMemory(&Wire[Offset],A->HtRates,A->HtRateCount);Offset+=N/4;
    V=(void*)&Wire[Offset];JHDR(V,S7_WMI_TAG_VHT_RATE_SET);Offset+=sizeof(*V)/4;
    if(A->Vht){V->rx_max_rate=Jle16(A->VhtCap+6);V->tx_max_rate=Jle16(A->VhtCap+10);V->rx_mcs_set=A->VhtMcs;V->tx_mcs_set=A->VhtMcs|0x01000000u;V->tx_max_mcs_nss=255;}
    Offset+=WcnPhyAdvancedRates(Dev,A,Wire+Offset)/4;
    return WcnWmiSend(Dev,S7_WMI_PEER_ASSOC_CMDID,Wire,Offset*4);
}

/* AP operation IEs share the exact channel geometry negotiated by clients. */
ULONG WcnPhyOperationIes(PWCN_JOIN_AP A,UCHAR *Out)
{
    ULONG N=0;UCHAR *P;ULONG Channel=WcnFrequencyChannel(A->Frequency);
    if(A->Ht){P=Out+N;RtlZeroMemory(P,24);P[0]=61;P[1]=22;P[2]=(UCHAR)Channel;
        if(A->Width>=40)P[3]=(UCHAR)(4|(((A->Frequency-(A->CenterFrequency-A->Width/2+10))/20)&1?3:1));P[8]=1;N+=24;}
    if(A->Vht){P=Out+N;RtlZeroMemory(P,7);P[0]=192;P[1]=5;P[2]=A->Width>=80?1:0;
        if(A->Width>=80)P[3]=(UCHAR)WcnFrequencyChannel(A->Width==160?A->CenterFrequency+(A->Frequency<A->CenterFrequency?-40:40):A->CenterFrequency);
        if(A->Width==160)P[4]=(UCHAR)WcnFrequencyChannel(A->CenterFrequency);Jputle16(P+5,65532);N+=7;}
    if(A->He){P=Out+N;P[0]=255;P[1]=7;P[2]=36;Jputle32(P+3,A->HeOps);Jputle16(P+7,65532);N+=9;}
    if(A->Eht){P=Out+N;P[0]=255;P[1]=6;P[2]=106;P[3]=0;RtlFillMemory(P+4,4,0x11);N+=8;}
    return N;
}
