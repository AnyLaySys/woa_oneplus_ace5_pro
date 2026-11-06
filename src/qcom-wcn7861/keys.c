/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Live CCMP rotation: one in-flight firmware key per peer, independent GTK
 * slots/PN streams, identical-key idempotence and deleted-key tombstones.
 * Windows owns the handshake. New key state commits only after firmware ACK
 * and the next RX drain; replacement PTKs also reset checked hardware PN state.
 */
#include "wcn7861.h"
#include "stage7-util.h"
#include "stage7-wire.h"
#include "stage7-rx-protocol.h"
static ULONG64 Kpn(const UCHAR P[6]){return Jle32(P)|((ULONG64)Jle16(P+4)<<32);}
static NTSTATUS Kfail(PWCN_DEVICE D,PWCN_JOIN J,PCSTR Why)
{RtlSecureZeroMemory(J->KeyUpdate.Key,16);WcnStage7Fail(D,J,Why);return STATUS_DEVICE_HARDWARE_ERROR;}
static VOID Kflags(PWCN_JOIN J)
{
    ULONG I;J->PairwiseInstalled=J->Cipher[0].Installed;J->GroupInstalled=J->Tdls;
    for(I=1;I<5;I++)if(J->Cipher[I].Installed)J->GroupInstalled=TRUE;
    if(!J->PairwiseInstalled||!J->GroupInstalled||(J->Ap.Pmf&&!J->Igtk[0].Valid&&!J->Igtk[1].Valid))J->Authorized=FALSE;
}
VOID WcnStationKeysClear(PWCN_JOIN J)
{
    RtlSecureZeroMemory(J->Cipher,sizeof(J->Cipher));RtlSecureZeroMemory(&J->KeyUpdate,sizeof(J->KeyUpdate));
    RtlSecureZeroMemory(J->RxPn,sizeof(J->RxPn));RtlSecureZeroMemory(J->RxPnValid,sizeof(J->RxPnValid));J->KeyHold=J->Authorized=J->PairwiseInstalled=J->GroupInstalled=FALSE;
}
static NTSTATUS Kbegin(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Group,UCHAR Index,const UCHAR *Key,const UCHAR *Rsc,BOOLEAN Delete)
{
    ULONG Slot=Group?Index+1u:0,I;WCN_CIPHER_KEY *K;WCN_KEY_UPDATE *U=&J->KeyUpdate;ULONG64 Floor;
    UNREFERENCED_PARAMETER(D);if(J->State<9||J->Failed||J->Resetting||Index>3||(!Group&&Index))return STATUS_INVALID_DEVICE_STATE;
    if(U->Slot||(!Delete&&Group&&!J->PairwiseInstalled))return STATUS_DEVICE_BUSY;K=&J->Cipher[Slot];
    if(Delete){if(!K->Installed)return STATUS_SUCCESS;}
    else{
        if(!Key||!Rsc)return STATUS_INVALID_PARAMETER;Floor=Kpn(Rsc);
        if(K->Known&&Jeq(K->Key,Key,16)){
            if(!K->Installed)return STATUS_INVALID_DEVICE_STATE; // Never revive a deleted key with a reset PN.
            if(Floor>K->Floor)K->Floor=Floor;for(I=0;I<17;I++)if(J->RxPn[Slot][I]<Floor){J->RxPn[Slot][I]=Floor;J->RxPnValid[Slot][I]=1;}
            return STATUS_SUCCESS; // No hardware install: TX PN and management PN remain untouched.
        }
        for(I=0;I<5;I++)if((J->Cipher[I].Known&&Jeq(J->Cipher[I].Key,Key,16))||
            (J->Cipher[I].PreviousKnown&&Jeq(J->Cipher[I].Previous,Key,16)))return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(U,sizeof(*U));U->Slot=(UCHAR)(Slot+1);U->Delete=Delete;U->ResetPn=!Group&&K->Known;U->At=WcnNowMs();
    if(!Delete){RtlCopyMemory(U->Key,Key,16);RtlCopyMemory(U->Rsc,Rsc,6);}J->KeyHold=!Group;
    if(Delete&&!Group)J->Authorized=FALSE;return STATUS_PENDING;
}
NTSTATUS WcnStage8Key(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Group,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6])
{return Kbegin(D,J,Group,Index,Key,Rsc,FALSE);}
NTSTATUS WcnStage8DeleteKey(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Group,UCHAR Index)
{return Kbegin(D,J,Group,Index,NULL,NULL,TRUE);}
VOID WcnStationKeyComplete(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N)
{
    WCN_KEY_UPDATE *U=&J->KeyUpdate;ULONG Slot=U->Slot?U->Slot-1u:5;
    if(N<24||Slot>4||!U->Sent||U->Ack||Jle32(P)!=J->Vdev||!Jeq(P+4,J->Ap.Bssid,6)||
       Jle32(P+12)!=(Slot?Slot-1u:0)||Jle32(P+16)!=(Slot?1u:0))return;
    if(Jle32(P+20)){Kfail(D,J,"firmware key operation rejected");return;}U->Ack=TRUE;
}
NTSTATUS WcnStationKeysService(PWCN_DEVICE D,PWCN_JOIN J)
{
    WCN_KEY_UPDATE *U=&J->KeyUpdate;WCN_CIPHER_KEY *K;ULONG Slot,I,Offset;NTSTATUS St;struct wmi_vdev_install_key_cmd *C;
    if(!U->Slot)return STATUS_SUCCESS;if(J->Failed||J->Resetting)return STATUS_INVALID_DEVICE_STATE;Slot=U->Slot-1u;K=&J->Cipher[Slot];
    if(WcnNowMs()-U->At>5000)return Kfail(D,J,"live key operation deadline");
    if(!U->Sent){
        St=WcnTwtStop(D,J);if(St==STATUS_DEVICE_BUSY)return St;if(!NT_SUCCESS(St))return Kfail(D,J,"key operation wake failure");
        if(!Slot){
            if(D->Scan.RuntimeScan||J->CsaState||D->MgmtInUse[J->Vdev])return STATUS_DEVICE_BUSY;
            WcnAggregationService(D,J);for(I=0;I<16;I++)if(J->Ba[I].Phase)return STATUS_DEVICE_BUSY;
            for(I=0;I<WCN_TX_SLOTS;I++)if(D->Runtime.TxUsed[I]&&WcnOwnerVdev(D->Runtime.TxUsed[I])==J->Vdev)return STATUS_DEVICE_BUSY;
        }
        RtlZeroMemory(J->Wire,sizeof(J->Wire));C=(void*)J->Wire;JHDR(C,S7_WMI_TAG_VDEV_INSTALL_KEY_CMD);C->vdev_id=J->Vdev;
        RtlCopyMemory(C->peer_macaddr.addr,J->Ap.Bssid,6);C->key_idx=Slot?Slot-1u:0;C->key_flags=Slot?1u:0;C->key_cipher=U->Delete?0:4;C->key_len=U->Delete?0:16;
        if(!U->Delete)RtlCopyMemory(&C->key_rsc_counter,U->Rsc,6);Offset=sizeof(*C)/4;J->Wire[Offset++]=Jtlv(17,U->Delete?0:16);
        if(!U->Delete)RtlCopyMemory(J->Wire+Offset,U->Key,16);St=WcnWmiSend(D,S7_WMI_VDEV_INSTALL_KEY_CMDID,J->Wire,(Offset+(U->Delete?0:4))*4);RtlSecureZeroMemory(J->Wire,sizeof(J->Wire));
        if(St==STATUS_DEVICE_BUSY)return St;if(!NT_SUCCESS(St))return Kfail(D,J,"key operation transport");U->Sent=TRUE;
        return STATUS_PENDING;
    }
    if(!U->Ack||!WcnStage7RxIdle(D))return STATUS_PENDING;
    /* CE ACK is handled before DpPoll. This worker runs after DpPoll, so RX
     * decrypted under the replaced slot is discarded before its PN is reset. */
    if(U->ResetPn&&J->RuntimeReady){
        if(U->ReoSent){
            if(!D->Runtime.ReoPending||D->Runtime.ReoOwner!=J->Vdev||D->Runtime.ReoBase!=J->ReoQueue.La||D->Runtime.ReoTid!=U->Tid)return Kfail(D,J,"key PN completion owner mismatch");
            if(!D->Runtime.ReoDone)return STATUS_PENDING;if(D->Runtime.ReoStatus)return Kfail(D,J,"key PN update rejected");
            D->Runtime.ReoPending=D->Runtime.ReoDone=FALSE;U->ReoSent=FALSE;U->Tid++;
        }
        if(U->Tid<S7_RX_TID_COUNT){St=WcnReoKeyUpdate(D,J,U->Tid,!U->Delete);if(St==STATUS_DEVICE_BUSY)return St;
            if(!NT_SUCCESS(St))return Kfail(D,J,"key PN update transport");U->ReoSent=TRUE;return STATUS_PENDING;}
    }
    if(!U->Delete){
        if(K->Known){RtlCopyMemory(K->Previous,K->Key,16);K->PreviousKnown=TRUE;}RtlCopyMemory(K->Key,U->Key,16);K->Known=K->Installed=TRUE;K->Floor=Kpn(U->Rsc);
        RtlZeroMemory(J->RxPn[Slot],sizeof(J->RxPn[Slot]));RtlZeroMemory(J->RxPnValid[Slot],sizeof(J->RxPnValid[Slot]));
        if(!Slot)J->MgmtRxPn=J->MgmtTxPn=0;
    }else K->Installed=FALSE; // Retain a tombstone until session teardown; no plaintext/key logging.
    Kflags(J);J->KeyHold=FALSE;RtlSecureZeroMemory(U,sizeof(*U));return STATUS_SUCCESS;
}
