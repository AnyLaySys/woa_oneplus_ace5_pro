/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * RX BlockAck setup is serialized: hardware REO completion, firmware queue
 * update, then the management response ACK. No in-use DMA descriptor editing. */
#include "wcn7861.h"
#include "stage7-wire.h"
#include "stage7-util.h"
#include "stage7-rx-protocol.h"
#include "hotspot.h"
/* PEACH uses firmware/TQM Ethernet encapsulation and TX aggregation, not a
 * second host-built aggregate. Zero selects the vendor's adaptive A-MSDU mode.
 * RX window remains owned by the checked REO/ADDBA path (disable RX override). */
NTSTATUS WcnAggregationConfigure(PWCN_DEVICE D,ULONG Vdev)
{ULONG B[]={Jtlv(526,16),Vdev,0,0,0x14};return WcnWmiSend(D,0x5019,B,sizeof(B));}
static VOID Bfail(PWCN_DEVICE D,ULONG Vdev,const UCHAR Mac[6],PCSTR Reason)
{if(Vdev==1)WcnHotspotFail(D,Reason);else{PWCN_JOIN J=WcnTdlsPeer(D,Vdev,Mac);WcnStage7Fail(D,J?J:WcnStation(D,Vdev),Reason);}}
VOID WcnAggregationReceivePeer(PWCN_DEVICE D,PWCN_JOIN_AP Peer,WCN_BA *Ba,const UCHAR *B,ULONG N)
{
    WCN_BA *A;UNREFERENCED_PARAMETER(D);ULONG Tid,Params,Window,Limit;USHORT Ssn;
    if(!Peer->Ht||N<2||B[0]!=3)return;
    if(B[1]==0){
        if(N!=9)return;Params=Jle16(B+3);Tid=(Params>>2)&15;Window=Params>>6;Ssn=Jle16(B+7)>>4;A=&Ba[Tid];
        if(A->Phase)return;
        if(A->Active&&A->Token==B[2]&&A->Ssn==Ssn){if(A->Amsdu!=((Params&1)!=0))return;A->Phase=6;A->Attempts=0;return;}
        Limit=Peer->He?S7_RX_BA_MAX:64u;
        A->Amsdu=(Params&1)!=0;A->Token=B[2];A->Ssn=Ssn;A->Window=(USHORT)(Window&&Window<Limit?Window:Limit);A->Attempts=0;A->Stop=FALSE;
        A->Status=(Params&2)?0:37;A->Phase=A->Status?4:1;

    }else if(B[1]==2){
        if(N!=6)return;Params=Jle16(B+2);if((Params&0x7ff)||!(Params&0x800))return;Tid=Params>>12;A=&Ba[Tid];A->Stop=TRUE;
        if(!A->Phase){A->Window=1;A->Ssn=65535;A->Phase=1;}

    }
}
static NTSTATUS Afw(PWCN_DEVICE D,ULONG Vdev,const UCHAR Peer[6],ULONG64 Reo,ULONG Tid,ULONG Window)
{
    struct wmi_peer_reorder_queue_setup_cmd C={0};ULONG64 Pa=Reo+Tid*512;
    JHDR(&C,S7_WMI_TAG_REORDER_QUEUE_SETUP_CMD);C.vdev_id=Vdev;RtlCopyMemory(C.peer_macaddr.addr,Peer,6);
    C.tid=C.queue_no=Tid;C.queue_ptr_lo=(ULONG)Pa;C.queue_ptr_hi=(ULONG)(Pa>>32);C.ba_window_size_valid=1;C.ba_window_size=Window;
    return WcnWmiSend(D,S7_WMI_PEER_REORDER_QUEUE_SETUP_CMDID,&C,sizeof(C));
}
VOID WcnAggregationServicePeer(PWCN_DEVICE D,ULONG Vdev,PWCN_JOIN_AP Peer,WCN_BA *Ba,const UCHAR Mac[6],ULONG64 Reo,BOOLEAN Busy,ULONG Status)
{
    ULONG Tid;WCN_BA *A;NTSTATUS St;UCHAR B[9];
    if(!Peer->Ht||(D->Runtime.ReoPending&&(D->Runtime.ReoOwner!=Vdev||D->Runtime.ReoBase!=Reo)))return;
    if(D->Runtime.ReoPending)Tid=D->Runtime.ReoTid;
    else{for(Tid=0;Tid<16;Tid++)if(Ba[Tid].Phase>=2&&Ba[Tid].Phase<=5)break;
        if(Tid==16)for(Tid=0;Tid<16;Tid++)if(Ba[Tid].Phase)break;}
    if(Tid>=16)return;A=&Ba[Tid];
    switch(A->Phase){
    case 1:
        St=WcnReoBaUpdate(D,Vdev,Reo,Tid,A->Window,A->Ssn);if(St==STATUS_DEVICE_BUSY)return;
        if(!NT_SUCCESS(St)){Bfail(D,Vdev,Mac,"REO BA command submission");return;}A->Phase=2;A->At=WcnNowMs();return;
    case 2:
        if(!D->Runtime.ReoDone){if(WcnNowMs()-A->At>1500)Bfail(D,Vdev,Mac,"REO BA command completion timeout");return;}
        if(D->Runtime.ReoTid!=Tid||D->Runtime.ReoStatus){Bfail(D,Vdev,Mac,"REO BA command rejected or mismatched");return;}
        D->Runtime.ReoPending=D->Runtime.ReoDone=FALSE;
        if(A->Stop&&A->Window>1){A->Window=1;A->Ssn=65535;A->Phase=1;return;}
        A->Phase=3;return;
    case 3:
        St=Afw(D,Vdev,Mac,Reo,Tid,A->Window);if(St==STATUS_DEVICE_BUSY)return;
        if(!NT_SUCCESS(St)){Bfail(D,Vdev,Mac,"firmware BA queue update");return;}
        if(A->Stop){A->Active=FALSE;A->Phase=0;A->Stop=FALSE;return;}
        A->Phase=4;return;
    case 6:A->Phase=4;return;
    case 4:
        if(A->Stop){A->Window=1;A->Ssn=65535;A->Phase=1;return;}
        if(Busy)return;
        B[0]=3;B[1]=1;B[2]=A->Token;Jputle16(B+3,A->Status);
        Jputle16(B+5,2u|(A->Amsdu&&!A->Status?1u:0)|(Tid<<2)|((ULONG)A->Window<<6)); /* echo A-MSDU permission only for an accepted agreement */
        Jputle16(B+7,0);St=(Vdev==1?WcnHotspotActionSend(D,B,sizeof(B)):WcnStage7ActionSend(D,WcnTdlsPeer(D,Vdev,Mac)?WcnTdlsPeer(D,Vdev,Mac):WcnStation(D,Vdev),B,sizeof(B)));if(St==STATUS_DEVICE_BUSY)return;
        if(!NT_SUCCESS(St)){Bfail(D,Vdev,Mac,"ADDBA response transport");return;}A->Phase=5;return;
    case 5:
        if(Busy)return;
        if(Status){if(++A->Attempts<3){A->Phase=4;return;}Bfail(D,Vdev,Mac,"ADDBA response not acknowledged");return;}
        if(A->Stop){A->Window=1;A->Ssn=65535;A->Phase=1;return;}
        A->Active=A->Status==0;A->Phase=0;
        return;
    }
}

VOID WcnAggregationReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *B,ULONG N)
{if(J->Authorized&&!J->CsaState&&!J->KeyHold)WcnAggregationReceivePeer(D,&J->Ap,J->Ba,B,N);}
VOID WcnAggregationService(PWCN_DEVICE D,PWCN_JOIN J)
{if(J->Authorized&&!J->Failed&&!J->Resetting)WcnAggregationServicePeer(D,J->Vdev,&J->Ap,J->Ba,J->Ap.Bssid,J->ReoQueue.La,J->MgmtBusy,J->MgmtStatus);}
