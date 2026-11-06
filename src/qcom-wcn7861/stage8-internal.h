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
#define S8_TX_FRAMES (2u*WCN_TX_SLOTS)
#define S8_RX_FRAMES 1024u
struct S8_FRAME { ULONG Length; UCHAR Bytes[1536]; };
struct S8_BSS { ULONG Length,Frequency,ScanId; LONG Rssi; BOOLEAN PrimarySample; LONGLONG At,SignalAt; UCHAR Bytes[2048]; };
struct S8_KEY { BOOLEAN Group,Sent,Management; UCHAR Index,Bytes[16],Rsc[6]; };
struct S8_FT {BOOLEAN Enabled,Reassoc,AwaitOs,Ready;UCHAR Mde[5],Request[512],AssocIes[512];ULONG RequestLength,AssocLength,Attempts;LONGLONG At;};
struct S8 {
    PWCN_DEVICE D;PWCN_JOIN J;BOOLEAN Secondary,Limited;ULONG SecondaryBand; BOOLEAN HotspotRole,P2pRole,WpsEnabled,Provisioning;UCHAR Mac[6]; KSPIN_LOCK Lock; TLV_CONTEXT Tlv; KEVENT ReadyEvent;
    NETADAPTER Adapter;ULONG Port; NETPACKETQUEUE TxQueue[WCN_HOTSPOT_CLIENTS+1],RxQueue;
    BOOLEAN Stopped,Connect,AssocIndicated,LinkUp,FailureReported,Radio,AdapterGone;
    S8_FT Ft;
    BOOLEAN Roaming,RoamPending,ActionPending,ActionSent;WCN_JOIN_AP RoamAp;WDI_MESSAGE_HEADER RoamHeader,ActionHeader;
    ULONG ActionLength,ActionTimeout;UINT16 ActionKind;UCHAR ActionBody[448],ActionDialog;LONGLONG ActionAt,RoamNotifyAt;
    WIFIREQUEST Requests[32]; ULONG ReqHead,ReqTail;
    WCN_SCAN_TARGET ScanTarget;
    BOOLEAN ScanPending,ScanIssued,ConnectPending; ULONG ExpectedScanId,ScanFrequencyCount,ScanFrequency[WCN_SCAN_MAX_CHANNELS]; WDI_MESSAGE_HEADER ScanHeader,ConnectHeader;
    BOOLEAN ScanAborting; NTSTATUS ScanAbortStatus;LONGLONG ScanAbortAt;
    LONGLONG ScanAt,ConnectAt,SignalAt,SignalAttemptAt,ReachabilityAt; WIFIREQUEST KeyRequest; S8_KEY Keys[4]; ULONG KeyCount,KeyNext;BOOLEAN KeyDelete;LONGLONG KeyAt;
    S8_BSS *Bss; ULONG BssCount;
    UCHAR AssocVendor[768];ULONG AssocVendorLength;BOOLEAN AdapterPeer;
    UCHAR AssocRequest[1536],AssocResponse[2048],AuthResponse[1024],ConnectBss[2048]; ULONG AssocRequestSize,AssocResponseSize,AuthResponseSize,ConnectBssSize;
    S8_FRAME Tx[S8_TX_FRAMES],Rx[S8_RX_FRAMES]; ULONG TxHead,TxTail,RxHead,RxTail,TxGeneration;
    ULONG TxFailed,RxDropped;
    BOOLEAN PmfAvailable,SaeRequested,SaeAwaitOs,SaeConfirmSent,SaePeerConfirm;
    ULONG SaeTxLength,SaeAttempts,SaeGroup;LONGLONG SaeAt;UCHAR SaeTx[488];
    USHORT RxSequence;
    BOOLEAN Fatal,Resetting,ResetPending;ULONG ResetStep,ResetMessage;WDI_MESSAGE_HEADER ResetHeader;LONGLONG ResetAt;
    ULONG UnreachableMs,DisabledBands,PacketFilter,MulticastCount;UCHAR Multicast[32][6];
};
// Hold the adapter only while invoking an adapter API, never across PnP stop.
struct S8_ADAPTER_REF {
    NETADAPTER Handle;
    explicit S8_ADAPTER_REF(S8 *S):Handle(nullptr){
        if(!S)return;
        KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);
        if(S->Adapter&&!S->AdapterGone){Handle=S->Adapter;WdfObjectReference(Handle);}
        KeReleaseSpinLock(&S->Lock,Irql);
    }
    ~S8_ADAPTER_REF(){if(Handle)WdfObjectDereference(Handle);}
    S8_ADAPTER_REF(const S8_ADAPTER_REF&)=delete;S8_ADAPTER_REF& operator=(const S8_ADAPTER_REF&)=delete;
};
struct S8_ADAPTER { S8 *S; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S8_ADAPTER,S8GetAdapter);
struct S8_QUEUE { S8 *S; BOOLEAN Rx,Armed,Running; NET_RING_COLLECTION const *Rings; NET_EXTENSION Va; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S8_QUEUE,S8GetQueue);
EVT_NET_ADAPTER_CREATE_TXQUEUE S8CreateTx;
EVT_NET_ADAPTER_CREATE_RXQUEUE S8CreateRx;
void S8QueuesService(S8 *S);
extern "C" VOID WcnStage8RxReady(PWCN_DEVICE Dev,PWCN_JOIN J);
void S8SetLink(S8 *S,bool Up);

