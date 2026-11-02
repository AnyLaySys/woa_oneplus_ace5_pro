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

VOID WcnStage7Fail(PWCN_DEVICE Dev,PCSTR Reason)
{
    PWCN_JOIN J=&Dev->Join;
    if(J->Failed)return;
    J->Failed=TRUE;
    WcnLog(Dev,"stage7: FAIL state=%u reason=%s auth=%u assoc=%u PTK=%u GTK=%u authorized=%u tx=%u ack=%u rx=%u dropped=%u runtime-errors=%u",
        J->State,Reason,J->Authenticated,J->Associated,J->PairwiseInstalled,J->GroupInstalled,J->Authorized,J->TxCompleted,J->TxAcked,J->RxFrames,J->RxDropped,J->RuntimeErrors);
    WcnLog(Dev,"stage7: protected RX summary nullq-admitted=%u replay-drops=%u",J->NullQueueAccepted,J->ReplayDrops);
    if(J->RuntimeReady&&Dev->Dp.Rdp.Va){
        volatile ULONG *R=(volatile ULONG*)Dev->Dp.Rdp.Va;
        WcnLog(Dev,"stage7: DP pointer snapshot reo1-hp=%u reo2-hp=%u tx128-hp=%u error131-hp=%u refill160-tp=%u mac161-tp=%u mac169-tp=%u rx186-hp=%u",R[1],R[2],R[128],R[131],R[160],R[161],R[169],R[186]);
    }
    if(Dev->Ce.Initialized&&Dev->Ce.Rdp.Va)WcnLog(Dev,"stage7: CE snapshot source3 reaped=%u producer=%u entries=%u dma-TP=%u credits=%u reclaimed=%u source-failed=%u",
        Dev->Ce.Ring[3].Head,Dev->Ce.Ring[3].Tail,Dev->Ce.Ring[3].Entries,((volatile ULONG*)Dev->Ce.Rdp.Va)[67],Dev->Ce.WmiCredits,Dev->Ce.TxReclaimed,Dev->Ce.SourceFailed);
}
BOOLEAN WcnJoinCandidate(PWCN_DEVICE Dev,const UCHAR *F,ULONG N,ULONG Frequency,LONG Rssi,const UCHAR *Name,ULONG NameLength,ULONG Auth,BOOLEAN Pmf,PWCN_JOIN_AP Out)
{
    WCN_JOIN_AP A={0};const UCHAR *Ssid=NULL,*Rsn=NULL;ULONG I,K,L,SsidLength=0,Rate,Index;BOOLEAN Wpa=FALSE;
    static const UCHAR Rates[]={2,4,11,22,12,18,24,36,48,72,96,108};
    if(!NameLength||NameLength>32||N<36||Rssi>0||Rssi< -127||((F[0]&0xfc)!=0x80&&(F[0]&0xfc)!=0x50))return FALSE;
    A.Frequency=WcnPrimaryFrequency(F,N,Frequency);
    if(!WcnConnectFrequency(A.Frequency))return FALSE;
    if(!WcnIs2g(A.Frequency)&&WcnIs2g(Frequency))return FALSE;
    for(I=0;I<Dev->Scan.ChannelCount;I++)if(Dev->Scan.Channel[I].Frequency==A.Frequency)break;
    if(I==Dev->Scan.ChannelCount)return FALSE;
    A.ChannelFlags=Dev->Scan.Channel[I].Flags;A.Power=Dev->Scan.Channel[I].Power;A.Gain=Dev->Scan.Channel[I].Gain;
    if(A.ChannelFlags&(1u|2u|8u|64u|(1u<<11)))return FALSE;
    for(I=36;I<N;I+=L+2){
        if(N-I<2)return FALSE;L=F[I+1];if(L>N-I-2)return FALSE;
        if(F[I]==0){if(Ssid||L>32)return FALSE;Ssid=F+I+2;SsidLength=L;}
        if(F[I]==221&&L>=4&&Jeq(F+I+2,(const UCHAR*)"\x00\x50\xf2\x01",4))Wpa=TRUE;
        if(F[I]==48){if(Rsn)return FALSE;Rsn=F+I;}
        if(F[I]==1||F[I]==50){
            if(!L||L>16)return FALSE;
            for(K=0;K<L;K++){
                Rate=F[I+2+K]&127;
                if(Rate==123&&(F[I+2+K]&128)&&Auth==WCN_AUTH_SAE){A.H2eOnly=TRUE;continue;}
                for(Index=WcnIs2g(A.Frequency)?0:4;Index<sizeof(Rates);Index++)if(Rate==Rates[Index])break;
                if(Index==sizeof(Rates)){if(F[I+2+K]&128)return FALSE;continue;}
                for(Index=0;Index<A.RateCount;Index++)if((ULONG)(A.Rates[Index]&127)==Rate)break;
                if(Index<A.RateCount)A.Rates[Index]|=F[I+2+K]&128;
                else if(A.RateCount<sizeof(A.Rates))A.Rates[A.RateCount++]=F[I+2+K];else return FALSE;
            }
        }
    }
    if(!Ssid||SsidLength!=NameLength||!Jeq(Ssid,Name,NameLength)||!A.RateCount||!WcnAuthSupported(Auth))return FALSE;
    if(Auth==WCN_AUTH_OPEN){if(Rsn||Wpa||(Jle16(F+34)&16)||Pmf)return FALSE;}
    else if(!Rsn||!WcnRsnAccept(Rsn+2,Rsn[1],Auth,Pmf))return FALSE;
    A.Capability=Jle16(F+34);A.BeaconInterval=Jle16(F+32);
    if(!(A.Capability&1)||!A.BeaconInterval||(F[16]&1))return FALSE;
    RtlCopyMemory(A.Bssid,F+16,6);A.Rssi=Rssi;A.Valid=TRUE;A.Auth=Auth;A.Pmf=Pmf;*Out=A;return TRUE;
}
static NTSTATUS Jvparam(PWCN_DEVICE Dev,ULONG Param,ULONG Value)
{ ULONG B[4]={Jtlv(S6_WMI_TAG_VDEV_SET_PARAM_CMD,12),0,0,0};B[2]=Param;B[3]=Value;return WcnWmiSend(Dev,S6_WMI_VDEV_SET_PARAM_CMDID,B,sizeof(B)); }
static NTSTATUS Jstart(PWCN_DEVICE Dev)
{
    PWCN_JOIN J=&Dev->Join;struct wmi_vdev_start_request_cmd *C=(void*)J->Wire;
    struct ath12k_wmi_channel_params *Ch;ULONG *End,Power,Gain;
    RtlZeroMemory(J->Wire,sizeof(J->Wire));JHDR(C,S7_WMI_TAG_VDEV_START_REQUEST_CMD);
    /* ath12k's zero-filled command leaves requestor_id at zero. This
     * PEACH image returns zero even for our former requestor=7. Match the
     * reference command instead of inventing a round-tripped transaction ID. */
    C->flags=J->Ap.Pmf?2:0;C->vdev_id=0;C->requestor_id=0;C->beacon_interval=J->Ap.BeaconInterval;C->dtim_period=1;
    C->preferred_tx_streams=1;C->preferred_rx_streams=1;C->mbssid_flags=1;
    C->ssid.ssid_len=J->SsidLength;RtlCopyMemory(C->ssid.ssid,J->Ssid,J->SsidLength);
    Ch=(void*)(C+1);JHDR(Ch,S6_WMI_TAG_CHANNEL);Ch->mhz=Ch->band_center_freq1=J->Ap.Frequency;
    Ch->info=(1u<<9)|(WcnIs2g(J->Ap.Frequency)?1u:0u); /* MODE_11G(b/g) or MODE_11A; no HT/VHT/HE */
    Power=J->Ap.Power*2;if(Power>127)Power=127;Gain=J->Ap.Gain*2;if(Gain>127)Gain=127;
    Ch->reg_info_1=(Power<<8)|(Power<<16);Ch->reg_info_2=Gain|(Power<<8);
    End=(void*)(Ch+1);*End=Jtlv(18,0); /* no NoA descriptors */
    return WcnWmiSend(Dev,S7_WMI_VDEV_START_REQUEST_CMDID,J->Wire,(ULONG)((UCHAR*)(End+1)-(UCHAR*)C));
}
static NTSTATUS Jcreate(PWCN_DEVICE Dev)
{
    UCHAR B[24]={0};struct wmi_peer_create_cmd *C=(void*)B;JHDR(C,S7_WMI_TAG_PEER_CREATE_CMD);
    C->peer_type=0;RtlCopyMemory(C->peer_macaddr.addr,Dev->Join.Ap.Bssid,6);
    *(ULONG*)(B+20)=Jtlv(18,0); /* MLO disabled */
    return WcnWmiSend(Dev,S7_WMI_PEER_CREATE_CMDID,B,sizeof(B));
}
static NTSTATUS Jqueue(PWCN_DEVICE Dev)
{
    struct wmi_peer_reorder_queue_setup_cmd C={0};PWCN_JOIN J=&Dev->Join;NTSTATUS St;ULONG Tid=J->ReoSetupTid;ULONG64 Pa;
    if(Tid>=S7_RX_TID_COUNT)return STATUS_SUCCESS;
    if(!J->ReoQueue.Buffer||J->ReoQueue.Length<S7_RX_TID_COUNT*S7_RX_QUEUE_STRIDE)return STATUS_DEVICE_NOT_READY;
    Pa=J->ReoQueue.La+(ULONG64)Tid*S7_RX_QUEUE_STRIDE;
    JHDR(&C,S7_WMI_TAG_REORDER_QUEUE_SETUP_CMD);RtlCopyMemory(C.peer_macaddr.addr,J->Ap.Bssid,6);
    C.tid=C.queue_no=Tid;C.queue_ptr_lo=(ULONG)Pa;C.queue_ptr_hi=(ULONG)(Pa>>32);
    C.ba_window_size_valid=1;C.ba_window_size=1;
    St=WcnWmiSend(Dev,S7_WMI_PEER_REORDER_QUEUE_SETUP_CMDID,&C,sizeof(C));
    if(!NT_SUCCESS(St))return St; /* including BUSY: no false advancement */
    J->ReoSetupTid++;
    WcnLog(Dev,"stage7: RX_QUEUE submitted tid=%u dma=0x%llx count=%u/17 PN-check=48bit; no ADDBA negotiated",Tid,Pa,J->ReoSetupTid);
    return J->ReoSetupTid==S7_RX_TID_COUNT?STATUS_SUCCESS:STATUS_PENDING;
}
static NTSTATUS Jsubmit(PWCN_DEVICE Dev,UCHAR *F,ULONG N)
{
    PWCN_JOIN J=&Dev->Join;struct wmi_mgmt_send_cmd *C=(void*)J->Wire;NTSTATUS St;
    ULONG Aligned=(N+3)&~3u;RtlZeroMemory(J->Wire,sizeof(J->Wire));JHDR(C,S7_WMI_TAG_MGMT_TX_SEND_CMD);
    C->desc_id=++J->MgmtId;
    /* Vdev already owns the selected channel. ath12k uses chanfreq=0
     * for ordinary auth/assoc; nonzero is reserved for off-channel ROC. */
    C->chanfreq=0;C->paddr_lo=(ULONG)J->MgmtDma.La;C->paddr_hi=(ULONG)(J->MgmtDma.La>>32);
    C->frame_len=C->buf_len=N;J->Wire[sizeof(*C)/4]=Jtlv(17,Aligned);RtlCopyMemory((UCHAR*)J->Wire+sizeof(*C)+4,F,N);
    KeMemoryBarrier();St=WcnWmiSend(Dev,S7_WMI_MGMT_TX_SEND_CMDID,J->Wire,sizeof(*C)+4+Aligned);
    if(NT_SUCCESS(St)){WcnStage8Mgmt(Dev,F,N,TRUE);J->MgmtBusy=TRUE;J->MgmtAt=WcnNowMs();J->MgmtStatus=MAXULONG;WcnLog(Dev,"stage7: %s management submitted descriptor=%u length=%u",(F[0]&0xfc)==0?"ASSOC":"AUTH/ACTION",J->MgmtId,N);}
    else J->MgmtId--;
    return St;
}

