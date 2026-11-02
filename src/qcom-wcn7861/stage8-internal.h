#pragma once
#include <ntddk.h>
#include <wdf.h>
#include <netadaptercx.h>
#include <wificx.h>
#include <net/ringcollection.h>
#include <net/virtualaddress.h>
#include <TLVGeneratorParser.hpp>
extern "C" {
#include "wcn7861.h"
}
struct S8_FRAME { ULONG Length; UCHAR Bytes[1536]; };
struct S8_BSS { ULONG Length,Frequency; LONG Rssi; LONGLONG At; UCHAR Bytes[2048]; };
struct S8_KEY { BOOLEAN Group,Sent,Management; UCHAR Index,Bytes[16],Rsc[6]; };
/* v0.12.7: instrumentation entry. Queue callbacks can run at DISPATCH_LEVEL
 * where WcnLog (PASSIVE-only) is a no-op, so producers only record here under
 * S->Lock and the passive worker drains it via S8DebugDrain. */
struct S8_DBG_ENTRY { ULONG Kind,A,B,C; };
#define S8_DBG_MAX 24
#define S8DBG(S,K,X,Y,Z) do{ S8 *_d=(S); ULONG _h=_d->DbgHead,_n=(_h+1)%S8_DBG_MAX; if(_n!=_d->DbgTail){_d->Dbg[_h].Kind=(ULONG)(K);_d->Dbg[_h].A=(ULONG)(X);_d->Dbg[_h].B=(ULONG)(Y);_d->Dbg[_h].C=(ULONG)(Z);_d->DbgHead=_n;} }while(0)
struct S8 {
    PWCN_DEVICE D; KSPIN_LOCK Lock; TLV_CONTEXT Tlv; KEVENT ReadyEvent;
    NETADAPTER Adapter; NETPACKETQUEUE TxQueue,RxQueue;
    BOOLEAN Stopped,Connect,AssocIndicated,LinkUp,FailureReported,Radio,AdapterReferenced,AdapterGone;
    WIFIREQUEST Requests[32]; ULONG ReqHead,ReqTail;
    BOOLEAN ScanPending,ScanIssued,ConnectPending; ULONG ExpectedScanId; WDI_MESSAGE_HEADER ScanHeader,ConnectHeader;
    BOOLEAN ScanAborting; LONGLONG ScanAbortAt;
    LONGLONG ScanAt,ConnectAt; WIFIREQUEST KeyRequest; S8_KEY Keys[4]; ULONG KeyCount; LONGLONG KeyAt;
    S8_BSS Bss[128]; ULONG BssCount;
    UCHAR AssocRequest[512],AssocResponse[2048],AuthResponse[64]; ULONG AssocRequestSize,AssocResponseSize,AuthResponseSize;
    S8_FRAME Tx[64],Rx[128]; ULONG TxHead,TxTail,RxHead,RxTail,TxGeneration;
    ULONG OsTx,OsRx,TxAck,TxFailed,RxDropped,EapolTx,EapolRx;
    ULONG QueueLogs;
    struct S8_DBG_ENTRY Dbg[S8_DBG_MAX]; ULONG DbgHead,DbgTail;
    BOOLEAN PmfAvailable,SaeRequested,SaeAwaitOs,SaeConfirmSent,SaePeerConfirm;
    ULONG SaeTxLength,SaeAttempts,SaeGroup;LONGLONG SaeAt;UCHAR SaeTx[488];
    USHORT RxSequence;
    BOOLEAN Fatal,Resetting,ResetPending;ULONG ResetStep,ResetMessage;WDI_MESSAGE_HEADER ResetHeader;LONGLONG ResetAt;
    ULONG PacketFilter,MulticastCount;UCHAR Multicast[32][6];
};
struct S8_ADAPTER { S8 *S; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S8_ADAPTER,S8GetAdapter);
struct S8_QUEUE { S8 *S; BOOLEAN Rx,Armed,Running; NET_RING_COLLECTION const *Rings; NET_EXTENSION Va; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S8_QUEUE,S8GetQueue);
EVT_NET_ADAPTER_CREATE_TXQUEUE S8CreateTx;
EVT_NET_ADAPTER_CREATE_RXQUEUE S8CreateRx;
void S8QueuesService(S8 *S);
void S8DebugDrain(S8 *S);
extern "C" VOID WcnStage8RxReady(PWCN_DEVICE Dev);
void S8SetLink(S8 *S,bool Up);

NTSTATUS S8SaeCommand(S8 *S,const WDI_MESSAGE_HEADER *Header,const UCHAR *Tlv,ULONG Length);

NTSTATUS S8Indicate(S8 *S,const WDI_MESSAGE_HEADER *Original,UINT16 Id,NTSTATUS Status,const UCHAR *Tlv=nullptr,ULONG Length=0,bool Unsolicited=false);
