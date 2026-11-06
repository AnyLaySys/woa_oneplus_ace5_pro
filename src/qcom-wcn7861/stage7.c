/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Firmware station MLME and CCMP key installation. Windows owns security
 * handshakes and the IP stack; network choice comes from its connect task. */
#include "wcn7861.h"
#include "stage6-protocol.h"
#include "stage7-wire.h"
#include "stage7-util.h"
#include "stage7-rx-protocol.h"
#include "network-policy.h"
#include "security-policy.h"
#include "tdls-wire.h"
#include "hotspot.h"

VOID WcnStage7Fail(PWCN_DEVICE Dev,PWCN_JOIN J,PCSTR Reason)
{

    if(J->Failed)return;
    J->Failed=TRUE;
    WcnLog(Dev,"stage7: FAIL state=%u reason=%s auth=%u assoc=%u PTK=%u GTK=%u authorized=%u tx=%u ack=%u rx=%u dropped=%u runtime-errors=%u",
        J->State,Reason,J->Authenticated,J->Associated,J->PairwiseInstalled,J->GroupInstalled,J->Authorized,Dev->Runtime.TxCompleted,Dev->Runtime.TxAcked,J->RxFrames,Dev->Runtime.RxDropped,Dev->Runtime.RuntimeErrors);

}
BOOLEAN WcnJoinCandidate(PWCN_DEVICE Dev,const UCHAR *F,ULONG N,ULONG Frequency,LONG Rssi,const UCHAR *Name,ULONG NameLength,ULONG Auth,BOOLEAN Pmf,UCHAR Akm,BOOLEAN Hidden,PWCN_JOIN_AP Out)
{
    WCN_JOIN_AP A={0};const UCHAR *Ssid=NULL,*Rsn=NULL;ULONG I,K,L,SsidLength=0,Rate,Index;BOOLEAN Wpa=FALSE,NeedHt=FALSE,NeedVht=FALSE,NeedHe=FALSE,NeedEht=FALSE;
    static const UCHAR Rates[]={2,4,11,22,12,18,24,36,48,72,96,108};
    if(!NameLength||NameLength>32||N<36||Rssi>0||Rssi< -127||((F[0]&0xfc)!=0x80&&(F[0]&0xfc)!=0x50))return FALSE;
    A.Frequency=WcnPrimaryFrequency(F,N,Frequency);
    if(!WcnConnectFrequency(A.Frequency))return FALSE;
    if(!WcnIs2g(A.Frequency)&&WcnIs2g(Frequency))return FALSE;
    for(I=0;I<Dev->Scan.ChannelCount;I++)if(Dev->Scan.Channel[I].Frequency==A.Frequency)break;
    if(I==Dev->Scan.ChannelCount)return FALSE;
    A.ChannelFlags=Dev->Scan.Channel[I].Flags;A.Power=Dev->Scan.Channel[I].Power;A.Gain=Dev->Scan.Channel[I].Gain;
    if(A.ChannelFlags&(1u|2u|64u|(1u<<11)))return FALSE;
    for(I=36;I<N;I+=L+2){
        if(N-I<2)return FALSE;L=F[I+1];if(L>N-I-2)return FALSE;
        if(F[I]==0){if(Ssid||L>32)return FALSE;Ssid=F+I+2;SsidLength=L;}
        if(F[I]==221&&L>=4&&Jeq(F+I+2,(const UCHAR*)"\x00\x50\xf2\x01",4))Wpa=TRUE;
        if(F[I]==127&&L>=5){A.TdlsProhibited=(F[I+6]&64)!=0;A.TdlsSwitchProhibited=(F[I+6]&128)!=0;}
        if(F[I]==48){if(Rsn)return FALSE;Rsn=F+I;}
        if(F[I]==1||F[I]==50){
            if(!L||L>16)return FALSE;
            for(K=0;K<L;K++){
                Rate=F[I+2+K]&127;
                if(Rate==123&&(F[I+2+K]&128)&&Auth==WCN_AUTH_SAE){A.H2eOnly=TRUE;continue;}
                if(F[I+2+K]&128){if(Rate==127){NeedHt=TRUE;continue;}if(Rate==126){NeedVht=TRUE;continue;}if(Rate==122){NeedHe=TRUE;continue;}if(Rate==121){NeedEht=TRUE;continue;}}
                for(Index=WcnIs2g(A.Frequency)?0:4;Index<sizeof(Rates);Index++)if(Rate==Rates[Index])break;
                if(Index==sizeof(Rates)){if(F[I+2+K]&128)return FALSE;continue;}
                for(Index=0;Index<A.RateCount;Index++)if((ULONG)(A.Rates[Index]&127)==Rate)break;
                if(Index<A.RateCount)A.Rates[Index]|=F[I+2+K]&128;
                else if(A.RateCount<sizeof(A.Rates))A.Rates[A.RateCount++]=F[I+2+K];else return FALSE;
            }
        }
    }
    if(!Ssid||!((SsidLength==NameLength&&Jeq(Ssid,Name,NameLength))||(Hidden&&WcnSsidHidden(Ssid,SsidLength)))||!A.RateCount||!WcnAuthSupported(Auth))return FALSE;
    if(Auth==WCN_AUTH_OPEN){if(Rsn||Wpa||(Jle16(F+34)&16))return FALSE;}
    else{A.Pmf=Pmf;if(!Rsn||!WcnRsnAcceptAkm(Rsn+2,Rsn[1],Auth,Akm,&A.Pmf))return FALSE;}
    A.Capability=Jle16(F+34);A.BeaconInterval=Jle16(F+32);
    if(!(A.Capability&1)||!A.BeaconInterval||(F[16]&1))return FALSE;
    if(!WcnPhySelect(Dev,F,N,&A))return FALSE;
    if((NeedHt&&!A.Ht)||(NeedVht&&!A.Vht)||(NeedHe&&!A.He)||(NeedEht&&!A.Eht))return FALSE;
    RtlCopyMemory(A.Bssid,F+16,6);A.Rssi=Rssi;A.Valid=TRUE;A.Auth=Auth;A.Akm=Akm;*Out=A;return TRUE;
}

