/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * NetAdapterCx owns its rings; the firmware worker owns hardware rings.
 * Bounded copied queues separate those lifetimes. No OS buffer is retained
 * after completion, and no firmware-supplied VA is dereferenced. */
#include "stage8-internal.h"
#include "stage8-frame.h"
#include "security-policy.h"
extern "C" {
#include "hotspot.h"
}
static EVT_PACKET_QUEUE_ADVANCE S8Advance;
static EVT_PACKET_QUEUE_SET_NOTIFICATION_ENABLED S8Notify;
static EVT_PACKET_QUEUE_CANCEL S8Cancel;
static EVT_PACKET_QUEUE_START S8Start;
static EVT_PACKET_QUEUE_STOP S8Stop;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP S8QueueCleanup;
static void S8ReturnAll(S8_QUEUE *Q)
{
    auto P=NetRingCollectionGetPacketRing(Q->Rings);auto F=NetRingCollectionGetFragmentRing(Q->Rings);
    while(P->BeginIndex!=P->EndIndex){auto B=NetRingGetPacketAtIndex(P,P->BeginIndex);B->Ignore=1;B->FragmentCount=0;P->BeginIndex=NetRingIncrementIndex(P,P->BeginIndex);}
    F->BeginIndex=F->EndIndex;
}