static NTSTATUS Jmanagement(PWCN_DEVICE Dev,BOOLEAN Assoc)
{
    PWCN_JOIN J=&Dev->Join;UCHAR *F=J->MgmtDma.Va;ULONG N=24;
    if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    RtlZeroMemory(F,512);F[0]=Assoc?0:0xb0;RtlCopyMemory(F+4,J->Ap.Bssid,6);RtlCopyMemory(F+10,Dev->Ce.MacAddress,6);RtlCopyMemory(F+16,J->Ap.Bssid,6);
    Jputle16(F+22,(J->Sequence++&4095)<<4);
    if(!Assoc){Jputle16(F+26,1);N=30;} /* open-system authentication */
    else{
        Jputle16(F+24,J->Ap.Auth==WCN_AUTH_OPEN?0x0401:0x0411);Jputle16(F+26,10);N=28;
        F[N++]=0;F[N++]=J->SsidLength;RtlCopyMemory(F+N,J->Ssid,J->SsidLength);N+=J->SsidLength;
        {ULONG First=J->Ap.RateCount>8?8:J->Ap.RateCount;
        F[N++]=1;F[N++]=(UCHAR)First;RtlCopyMemory(F+N,J->Ap.Rates,First);N+=First;
        if(J->Ap.RateCount>First){ULONG More=J->Ap.RateCount-First;F[N++]=50;F[N++]=(UCHAR)More;RtlCopyMemory(F+N,J->Ap.Rates+First,More);N+=More;}}
        N+=WcnRsnBuild(F+N,J->Ap.Auth,J->Ap.Pmf,J->H2e);
    }
    return Jsubmit(Dev,F,N);
}
static NTSTATUS JpeerAssoc(PWCN_DEVICE Dev)
{
    PWCN_JOIN J=&Dev->Join;struct wmi_peer_assoc_complete_cmd *C=(void*)J->Wire;
    struct ath12k_wmi_vht_rate_set_params *V;ULONG Offset,I,N=(J->Ap.RateCount+3)&~3u;
    RtlZeroMemory(J->Wire,sizeof(J->Wire));JHDR(C,S7_WMI_TAG_PEER_ASSOC_COMPLETE_CMD);
    RtlCopyMemory(C->peer_macaddr.addr,J->Ap.Bssid,6);C->peer_new_assoc=1;C->peer_associd=J->Aid;
    C->peer_flags=(J->Ap.Auth==WCN_AUTH_OPEN?0:S7_WMI_PEER_NEED_PTK_4_WAY)|(J->Ap.Pmf?0x08000000u:0); /* GTK two-way flag is WPA1-only, not RSN. */
    C->peer_caps=J->Ap.Capability;C->peer_listen_intval=10;C->peer_nss=1;C->peer_phymode=WcnIs2g(J->Ap.Frequency)?1:0;C->num_peer_legacy_rates=J->Ap.RateCount;
    Offset=sizeof(*C)/4;J->Wire[Offset++]=Jtlv(17,N);
    for(I=0;I<J->Ap.RateCount;I++)((UCHAR*)&J->Wire[Offset])[I]=J->Ap.Rates[I]&127;
    Offset+=N/4;J->Wire[Offset++]=Jtlv(17,0);
    V=(void*)&J->Wire[Offset];JHDR(V,S7_WMI_TAG_VHT_RATE_SET);Offset+=sizeof(*V)/4;
    for(I=0;I<4;I++)J->Wire[Offset++]=Jtlv(18,0); /* HE, MLO, EHT, MLO partners */
    return WcnWmiSend(Dev,S7_WMI_PEER_ASSOC_CMDID,J->Wire,Offset*4);
}
static NTSTATUS Jup(PWCN_DEVICE Dev)
{
    struct wmi_vdev_up_cmd C={0};JHDR(&C,S7_WMI_TAG_VDEV_UP_CMD);C.vdev_assoc_id=Dev->Join.Aid;
    RtlCopyMemory(C.vdev_bssid.addr,Dev->Join.Ap.Bssid,6);
    return WcnWmiSend(Dev,S7_WMI_VDEV_UP_CMDID,&C,sizeof(C));
}
static VOID JmgmtRx(PWCN_DEVICE Dev,const UCHAR *P,ULONG N)
{
    PWCN_JOIN J=&Dev->Join;const UCHAR *H,*F;ULONG L,Size,Status;USHORT Fc;
    H=Jfind(P,N,44,&L);if(!H||L<68)return;
    Size=Jle32(H+16);Status=Jle32(H+20);
    if((Status&0x39)||(Jle32(H+60)!=1&&Jle32(H+60)!=2))return;
    F=Jfind(P,N,17,&L);if(!F||Size>L||Size<24)return;
    if((!Jeq(F+4,Dev->Ce.MacAddress,6)&&!(J->Ap.Pmf&&(F[4]&1)))||!Jeq(F+10,J->Ap.Bssid,6)||!Jeq(F+16,J->Ap.Bssid,6))return;
    Fc=Jle16(F);if(Fc&0x030f)return;
    if(J->Ap.Auth==WCN_AUTH_SAE&&(Fc&0xfc)==0xb0&&J->State==5){
        if(Fc&0x4000)return;
        if(Size>=30&&Jle16(F+24)==3)WcnStage8SaeReceive(Dev,F+24,Size-24);return;
    }
    if(((Fc&0xfc)==0xa0||(Fc&0xfc)==0xc0||(Fc&0xfc)==0xd0)&&J->Ap.Pmf){
        ULONG Off;if(!WcnPmfReceive(Dev,F,Size,&Off))return;
        if((Fc&0xfc)==0xd0){if(Size>=Off+4&&F[Off]==8&&F[Off+1]==0){J->SaQueryPending=TRUE;RtlCopyMemory(J->SaQueryId,F+Off+2,2);}return;}
        if(Size>=Off+2){WcnLog(Dev,"stage7: PMF-verified AP disconnect reason=%u",Jle16(F+Off));WcnStage7Fail(Dev,"protected AP disconnect");}return;
    }
    if(Fc&0x4000)return; /* protocol/type, To/FromDS, protected */
    WcnStage8Mgmt(Dev,F,Size,FALSE);
    if((Fc&0xfc)==0xb0&&J->State==5&&Size>=30){
        if(Jle16(F+24)!=0||Jle16(F+26)!=2)return;
        if(Jle16(F+28)){WcnStage7Fail(Dev,"AP rejected open-system authentication");return;}
        J->Authenticated=TRUE;WcnLog(Dev,"stage7: authentication response status0 from selected BSSID");
    }else if((Fc&0xfc)==0x10&&J->State==6&&Size>=30){
        if(Jle16(F+26)){WcnLog(Dev,"stage7: association status=%u",Jle16(F+26));WcnStage7Fail(Dev,"AP rejected association");return;}
        J->Aid=Jle16(F+28)&0x3fff;
        if(!J->Aid||J->Aid>2007){WcnStage7Fail(Dev,"invalid association ID");return;}
        /* Validate the complete response IE stream before accepting it. */
        for(N=30;N<Size;N+=L+2){if(Size-N<2){J->BadEvents++;return;}L=F[N+1];if(L>Size-N-2){J->BadEvents++;return;}}
        J->Associated=TRUE;WcnLog(Dev,"stage7: association response status0 AID=%u; WPA2 port still unauthorized",J->Aid);
    }else if(((Fc&0xfc)==0xa0||(Fc&0xfc)==0xc0)&&Size>=26){WcnLog(Dev,"stage7: AP disconnect reason=%u",Jle16(F+24));WcnStage7Fail(Dev,"selected AP disconnected station");}
}
VOID WcnStage7Receive(PWCN_DEVICE Dev,ULONG Event,const UCHAR *P,ULONG N)
{
    PWCN_JOIN J=&Dev->Join;const UCHAR *F;ULONG L,Tag=0;
    if(J->Resetting&&Event==S7_WMI_MGMT_TX_COMPLETION_EVENTID){
        if(!Jtlvs(P,N))return;F=Jfind(P,N,S7_WMI_TAG_MGMT_TX_COMPL_EVENT,&L);
        if(F&&L>=12&&J->MgmtBusy&&Jle32(F)==J->MgmtId&&Jle32(F+8)==1){J->MgmtStatus=Jle32(F+4);J->MgmtBusy=FALSE;}return;
    }
    if(J->Resetting&&(Event==0x5002u||Event==0x6007u)){
        if(!Jtlvs(P,N))return;
        F=Jfind(P,N,Event==0x5002u?41u:451u,&L);
        if(!F||L<(Event==0x5002u?4u:12u)||Jle32(F)!=0)return;
        if(Event==0x5002u){J->VdevStopConfirmed=TRUE;WcnLog(Dev,"stage8: matched firmware VDEV_STOPPED");}
        else if(Jeq(F+4,J->Ap.Bssid,6)){J->PeerDeleteConfirmed=TRUE;WcnLog(Dev,"stage8: matched firmware peer-delete response");}
        return;
    }
    if(J->Failed||!J->Started||J->Resetting)return;
    switch(Event){
    case S7_WMI_VDEV_START_RESP_EVENTID:Tag=S7_WMI_TAG_VDEV_START_RESPONSE_EVENT;break;
    case S7_WMI_MGMT_TX_COMPLETION_EVENTID:Tag=S7_WMI_TAG_MGMT_TX_COMPL_EVENT;break;
    case S7_WMI_PEER_ASSOC_CONF_EVENTID:Tag=S7_WMI_TAG_PEER_ASSOC_CONF_EVENT;break;
    case S7_WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID:Tag=S7_WMI_TAG_VDEV_INSTALL_KEY_COMPLETE_EVENT;break;
    case 0x7001:if(Jtlvs(P,N))JmgmtRx(Dev,P,N);else J->BadEvents++;return;
    default:return;
    }
    if(!Jtlvs(P,N)){J->BadEvents++;return;}F=Jfind(P,N,Tag,&L);if(!F){J->BadEvents++;return;}
    if(Event==S7_WMI_VDEV_START_RESP_EVENTID){
        if(L<sizeof(struct wmi_vdev_start_resp_event)){J->BadEvents++;return;}
        if(J->State!=3||Jle32(F)!=0||Jle32(F+4)!=0||Jle32(F+8)!=0){
            if(++J->BadEvents<=4)WcnLog(Dev,"stage7: ignored VDEV_START response state=%u vdev=%u requestor=%u type=%u status=%u length=%u",
                J->State,Jle32(F),Jle32(F+4),Jle32(F+8),Jle32(F+12),L);
            return;
        }
        WcnLog(Dev,"stage7: VDEV_START response vdev=%u requestor=%u type=%u status=%u chain-mask=0x%x mac-id=%u tx-streams=%u rx-streams=%u length=%u",
            Jle32(F),Jle32(F+4),Jle32(F+8),Jle32(F+12),Jle32(F+16),Jle32(F+24),Jle32(F+28),Jle32(F+32),L);
        if(Jle32(F+12)){WcnLog(Dev,"stage7: VDEV_START rejected status=%u",Jle32(F+12));WcnStage7Fail(Dev,"VDEV_START rejected");return;}
        J->MacId=Jle32(F+24);if(J->MacId>1){WcnStage7Fail(Dev,"unsupported firmware MAC routing");return;}J->VdevStarted=TRUE;
        WcnLog(Dev,"stage7: VDEV_START confirmed status0 mac-id=%u",J->MacId);
    }else if(Event==S7_WMI_MGMT_TX_COMPLETION_EVENTID){
        if(L<12||!J->MgmtBusy||Jle32(F)!=J->MgmtId||Jle32(F+8)!=1)return;
        J->MgmtStatus=Jle32(F+4);J->MgmtBusy=FALSE;
        WcnLog(Dev,"stage7: management completion descriptor=%u status=%u",J->MgmtId,J->MgmtStatus);
    }else if(Event==S7_WMI_PEER_ASSOC_CONF_EVENTID){
        if(L<12||J->State!=8||Jle32(F)||!Jeq(F+4,J->Ap.Bssid,6))return;
        J->AssocConfirmed=TRUE;WcnLog(Dev,"stage7: firmware PEER_ASSOC_CONF matched selected peer");
    }else{
        if(L<24||Jle32(F)||!Jeq(F+4,J->Ap.Bssid,6))return;
        if(Jle32(F+16)==0&&Jle32(F+12)==0&&J->PairwiseSent&&!J->PairwiseInstalled){
            if(Jle32(F+20)){WcnStage7Fail(Dev,"pairwise key installation rejected");return;}J->PairwiseInstalled=TRUE;
        }else if(Jle32(F+16)==1&&Jle32(F+12)==J->GtkIndex&&J->GroupSent&&!J->GroupInstalled){
            if(Jle32(F+20)){WcnStage7Fail(Dev,"group key installation rejected");return;}J->GroupInstalled=TRUE;
        }else return;
        WcnLog(Dev,"stage7: key installation confirmed type=%s status0 (key material redacted)",Jle32(F+16)?"group":"pairwise");
    }
}
static NTSTATUS JsaQueryReply(PWCN_DEVICE Dev);
VOID WcnStage7Service(PWCN_DEVICE Dev)
{
    PWCN_JOIN J=&Dev->Join;LONGLONG Now=WcnNowMs();NTSTATUS St=STATUS_SUCCESS;
    if(J->Failed||J->Resetting||!Dev->Ce.WmiReady)return;
    if(Dev->Ce.SourceFailed){WcnStage7Fail(Dev,"CE source consumer pointer/ownership failure");return;}
    if(Dev->Mhi.SysErr||Dev->Mhi.RddmSaved||Dev->Dp.Failed){WcnStage7Fail(Dev,"firmware or data-path failure");return;}
    if(Dev->Scan.Failed){WcnStage7Fail(Dev,"country/scan prerequisites failed");return;}
    if(!Dev->Scan.Passed)return;
    if(!WcnStage8ConnectRequested(Dev))return;
    if(!J->Started){
        J->Started=TRUE;J->StartedAt=J->StateAt=Now;
        if(!J->Ap.Valid){WcnStage7Fail(Dev,"selected BSS absent or incompatible; no fallback to another SSID");return;}
        if(Dev->Scan.Alpha2[0]!='C'||Dev->Scan.Alpha2[1]!='N'){WcnStage7Fail(Dev,"CN country not confirmed");return;}
        St=J->MgmtDma.Buffer?STATUS_SUCCESS:WcnDmaAllocAligned(Dev,512,0xfff,&J->MgmtDma);if(!NT_SUCCESS(St)){WcnStage7Fail(Dev,"management DMA allocation");return;}
        St=WcnStage7DpInit(Dev);if(!NT_SUCCESS(St)){WcnStage7Fail(Dev,"runtime data-path preparation");return;}
        WcnLog(Dev,"stage8: Windows-selected association starts frequency=%u; Windows owns WPA2/DHCP",J->Ap.Frequency);
    }
    WcnStage7DpPoll(Dev);if(J->Failed)return;
    if(J->SaQueryPending){St=JsaQueryReply(Dev);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->SaQueryPending=FALSE;}
    if(Now-J->StartedAt>24000&&J->State<9){WcnStage7Fail(Dev,"24s unified-test deadline; no false pass or stage8 advancement");return;}
    if(J->MgmtBusy&&Now-J->MgmtAt>1500){WcnStage7Fail(Dev,"management completion deadline; DMA not reused while firmware owns it");return;}
    switch(J->State){
    case 0:St=Jvparam(Dev,S7_WMI_VDEV_PARAM_TX_ENCAP_TYPE,2);break;
    case 1:St=Jvparam(Dev,S7_WMI_VDEV_PARAM_RX_DECAP_TYPE,2);break;
    case 2:
        if(J->Ap.Pmf&&!J->PmfConfigured){ULONG B[]={Jtlv(S6_WMI_TAG_PDEV_SET_PARAM_CMD,12),1,34,1};St=WcnWmiSend(Dev,S6_WMI_PDEV_SET_PARAM_CMDID,B,sizeof(B));if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;J->PmfConfigured=TRUE;return;}
        St=Jstart(Dev);break;
    case 3:
        if(!J->VdevStarted){if(Now-J->StateAt>3000)WcnStage7Fail(Dev,"VDEV_START response deadline");return;}
        St=Jcreate(Dev);if(NT_SUCCESS(St))J->PeerCreated=TRUE;break;
    case 4:
        if(!J->PeerMapped){if(Now-J->StateAt>2000)WcnStage7Fail(Dev,"HTT peer-map deadline");return;}
        if(!J->RoutingSet){
            struct wmi_peer_set_param_cmd C={0};JHDR(&C,S7_WMI_TAG_PEER_SET_PARAM_CMD);
            RtlCopyMemory(C.peer_macaddr.addr,J->Ap.Bssid,6);C.param_id=19;C.param_value=1u|((J->MacId+1)<<1);
            St=WcnWmiSend(Dev,S7_WMI_PEER_SET_PARAM_CMDID,&C,sizeof(C));
            if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;
            J->RoutingSet=TRUE;return;
        }
        St=Jqueue(Dev);if(St==STATUS_PENDING)return;break;
    case 5:
        if(J->Ap.Auth==WCN_AUTH_SAE&&!J->Authenticated){WcnStage8SaeService(Dev);return;}
        if(J->Authenticated){J->Attempts=0;break;}
        if(J->Attempts&&Now-J->LastSendAt<600)return;
        if(J->Attempts>=3){WcnStage7Fail(Dev,"authentication response deadline");return;}
        St=Jmanagement(Dev,FALSE);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;
        J->Attempts++;J->LastSendAt=Now;return;
    case 6:
        if(J->Associated){J->Attempts=0;break;}
        if(J->Attempts&&Now-J->LastSendAt<700)return;
        if(J->Attempts>=3){WcnStage7Fail(Dev,"association response deadline");return;}
        St=Jmanagement(Dev,TRUE);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St))goto Transport;
        J->Attempts++;J->LastSendAt=Now;return;
    case 7:St=JpeerAssoc(Dev);break;
    case 8:
        if(!J->AssocConfirmed){if(Now-J->StateAt>2000)WcnStage7Fail(Dev,"PEER_ASSOC_CONF deadline");return;}
        St=Jup(Dev);break;
    default:return; /* Windows now owns EAPOL, keys and networking. */
    }
    if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St))goto Transport;
    J->State++;J->StateAt=Now;J->LastSendAt=0;
    WcnLog(Dev,"stage7: state=%u",J->State);return;