static NTSTATUS Jstart(PWCN_DEVICE Dev,PWCN_JOIN J,BOOLEAN Restart)
{
    const WCN_JOIN_AP *A=Restart?&J->NextChannel:&J->Ap;
    struct wmi_vdev_start_request_cmd *C=(void*)J->Wire;
    struct ath12k_wmi_channel_params *Ch;ULONG *End,Power,Gain;
    RtlZeroMemory(J->Wire,sizeof(J->Wire));JHDR(C,S7_WMI_TAG_VDEV_START_REQUEST_CMD);
    /* ath12k's zero-filled command leaves requestor_id at zero. This
     * PEACH image returns zero even for our former requestor=7. Match the
     * reference command instead of inventing a round-tripped transaction ID. */
    C->flags=A->Pmf?2:0;C->vdev_id=J->Vdev;C->requestor_id=0;C->beacon_interval=A->BeaconInterval;C->dtim_period=1;
    C->preferred_tx_streams=A->Nss;C->preferred_rx_streams=A->Nss;C->mbssid_flags=1;
    C->he_ops=A->He?A->HeOps&0xffffffu:0;
    if(A->Eht)C->punct_bitmap=~(ULONG)A->Punctured;
    C->ssid.ssid_len=J->SsidLength;RtlCopyMemory(C->ssid.ssid,J->Ssid,J->SsidLength);
    Ch=(void*)(C+1);JHDR(Ch,S6_WMI_TAG_CHANNEL);Ch->mhz=A->Frequency;Ch->band_center_freq1=A->CenterFrequency;
    Ch->info=(1u<<9)|A->PhyMode|(A->Ht?(1u<<11):0)|(A->Vht?(1u<<12):0)|(A->He?(1u<<17):0)|(A->Dfs?(1u<<10):0)|((A->Width==40&&A->CenterFrequency>A->Frequency)?(1u<<6):0);
    Power=A->Power*2;if(Power>127)Power=127;Gain=A->Gain*2;if(Gain>127)Gain=127;
    Ch->reg_info_1=(Power<<8)|(Power<<16);Ch->reg_info_2=Gain|(Power<<8);
    End=(void*)(Ch+1);*End=Jtlv(18,0); /* no NoA descriptors */
    return WcnWmiSend(Dev,Restart?0x5004u:S7_WMI_VDEV_START_REQUEST_CMDID,J->Wire,(ULONG)((UCHAR*)(End+1)-(UCHAR*)C));
}
static NTSTATUS Jcreate(PWCN_DEVICE Dev,PWCN_JOIN J)
{
    UCHAR B[24]={0};struct wmi_peer_create_cmd *C=(void*)B;JHDR(C,S7_WMI_TAG_PEER_CREATE_CMD);
    C->vdev_id=J->Vdev;C->peer_type=0;RtlCopyMemory(C->peer_macaddr.addr,J->Ap.Bssid,6);
    *(ULONG*)(B+20)=Jtlv(18,0); /* MLO disabled */
    return WcnWmiSend(Dev,S7_WMI_PEER_CREATE_CMDID,B,sizeof(B));
}
static NTSTATUS Jqueue(PWCN_DEVICE Dev,PWCN_JOIN J)
{
    struct wmi_peer_reorder_queue_setup_cmd C={0};NTSTATUS St;ULONG Tid=J->ReoSetupTid;ULONG64 Pa;
    if(Tid>=S7_RX_TID_COUNT)return STATUS_SUCCESS;
    if(!J->ReoQueue.Buffer||J->ReoQueue.Length<S7_RX_TID_COUNT*S7_RX_QUEUE_STRIDE)return STATUS_DEVICE_NOT_READY;
    Pa=J->ReoQueue.La+(ULONG64)Tid*S7_RX_QUEUE_STRIDE;
    JHDR(&C,S7_WMI_TAG_REORDER_QUEUE_SETUP_CMD);C.vdev_id=J->Vdev;RtlCopyMemory(C.peer_macaddr.addr,J->Ap.Bssid,6);
    C.tid=C.queue_no=Tid;C.queue_ptr_lo=(ULONG)Pa;C.queue_ptr_hi=(ULONG)(Pa>>32);
    C.ba_window_size_valid=1;C.ba_window_size=1;
    St=WcnWmiSend(Dev,S7_WMI_PEER_REORDER_QUEUE_SETUP_CMDID,&C,sizeof(C));
    if(!NT_SUCCESS(St))return St; /* including BUSY: no false advancement */
    J->ReoSetupTid++;

    return J->ReoSetupTid==S7_RX_TID_COUNT?STATUS_SUCCESS:STATUS_PENDING;
}
static NTSTATUS Jsubmit(PWCN_DEVICE Dev,PWCN_JOIN J,UCHAR *F,ULONG N)
{
    struct wmi_mgmt_send_cmd *C=(void*)J->Wire;NTSTATUS St;
    ULONG Aligned=(N+3)&~3u;RtlZeroMemory(J->Wire,sizeof(J->Wire));JHDR(C,S7_WMI_TAG_MGMT_TX_SEND_CMD);C->vdev_id=J->Vdev;
    if(J->KeyHold&&J->KeyUpdate.Sent)return STATUS_DEVICE_BUSY;
    St=WcnTdlsAwake(Dev,J);if(!NT_SUCCESS(St))return St;
    C->desc_id=WcnMgmtAllocate(Dev,J->Vdev);if(!C->desc_id)return STATUS_DEVICE_BUSY;J->MgmtId=C->desc_id;
    /* Vdev already owns the selected channel. ath12k uses chanfreq=0
     * for ordinary auth/assoc; nonzero is reserved for off-channel ROC. */
    C->chanfreq=0;C->paddr_lo=(ULONG)J->MgmtDma.La;C->paddr_hi=(ULONG)(J->MgmtDma.La>>32);
    C->frame_len=C->buf_len=N;J->Wire[sizeof(*C)/4]=Jtlv(17,Aligned);RtlCopyMemory((UCHAR*)J->Wire+sizeof(*C)+4,F,N);
    KeMemoryBarrier();St=WcnWmiSend(Dev,S7_WMI_MGMT_TX_SEND_CMDID,J->Wire,sizeof(*C)+4+Aligned);
    if(NT_SUCCESS(St)){WcnStage8Mgmt(Dev,J,F,N,TRUE);J->MgmtBusy=TRUE;J->MgmtAt=WcnNowMs();J->MgmtStatus=MAXULONG;}
    else Dev->MgmtInUse[J->Vdev]=0;
    return St;
}