static void S8Start(NETPACKETQUEUE Handle)
{
    auto Q=S8GetQueue(Handle);KIRQL Irql;KeAcquireSpinLock(&Q->S->Lock,&Irql);Q->Running=TRUE;KeReleaseSpinLock(&Q->S->Lock,Irql);
}
static void S8Stop(NETPACKETQUEUE Handle)
{
    auto Q=S8GetQueue(Handle);KIRQL Irql;KeAcquireSpinLock(&Q->S->Lock,&Irql);Q->Running=Q->Armed=FALSE;KeReleaseSpinLock(&Q->S->Lock,Irql);
}
static void S8QueueCleanup(WDFOBJECT Handle)
{
    auto Q=S8GetQueue(Handle);KIRQL Irql;KeAcquireSpinLock(&Q->S->Lock,&Irql);
    if(Q->Rx)Q->S->RxQueue=nullptr;else for(auto &Tx:Q->S->TxQueue)if(Tx==Handle)Tx=nullptr;Q->Running=Q->Armed=FALSE;
    KeReleaseSpinLock(&Q->S->Lock,Irql);
}
static void S8Notify(NETPACKETQUEUE Handle,BOOLEAN Enabled)
{
    auto Q=S8GetQueue(Handle);auto S=Q->S;KIRQL Irql;bool Ready;
    KeAcquireSpinLock(&S->Lock,&Irql);Q->Armed=Enabled;
    /* hold OS RX delivery back until the association-result
     * indication is out; EAPOL delivered before that is discarded on hardware
     * (2026-11-01 evidence) and stalls the four-way handshake. Frames are kept
     * and released by WcnStage8RxReady. */
    Ready=Enabled&&Q->Running&&(Q->Rx?(S->AssocIndicated&&S->RxHead!=S->RxTail):(((S->TxHead+1)%S8_TX_FRAMES)!=S->TxTail));
    if(Ready){Q->Armed=FALSE;}
    KeReleaseSpinLock(&S->Lock,Irql);
    if(Ready){if(Q->Rx)NetRxQueueNotifyMoreReceivedPacketsAvailable(Handle);else NetTxQueueNotifyMoreCompletedPacketsAvailable(Handle);}
}
static void S8Cancel(NETPACKETQUEUE Handle)
{
    auto Q=S8GetQueue(Handle);S8Stop(Handle);S8ReturnAll(Q);
    KIRQL Irql;KeAcquireSpinLock(&Q->S->Lock,&Irql);
    if(Q->Rx){RtlSecureZeroMemory(Q->S->Rx,sizeof(Q->S->Rx));Q->S->RxTail=Q->S->RxHead=0;}
    else{RtlSecureZeroMemory(Q->S->Tx,sizeof(Q->S->Tx));Q->S->TxTail=Q->S->TxHead=0;Q->S->TxGeneration++;}
    KeReleaseSpinLock(&Q->S->Lock,Irql);
}
static void S8Advance(NETPACKETQUEUE Handle)
{
    auto Q=S8GetQueue(Handle);auto S=Q->S;auto P=NetRingCollectionGetPacketRing(Q->Rings);auto F=NetRingCollectionGetFragmentRing(Q->Rings);KIRQL Irql;
    KeAcquireSpinLock(&S->Lock,&Irql);
    if(!Q->Running||S->Stopped||S->Resetting||S->Fatal){KeReleaseSpinLock(&S->Lock,Irql);return;}
    while(P->BeginIndex!=P->EndIndex){
        auto B=NetRingGetPacketAtIndex(P,P->BeginIndex);
        if(Q->Rx){
            if(!S->AssocIndicated)break;
            if(S->RxHead==S->RxTail||F->BeginIndex==F->EndIndex)break;
            auto Frag=NetRingGetFragmentAtIndex(F,F->BeginIndex);auto Va=NetExtensionGetFragmentVirtualAddress(&Q->Va,F->BeginIndex)->VirtualAddress;
            auto Data=&S->Rx[S->RxTail];
            RtlZeroMemory(B,sizeof(*B));B->FragmentIndex=F->BeginIndex;B->FragmentCount=1;B->Layout.Layer2Type=NetPacketLayer2TypeIeee80211;B->Layout.Layer2HeaderLength=32;
            if(!Va||Frag->Capacity<Data->Length){B->Ignore=1;Frag->ValidLength=0;S->RxDropped++;}
            else{RtlCopyMemory(Va,Data->Bytes,Data->Length);Frag->Offset=0;Frag->ValidLength=Data->Length;
                 }
            RtlSecureZeroMemory(Data,sizeof(*Data));S->RxTail=(S->RxTail+1)%S8_RX_FRAMES;
            F->BeginIndex=NetRingIncrementIndex(F,F->BeginIndex);
        }else{
            ULONG Next=(S->TxHead+1)%S8_TX_FRAMES,Size=0,Index=B->FragmentIndex,Reason=0;
            bool Native=B->Layout.Layer2Type!=NetPacketLayer2TypeEthernet;
            bool Bad=B->Ignore||!B->FragmentCount||B->FragmentCount>16;
            if(Next==S->TxTail)break;
            if(Bad)Reason=B->Ignore?5:(B->FragmentCount?7:6);
            auto Data=&S->Tx[S->TxHead];
            for(ULONG I=0;!Bad&&I<B->FragmentCount;I++){
                if(Index>=F->NumberOfElements){Bad=true;Reason=1;break;}
                auto Frag=NetRingGetFragmentAtIndex(F,Index);auto Va=NetExtensionGetFragmentVirtualAddress(&Q->Va,Index)->VirtualAddress;
                ULONG Len=(ULONG)Frag->ValidLength,Off=(ULONG)Frag->Offset;
                if(!Va||Off>Frag->Capacity||Len>Frag->Capacity-Off||Len>1532-Size){Bad=true;Reason=!Va?2:(Off>Frag->Capacity?3:(Len>Frag->Capacity-Off?4:8));break;}
                RtlCopyMemory(Data->Bytes+Size,(UCHAR*)Va+Off,Len);Size+=Len;Index=NetRingIncrementIndex(F,Index);
            }
            if(!Bad&&Native){Size=S8NativeTx(Data->Bytes,Size,S->J->Ap.Bssid,S->Mac,S->HotspotRole);if(!Size){Bad=true;Reason=10;}}
            if(!Bad&&Size>1514){Bad=true;Reason=8;}
            if(!Bad&&Size<14){Bad=true;Reason=9;}
            if(!Bad){Data->Length=Size;S->TxHead=Next;
                }
            else{B->Ignore=1;RtlSecureZeroMemory(Data,sizeof(*Data));S->TxFailed++;}
            // Fragments belong to this packet even if its payload was rejected.
            if(B->FragmentIndex<F->NumberOfElements&&B->FragmentCount<=F->NumberOfElements)F->BeginIndex=NetRingAdvanceIndex(F,B->FragmentIndex,B->FragmentCount);
        }
        P->BeginIndex=NetRingIncrementIndex(P,P->BeginIndex);
    }
    KeReleaseSpinLock(&S->Lock,Irql);KeSetEvent(&S->D->WorkEvent,IO_NO_INCREMENT,FALSE);
}
static NTSTATUS S8CreateQueue(NETADAPTER A,void *Init,bool Rx)
{
    auto S=S8GetAdapter(A)->S;WDF_OBJECT_ATTRIBUTES Attr;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr,S8_QUEUE);Attr.EvtCleanupCallback=S8QueueCleanup;
    NETPACKETQUEUE Queue;NTSTATUS St;
    if(Rx){NET_PACKET_QUEUE_CONFIG C;NET_PACKET_QUEUE_CONFIG_INIT(&C,S8Advance,S8Notify,S8Cancel);C.EvtStart=S8Start;C.EvtStop=S8Stop;St=NetRxQueueCreate((NETRXQUEUE_INIT*)Init,&Attr,&C,&Queue);}
    else{NET_PACKET_QUEUE_CONFIG C;NET_PACKET_QUEUE_CONFIG_INIT(&C,S8Advance,S8Notify,S8Cancel);C.EvtStart=S8Start;C.EvtStop=S8Stop;St=NetTxQueueCreate((NETTXQUEUE_INIT*)Init,&Attr,&C,&Queue);}
    if(!NT_SUCCESS(St))return St;
    auto Q=S8GetQueue(Queue);Q->S=S;Q->Rx=Rx;Q->Rings=Rx?NetRxQueueGetRingCollection(Queue):NetTxQueueGetRingCollection(Queue);
    NET_EXTENSION_QUERY Query;NET_EXTENSION_QUERY_INIT(&Query,NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_NAME,NET_FRAGMENT_EXTENSION_VIRTUAL_ADDRESS_VERSION_1,NetExtensionTypeFragment);
    if(Rx)NetRxQueueGetExtension(Queue,&Query,&Q->Va);else NetTxQueueGetExtension(Queue,&Query,&Q->Va);

    KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);if(Rx)S->RxQueue=Queue;else{ULONG I=0;while(I<ARRAYSIZE(S->TxQueue)&&S->TxQueue[I])I++;if(I==ARRAYSIZE(S->TxQueue)){KeReleaseSpinLock(&S->Lock,Irql);WdfObjectDelete(Queue);return STATUS_INSUFFICIENT_RESOURCES;}S->TxQueue[I]=Queue;}KeReleaseSpinLock(&S->Lock,Irql);
    return STATUS_SUCCESS;
}
NTSTATUS S8CreateTx(NETADAPTER A,NETTXQUEUE_INIT *I){return S8CreateQueue(A,I,false);}
NTSTATUS S8CreateRx(NETADAPTER A,NETRXQUEUE_INIT *I){return S8CreateQueue(A,I,true);}
extern "C" VOID WcnStage8Ethernet(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N)
{
    /* EAPOL must reach Windows as soon as the MLME association is
     * confirmed; the WDI association-result indication can lag the AP's M1 by
     * hundreds of ms (hardware evidence 2026-11-01), which used to drop M1 and
     * stall the four-way handshake. Data still requires authorization below.
     * the frame is queued but not announced to the OS before the
     * association-result indication is out (WcnStage8RxReady releases it). */
    if(!WcnTdlsTunnel(D,J,F,N))S8ReceiveEthernet(S8Station(D,J),F,N);
}
extern "C" VOID WcnStage8DirectEthernet(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N)
{if(!WcnTdlsTunnel(D,J,F,N))S8ReceiveEthernet(S8Station(D,J),F,N,true);}
void S8ReceiveEthernet(S8 *S,const UCHAR *F,ULONG N,bool Direct)
{
    if(!S||S->Stopped||S->Resetting||S->Fatal||N<14||N>1514)return;auto D=S->D;
    if(S->HotspotRole){if(!S->LinkUp)return;}
    else{if((!S->AssocIndicated&&!S->J->Associated)||S->J->Failed)return;if(!Direct)S->J->PeerSeenAt=WcnNowMs();}
    bool Eapol=F[12]==0x88&&F[13]==0x8e;
    if(!Eapol&&(S->HotspotRole?!HotspotAuthorized(D,F+6):!S->J->Authorized))return;
    if(Eapol&&((!S->HotspotRole&&RtlCompareMemory(F+6,S->J->Ap.Bssid,6)!=6)||(RtlCompareMemory(F,S->Mac,6)!=6&&RtlCompareMemory(F,"\x01\x80\xc2\x00\x00\x03",6)!=6)))return;
    KIRQL Irql;NETPACKETQUEUE Notify=nullptr;KeAcquireSpinLock(&S->Lock,&Irql);
    if(!Eapol){bool Broadcast=true;for(ULONG I=0;I<6;I++)if(F[I]!=255)Broadcast=false;
        bool Accept=Broadcast?(S->PacketFilter&8)!=0:((F[0]&1)?(S->PacketFilter&4)!=0:(S->PacketFilter&1)!=0);
        if(!Broadcast&&(F[0]&1)&&(S->PacketFilter&2))for(ULONG I=0;I<S->MulticastCount;I++)if(RtlCompareMemory(F,S->Multicast[I],6)==6)Accept=true;
        if(!Accept){KeReleaseSpinLock(&S->Lock,Irql);return;}}
    ULONG Next=(S->RxHead+1)%S8_RX_FRAMES;
    if(Next==S->RxTail){S->RxDropped++;}else{
        auto P=&S->Rx[S->RxHead];P->Length=S8NativeRx(F,N,S->HotspotRole?S->Mac:S->J->Ap.Bssid,!Eapol&&(S->HotspotRole||S->J->Ap.Auth!=WCN_AUTH_OPEN),++S->RxSequence,P->Bytes,sizeof(P->Bytes),S->HotspotRole);
        if(!P->Length){S->RxDropped++;KeReleaseSpinLock(&S->Lock,Irql);return;}S->RxHead=Next;
        if(S->RxQueue){auto Q=S8GetQueue(S->RxQueue);
            if(Q->Running&&Q->Armed&&S->AssocIndicated){Q->Armed=FALSE;Notify=S->RxQueue;WdfObjectReference(Notify);}
            }
    }
    KeReleaseSpinLock(&S->Lock,Irql);
    if(Notify){NetRxQueueNotifyMoreReceivedPacketsAvailable(Notify);WdfObjectDereference(Notify);}
}
/* the association-result indication is out, so the OS now has the
 * peer/port context; release any RX frame that was queued earlier (the AP's
 * M1 arrives before the indication on hardware). PASSIVE caller only. */