Transport:WcnLog(Dev,"stage7: transport status=0x%08x",St);WcnStage7Fail(Dev,"command transport error");
}

NTSTATUS WcnStage8Key(PWCN_DEVICE Dev,BOOLEAN Group,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6])
{
    PWCN_JOIN J=&Dev->Join;struct wmi_vdev_install_key_cmd *C=(void*)J->Wire;ULONG Offset;NTSTATUS St;
    if(J->State<9||J->Failed||Index>3||(!Group&&Index))return STATUS_INVALID_DEVICE_STATE;
    if(Group?J->GroupSent:J->PairwiseSent)return STATUS_NOT_SUPPORTED; /* No unsafe key reinstall. */
    RtlZeroMemory(J->Wire,sizeof(J->Wire));JHDR(C,S7_WMI_TAG_VDEV_INSTALL_KEY_CMD);
    RtlCopyMemory(C->peer_macaddr.addr,J->Ap.Bssid,6);C->key_idx=Index;C->key_flags=Group?1:0;C->key_cipher=4;C->key_len=16;
    if(Group)RtlCopyMemory(&C->key_rsc_counter,Rsc,6);
    Offset=sizeof(*C)/4;J->Wire[Offset++]=Jtlv(17,16);RtlCopyMemory(&J->Wire[Offset],Key,16);
    St=WcnWmiSend(Dev,S7_WMI_VDEV_INSTALL_KEY_CMDID,J->Wire,(Offset+4)*4);
    RtlSecureZeroMemory(J->Wire,sizeof(J->Wire));
    if(NT_SUCCESS(St)){if(Group){J->GtkIndex=Index;J->GroupSent=TRUE;RtlCopyMemory(J->Rsc,Rsc,6);}else J->PairwiseSent=TRUE;}
    return St;
}