NTSTATUS S8SaeIndicate(S8 *S,const WDI_MESSAGE_HEADER &Header,const UCHAR Peer[6],WDI_SAE_INDICATION_TYPE Type,const UCHAR *Body=nullptr,ULONG Length=0);
NTSTATUS S8ParseKeys(S8 *S,WIFIREQUEST R,const UCHAR *P,ULONG N,bool Delete=false);
void S8KeysService(S8 *S);
bool S8ActionCommand(S8 *S,WIFIREQUEST R,const WDI_MESSAGE_HEADER &H,UINT16 Id,const UCHAR *P,ULONG N);
void S8ActionService(S8 *S);
void S8ActionCancel(S8 *S,NTSTATUS Status);
bool S8FtCandidate(S8 *S,const WDI_CONNECT_BSS_ENTRY_CONTAINER &B,const UCHAR *Frame,ULONG N,ULONG Auth,UCHAR *Akm,UCHAR *Mode);
NTSTATUS S8FtPrepare(S8 *S,const WDI_CONNECT_BSS_ENTRY_CONTAINER &B,const WCN_JOIN_AP &A);
NTSTATUS S8FtCommand(S8 *S,const WDI_MESSAGE_HEADER &H,const UCHAR *P,ULONG N);
NTSTATUS S8SaeCommand(S8 *S,const WDI_MESSAGE_HEADER *Header,const UCHAR *Tlv,ULONG Length);

NTSTATUS S8Indicate(S8 *S,const WDI_MESSAGE_HEADER *Original,UINT16 Id,NTSTATUS Status,const UCHAR *Tlv=nullptr,ULONG Length=0,bool Unsolicited=false);

NTSTATUS S8CreateAdapter(S8 *S,NETADAPTER_INIT *Init);
void S8ReceiveEthernet(S8 *S,const UCHAR *F,ULONG N,bool Direct=false);
void S8ClearStaging(S8 *S);
NTSTATUS HotspotCreate(PWCN_DEVICE D);
NTSTATUS HotspotCapabilities(PWCN_DEVICE D);
EVT_WIFI_DEVICE_CREATE_WIFIDIRECTDEVICE HotspotDirectDevice;
S8 *HotspotAdapter(PWCN_DEVICE D);
bool HotspotCommand(S8 *S,WIFIREQUEST R,const WDI_MESSAGE_HEADER &H,UINT16 Id,const UCHAR *P,ULONG N);
void HotspotService(PWCN_DEVICE D);
void HotspotStop(PWCN_DEVICE D);
void HotspotDestroy(PWCN_DEVICE D);
bool HotspotAuthorized(PWCN_DEVICE D,const UCHAR *Peer);

void S8Complete(WIFIREQUEST R,NTSTATUS Status,ULONG Bytes=sizeof(WDI_MESSAGE_HEADER));

NTSTATUS S8Configure(S8 *S,const UCHAR *P,ULONG N,const UCHAR ExpectedMac[6]);

bool P2pCommand(S8 *S,WIFIREQUEST R,const WDI_MESSAGE_HEADER &H,UINT16 Id,const UCHAR *P,ULONG N);
NTSTATUS HotspotReleaseRole(PWCN_DEVICE D);
bool HotspotAdvertBusy(PWCN_DEVICE D);
ULONG HotspotDevicePort(PWCN_DEVICE D);
const UCHAR *HotspotDeviceMac(PWCN_DEVICE D);
void P2pChannel(S8 *S,ULONG Frequency);
ULONG P2pGroupFrequency(PWCN_DEVICE D);
void P2pRoleStopped(PWCN_DEVICE D);
ULONG P2pGroupIes(PWCN_DEVICE D,const UCHAR *Ssid,ULONG SsidLength,UCHAR *Out,ULONG Capacity,bool Wps);
NTSTATUS P2pConnectIes(S8 *S,const WDI_BYTE_BLOB &General,const WDI_BYTE_BLOB *Selected,bool Provisioning);

inline S8 *S8Station(PWCN_DEVICE D,PWCN_JOIN J){return (S8*)(J&&J->Vdev==2?D->SecondaryStage8:J&&J->Vdev==1?D->RoleStage8:D->Stage8);}
