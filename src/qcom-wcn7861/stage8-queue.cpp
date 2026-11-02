/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * NetAdapterCx owns its rings; the firmware worker owns hardware rings.
 * Bounded copied queues separate those lifetimes. No OS buffer is retained
 * after completion, and no firmware-supplied VA is dereferenced. */
#include "stage8-internal.h"
#include "stage8-frame.h"
#include "security-policy.h"
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
/* v0.12.7: PASSIVE-only drain of the IRQL-safe debug ring. Called from the
 * firmware worker (WcnStage8Service) so lines are written off the queue
 * callbacks, which may run at DISPATCH_LEVEL. */
void S8DebugDrain(S8 *S)
{
    for(ULONG I=0;I<S8_DBG_MAX;I++){
        S8_DBG_ENTRY E;KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);
        if(S->DbgTail==S->DbgHead){KeReleaseSpinLock(&S->Lock,Irql);break;}
        E=S->Dbg[S->DbgTail];S->DbgTail=(S->DbgTail+1)%S8_DBG_MAX;KeReleaseSpinLock(&S->Lock,Irql);
        switch(E.Kind){
        case 1:WcnLog(S->D,"stage8: OS RX copy ok len=%u osrx=%u pending=%u",E.A,E.B,E.C);break;
        case 2:WcnLog(S->D,"stage8: OS RX copy rejected len=%u capacity=%u rxdropped=%u",E.A,E.B,E.C);break;
        case 3:WcnLog(S->D,"stage8: EAPOL kept in staging ring, OS RX notify withheld until association result len=%u eapolrx=%u",E.A,E.B);break;
        case 4:WcnLog(S->D,"stage8: EAPOL queued and OS RX notified len=%u eapolrx=%u",E.A,E.B);break;
        case 5:WcnLog(S->D,"stage8: EAPOL dropped, OS RX staging ring full len=%u rxdropped=%u",E.A,E.B);break;
        case 7:WcnLog(S->D,"stage8: OS TX copy ok len=%u ostx=%u ethertype=0x%04x",E.A,E.B,E.C);break;
        case 8:WcnLog(S->D,"stage8: OS TX copy rejected reason=%u frags=%u size=%u",E.A,E.B,E.C);break;
        case 9:WcnLog(S->D,"stage8: OS TX dropped, controlled port closed len=%u type=0x%04x failed=%u",E.A,E.B,E.C);break;
        case 10:WcnLog(S->D,"stage8: EAPOL TX submitted to firmware type=0x%04x status=0x%x eapoltx=%u",E.A,E.B,E.C);break;
        case 11:WcnLog(S->D,"stage8: queue notify armed rx=%u pending=%u",E.A,E.C);break;
        case 12:WcnLog(S->D,"stage8: OS TX submit failed status=0x%x type=0x%04x failed=%u",E.A,E.B,E.C);break;
        default:WcnLog(S->D,"stage8: debug entry kind=%u",E.Kind);break;}
    }
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
    if(Q->Rx)Q->S->RxQueue=nullptr;else Q->S->TxQueue=nullptr;Q->Running=Q->Armed=FALSE;
    KeReleaseSpinLock(&Q->S->Lock,Irql);
}
static void S8Notify(NETPACKETQUEUE Handle,BOOLEAN Enabled)
{
    auto Q=S8GetQueue(Handle);auto S=Q->S;KIRQL Irql;bool Ready;
    KeAcquireSpinLock(&S->Lock,&Irql);Q->Armed=Enabled;
    /* v0.12.7: hold OS RX delivery back until the association-result
     * indication is out; EAPOL delivered before that is discarded on hardware
     * (2026-11-01 evidence) and stalls the four-way handshake. Frames are kept
     * and released by WcnStage8RxReady. */
    Ready=Enabled&&Q->Running&&(Q->Rx?(S->AssocIndicated&&S->RxHead!=S->RxTail):(((S->TxHead+1)%64)!=S->TxTail));
    if(Ready){Q->Armed=FALSE;if(S->QueueLogs<16){S->QueueLogs++;S8DBG(S,11,Q->Rx?1:2,1,(Q->Rx?(S->RxHead+128-S->RxTail)%128:(S->TxHead+64-S->TxTail)%64));}}
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
            if(!Va||Frag->Capacity<Data->Length){B->Ignore=1;Frag->ValidLength=0;S->RxDropped++;if(S->RxDropped<=4)S8DBG(S,2,Data->Length,(ULONG)Frag->Capacity,S->RxDropped);}
            else{RtlCopyMemory(Va,Data->Bytes,Data->Length);Frag->Offset=0;Frag->ValidLength=Data->Length;S->OsRx++;
                 if(S->OsRx<=4||S->OsRx%256==0)S8DBG(S,1,Data->Length,S->OsRx,(S->RxHead+128-S->RxTail)%128);}
            RtlSecureZeroMemory(Data,sizeof(*Data));S->RxTail=(S->RxTail+1)%128;
            F->BeginIndex=NetRingIncrementIndex(F,F->BeginIndex);
        }else{
            ULONG Next=(S->TxHead+1)%64,Size=0,Index=B->FragmentIndex,Reason=0;
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
            if(!Bad&&Native){Size=S8NativeTx(Data->Bytes,Size,S->D->Join.Ap.Bssid,S->D->Ce.MacAddress);if(!Size){Bad=true;Reason=10;}}
            if(!Bad&&Size>1514){Bad=true;Reason=8;}
            if(!Bad&&Size<14){Bad=true;Reason=9;}
            if(!Bad){Data->Length=Size;S->TxHead=Next;S->OsTx++;
                if(S->OsTx<=4||S->OsTx%256==0)S8DBG(S,7,Size,S->OsTx,((ULONG)Data->Bytes[12]<<8)|Data->Bytes[13]);}
            else{B->Ignore=1;RtlSecureZeroMemory(Data,sizeof(*Data));S->TxFailed++;if(S->TxFailed<=8||(S->TxFailed&63)==0)S8DBG(S,8,Reason,B->FragmentCount,Size);}
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
    WcnLog(S->D,"stage8: %s queue created; Windows link=Native802.11, firmware link=Ethernet",Rx?"RX":"TX");
    KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);if(Rx)S->RxQueue=Queue;else S->TxQueue=Queue;KeReleaseSpinLock(&S->Lock,Irql);
    return STATUS_SUCCESS;
}
NTSTATUS S8CreateTx(NETADAPTER A,NETTXQUEUE_INIT *I){return S8CreateQueue(A,I,false);}
NTSTATUS S8CreateRx(NETADAPTER A,NETRXQUEUE_INIT *I){return S8CreateQueue(A,I,true);}
extern "C" VOID WcnStage8Ethernet(PWCN_DEVICE D,const UCHAR *F,ULONG N)
{
    /* v0.12.6: EAPOL must reach Windows as soon as the MLME association is
     * confirmed; the WDI association-result indication can lag the AP's M1 by
     * hundreds of ms (hardware evidence 2026-11-01), which used to drop M1 and
     * stall the four-way handshake. Data still requires authorization below.
     * v0.12.7: the frame is queued but not announced to the OS before the
     * association-result indication is out (WcnStage8RxReady releases it). */
    auto S=(S8*)D->Stage8;if(!S||S->Stopped||S->Resetting||S->Fatal||(!S->AssocIndicated&&!D->Join.Associated)||D->Join.Failed||N<14||N>1514)return;
    bool Eapol=F[12]==0x88&&F[13]==0x8e;
    if(!Eapol&&!D->Join.Authorized)return;
    if(Eapol&&(RtlCompareMemory(F+6,D->Join.Ap.Bssid,6)!=6||RtlCompareMemory(F,D->Ce.MacAddress,6)!=6&&RtlCompareMemory(F,"\x01\x80\xc2\x00\x00\x03",6)!=6))return;
    KIRQL Irql;NETPACKETQUEUE Notify=nullptr;KeAcquireSpinLock(&S->Lock,&Irql);
    if(!Eapol){bool Broadcast=true;for(ULONG I=0;I<6;I++)if(F[I]!=255)Broadcast=false;
        bool Accept=Broadcast?(S->PacketFilter&8)!=0:((F[0]&1)?(S->PacketFilter&4)!=0:(S->PacketFilter&1)!=0);
        if(!Broadcast&&(F[0]&1)&&(S->PacketFilter&2))for(ULONG I=0;I<S->MulticastCount;I++)if(RtlCompareMemory(F,S->Multicast[I],6)==6)Accept=true;
        if(!Accept){KeReleaseSpinLock(&S->Lock,Irql);return;}}
    ULONG Next=(S->RxHead+1)%128;
    if(Next==S->RxTail){S->RxDropped++;if(Eapol&&S->RxDropped<=4)S8DBG(S,5,N,S->RxDropped,0);}else{
        auto P=&S->Rx[S->RxHead];P->Length=S8NativeRx(F,N,D->Join.Ap.Bssid,!Eapol&&D->Join.Ap.Auth!=WCN_AUTH_OPEN,++S->RxSequence,P->Bytes,sizeof(P->Bytes));
        if(!P->Length){S->RxDropped++;KeReleaseSpinLock(&S->Lock,Irql);return;}S->RxHead=Next;if(Eapol)S->EapolRx++;
        if(S->RxQueue){auto Q=S8GetQueue(S->RxQueue);
            if(Q->Running&&Q->Armed&&S->AssocIndicated){Q->Armed=FALSE;Notify=S->RxQueue;WdfObjectReference(Notify);if(Eapol&&S->EapolRx<=4)S8DBG(S,4,N,S->EapolRx,0);}
            else if(Eapol&&Q->Running&&Q->Armed&&!S->AssocIndicated&&S->EapolRx<=4)S8DBG(S,3,N,S->EapolRx,0);}
    }
    KeReleaseSpinLock(&S->Lock,Irql);
    if(Notify){NetRxQueueNotifyMoreReceivedPacketsAvailable(Notify);WdfObjectDereference(Notify);}
}
/* v0.12.7: the association-result indication is out, so the OS now has the
 * peer/port context; release any RX frame that was queued earlier (the AP's
 * M1 arrives before the indication on hardware). PASSIVE caller only. */