/* Serialized teardown: every command honors HTC credits, and the owner waits
 * for firmware response/HTT unmap before permitting descriptor reuse. */
NTSTATUS WcnStage7TeardownCommand(PWCN_DEVICE Dev,ULONG Step)
{
    UCHAR Body[16]={0};ULONG *B=(ULONG*)Body;
    if(Step==0){B[0]=Jtlv(94,4);B[1]=0;return WcnWmiSend(Dev,0x5007,Body,8);}
    if(Step==1){B[0]=Jtlv(98,12);B[1]=0;RtlCopyMemory(Body+8,Dev->Join.Ap.Bssid,6);return WcnWmiSend(Dev,0x6002,Body,16);}
    if(Step==2){B[0]=Jtlv(93,4);B[1]=0;return WcnWmiSend(Dev,0x5006,Body,8);}
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS WcnStage7AuthSend(PWCN_DEVICE Dev,const UCHAR *Body,ULONG Length)
{
    PWCN_JOIN J=&Dev->Join;UCHAR *F=J->MgmtDma.Va;
    if(J->State!=5||J->Ap.Auth!=WCN_AUTH_SAE||Length<6||Length>488||J->Failed)return STATUS_INVALID_DEVICE_STATE;
    if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    RtlZeroMemory(F,512);F[0]=0xb0;RtlCopyMemory(F+4,J->Ap.Bssid,6);RtlCopyMemory(F+10,Dev->Ce.MacAddress,6);RtlCopyMemory(F+16,J->Ap.Bssid,6);
    Jputle16(F+22,(J->Sequence++&4095)<<4);RtlCopyMemory(F+24,Body,Length);return Jsubmit(Dev,F,24+Length);
}
static NTSTATUS JsaQueryReply(PWCN_DEVICE Dev)
{
    PWCN_JOIN J=&Dev->Join;UCHAR *F=J->MgmtDma.Va;ULONG64 Pn;
    if(J->MgmtBusy)return STATUS_DEVICE_BUSY;
    if(!J->PairwiseInstalled||!J->Ap.Pmf)return STATUS_DEVICE_NOT_READY;
    if(J->MgmtTxPn>=0xffffffffffffull)return STATUS_INTEGER_OVERFLOW;
    Pn=++J->MgmtTxPn;RtlZeroMemory(F,512);F[0]=0xd0;F[1]=0x40;
    RtlCopyMemory(F+4,J->Ap.Bssid,6);RtlCopyMemory(F+10,Dev->Ce.MacAddress,6);RtlCopyMemory(F+16,J->Ap.Bssid,6);Jputle16(F+22,(J->Sequence++&4095)<<4);
    F[24]=(UCHAR)Pn;F[25]=(UCHAR)(Pn>>8);F[27]=0x20;F[28]=(UCHAR)(Pn>>16);F[29]=(UCHAR)(Pn>>24);F[30]=(UCHAR)(Pn>>32);F[31]=(UCHAR)(Pn>>40);
    F[32]=8;F[33]=1;RtlCopyMemory(F+34,J->SaQueryId,2);return Jsubmit(Dev,F,36);
}
