/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Associated-STA CSA/eCSA, retaining peer keys, replay counters and REO queues.
 * Only a validated same-band channel and already-negotiated width may switch.
 * Ambiguous geometry/shared-radio conflicts keep the safe reconnect path.
 * A STA follows its AP on DFS channels; it never becomes a DFS master. */
#include "wcn7861.h"
#include "stage7-wire.h"
#include "stage7-util.h"
#include "hotspot.h"
#include "network-policy.h"
static BOOLEAN Cgeometry(PWCN_DEVICE D,PWCN_JOIN J,ULONG Channel,ULONG Class,ULONG Sec,const UCHAR *Wide,PWCN_JOIN_AP A)
{
    ULONG I,F,Center,Width=J->Ap.Width;static const UCHAR Centers80[]={42,58,155};
    *A=J->Ap;A->Frequency=WcnChannelFrequency(Channel);A->Punctured=0;
    if(!A->Frequency||WcnIs2g(A->Frequency)!=WcnIs2g(J->Ap.Frequency)||J->Ap.Punctured)return FALSE;
    Center=A->Frequency;
    if(Class){
        if(Class==81||Class==115||Class==118||Class==124||Class==125)Width=20;
        else if(Class==83||Class==84||(Class>=116&&Class<=120)||Class==126||Class==127){Width=40;if(Sec==MAXULONG)Sec=(Class==84||Class==117||Class==120||Class==127)?3:1;}
        else if(Class==128)Width=80;else if(Class==129)Width=160;else return FALSE;
        if((Class<=84)!=WcnIs2g(A->Frequency))return FALSE;
        if((Class==81&&(Channel<1||Channel>13))||(Class==83&&(Channel<1||Channel>9||Sec!=1))||
           (Class==84&&(Channel<5||Channel>13||Sec!=3))||
           (Class>=115&&Class<=117&&(Channel<36||Channel>48||Channel%4))||
           (Class>=118&&Class<=120&&(Channel<52||Channel>64||Channel%4))||
           (Class==124&&(Channel<149||Channel>161||(Channel-149)%4))||
           (Class==125&&(Channel<149||Channel>165||(Channel-149)%4))||
           ((Class==116||Class==119)&&((Channel%8)!=4||Sec!=1))||
           ((Class==117||Class==120)&&((Channel%8)!=0||Sec!=3))||
           (Class==126&&((Channel!=149&&Channel!=157)||Sec!=1))||
           (Class==127&&((Channel!=153&&Channel!=161)||Sec!=3)))return FALSE;
    }
    if(Wide){
        if(Wide[0]==0)Width=Sec==1||Sec==3?40:20;
        else if(Wide[0]==1){Width=80;Center=WcnChannelFrequency(Wide[1]);
            if(Wide[2]){if(Wide[1]+8!=Wide[2]&&Wide[2]+8!=Wide[1])return FALSE;Width=160;Center=WcnChannelFrequency(Wide[2]);}}
        else if(Wide[0]==2&&!Wide[2]){Width=160;Center=WcnChannelFrequency(Wide[1]);}
        else return FALSE;
    }else if(Sec!=MAXULONG&&Width<=40)Width=Sec==1||Sec==3?40:20;
    if(Class){ULONG Expected=(Class==128?80:Class==129?160:Class==81||Class==115||Class==118||Class==124||Class==125?20:40);if(Width!=Expected)return FALSE;}
    if(Width>J->Ap.Width||(Sec!=MAXULONG&&Sec!=0&&Sec!=1&&Sec!=3))return FALSE;
    if(Width==40){
        if(Sec==MAXULONG)Sec=J->Ap.CenterFrequency>J->Ap.Frequency?1:3;
        if(Sec!=1&&Sec!=3)return FALSE;Center=Sec==1?A->Frequency+10:A->Frequency-10;
    }else if(Width>=80&&!Wide){
        if(Width==160)Center=5250;
        else{for(I=0;I<ARRAYSIZE(Centers80);I++){F=WcnChannelFrequency(Centers80[I]);if(A->Frequency+30>=F&&F+30>=A->Frequency){Center=F;break;}}
            if(I==ARRAYSIZE(Centers80))return FALSE;}
    }
    if(Width==20)Center=A->Frequency;
    if(!Center||A->Frequency<Center-Width/2+10||A->Frequency>Center+Width/2-10||
       (A->Frequency-(Center-Width/2+10))%20||!WcnWidthAllowed(D,Center,Width))return FALSE;
    if(Width>=80&&Sec!=MAXULONG&&Sec&&Sec!=((((A->Frequency-(Center-Width/2+10))/20)&1)?3u:1u))return FALSE;
    A->Power=A->Gain=MAXULONG;A->Dfs=FALSE;
    for(F=Center-Width/2+10;F<Center+Width/2;F+=20){
        for(I=0;I<D->Scan.ChannelCount;I++)if(D->Scan.Channel[I].Frequency==F){
            PWCN_SCAN_CHANNEL C=&D->Scan.Channel[I];if(C->Power<A->Power)A->Power=C->Power;if(C->Gain<A->Gain)A->Gain=C->Gain;
            if(F==A->Frequency)A->ChannelFlags=C->Flags;if(C->Flags&8)A->Dfs=TRUE;break;}
        if(I==D->Scan.ChannelCount)return FALSE;
    }
    A->Width=(UCHAR)Width;A->CenterFrequency=Center;
    if(A->Ht&&!A->He)A->Sgi=!!((Jle16(A->HtCap)&(Width==40?64:32))&&(D->Ce.HtCap&(Width==40?0x800:2)));
    A->PhyMode=WcnPhyMode(A);A->RateKbps=WcnPhyRate(A);return TRUE;
}
BOOLEAN WcnPhyChannelChanged(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N,ULONG Body)
{
    ULONG I,L,Kind,Start,Channel=0,Count=0,Mode=0,Sec=MAXULONG,Class=0;const UCHAR *Wide=NULL;BOOLEAN Found=FALSE;
    WCN_JOIN_AP Next;LONGLONG At;
    if(!F||N<24)return FALSE;Kind=F[0]&0xfc;
    if(!J->Authorized)return FALSE;
    if(Kind==0x80||Kind==0x50){if(N<36)return FALSE;Start=36;}
    else if(Kind==0xd0){
        if(Body>N||N-Body<2)return FALSE;
        if(F[Body]==4&&F[Body+1]==4){if(N-Body<6)return FALSE;Mode=F[Body+2];Class=F[Body+3];if(!Class)return TRUE;Channel=F[Body+4];Count=F[Body+5];Found=TRUE;Start=Body+6;}
        else if(F[Body]==0&&F[Body+1]==4)Start=Body+2;else return FALSE;
    }else return FALSE;
    for(I=Start;I<N;I+=L+2){
        if(N-I<2||(L=F[I+1])>N-I-2)return FALSE;
        if(F[I]==37||F[I]==60){ULONG M,C,K;
            if(L!=(F[I]==37?3u:4u))return FALSE;M=F[I+2];C=F[I+(F[I]==37?3:4)];K=F[I+(F[I]==37?4:5)];
            if(Found&&(C!=Channel||K!=Count||M!=Mode))return TRUE;
            if(F[I]==60){if(!F[I+3])return TRUE;Class=F[I+3];}Mode=M;Channel=C;Count=K;Found=TRUE;
        }else if(F[I]==62){if(L!=1||Sec!=MAXULONG)return FALSE;Sec=F[I+2];}
        else if(F[I]==194){if(L!=3||Wide)return FALSE;Wide=F+I+2;}
        else if(F[I]==196){ /* Channel Switch Wrapper: bounded nested IE stream. */
            ULONG K,Q;for(K=I+2;K<I+2+L;K+=Q+2){if(I+2+L-K<2||(Q=F[K+1])>I+L-K)return FALSE;
                if(F[K]==194){if(Q!=3||Wide)return FALSE;Wide=F+K+2;}}
        }
    }
    if(!Found){
        if((Kind==0x80||Kind==0x50)&&!J->CsaState){ULONG P=WcnPrimaryFrequency(F,N,J->Ap.Frequency);if(P&&P!=J->Ap.Frequency)return TRUE;}
        if((Kind==0x80||Kind==0x50)&&!J->CsaState&&J->Ap.Width==160){for(I=36;I+2<=N;I+=F[I+1]+2)if(F[I]==192&&F[I+1]==5){BOOLEAN Full=F[I+2]==2||(F[I+2]==1&&F[I+4]&&(F[I+3]+8==F[I+4]||F[I+4]+8==F[I+3]));if(!Full)return TRUE;}}
        return FALSE;
    }
    if(Mode>1||!Channel)return TRUE;
    /* Only the CN global operating classes in our exported channel table. */
    if(Class&&Class!=81&&Class!=83&&Class!=84&&(Class<115||Class>129))return TRUE;
    if(!Cgeometry(D,J,Channel,Class,Sec,Wide,&Next))return TRUE;
    if(Next.Frequency==J->Ap.Frequency&&Next.Width==J->Ap.Width&&Next.CenterFrequency==J->Ap.CenterFrequency)return FALSE;
    {PWCN_JOIN Other=J->Vdev==0?&D->Secondary:&D->Join;
    if(WcnHotspotActive(D)||(Other->Started&&!Other->Failed&&Other->MacId==J->MacId))return TRUE;}
    if(J->CsaState>=2)return Next.Frequency!=J->NextChannel.Frequency||Next.CenterFrequency!=J->NextChannel.CenterFrequency||Next.Width!=J->NextChannel.Width;
    At=WcnNowMs()+((LONGLONG)Count*J->Ap.BeaconInterval*1024)/1000;
    if(J->CsaState&&(Next.Frequency!=J->NextChannel.Frequency||Next.CenterFrequency!=J->NextChannel.CenterFrequency||Next.Width!=J->NextChannel.Width))return TRUE;
    if(!J->CsaState||At<J->CsaAt)J->CsaAt=At;
    J->NextChannel=Next;J->CsaBlock=J->CsaBlock||Mode==1;J->CsaState=1;J->CsaDeadline=J->CsaAt+5000;
    return FALSE;
}
BOOLEAN WcnChannelEvent(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N)
{
    ULONG L,Nss;const UCHAR *F=Jfind(P,N,40,&L);
    if(!F||L<36||Jle32(F)!=J->Vdev||J->CsaState!=3)return FALSE;
    if(Jle32(F+4)||Jle32(F+8)!=1)return TRUE;
    Nss=Jle32(F+28)<Jle32(F+32)?Jle32(F+28):Jle32(F+32);
    if(Jle32(F+12)||Nss<J->Ap.Nss||Jle32(F+24)>1||
       ((J->Vdev==0?D->Secondary.Started:D->Join.Started)&&Jle32(F+24)!=J->MacId)){WcnStage7Fail(D,J,"CSA VDEV_RESTART rejected geometry or NSS");return TRUE;}
    J->MacId=Jle32(F+24);J->CsaAck=TRUE;return TRUE;
}
BOOLEAN WcnChannelService(PWCN_DEVICE D,PWCN_JOIN J)
{
    ULONG I;NTSTATUS St;LONGLONG Now=WcnNowMs();
    if(!J->CsaState)return FALSE;
    if(Now>J->CsaDeadline){WcnStage7Fail(D,J,"CSA completion deadline");return TRUE;}
    if(J->CsaState==1){
        if(Now<J->CsaAt)return FALSE;
        if(D->Scan.RuntimeScan)return TRUE;
        St=WcnTdlsStop(D,J);if(St==STATUS_DEVICE_BUSY)return TRUE;if(!NT_SUCCESS(St))goto Fail;
        St=WcnTwtStop(D,J);if(St==STATUS_DEVICE_BUSY)return TRUE;if(!NT_SUCCESS(St))goto Fail;
        if(J->MgmtBusy)return TRUE;for(I=0;I<16;I++)if(J->Ba[I].Phase)return FALSE;
        if(D->Runtime.ReoPending&&D->Runtime.ReoOwner==J->Vdev)return FALSE;
        J->CsaState=2;J->CsaStep=0;J->CsaAck=FALSE;return TRUE;
    }
    if(J->CsaState==2){
        for(I=0;I<WCN_TX_SLOTS;I++)if(D->Runtime.TxUsed[I]&&WcnOwnerVdev(D->Runtime.TxUsed[I])==J->Vdev)return TRUE;
        St=WcnStage7ChannelCommand(D,J,FALSE);if(NT_SUCCESS(St))J->CsaState=3;
    }else if(J->CsaState==3){
        if(!J->CsaAck)return TRUE;
        if(J->CsaStep<2){
            St=WcnPeerParam(D,J->Vdev,J->Ap.Bssid,J->CsaStep?13:4,J->CsaStep?J->NextChannel.PhyMode:(J->NextChannel.Width==160?3:J->NextChannel.Width==80?2:J->NextChannel.Width==40?1:0));if(NT_SUCCESS(St))J->CsaStep++;
        }else{St=WcnStage7ChannelCommand(D,J,TRUE);if(NT_SUCCESS(St)){J->Ap=J->NextChannel;J->CsaState=0;J->CsaBlock=FALSE;WcnStage8ChannelChanged(D,J);}}
    }else{St=STATUS_INVALID_DEVICE_STATE;goto Fail;}
    if(St==STATUS_DEVICE_BUSY)return TRUE;
    if(!NT_SUCCESS(St))goto Fail;return TRUE;
Fail:WcnStage7Fail(D,J,"CSA command transport or TWT teardown");return TRUE;
}