static NTSTATUS Jmanagement(PWCN_DEVICE Dev,PWCN_JOIN J,BOOLEAN Assoc)
{
    UCHAR *F=J->MgmtDma.Va;ULONG N=24;BOOLEAN Provisioning=Assoc&&WcnStage8Provisioning(Dev,J);
    if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    RtlZeroMemory(F,J->MgmtDma.Length);F[0]=Assoc?(J->Reassociate?0x20:0):0xb0;RtlCopyMemory(F+4,J->Ap.Bssid,6);RtlCopyMemory(F+10,J->Mac,6);RtlCopyMemory(F+16,J->Ap.Bssid,6);
    Jputle16(F+22,(J->Sequence++&4095)<<4);
    if(!Assoc){Jputle16(F+26,1);N=30;} /* open-system authentication */
    else{
        Jputle16(F+24,J->Ap.Auth==WCN_AUTH_OPEN||Provisioning?0x0401:0x0411);Jputle16(F+26,10);N=28;if(J->Reassociate){RtlCopyMemory(F+N,J->PreviousBssid,6);N+=6;}
        F[N++]=0;F[N++]=J->SsidLength;RtlCopyMemory(F+N,J->Ssid,J->SsidLength);N+=J->SsidLength;
        {ULONG First=J->Ap.RateCount>8?8:J->Ap.RateCount;
        F[N++]=1;F[N++]=(UCHAR)First;RtlCopyMemory(F+N,J->Ap.Rates,First);N+=First;
        if(J->Ap.RateCount>First){ULONG More=J->Ap.RateCount-First;F[N++]=50;F[N++]=(UCHAR)More;RtlCopyMemory(F+N,J->Ap.Rates+First,More);N+=More;}}
        if(J->Ap.FtMode){ULONG L=WcnStage8FtIes(Dev,J,F+N);if(!L)return STATUS_INVALID_DEVICE_STATE;N+=L;}
        else if(!Provisioning)N+=WcnRsnBuildCached(F+N,J->Ap.Auth,J->Ap.Pmf,J->H2e,J->Ap.HasPmkid?J->Ap.Pmkid:NULL);
        N+=WcnPhyBuildIes(Dev,&J->Ap,F+N,FALSE);
        N+=WcnTdlsCapabilities(Dev,J,F+N);
        if(N>J->MgmtDma.Length)return STATUS_INVALID_BUFFER_SIZE;ULONG L=WcnStage8AssocIes(Dev,J,F+N,(ULONG)J->MgmtDma.Length-N);if(L==MAXULONG)return STATUS_INVALID_BUFFER_SIZE;N+=L;
    }
    return Jsubmit(Dev,J,F,N);
}
static NTSTATUS JpeerAssoc(PWCN_DEVICE D,PWCN_JOIN J)
{return WcnPhyPeer(D,&J->Ap,J->Ap.Bssid,J->Vdev,J->Aid,10,J->Wire);}
static NTSTATUS Jup(PWCN_DEVICE Dev,PWCN_JOIN J)
{
    struct wmi_vdev_up_cmd C={0};JHDR(&C,S7_WMI_TAG_VDEV_UP_CMD);C.vdev_id=J->Vdev;C.vdev_assoc_id=J->Aid;
    RtlCopyMemory(C.vdev_bssid.addr,J->Ap.Bssid,6);
    return WcnWmiSend(Dev,S7_WMI_VDEV_UP_CMDID,&C,sizeof(C));
}
NTSTATUS WcnStage7ChannelCommand(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Up)
{return Up?Jup(D,J):Jstart(D,J,TRUE);}
static VOID JmgmtRx(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *P,ULONG N)
{
    const UCHAR *H,*F;ULONG L,Size,Status;USHORT Fc;
    H=Jfind(P,N,44,&L);if(!H||L<68)return;
    Size=Jle32(H+16);Status=Jle32(H+20);
    if((Status&0x39)||(Jle32(H+60)!=1&&Jle32(H+60)!=2))return;
    F=Jfind(P,N,17,&L);if(!F||Size>L||Size<24)return;
    if(!Jeq(F+10,J->Ap.Bssid,6)||!Jeq(F+16,J->Ap.Bssid,6)||(!(F[4]&1)&&!Jeq(F+4,J->Mac,6)))return;
    Fc=Jle16(F);if(Fc&0x030f)return;
    if((Fc&0xfc)==0x80||(Fc&0xfc)==0x50){
        // Scan discovery already forwards these frames; outside scans, keep the associated signal live.
        if(Size>=36&&!Dev->Scan.RuntimeScan){ULONG Frequency=Jle32(H+64);if(!Frequency)Frequency=WcnChannelFrequency(Jle32(H));WcnStage8Observe(Dev,F,Size,Frequency,(LONG)Jle32(H+44));}
        if(Size>=36)WcnTdlsObserve(J,F+36,Size-36);
        if(WcnPhyChannelChanged(Dev,J,F,Size,36))WcnStage7Fail(Dev,J,"AP changed channel geometry; fresh regulatory discovery required");return;
    }
    if(!Jeq(F+4,J->Mac,6)&&!(J->Ap.Pmf&&(F[4]&1)))return;
    if((Fc&0xfc)==0xd0&&!(Fc&0x4000)&&Size>=30&&F[24]==4&&F[25]==4){if(WcnPhyChannelChanged(Dev,J,F,Size,24))WcnStage7Fail(Dev,J,"AP announced extended channel switch");return;}
    if(J->Ap.FtMode==2&&(Fc&0xfc)==0xb0&&J->State==5){if(!(Fc&0x4000))WcnStage8FtReceive(Dev,J,F,Size);return;}
    if(J->Ap.Auth==WCN_AUTH_SAE&&(Fc&0xfc)==0xb0&&J->State==5){
        if(Fc&0x4000)return;
        if(Size>=30&&Jle16(F+24)==3)WcnStage8SaeReceive(Dev,J,F+24,Size-24);return;
    }
    if(((Fc&0xfc)==0xa0||(Fc&0xfc)==0xc0||(Fc&0xfc)==0xd0)&&J->Ap.Pmf){
        ULONG Off;if(!WcnPmfReceive(Dev,J,F,Size,&Off))return;
        if((Fc&0xfc)==0xd0){if(WcnPhyChannelChanged(Dev,J,F,Size,Off)){WcnStage7Fail(Dev,J,"PMF-verified channel switch");return;}WcnStage8ActionReceive(Dev,J,F+Off,Size-Off);if(Size>=Off+2&&F[Off]==3)WcnAggregationReceive(Dev,J,F+Off,Size-Off);if(Size>=Off+4&&F[Off]==8&&F[Off+1]==0){J->SaQueryPending=TRUE;RtlCopyMemory(J->SaQueryId,F+Off+2,2);}return;}
        if(Size>=Off+2){WcnStage7Fail(Dev,J,"protected AP disconnect");}return;
    }
    if(Fc&0x4000)return; /* protocol/type, To/FromDS, protected */
    if((Fc&0xfc)==0xd0){if(WcnPhyChannelChanged(Dev,J,F,Size,24)){WcnStage7Fail(Dev,J,"AP announced channel switch");return;}WcnStage8ActionReceive(Dev,J,F+24,Size-24);if(Size>=26&&F[24]==3)WcnAggregationReceive(Dev,J,F+24,Size-24);return;}
    WcnStage8Mgmt(Dev,J,F,Size,FALSE);
    if((Fc&0xfc)==0xb0&&J->State==5&&Size>=30){
        if(Jle16(F+24)!=0||Jle16(F+26)!=2)return;
        if(Jle16(F+28)){WcnStage7Fail(Dev,J,"AP rejected open-system authentication");return;}
        J->Authenticated=TRUE;
    }else if((Fc&0xfc)==(J->Reassociate?0x30:0x10)&&J->State==6&&Size>=30){
        if(Jle16(F+26)){if(WcnRsnCacheRejected(Jle16(F+26),&J->Ap.HasPmkid,J->Ap.Pmkid)){J->Attempts=0;J->LastSendAt=0;return;}WcnStage7Fail(Dev,J,"AP rejected association");return;}
        J->Aid=Jle16(F+28)&0x3fff;
        if(!J->Aid||J->Aid>2007){WcnStage7Fail(Dev,J,"invalid association ID");return;}
        /* Validate the complete response IE stream before accepting it. */
        {BOOLEAN Ht=FALSE,Vht=FALSE,He=FALSE,Eht=FALSE;
        for(N=30;N<Size;N+=L+2){if(Size-N<2){J->BadEvents++;return;}L=F[N+1];if(L>Size-N-2){J->BadEvents++;return;}
            if(F[N]==45&&L==26)Ht=TRUE;if(F[N]==191&&L==12)Vht=TRUE;if(F[N]==255&&L>=22&&F[N+2]==35)He=TRUE;if(F[N]==255&&L>=15&&F[N+2]==108)Eht=TRUE;}
        if((J->Ap.Ht&&!Ht)||(J->Ap.Vht&&!Vht)||(J->Ap.He&&!He)||(J->Ap.Eht&&!Eht)){WcnStage7Fail(Dev,J,"AP association response omitted negotiated HT/VHT capabilities");return;}
        }
        J->Associated=TRUE;
    }else if(((Fc&0xfc)==0xa0||(Fc&0xfc)==0xc0)&&Size>=26){WcnStage7Fail(Dev,J,"selected AP disconnected station");}
}
static VOID Jreceive(PWCN_DEVICE Dev,PWCN_JOIN J,ULONG Event,const UCHAR *P,ULONG N)
{
    const UCHAR *F;ULONG L,Tag=0;
    if(J->Resetting&&Event==S7_WMI_MGMT_TX_COMPLETION_EVENTID){
        if(!Jtlvs(P,N))return;F=Jfind(P,N,S7_WMI_TAG_MGMT_TX_COMPL_EVENT,&L);
        if(F&&L>=12&&J->MgmtBusy&&Jle32(F)==J->MgmtId&&Jle32(F+8)>=1&&Jle32(F+8)<=2){J->MgmtStatus=Jle32(F+4);J->MgmtBusy=FALSE;Dev->MgmtInUse[J->Vdev]=0;}return;
    }
    if(J->Resetting&&(Event==0x5002u||Event==0x6007u)){
        if(!Jtlvs(P,N))return;
        F=Jfind(P,N,Event==0x5002u?41u:451u,&L);
        if(!F||L<(Event==0x5002u?4u:12u)||Jle32(F)!=J->Vdev)return;
        if(Event==0x5002u){J->VdevStopConfirmed=TRUE;}
        else if(Jeq(F+4,J->Ap.Bssid,6)){J->PeerDeleteConfirmed=TRUE;}
        return;
    }
    if(J->Failed||!J->Started||J->Resetting)return;
    switch(Event){
    case S7_WMI_VDEV_START_RESP_EVENTID:Tag=S7_WMI_TAG_VDEV_START_RESPONSE_EVENT;break;
    case S7_WMI_MGMT_TX_COMPLETION_EVENTID:Tag=S7_WMI_TAG_MGMT_TX_COMPL_EVENT;break;
    case S7_WMI_PEER_ASSOC_CONF_EVENTID:Tag=S7_WMI_TAG_PEER_ASSOC_CONF_EVENT;break;
    case S7_WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID:Tag=S7_WMI_TAG_VDEV_INSTALL_KEY_COMPLETE_EVENT;break;
    case 0x7001:if(Jtlvs(P,N))JmgmtRx(Dev,J,P,N);else J->BadEvents++;return;
    default:return;
    }
    if(!Jtlvs(P,N)){J->BadEvents++;return;}F=Jfind(P,N,Tag,&L);if(!F){J->BadEvents++;return;}
    if(Event==S7_WMI_VDEV_START_RESP_EVENTID){
        if(WcnChannelEvent(Dev,J,P,N))return;
        if(L<sizeof(struct wmi_vdev_start_resp_event)){J->BadEvents++;return;}
        if(J->State!=3||Jle32(F)!=J->Vdev||Jle32(F+4)!=0||Jle32(F+8)!=0){

            return;
        }

        if(Jle32(F+12)){WcnLog(Dev,"stage7: VDEV_START rejected status=%u",Jle32(F+12));WcnStage7Fail(Dev,J,"VDEV_START rejected");return;}
        if(!Jle32(F+28)||!Jle32(F+32)){WcnStage7Fail(Dev,J,"firmware VDEV_START returned zero NSS");return;}
        WcnPhyLimitNss(Dev,J,Jle32(F+28)<Jle32(F+32)?Jle32(F+28):Jle32(F+32));
        J->MacId=Jle32(F+24);if(J->MacId>1){WcnStage7Fail(Dev,J,"unsupported firmware MAC routing");return;}J->VdevStarted=TRUE;

    }else if(Event==S7_WMI_MGMT_TX_COMPLETION_EVENTID){
        /* DBS can migrate vdev0 between firmware pdev1/pdev2. Descriptor IDs
         * are globally reserved until completion, so match ownership, not the initial pdev. */
        if(L<12||!J->MgmtBusy||Jle32(F)!=J->MgmtId||Dev->MgmtInUse[J->Vdev]!=J->MgmtId||Jle32(F+8)<1||Jle32(F+8)>2)return;
        J->MgmtStatus=Jle32(F+4);J->MgmtBusy=FALSE;Dev->MgmtInUse[J->Vdev]=0;

    }else if(Event==S7_WMI_PEER_ASSOC_CONF_EVENTID){
        if(L<12||J->State!=8||Jle32(F)!=J->Vdev||!Jeq(F+4,J->Ap.Bssid,6))return;
        J->AssocConfirmed=TRUE;
    }else WcnStationKeyComplete(Dev,J,F,L);
}
static NTSTATUS JsaQueryReply(PWCN_DEVICE Dev,PWCN_JOIN J);
NTSTATUS WcnStage7EnsureVdev(PWCN_DEVICE D,PWCN_JOIN J)
{
    if(J->Created)return STATUS_SUCCESS;
    // Stage6 alone creates vdev0. OS scan requests can arrive before it finishes.
    if((J->Vdev!=2&&!(J->Vdev==1&&D->P2pClient))||!D->Scan.Passed)return STATUS_DEVICE_BUSY;
    if(!D->Ce.WmiReady)return STATUS_DEVICE_NOT_READY;
    if(J->Vdev==1){NTSTATUS T=WcnTdlsStop(D,&D->Join);if(!NT_SUCCESS(T))return T;T=WcnTdlsStop(D,&D->Secondary);if(!NT_SUCCESS(T))return T;T=WcnTwtStop(D,&D->Secondary);if(!NT_SUCCESS(T))return T;}
    NTSTATUS Awake=WcnTwtStop(D,&D->Join);if(!NT_SUCCESS(Awake))return Awake;
    ULONG B[21]={0};B[0]=Jtlv(86,44);B[1]=J->Vdev;B[2]=2;B[3]=J->Vdev==1?2u:0u;RtlCopyMemory(B+4,J->Mac,6);B[6]=2;B[7]=1;B[12]=Jtlv(18,32);
    for(ULONG I=0;I<2;I++){B[13+I*4]=Jtlv(411,12);B[14+I*4]=I;B[15+I*4]=B[16+I*4]=D->Ce.NumRfChains;}
    NTSTATUS St=WcnWmiSend(D,0x5001,B,sizeof(B));if(NT_SUCCESS(St)){J->Created=TRUE;J->VdevAt=WcnNowMs();}return St;
}
static VOID Jservice(PWCN_DEVICE Dev,PWCN_JOIN J)
{
    LONGLONG Now=WcnNowMs();NTSTATUS St=STATUS_SUCCESS;
    if(J->Failed||J->Resetting||!Dev->Ce.WmiReady)return;
    if(Dev->Ce.SourceFailed){WcnStage7Fail(Dev,J,"CE source consumer pointer/ownership failure");return;}
    if(Dev->Mhi.SysErr||Dev->Mhi.RddmSaved||Dev->Dp.Failed){WcnStage7Fail(Dev,J,"firmware or data-path failure");return;}
    if(Dev->Scan.Failed){WcnStage7Fail(Dev,J,"country/scan prerequisites failed");return;}
    if(!Dev->Scan.Passed)return;
    if(!WcnStage8ConnectRequested(Dev,J))return;
    if(!J->Created){St=WcnStage7EnsureVdev(Dev,J);if(!NT_SUCCESS(St)&&St!=STATUS_DEVICE_BUSY)WcnStage7Fail(Dev,J,"vdev creation transport");return;}if(J->VdevAt&&Now-J->VdevAt<200)return;
    if(!J->Started){
        if(J->Vdev==1&&!WcnStage8SecondaryIdle(Dev))return;
        if(J->Vdev==0&&Dev->Secondary.Started&&(WcnIs2g(J->Ap.Frequency)==WcnIs2g(Dev->Secondary.Ap.Frequency)||!WcnHotspotTriple(Dev,&J->Ap,&Dev->Secondary.Ap)))return;
        J->Started=TRUE;J->StartedAt=J->StateAt=Now;
        if(!J->Ap.Valid){WcnStage7Fail(Dev,J,"selected BSS absent or incompatible; no fallback to another SSID");return;}
        if(Dev->Scan.Alpha2[0]!='C'||Dev->Scan.Alpha2[1]!='N'){WcnStage7Fail(Dev,J,"CN country not confirmed");return;}
        St=J->MgmtDma.Buffer?STATUS_SUCCESS:WcnDmaAllocAligned(Dev,1536,0xfff,&J->MgmtDma);if(!NT_SUCCESS(St)){WcnStage7Fail(Dev,J,"management DMA allocation");return;}
        St=WcnStage7DpInit(Dev,J);if(!NT_SUCCESS(St)){WcnStage7Fail(Dev,J,"runtime data-path preparation");return;}

    }
    if(J->Failed)return;
    if(J->KeyUpdate.Slot){WcnStationKeysService(Dev,J);if(J->KeyUpdate.Slot||J->Failed)return;}
    if(J->State>=9&&J->Authorized){if(J->CsaState<2)WcnAggregationService(Dev,J);if(WcnChannelService(Dev,J))return;}
    if(J->SaQueryPending){St=JsaQueryReply(Dev,J);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->SaQueryPending=FALSE;}
    if(Now-J->StartedAt>24000&&J->State<9){WcnStage7Fail(Dev,J,"association deadline");return;}
    if(J->MgmtBusy&&Now-J->MgmtAt>1500){WcnStage7Fail(Dev,J,"management completion deadline; DMA not reused while firmware owns it");return;}
    switch(J->State){
    case 0:St=WcnVdevParam(Dev,J->Vdev,S7_WMI_VDEV_PARAM_TX_ENCAP_TYPE,2);break;
    case 1:St=WcnVdevParam(Dev,J->Vdev,S7_WMI_VDEV_PARAM_RX_DECAP_TYPE,2);break;
    case 2:
        if(J->Ap.Pmf&&!J->PmfConfigured){ULONG B[]={Jtlv(S6_WMI_TAG_PDEV_SET_PARAM_CMD,12),1,34,1};St=WcnWmiSend(Dev,S6_WMI_PDEV_SET_PARAM_CMDID,B,sizeof(B));if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->PmfConfigured=TRUE;return;}
        if(!J->WmmSet){St=WcnPhyWmm(Dev,&J->Ap,J->Vdev);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->WmmSet=TRUE;return;}
        if(!J->BeamformSet){St=WcnPhyBeamform(Dev,&J->Ap,J->Vdev);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->BeamformSet=TRUE;return;}
        St=Jstart(Dev,J,FALSE);break;
    case 3:
        if(!J->VdevStarted){if(Now-J->StateAt>3000)WcnStage7Fail(Dev,J,"VDEV_START response deadline");return;}
        if(!J->AggregationSet){St=WcnAggregationConfigure(Dev,J->Vdev);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->AggregationSet=TRUE;return;}
        St=Jcreate(Dev,J);if(NT_SUCCESS(St))J->PeerCreated=TRUE;break;
    case 4:
        if(!J->PeerMapped){if(Now-J->StateAt>2000)WcnStage7Fail(Dev,J,"HTT peer-map deadline");return;}
        if(!J->RoutingSet){
            St=WcnPeerParam(Dev,J->Vdev,J->Ap.Bssid,19,1u|((J->MacId+1)<<1));
            if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;
            J->RoutingSet=TRUE;return;
        }
        St=Jqueue(Dev,J);if(St==STATUS_PENDING)return;break;
    case 5:
        if(J->Ap.FtMode==2&&!J->Authenticated){WcnStage8FtService(Dev,J);return;}
        if(J->Ap.Auth==WCN_AUTH_SAE&&!J->Authenticated){WcnStage8SaeService(Dev,J);return;}
        if(J->Authenticated){J->Attempts=0;break;}
        if(J->Attempts&&Now-J->LastSendAt<600)return;
        if(J->Attempts>=3){WcnStage7Fail(Dev,J,"authentication response deadline");return;}
        St=Jmanagement(Dev,J,FALSE);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;
        J->Attempts++;J->LastSendAt=Now;return;
    case 6:
        if(J->Associated){J->Attempts=0;break;}
        if(J->Attempts&&Now-J->LastSendAt<700)return;
        if(J->Attempts>=3){WcnStage7Fail(Dev,J,"association response deadline");return;}
        St=Jmanagement(Dev,J,TRUE);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;
        J->Attempts++;J->LastSendAt=Now;return;
    case 7:St=JpeerAssoc(Dev,J);break;
    case 8:
        if(!J->AssocConfirmed){if(Now-J->StateAt>2000)WcnStage7Fail(Dev,J,"PEER_ASSOC_CONF deadline");return;}
        St=Jup(Dev,J);break;
    default:return; /* Windows now owns EAPOL, keys and networking. */
    }
    if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St))goto Transport;
    J->State++;J->StateAt=Now;J->LastSendAt=0;
    return;