extern "C" VOID WcnStage8RxReady(PWCN_DEVICE D,PWCN_JOIN J)
{
    auto S=S8Station(D,J);if(!S)return;KIRQL Irql;NETPACKETQUEUE Notify=nullptr;ULONG Pending;
    KeAcquireSpinLock(&S->Lock,&Irql);
    Pending=(S->RxHead+S8_RX_FRAMES-S->RxTail)%S8_RX_FRAMES;
    if(S->RxQueue&&Pending){auto Q=S8GetQueue(S->RxQueue);if(Q->Running&&Q->Armed){Q->Armed=FALSE;Notify=S->RxQueue;WdfObjectReference(Notify);}}
    KeReleaseSpinLock(&S->Lock,Irql);
    if(Notify){NetRxQueueNotifyMoreReceivedPacketsAvailable(Notify);WdfObjectDereference(Notify);}

}
void S8QueuesService(S8 *S)
{
    bool Freed=false,Submitted=false;
    for(ULONG I=0;I<(S->HotspotRole?WCN_TX_SLOTS/2:WCN_TX_SLOTS);I++){
        S8_FRAME Copy;KIRQL Irql;ULONG Slot,Generation;KeAcquireSpinLock(&S->Lock,&Irql);
        if(S->TxTail==S->TxHead||S->Stopped||S->Resetting||S->Fatal){KeReleaseSpinLock(&S->Lock,Irql);break;}
        Slot=S->TxTail;Generation=S->TxGeneration;Copy.Length=S->Tx[Slot].Length;RtlCopyMemory(Copy.Bytes,S->Tx[Slot].Bytes,Copy.Length);KeReleaseSpinLock(&S->Lock,Irql);
        bool Eapol=Copy.Bytes[12]==0x88&&Copy.Bytes[13]==0x8e;
        // Controlled port only admits EAPOL before Windows installs both keys.
        if(!S->HotspotRole&&!Eapol&&!S->J->Authorized){
            // Never let an early IPv6/DHCP packet head-of-line block EAPOL.
            KeAcquireSpinLock(&S->Lock,&Irql);
            if(S->TxGeneration==Generation&&S->TxTail==Slot&&S->TxTail!=S->TxHead){RtlSecureZeroMemory(&S->Tx[Slot],sizeof(S8_FRAME));S->TxTail=(Slot+1)%S8_TX_FRAMES;S->TxFailed++;Freed=true;
                }
            KeReleaseSpinLock(&S->Lock,Irql);RtlSecureZeroMemory(&Copy,sizeof(Copy));continue;
        }
        NTSTATUS St=S->HotspotRole?WcnHotspotTx(S->D,Copy.Bytes,Copy.Length,Eapol):WcnStage7Tx(S->D,S->J,Copy.Bytes,Copy.Length,Eapol);
        RtlSecureZeroMemory(&Copy,sizeof(Copy));if(St==STATUS_DEVICE_BUSY)break;
        if(NT_SUCCESS(St))Submitted=true;
        KeAcquireSpinLock(&S->Lock,&Irql);
        // Cancel may have emptied the queue while the copied buffer was submitted.
        if(S->TxGeneration==Generation&&S->TxTail==Slot&&S->TxTail!=S->TxHead){RtlSecureZeroMemory(&S->Tx[Slot],sizeof(S8_FRAME));S->TxTail=(Slot+1)%S8_TX_FRAMES;Freed=true;}
        if(!NT_SUCCESS(St)){S->TxFailed++;}

        KeReleaseSpinLock(&S->Lock,Irql);
        if(!NT_SUCCESS(St)&&!S->HotspotRole&&S->J->Started){WcnStage7Fail(S->D,S->J,"OS TX transport rejected");break;}
    }
    if(Submitted)WcnStage7TxFlush(S->D);
    if(Freed){
        NETPACKETQUEUE Notify[WCN_HOTSPOT_CLIENTS+1]={};KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);
        for(ULONG I=0;I<ARRAYSIZE(S->TxQueue);I++)if(S->TxQueue[I]){auto Q=S8GetQueue(S->TxQueue[I]);if(Q->Running&&Q->Armed){Q->Armed=FALSE;Notify[I]=S->TxQueue[I];WdfObjectReference(Notify[I]);}}
        KeReleaseSpinLock(&S->Lock,Irql);
        for(auto Queue:Notify)if(Queue){NetTxQueueNotifyMoreCompletedPacketsAvailable(Queue);WdfObjectDereference(Queue);}
    }
}
extern "C" VOID WcnStage8TxFailed(PWCN_DEVICE D,ULONG Vdev)
{auto S=S8Station(D,WcnStation(D,Vdev));if(S)S->TxFailed++;}