extern "C" VOID WcnStage8RxReady(PWCN_DEVICE D)
{
    auto S=(S8*)D->Stage8;if(!S)return;KIRQL Irql;NETPACKETQUEUE Notify=nullptr;ULONG Pending;
    KeAcquireSpinLock(&S->Lock,&Irql);
    Pending=(S->RxHead+128-S->RxTail)%128;
    if(S->RxQueue&&Pending){auto Q=S8GetQueue(S->RxQueue);if(Q->Running&&Q->Armed){Q->Armed=FALSE;Notify=S->RxQueue;WdfObjectReference(Notify);}}
    KeReleaseSpinLock(&S->Lock,Irql);
    if(Notify){NetRxQueueNotifyMoreReceivedPacketsAvailable(Notify);WdfObjectDereference(Notify);}
    WcnLog(D,"stage8: association result out; deferred OS RX delivery released pending=%u eapolrx=%u",Pending,S->EapolRx);
}
void S8QueuesService(S8 *S)
{
    S8DebugDrain(S);
    for(ULONG I=0;I<16;I++){
        S8_FRAME Copy;KIRQL Irql;ULONG Slot,Generation;KeAcquireSpinLock(&S->Lock,&Irql);
        if(S->TxTail==S->TxHead||S->Stopped||S->Resetting||S->Fatal){KeReleaseSpinLock(&S->Lock,Irql);break;}
        Slot=S->TxTail;Generation=S->TxGeneration;Copy=S->Tx[Slot];KeReleaseSpinLock(&S->Lock,Irql);
        bool Eapol=Copy.Bytes[12]==0x88&&Copy.Bytes[13]==0x8e;
        ULONG Etype=((ULONG)Copy.Bytes[12]<<8)|Copy.Bytes[13];
        // Controlled port only admits EAPOL before Windows installs both keys.
        if(!Eapol&&!S->D->Join.Authorized){
            // Never let an early IPv6/DHCP packet head-of-line block EAPOL.
            KeAcquireSpinLock(&S->Lock,&Irql);
            if(S->TxGeneration==Generation&&S->TxTail==Slot&&S->TxTail!=S->TxHead){RtlSecureZeroMemory(&S->Tx[Slot],sizeof(S8_FRAME));S->TxTail=(Slot+1)%64;S->TxFailed++;
                if(S->TxFailed<=8||(S->TxFailed&63)==0)S8DBG(S,9,Copy.Length,Etype,S->TxFailed);}
            KeReleaseSpinLock(&S->Lock,Irql);RtlSecureZeroMemory(&Copy,sizeof(Copy));continue;
        }
        NTSTATUS St=WcnStage7Tx(S->D,Copy.Bytes,Copy.Length,Eapol,Eapol?32:33);
        RtlSecureZeroMemory(&Copy,sizeof(Copy));if(St==STATUS_DEVICE_BUSY)break;
        KeAcquireSpinLock(&S->Lock,&Irql);
        // Cancel may have emptied the queue while the copied buffer was submitted.
        if(S->TxGeneration==Generation&&S->TxTail==Slot&&S->TxTail!=S->TxHead){RtlSecureZeroMemory(&S->Tx[Slot],sizeof(S8_FRAME));S->TxTail=(Slot+1)%64;}
        if(!NT_SUCCESS(St)){S->TxFailed++;if(S->TxFailed<=8||(S->TxFailed&63)==0)S8DBG(S,12,Etype,(ULONG)St,S->TxFailed);}
        else if(Eapol){S->EapolTx++;if(S->EapolTx<=8)S8DBG(S,10,Etype,(ULONG)St,S->EapolTx);}
        NETPACKETQUEUE Notify=nullptr;if(S->TxQueue){auto Q=S8GetQueue(S->TxQueue);if(Q->Running&&Q->Armed){Q->Armed=FALSE;Notify=S->TxQueue;WdfObjectReference(Notify);}}
        KeReleaseSpinLock(&S->Lock,Irql);
        if(Notify){NetTxQueueNotifyMoreCompletedPacketsAvailable(Notify);WdfObjectDereference(Notify);}
        if(!NT_SUCCESS(St)&&S->D->Join.Started)WcnStage7Fail(S->D,"OS TX transport rejected");
    }
}
extern "C" VOID WcnStage8TxDone(PWCN_DEVICE D,UCHAR Purpose,BOOLEAN Ack)
{
    auto S=(S8*)D->Stage8;if(!S||Purpose<32)return;
    if(Ack)S->TxAck++;else S->TxFailed++;
}