Transport:WcnStage7Fail(Dev,J,"command transport error");
}



/* Serialized teardown: every command honors HTC credits, and the owner waits
 * for firmware response/HTT unmap before permitting descriptor reuse. */
NTSTATUS WcnStage7TeardownCommand(PWCN_DEVICE Dev,PWCN_JOIN J,ULONG Step)
{
    UCHAR Body[16]={0};ULONG *B=(ULONG*)Body;
    if(Step==0){NTSTATUS St=WcnTdlsStop(Dev,J);if(!NT_SUCCESS(St))return St;St=WcnTwtStop(Dev,J);if(!NT_SUCCESS(St))return St;B[0]=Jtlv(94,4);B[1]=J->Vdev;return WcnWmiSend(Dev,0x5007,Body,8);}
    if(Step==1){B[0]=Jtlv(98,12);B[1]=J->Vdev;RtlCopyMemory(Body+8,J->Ap.Bssid,6);return WcnWmiSend(Dev,0x6002,Body,16);}
    if(Step==2){B[0]=Jtlv(93,4);B[1]=J->Vdev;return WcnWmiSend(Dev,0x5006,Body,8);}
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS WcnStage7AuthSend(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Body,ULONG Length)
{
    UCHAR *F=J->MgmtDma.Va;
    if(J->State!=5||(J->Ap.Auth!=WCN_AUTH_SAE&&J->Ap.FtMode!=2)||Length<6||Length>488||J->Failed)return STATUS_INVALID_DEVICE_STATE;
    if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    RtlZeroMemory(F,512);F[0]=0xb0;RtlCopyMemory(F+4,J->Ap.Bssid,6);RtlCopyMemory(F+10,J->Mac,6);RtlCopyMemory(F+16,J->Tdls?J->Infrastructure:J->Ap.Bssid,6);
    Jputle16(F+22,(J->Sequence++&4095)<<4);RtlCopyMemory(F+24,Body,Length);return Jsubmit(Dev,J,F,24+Length);
}
NTSTATUS WcnStage7ActionSend(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Body,ULONG Length)
{
    UCHAR *F=J->MgmtDma.Va;ULONG N=24+Length;NTSTATUS St;
    if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    if(!J->Authorized||!Body||Length>448||Length<2)return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(F,512);F[0]=0xd0;RtlCopyMemory(F+4,J->Ap.Bssid,6);RtlCopyMemory(F+10,J->Mac,6);RtlCopyMemory(F+16,J->Tdls?J->Infrastructure:J->Ap.Bssid,6);
    Jputle16(F+22,(J->Sequence++&4095)<<4);RtlCopyMemory(F+24,Body,Length);
    if(J->Ap.Pmf){if(!J->PairwiseInstalled)return STATUS_INVALID_DEVICE_STATE;St=WcnPmfTxUnicast(F,&N,512,&J->MgmtTxPn);if(!NT_SUCCESS(St))return St;}
    return Jsubmit(Dev,J,F,N);
}
NTSTATUS WcnStage7PublicAction(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR Peer[6],const UCHAR *Body,ULONG Length)
{
    UCHAR *F=J->MgmtDma.Va;if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    if(!J->Authorized||!F||!Body||Length<2||Length>900||Body[0]!=4||Body[1]!=14)return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(F,1024);F[0]=0xd0;RtlCopyMemory(F+4,Peer,6);RtlCopyMemory(F+10,J->Mac,6);RtlCopyMemory(F+16,J->Ap.Bssid,6);Jputle16(F+22,(J->Sequence++&4095)<<4);RtlCopyMemory(F+24,Body,Length);return Jsubmit(D,J,F,Length+24);
}
static NTSTATUS JsaQueryReply(PWCN_DEVICE Dev,PWCN_JOIN J)
{
    UCHAR Body[]={8,1,0,0};RtlCopyMemory(Body+2,J->SaQueryId,2);
    return WcnStage7ActionSend(Dev,J,Body,sizeof(Body));
}

VOID WcnStage7FailAll(PWCN_DEVICE D,PCSTR Reason)
{WcnStage7Fail(D,&D->Join,Reason);WcnStage7Fail(D,&D->Secondary,Reason);if(D->P2pClient)WcnStage7Fail(D,&D->P2pJoin,Reason);}
VOID WcnStage7Receive(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{WcnTwtEvent(D,Event,P,N);WcnStage8RoamEvent(D,Event,P,N);Jreceive(D,&D->Join,Event,P,N);Jreceive(D,&D->Secondary,Event,P,N);if(D->P2pClient)Jreceive(D,&D->P2pJoin,Event,P,N);WcnTdlsWmi(D,Event,P,N);}
VOID WcnStage7PeerReceive(PWCN_DEVICE D,PWCN_JOIN J,ULONG Event,const UCHAR *P,ULONG N){Jreceive(D,J,Event,P,N);}
NTSTATUS WcnStage7PeerQueues(PWCN_DEVICE D,PWCN_JOIN J){return Jqueue(D,J);}
VOID WcnStage7Service(PWCN_DEVICE D)
{WcnTwtService(D,&D->Join);WcnTwtService(D,&D->Secondary);Jservice(D,&D->Join);Jservice(D,&D->Secondary);if(D->P2pClient)Jservice(D,&D->P2pJoin);}
