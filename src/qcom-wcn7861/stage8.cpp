/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * WiFiCx station: Windows owns network choice, WPA2-CCMP, keys and IP.
 * Firmware MLME supports CN 2.4/5GHz non-DFS 20MHz legacy/ERP operation.
 * Microsoft WDK TLV parser/generator is used; never sample synthetic BSS data. */
#include "stage8-internal.h"
extern "C" {

#include "stage7-wire.h"
#include "network-policy.h"
#include "security-policy.h"
}
static bool Jeq(const UCHAR *A,const UCHAR *B,ULONG N){return RtlCompareMemory(A,B,N)==N;}
static ULONG Jtlv(ULONG T,ULONG N){return (T<<16)|N;}
// The WDK TLV library calls these operators; bounded allocations are zeroed
// before release, including parsed key material. No exceptions or C++ runtime.
struct S8_ALLOC { SIZE_T Length; ULONGLONG Reserved; };
static void *S8Alloc(size_t N)
{
    if(N>1024*1024||N>MAXULONG_PTR-sizeof(S8_ALLOC))return nullptr;
    auto H=(S8_ALLOC*)ExAllocatePool2(POOL_FLAG_NON_PAGED,N+sizeof(S8_ALLOC),WCN_TAG);
    if(!H)return nullptr;H->Length=N;return H+1;
}
static void S8Free(void *P)
{
    if(!P)return;auto H=((S8_ALLOC*)P)-1;SIZE_T N=H->Length;
    RtlSecureZeroMemory(H,N+sizeof(*H));ExFreePoolWithTag(H,WCN_TAG);
}
void * __cdecl operator new(size_t N) {return S8Alloc(N);}
void * __cdecl operator new[](size_t N) {return S8Alloc(N);}
void * __cdecl operator new(size_t N,ULONG_PTR Context) noexcept {return Context?nullptr:S8Alloc(N);}
void * __cdecl operator new[](size_t N,ULONG_PTR Context) noexcept {return Context?nullptr:S8Alloc(N);}
void __cdecl operator delete(void *P) noexcept {S8Free(P);}
void __cdecl operator delete[](void *P) noexcept {S8Free(P);}
void __cdecl operator delete(void *P,size_t) noexcept {S8Free(P);}
void __cdecl operator delete[](void *P,size_t) noexcept {S8Free(P);}
static EVT_WIFI_DEVICE_SEND_COMMAND S8Command;
static EVT_WIFI_DEVICE_CREATE_ADAPTER S8Adapter;
static EVT_WDF_OBJECT_CONTEXT_DESTROY S8Cleanup;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP S8AdapterCleanup;
static EVT_NET_ADAPTER_SET_RECEIVE_FILTER S8ReceiveFilter;
struct S8_HOLDER { S8 *S; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(S8_HOLDER,S8GetHolder);
NTSTATUS S8Indicate(S8 *S,const WDI_MESSAGE_HEADER *Original,UINT16 Id,NTSTATUS Status,const UCHAR *Tlv,ULONG N,bool Unsolicited)
{
    if(N>1024*1024)return STATUS_INVALID_BUFFER_SIZE;
    WDFMEMORY M;UCHAR *P;WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT(&A);A.ParentObject=S->D->Device;
    NTSTATUS St=WdfMemoryCreate(&A,NonPagedPoolNx,WCN_TAG,sizeof(WDI_MESSAGE_HEADER)+N,&M,(void**)&P);if(!NT_SUCCESS(St))return St;
    RtlZeroMemory(P,sizeof(WDI_MESSAGE_HEADER)+N);auto H=(WDI_MESSAGE_HEADER*)P;
    *H=*Original;H->Status=Status;if(Unsolicited)H->TransactionId=0;
    if(N)RtlCopyMemory(P+sizeof(*H),Tlv,N);
    WifiDeviceReceiveIndication(S->D->Device,Id,M);WdfObjectDelete(M);return STATUS_SUCCESS;
}
static void S8M3(WIFIREQUEST R,NTSTATUS St,ULONG Bytes=sizeof(WDI_MESSAGE_HEADER))
{
    UINT In,Out;auto P=(WDI_MESSAGE_HEADER*)WifiRequestGetInOutBuffer(R,&In,&Out);
    if(P&&Out>=sizeof(*P))P->Status=St;else Bytes=0;
    WifiRequestComplete(R,St,Bytes);
}
static NTSTATUS S8Caps(S8 *S)
{
    WIFI_DEVICE_CAPABILITIES D;WIFI_DEVICE_CAPABILITIES_INIT(&D);D.HardwareRadioState=D.SoftwareRadioState=TRUE;
    RtlCopyMemory(D.FirmwareVersion,"WLAN.GNG.1.0-03342",sizeof("WLAN.GNG.1.0-03342"));D.NumRxStreams=D.NumTxStreams=1;D.NumRadios=1;D.SAEAuthenticationSupported=S->PmfAvailable;
    NTSTATUS St=WifiDeviceSetDeviceCapabilities(S->D->Device,&D);if(!NT_SUCCESS(St))return St;
    WIFI_STATION_CAPABILITIES C;WIFI_STATION_CAPABILITIES_INIT(&C);C.ScanSSIDListSize=1;C.DesiredSSIDListSize=1;C.PrivacyExemptionListSize=1;C.KeyMappingTableSize=1;C.DefaultKeyTableSize=4;C.MaxNumPerSTA=1;
    DOT11_AUTH_CIPHER_PAIR Pairs[]={
        {DOT11_AUTH_ALGO_80211_OPEN,DOT11_CIPHER_ALGO_NONE},{DOT11_AUTH_ALGO_RSNA_PSK,DOT11_CIPHER_ALGO_CCMP},
        {DOT11_AUTH_ALGO_RSNA,DOT11_CIPHER_ALGO_CCMP},{DOT11_AUTH_ALGO_WPA3_SAE,DOT11_CIPHER_ALGO_CCMP},
        {DOT11_AUTH_ALGO_WPA3_ENT,DOT11_CIPHER_ALGO_CCMP}};
    DOT11_AUTH_CIPHER_PAIR Mgmt[]={
        {DOT11_AUTH_ALGO_RSNA_PSK,DOT11_CIPHER_ALGO_BIP},{DOT11_AUTH_ALGO_RSNA,DOT11_CIPHER_ALGO_BIP},
        {DOT11_AUTH_ALGO_WPA3_SAE,DOT11_CIPHER_ALGO_BIP},{DOT11_AUTH_ALGO_WPA3_ENT,DOT11_CIPHER_ALGO_BIP}};
    C.MFPCapable=S->PmfAvailable;C.NumSupportedUnicastAlgorithms=C.NumSupportedMulticastDataAlgorithms=S->PmfAvailable?ARRAYSIZE(Pairs):3;
    C.UnicastAlgorithmsList=C.MulticastDataAlgorithmsList=Pairs;
    if(S->PmfAvailable){C.NumSupportedMulticastMgmtAlgorithms=ARRAYSIZE(Mgmt);C.MulticastMgmtAlgorithmsList=Mgmt;}
    // Only AES-CCMP128/BIP-CMAC128 are advertised. No WEP/TKIP/192-bit suites.
    St=WifiDeviceSetStationCapabilities(S->D->Device,&C);if(!NT_SUCCESS(St))return St;
    WDI_PHY_TYPE Types[]={WDI_PHY_TYPE_ERP,WDI_PHY_TYPE_OFDM};UINT32 Width=20;
    WDI_CHANNEL_MAPPING_ENTRY Channels2[13],Channels5[]={{36,5180},{40,5200},{44,5220},{48,5240},{149,5745},{153,5765},{157,5785},{161,5805},{165,5825}};
    for(ULONG I=0;I<13;I++){Channels2[I].ChannelNumber=I+1;Channels2[I].ChannelCenterFrequency=2412+I*5;}
    WIFI_BAND_INFO Info[2]={};
    for(ULONG I=0;I<2;I++){
        Info[I].BandID=I?WDI_BAND_ID_5000:WDI_BAND_ID_2400;Info[I].BandState=TRUE;
        Info[I].NumValidPhyTypes=1;Info[I].ValidPhyTypeList=&Types[I];
        Info[I].NumValidChannelTypes=I?ARRAYSIZE(Channels5):ARRAYSIZE(Channels2);Info[I].ValidChannelTypes=I?Channels5:Channels2;
        Info[I].NumChannelWidths=1;Info[I].ChannelWidthList=&Width;
    }
    WIFI_BAND_CAPABILITIES Bands;WIFI_BAND_CAPABILITIES_INIT(&Bands);Bands.NumBands=2;Bands.BandInfoList=Info;
    St=WifiDeviceSetBandCapabilities(S->D->Device,&Bands);if(!NT_SUCCESS(St))return St;
    WIFI_PHY_INFO Phy[2]={};USHORT Rates[]={2,4,11,22,12,18,24,36,48,72,96,108};
    for(ULONG I=0;I<2;I++){
        Phy[I].PhyType=Types[I];ULONG First=I?4:0;Phy[I].NumberDataRateEntries=ARRAYSIZE(Rates)-First;
        for(ULONG K=First;K<ARRAYSIZE(Rates);K++){Phy[I].DataRateList[K-First].DataRateValue=Rates[K];Phy[I].DataRateList[K-First].DataRateFlag=WDI_DATA_RATE_RX_RATE|WDI_DATA_RATE_TX_RATE;}
    }
    WIFI_PHY_CAPABILITIES P;WIFI_PHY_CAPABILITIES_INIT(&P);P.NumPhyTypes=2;P.PhyInfoList=Phy;
    return WifiDeviceSetPhyCapabilities(S->D->Device,&P);
}
extern "C" NTSTATUS WcnStage8Init(PWDFDEVICE_INIT I)
{NTSTATUS St=NetDeviceInitConfig(I);return NT_SUCCESS(St)?WifiDeviceInitConfig(I):St;}
extern "C" NTSTATUS WcnStage8Create(PWCN_DEVICE D)
{
    WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,S8_HOLDER);A.EvtDestroyCallback=S8Cleanup;S8_HOLDER *H;
    NTSTATUS St=WdfObjectAllocateContext(D->Device,&A,(void**)&H);if(!NT_SUCCESS(St))return St;
    auto S=(S8*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(S8),WCN_TAG);if(!S)return STATUS_INSUFFICIENT_RESOURCES;
    H->S=S;D->Stage8=S;S->D=D;S->Radio=TRUE;KeInitializeSpinLock(&S->Lock);KeInitializeEvent(&S->ReadyEvent,NotificationEvent,FALSE);
    WIFI_DEVICE_CONFIG C;WIFI_DEVICE_CONFIG_INIT(&C,WDI_VERSION_LATEST,S8Command,S8Adapter,nullptr);
    St=WifiDeviceInitialize(D->Device,&C);if(!NT_SUCCESS(St))return St;
    S->Tlv.PeerVersion=WifiDeviceGetOsWdiVersion(D->Device);return STATUS_SUCCESS;
}
extern "C" NTSTATUS WcnStage8Ready(PWCN_DEVICE D)
{
    auto S=(S8*)D->Stage8;LARGE_INTEGER Timeout;Timeout.QuadPart=-150000000LL;
    // Hardware queries require interrupts. Publish capabilities from D0-post,
    // not DeviceAdd, only after the serialized firmware worker has real READY.
    NTSTATUS St=KeWaitForSingleObject(&S->ReadyEvent,Executive,KernelMode,FALSE,&Timeout);
    if(St==STATUS_TIMEOUT)return STATUS_IO_TIMEOUT;
    if(!NT_SUCCESS(St)||!D->Ce.WmiReady)return STATUS_DEVICE_NOT_READY;
    S->PmfAvailable=NT_SUCCESS(WcnPmfInit(D));
    WcnLog(D,"stage8: PMF-CMAC provider ready=%u; SAE/PMF advertised only when ready",S->PmfAvailable);
    St=S8Caps(S);WcnLog(D,"stage8: capabilities published after real firmware READY status=0x%x; Open/WPA2/AES plus OS-SAE/PMF when provider available; CN 2.4/5GHz 20MHz",St);return St;
}
static void S8Cleanup(WDFOBJECT Device)
{
    auto H=S8GetHolder(Device);if(!H->S)return;
    // WDF destroys queue children first; worker joined during D0 exit.
    RtlSecureZeroMemory(H->S,sizeof(S8));ExFreePoolWithTag(H->S,WCN_TAG);H->S=nullptr;
}
extern "C" BOOLEAN WcnStage8ConnectRequested(PWCN_DEVICE D){auto S=(S8*)D->Stage8;return S&&S->Connect&&!S->Stopped&&!S->Resetting&&!S->Fatal;}
void S8SetLink(S8 *S,bool Up)
{
    if(!S->Adapter||S->AdapterGone)return;NET_ADAPTER_LINK_STATE L;
    if(Up)NET_ADAPTER_LINK_STATE_INIT(&L,54000000,MediaConnectStateConnected,MediaDuplexStateHalf,NetAdapterPauseFunctionTypeUnsupported,NetAdapterAutoNegotiationFlagNone);
    else NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&L);
    NetAdapterSetLinkState(S->Adapter,&L);S->LinkUp=Up;
}
static void S8AdapterCleanup(WDFOBJECT A)
{
    auto S=S8GetAdapter(A)->S;if(S){KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);S->AdapterGone=TRUE;S->Stopped=TRUE;S->LinkUp=FALSE;KeReleaseSpinLock(&S->Lock,Irql);}
}
static void S8ReceiveFilter(NETADAPTER Adapter,NETRECEIVEFILTER Filter)
{
    auto S=S8GetAdapter(Adapter)->S;KIRQL Irql;ULONG Count=(ULONG)NetReceiveFilterGetMulticastAddressCount(Filter);
    auto Addresses=NetReceiveFilterGetMulticastAddressList(Filter);
    KeAcquireSpinLock(&S->Lock,&Irql);S->PacketFilter=NetReceiveFilterGetPacketFilter(Filter)&15u;S->MulticastCount=0;
    RtlZeroMemory(S->Multicast,sizeof(S->Multicast));
    if(Count<=32)for(ULONG I=0;I<Count;I++)if(Addresses[I].Length==6&&(Addresses[I].Address[0]&1)){RtlCopyMemory(S->Multicast[S->MulticastCount++],Addresses[I].Address,6);}
    KeReleaseSpinLock(&S->Lock,Irql);
}
static NTSTATUS S8Adapter(WDFDEVICE Device,NETADAPTER_INIT *Init)
{
    auto S=(S8*)WcnGetDevice(Device)->Stage8;
    if(!S||S->Adapter||WifiAdapterInitGetType(Init)!=WIFI_ADAPTER_EXTENSIBLE_STATION)return STATUS_NOT_SUPPORTED;
    if(!S->D->Ce.WmiReady||(S->D->Ce.MacAddress[0]&1))return STATUS_DEVICE_NOT_READY;
    NET_ADAPTER_DATAPATH_CALLBACKS C;NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&C,S8CreateTx,S8CreateRx);NetAdapterInitSetDatapathCallbacks(Init,&C);
    WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,S8_ADAPTER);A.EvtCleanupCallback=S8AdapterCleanup;
    NETADAPTER Adapter;NTSTATUS St=NetAdapterCreate(Init,&A,&Adapter);if(!NT_SUCCESS(St))return St;
    S8GetAdapter(Adapter)->S=S;S->Adapter=Adapter;WdfObjectReference(Adapter);S->AdapterReferenced=TRUE;
    St=WifiAdapterInitialize(Adapter);if(!NT_SUCCESS(St))return St;
    NET_ADAPTER_TX_CAPABILITIES Tx;NET_ADAPTER_TX_CAPABILITIES_INIT(&Tx,1);Tx.MaximumNumberOfFragments=16;
    NET_ADAPTER_RX_CAPABILITIES Rx;NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&Rx,1536,1);
    NET_ADAPTER_LINK_LAYER_CAPABILITIES L;NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&L,54000000,54000000);
    NetAdapterSetLinkLayerCapabilities(Adapter,&L);NetAdapterSetLinkLayerMtuSize(Adapter,1500);NetAdapterSetDataPathCapabilities(Adapter,&Tx,&Rx);
    NET_ADAPTER_LINK_LAYER_ADDRESS Mac;NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&Mac,6,S->D->Ce.MacAddress);
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES Filters;NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&Filters,S8ReceiveFilter);
    Filters.SupportedPacketFilters=(NET_PACKET_FILTER_FLAGS)(NDIS_PACKET_TYPE_DIRECTED|NDIS_PACKET_TYPE_MULTICAST|NDIS_PACKET_TYPE_ALL_MULTICAST|NDIS_PACKET_TYPE_BROADCAST);Filters.MaximumMulticastAddresses=32;
    NetAdapterSetReceiveFilterCapabilities(Adapter,&Filters);
    NetAdapterSetPermanentLinkLayerAddress(Adapter,&Mac);NetAdapterSetCurrentLinkLayerAddress(Adapter,&Mac);S8SetLink(S,false);
    St=NetAdapterStart(Adapter);WcnLog(S->D,"stage8: system WiFiCx adapter start status=0x%x port=%u; OS networking not yet verified",St,WifiAdapterGetPortId(Adapter));return St;
}
static void S8Command(WDFDEVICE Device,WIFIREQUEST Request)
{
    auto S=(S8*)WcnGetDevice(Device)->Stage8;KIRQL Irql;if(!S){S8M3(Request,STATUS_DEVICE_NOT_READY,0);return;}
    KeAcquireSpinLock(&S->Lock,&Irql);ULONG Next=(S->ReqHead+1)%32;
    if(S->Stopped||Next==S->ReqTail){KeReleaseSpinLock(&S->Lock,Irql);S8M3(Request,STATUS_DEVICE_BUSY,0);return;}
    S->Requests[S->ReqHead]=Request;S->ReqHead=Next;KeReleaseSpinLock(&S->Lock,Irql);KeSetEvent(&S->D->WorkEvent,IO_NO_INCREMENT,FALSE);
}
extern "C" VOID WcnStage8Observe(PWCN_DEVICE D,const UCHAR *F,ULONG N,ULONG Frequency,LONG Rssi)
{
    auto S=(S8*)D->Stage8;if(!S||N<36||N>2048)return;
    ULONG Primary=WcnPrimaryFrequency(F,N,Frequency);
    if(!WcnConnectFrequency(Primary))return;
    ULONG I;for(I=0;I<S->BssCount;I++)if(S->Bss[I].Frequency==Primary&&Jeq(S->Bss[I].Bytes+16,F+16,6))break;
    if(I==S->BssCount){
        if(I<ARRAYSIZE(S->Bss))S->BssCount++;
        else{I=0;for(ULONG K=1;K<S->BssCount;K++)if(S->Bss[K].At<S->Bss[I].At)I=K;}
    }
    auto B=&S->Bss[I];B->Length=N;B->Frequency=Primary;B->Rssi=Rssi;B->At=WcnNowMs();RtlCopyMemory(B->Bytes,F,N);
}
extern "C" VOID WcnStage8Mgmt(PWCN_DEVICE D,const UCHAR *F,ULONG N,BOOLEAN Sent)
{
    auto S=(S8*)D->Stage8;if(!S||N<24)return;
    if(Sent&&(F[0]&0xfc)==0&&N<=sizeof(S->AssocRequest)){RtlCopyMemory(S->AssocRequest,F,N);S->AssocRequestSize=N;}
    else if(!Sent&&(F[0]&0xfc)==0x10&&N<=sizeof(S->AssocResponse)){RtlCopyMemory(S->AssocResponse,F,N);S->AssocResponseSize=N;}
    else if(!Sent&&(F[0]&0xfc)==0xb0&&N<=sizeof(S->AuthResponse)){RtlCopyMemory(S->AuthResponse,F,N);S->AuthResponseSize=N;}
}
static NTSTATUS S8BssList(S8 *S,const WDI_MESSAGE_HEADER *H,bool EmptyAllowed=false)
{
    ULONG Count=0;for(ULONG I=0;I<S->BssCount;I++){
        auto B=&S->Bss[I];if(WcnNowMs()-B->At>30000)continue;
        WDI_BSS_ENTRY_CONTAINER E;RtlCopyMemory(E.BSSID.Address,B->Bytes+16,6);
        // WDI requires the management frame body (timestamp through IEs).
        if((B->Bytes[0]&0xfc)==0x80){E.Optional.BeaconFrame_IsPresent=TRUE;E.BeaconFrame.SimpleAssign(B->Bytes+24,B->Length-24);}
        else{E.Optional.ProbeResponseFrame_IsPresent=TRUE;E.ProbeResponseFrame.SimpleAssign(B->Bytes+24,B->Length-24);}
        E.SignalInfo.RSSI=B->Rssi;LONG Q=(B->Rssi+100)*2;E.SignalInfo.LinkQuality=(ULONG)(Q<0?0:(Q>100?100:Q));
        E.ChannelInfo.BandId=WcnIs2g(B->Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;E.ChannelInfo.ChannelNumber=WcnFrequencyChannel(B->Frequency);
        WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS P;P.Optional.DeviceDescriptor_IsPresent=TRUE;P.DeviceDescriptor.SimpleAssign(&E,1);
        UCHAR *Out=nullptr;ULONG N=0;NDIS_STATUS Ns=GenerateWdiIndicationBssEntryListFromIhv(&P,0,&S->Tlv,&N,&Out);
        if(Ns!=NDIS_STATUS_SUCCESS)return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS St=S8Indicate(S,H,WDI_INDICATION_BSS_ENTRY_LIST,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);if(!NT_SUCCESS(St))return St;Count++;
    }
    WcnLog(S->D,"stage8: real firmware BSS cache indicated to Windows entries=%u (no synthetic APs, freshness<=30s)",Count);
    return Count||EmptyAllowed?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
static NTSTATUS S8Authorize(S8 *S)
{
    struct wmi_peer_set_param_cmd C={};C.tlv_header=Jtlv(S7_WMI_TAG_PEER_SET_PARAM_CMD,sizeof(C)-4);
    RtlCopyMemory(C.peer_macaddr.addr,S->D->Join.Ap.Bssid,6);C.param_id=S7_WMI_PEER_AUTHORIZE;C.param_value=1;
    NTSTATUS St=WcnWmiSend(S->D,S7_WMI_PEER_SET_PARAM_CMDID,&C,sizeof(C));if(NT_SUCCESS(St))S->D->Join.Authorized=TRUE;return St;
}
static NTSTATUS S8Association(S8 *S)
{
    auto J=&S->D->Join;if(!S->AssocRequestSize||!S->AssocResponseSize)return STATUS_DEVICE_NOT_READY;
    if(J->Ap.Auth==WCN_AUTH_OPEN&&!J->Authorized){NTSTATUS St=S8Authorize(S);if(!NT_SUCCESS(St))return St;WcnLog(S->D,"stage8: open network controlled port authorized after explicit OS OPEN/NONE selection");}
    WDI_ASSOCIATION_RESULT_CONTAINER A;RtlCopyMemory(A.BSSID.Address,J->Ap.Bssid,6);
    auto P=&A.AssociationResultParameters;P->AssociationStatus=WDI_ASSOC_STATUS_SUCCESS;P->AuthAlgorithm=(WDI_AUTH_ALGORITHM)J->Ap.Auth;
    P->UnicastCipherAlgorithm=P->MulticastDataCipherAlgorithm=J->Ap.Auth==WCN_AUTH_OPEN?WDI_CIPHER_ALGO_NONE:WDI_CIPHER_ALGO_CCMP;P->MulticastMgmtCipherAlgorithm=J->Ap.Pmf?WDI_CIPHER_ALGO_BIP:WDI_CIPHER_ALGO_NONE;P->BandID=WcnIs2g(J->Ap.Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;P->PortAuthorized=J->Ap.Auth==WCN_AUTH_OPEN;
    A.Optional.AssociationRequestFrame_IsPresent=A.Optional.AssociationResponseFrame_IsPresent=TRUE;
    A.AssociationRequestFrame.SimpleAssign(S->AssocRequest+24,S->AssocRequestSize-24);A.AssociationResponseFrame.SimpleAssign(S->AssocResponse+24,S->AssocResponseSize-24);
    if(S->AuthResponseSize>=30){A.Optional.AuthenticationResponseFrame_IsPresent=TRUE;A.AuthenticationResponseFrame.SimpleAssign(S->AuthResponse+24,S->AuthResponseSize-24);}
    for(ULONG I=0;I<S->BssCount;I++)if(S->Bss[I].Length>=36&&Jeq(S->Bss[I].Bytes+16,J->Ap.Bssid,6)){
        A.Optional.BeaconProbeResponse_IsPresent=TRUE;A.BeaconProbeResponse.SimpleAssign(S->Bss[I].Bytes+24,S->Bss[I].Length-24);break;}
    WcnLog(S->D,"stage8: association algorithms auth=%u unicast=%u group=%u port-authorized=%u; OS RX=Native802.11",
        P->AuthAlgorithm,P->UnicastCipherAlgorithm,P->MulticastDataCipherAlgorithm,P->PortAuthorized);
    WDI_PHY_TYPE Type=WcnIs2g(J->Ap.Frequency)?WDI_PHY_TYPE_ERP:WDI_PHY_TYPE_OFDM;A.ActivePhyTypeList.SimpleAssign(&Type,1);
    WDI_INDICATION_ASSOCIATION_RESULT_LIST List;List.AssociationResults.SimpleAssign(&A,1);UCHAR *Out=nullptr;ULONG N;
    NDIS_STATUS Ns=GenerateWdiIndicationAssociationResultFromIhv(&List,0,&S->Tlv,&N,&Out);if(Ns!=NDIS_STATUS_SUCCESS)return STATUS_INSUFFICIENT_RESOURCES;
    /* One shared TX queue: no WifiAdapterInitAddTxDemux was registered.
     * WifiAdapterAddPeer is ONLY for configured peer-address demultiplexing,
     * not the hardware peer/association already confirmed by WMI/HTT.
     * Calling it here exceeds the configured (zero) demux-peer limit. */
    WcnLog(S->D,"stage8: association-result submit begin; shared TX queue, no peer-demux API");
    NTSTATUS St=S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_ASSOCIATION_RESULT,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);
    if(!NT_SUCCESS(St))return St;
    WcnLog(S->D,"stage8: association-result submit returned status0; setting system link");
    S->AssocIndicated=TRUE;S8SetLink(S,true);S->ConnectPending=FALSE;
    WcnLog(S->D,"stage8: system link update returned; submitting connect-complete");
    S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_CONNECT_COMPLETE,STATUS_SUCCESS);
WcnStage8RxReady(S->D);
    WcnLog(S->D,"stage8: actual association indicated; Windows owns the selected security handshake and keys");return STATUS_SUCCESS;
}
/* Clear software staging only under its lock. Never zero/reuse firmware
 * descriptors or drop ownership merely because a control request arrived. */
static void S8ClearStaging(S8 *S)
{
    KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);
    RtlSecureZeroMemory(S->Tx,sizeof(S->Tx));RtlSecureZeroMemory(S->Rx,sizeof(S->Rx));
    S->TxHead=S->TxTail=S->RxHead=S->RxTail=0;S->TxGeneration++;
    S->AssocRequestSize=S->AssocResponseSize=S->AuthResponseSize=0;
    S->AssocIndicated=FALSE;S->SaeRequested=S->SaeAwaitOs=S->SaeConfirmSent=S->SaePeerConfirm=FALSE;S->SaeTxLength=0;RtlSecureZeroMemory(S->SaeTx,sizeof(S->SaeTx));KeReleaseSpinLock(&S->Lock,Irql);
}
static void S8FinishReset(S8 *S,NTSTATUS Status)
{
    auto D=S->D;auto J=&D->Join;
    if(!NT_SUCCESS(Status)){
        S->Fatal=TRUE;J->Failed=TRUE;S->FailureReported=TRUE;
        if(S->ScanPending){S->ScanPending=S->ScanIssued=S->ScanAborting=FALSE;S8Indicate(S,&S->ScanHeader,WDI_INDICATION_SCAN_COMPLETE,Status);}
    }
    else{
        J->Failed=J->Started=J->Resetting=FALSE;J->State=J->Attempts=J->ReoSetupTid=0;J->RoutingSet=FALSE;
        J->Authenticated=J->Associated=J->AssocConfirmed=J->Authorized=FALSE;
        J->PairwiseInstalled=J->GroupInstalled=J->PairwiseSent=J->GroupSent=FALSE;
        J->PeerCreated=J->PeerMapped=J->MgmtBusy=J->VdevStarted=FALSE;J->MacId=0;
        J->FreshReo=J->RuntimeReady;J->H2e=FALSE;
        RtlZeroMemory(J->RxPn,sizeof(J->RxPn));RtlZeroMemory(J->RxPnValid,sizeof(J->RxPnValid));
        S->FailureReported=FALSE;
    }
    WcnPmfClear(D);S->Connect=S->Resetting=FALSE;
    if(S->ResetPending){S->ResetPending=FALSE;S8Indicate(S,&S->ResetHeader,S->ResetMessage==WDI_TASK_DISCONNECT?WDI_INDICATION_DISCONNECT_COMPLETE:WDI_INDICATION_DOT11_RESET_COMPLETE,Status);}
    WcnLog(D,"stage8: session teardown finished status=0x%x fatal=%u; firmware ownership %s",Status,S->Fatal,NT_SUCCESS(Status)?"confirmed stopped/unmapped":"retained, reconnect blocked");
}
static void S8ResetSession(S8 *S)
{
    auto D=S->D;auto J=&D->Join;
    if(S->Resetting)return;
    if(S->AssocIndicated){
        WDI_INDICATION_DISASSOCIATION_PARAMETERS P;RtlCopyMemory(P.DisconnectIndicationParameters.MacAddress.Address,J->Ap.Bssid,6);
        P.DisconnectIndicationParameters.DisassociationWABIReason=WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN;
        P.Optional.NeedPeerStateCleanup_IsPresent=TRUE;P.NeedPeerStateCleanup=FALSE;UCHAR *Out=nullptr;ULONG N=0;
        if(GenerateWdiIndicationDisassociationFromIhv(&P,0,&S->Tlv,&N,&Out)==NDIS_STATUS_SUCCESS){S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_DISASSOCIATION,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
    }
    S8SetLink(S,false);S8ClearStaging(S);
    if(S->KeyRequest){S8M3(S->KeyRequest,STATUS_CANCELLED,0);S->KeyRequest=nullptr;}
    RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));S->KeyCount=0;
    if(S->ConnectPending){S->ConnectPending=FALSE;S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_CONNECT_COMPLETE,STATUS_CONNECTION_DISCONNECTED);}
    S->Connect=FALSE;
    if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed||J->RuntimeErrors){S8FinishReset(S,STATUS_DEVICE_HARDWARE_ERROR);return;}
    if(!J->Started&&!J->PeerCreated&&!J->VdevStarted){S8FinishReset(S,STATUS_SUCCESS);return;}
    S->Resetting=J->Resetting=TRUE;S->ResetStep=0;S->ResetAt=WcnNowMs();
    J->VdevStopConfirmed=J->PeerDeleteConfirmed=J->PeerUnmapped=FALSE;
    WcnLog(D,"stage8: asynchronous teardown starts; down/delete/unmap/stop confirmations required; hardware-owned REO DMA retained");
}
static void S8ResetService(S8 *S)
{
    auto D=S->D;auto J=&D->Join;if(!S->Resetting)return;
    if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed||J->RuntimeErrors){S8FinishReset(S,STATUS_DEVICE_HARDWARE_ERROR);return;}
    WcnStage7DpPoll(D);
    if(J->RuntimeErrors){S8FinishReset(S,STATUS_DEVICE_HARDWARE_ERROR);return;}
    if(WcnNowMs()-S->ResetAt>5000){S8FinishReset(S,STATUS_IO_TIMEOUT);return;}
    NTSTATUS St=STATUS_SUCCESS;
    switch(S->ResetStep){
    case 0:if(J->MgmtBusy)return;if(J->VdevStarted)St=WcnStage7TeardownCommand(D,0);break;
    case 1:if(J->PeerCreated)St=WcnStage7TeardownCommand(D,1);else J->PeerDeleteConfirmed=J->PeerUnmapped=TRUE;break;
    case 2:if(!J->PeerDeleteConfirmed||(!J->PeerUnmapped&&J->PeerMapped))return;break;
    case 3:if(J->VdevStarted)St=WcnStage7TeardownCommand(D,2);else J->VdevStopConfirmed=TRUE;break;
    case 4:
        if(!J->VdevStopConfirmed)return;
        if(!WcnStage7DataIdle(D))return;
        S8FinishReset(S,STATUS_SUCCESS);return;
    default:S8FinishReset(S,STATUS_INVALID_DEVICE_STATE);return;
    }
    if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St)){S8FinishReset(S,St);return;}
    S->ResetStep++;
}
static NTSTATUS S8ParseKeys(S8 *S,WIFIREQUEST R,const UCHAR *P,ULONG N)
{
    if(S->KeyRequest||!S->AssocIndicated||S->D->Join.Failed)return STATUS_INVALID_DEVICE_STATE;
    WDI_SET_ADD_CIPHER_KEYS_PARAMETERS Keys;NDIS_STATUS Ns=ParseWdiSetAddCipherKeysToIhv(N,P,&S->Tlv,&Keys);if(Ns!=NDIS_STATUS_SUCCESS){CleanupParsedWdiSetAddCipherKeys(&Keys);return STATUS_INVALID_PARAMETER;}
    NTSTATUS St=STATUS_SUCCESS;
    if(S->D->Join.Ap.Auth==WCN_AUTH_OPEN||!Keys.SetCipherKey.ElementCount||Keys.SetCipherKey.ElementCount>ARRAYSIZE(S->Keys))St=STATUS_NOT_SUPPORTED;
    for(ULONG I=0;NT_SUCCESS(St)&&I<Keys.SetCipherKey.ElementCount;I++){
        auto K=&Keys.SetCipherKey.pElements[I];auto To=&S->Keys[I];ULONG Index=K->Optional.CipherKeyID_IsPresent?K->CipherKeyID.CipherKeyID:0;
        bool Pair=K->CipherKeyTypeInfo.KeyType==WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY,Management=K->CipherKeyTypeInfo.KeyType==WDI_CIPHER_KEY_TYPE_IGTK;
        auto Direction=K->CipherKeyTypeInfo.Direction;
        if(K->Optional.LinkId_IsPresent||Index>5||(Direction!=WDI_CIPHER_KEY_DIRECTION_BOTH&&Direction!=WDI_CIPHER_KEY_DIRECTION_INBOUND)){St=STATUS_NOT_SUPPORTED;break;}
        if(Pair&&(Direction!=WDI_CIPHER_KEY_DIRECTION_BOTH||!K->Optional.PeerMacAddress_IsPresent||!Jeq(K->PeerMacAddress.Address,S->D->Join.Ap.Bssid,6))){St=STATUS_INVALID_PARAMETER;break;}
        if(Management){
            if(!S->D->Join.Ap.Pmf||Index<4||K->CipherKeyTypeInfo.CipherAlgorithm!=WDI_CIPHER_ALGO_BIP||!K->Optional.BIPKey_IsPresent||K->BIPKey.ElementCount!=16){St=STATUS_NOT_SUPPORTED;break;}
            RtlCopyMemory(To->Bytes,K->BIPKey.pElements,16);
        }else{
            if((!Pair&&K->CipherKeyTypeInfo.KeyType!=WDI_CIPHER_KEY_TYPE_GROUP_KEY)||Index>3||(Pair&&Index)||K->CipherKeyTypeInfo.CipherAlgorithm!=WDI_CIPHER_ALGO_CCMP||!K->Optional.CCMPKey_IsPresent||K->CCMPKey.ElementCount!=16||(Pair?S->D->Join.PairwiseSent:S->D->Join.GroupSent)){St=STATUS_NOT_SUPPORTED;break;}
            RtlCopyMemory(To->Bytes,K->CCMPKey.pElements,16);
        }
        for(ULONG Previous=0;Previous<I;Previous++)if(S->Keys[Previous].Management==(BOOLEAN)Management&&S->Keys[Previous].Group==(BOOLEAN)!Pair&&(!Management||S->Keys[Previous].Index==Index))St=STATUS_INVALID_PARAMETER;
        To->Management=Management;To->Group=!Pair;To->Index=(UCHAR)Index;
        if(K->Optional.ReceiveSequenceCount_IsPresent)RtlCopyMemory(To->Rsc,K->ReceiveSequenceCount.ReceiveSequenceCount,6);
    }
    if(NT_SUCCESS(St)){S->KeyCount=Keys.SetCipherKey.ElementCount;S->KeyRequest=R;S->KeyAt=WcnNowMs();St=STATUS_PENDING;}
    else RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));
    CleanupParsedWdiSetAddCipherKeys(&Keys);return St;
}
static void S8Handle(S8 *S,WIFIREQUEST R)
{
    UINT In=0,Out=0;auto Raw=(UCHAR*)WifiRequestGetInOutBuffer(R,&In,&Out);UINT16 Id=WifiRequestGetMessageId(R);
    if(!Raw||In<sizeof(WDI_MESSAGE_HEADER)||Out<sizeof(WDI_MESSAGE_HEADER)||In>65536){S8M3(R,STATUS_INVALID_PARAMETER,0);return;}
    WDI_MESSAGE_HEADER H=*(WDI_MESSAGE_HEADER*)Raw;auto P=Raw+sizeof(H);ULONG N=In-sizeof(H);NTSTATUS St=STATUS_NOT_SUPPORTED;
    if(S->Fatal){S8M3(R,STATUS_DEVICE_HARDWARE_ERROR);return;}
    if(S->Resetting&&Id!=WDI_GET_STATISTICS&&Id!=WDI_ABORT_TASK){S8M3(R,STATUS_DEVICE_BUSY);return;}
    if(S->Adapter&&H.PortId!=WDI_PORT_ID_ADAPTER&&H.PortId!=WifiAdapterGetPortId(S->Adapter)){S8M3(R,STATUS_NOT_SUPPORTED);return;}
    WcnLog(S->D,"stage8: OS command id=%u transaction=%u bytes=%u (TLV/key values not logged)",Id,H.TransactionId,In);
    if((Id==WDI_TASK_SCAN||Id==WDI_TASK_CONNECT||Id==WDI_SET_ADD_CIPHER_KEYS)&&(!S->Adapter||H.PortId!=WifiAdapterGetPortId(S->Adapter))){S8M3(R,STATUS_NOT_SUPPORTED);return;}
    switch(Id){
    case WDI_TASK_SCAN:{
        WDI_SCAN_PARAMETERS Params;NDIS_STATUS Ns=ParseWdiTaskScanToIhv(N,P,&S->Tlv,&Params);
        St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;CleanupParsedWdiTaskScan(&Params);
        if(!S->Radio||S->ScanPending)St=STATUS_DEVICE_BUSY;
        if(NT_SUCCESS(St)){S->ScanHeader=H;S->ScanPending=TRUE;S->ScanIssued=FALSE;S->ExpectedScanId=0;S->ScanAt=WcnNowMs();}break;}
    case WDI_ABORT_TASK:{
        WDI_TASK_ABORT_PARAMETERS K;auto Ns=ParseWdiAbortTaskToIhv(N,P,&S->Tlv,&K);St=STATUS_NOT_SUPPORTED;
        bool Match=Ns==NDIS_STATUS_SUCCESS&&K.CancelParameters.OriginalTaskOID==OID_WDI_TASK_SCAN&&S->ScanPending&&
            K.CancelParameters.OriginalTransactionId==S->ScanHeader.TransactionId&&K.CancelParameters.OriginalPortId==S->ScanHeader.PortId;
        // WDI_ABORT_TASK is a signal/property, not a task: acknowledge it
        // immediately and complete the ORIGINAL scan only after HW stops.
        St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
        CleanupParsedWdiAbortTaskToIhv(&K);
        if(Match&&S->ScanAborting){S8M3(R,St);return;}
        if(Match&&S->D->Scan.RuntimeScan){if(!S->ScanAborting){S->ScanAborting=TRUE;S->ScanAbortAt=WcnNowMs();}S8M3(R,St);return;}
        S8M3(R,St);
        if(Match){WDI_MESSAGE_HEADER Old=S->ScanHeader;S->ScanPending=S->ScanIssued=FALSE;S8Indicate(S,&Old,WDI_INDICATION_SCAN_COMPLETE,(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED);
            WcnLog(S->D,"stage8: OS scan task cancelled; initial discovery or already-finished scan remains serialized");}return;}
    case WDI_TASK_CONNECT:{
        WDI_TASK_CONNECT_PARAMETERS C;NDIS_STATUS Ns=ParseWdiTaskConnectToIhv(N,P,&S->Tlv,&C);St=STATUS_NOT_SUPPORTED;
        if(Ns==NDIS_STATUS_SUCCESS&&S->Radio&&!S->Connect&&!S->ScanPending&&!S->D->Scan.RuntimeScan&&S->D->Scan.Passed&&C.ConnectParameters.SSIDList.ElementCount==1){
            auto Q=&C.ConnectParameters;auto Name=&Q->SSIDList.pElements[0];WCN_JOIN_AP Selected={};
            ULONG Auth=Q->AuthenticationAlgorithms.ElementCount==1?(ULONG)Q->AuthenticationAlgorithms.pElements[0]:0;
            BOOLEAN Pmf=Q->ConnectionSettings.MFPEnabled;
            ULONG Cipher=Auth==WCN_AUTH_OPEN?WDI_CIPHER_ALGO_NONE:WDI_CIPHER_ALGO_CCMP;
            bool Good=Name->ElementCount>=1&&Name->ElementCount<=32&&WcnAuthSupported(Auth)&&(!Pmf||S->PmfAvailable)&&(!WcnAuthNeedsPmf(Auth)||Pmf)&&
                Q->UnicastCipherAlgorithms.ElementCount==1&&(ULONG)Q->UnicastCipherAlgorithms.pElements[0]==Cipher&&Q->MulticastCipherAlgorithms.ElementCount==1&&(ULONG)Q->MulticastCipherAlgorithms.pElements[0]==Cipher;
            if(Good)for(ULONG I=0;I<C.PreferredBSSEntryList.ElementCount;I++)for(ULONG K=0;K<S->BssCount;K++){
                auto B=&S->Bss[K];WCN_JOIN_AP Candidate;
                if(WcnNowMs()-B->At>30000||!Jeq(C.PreferredBSSEntryList.pElements[I].BSSID.Address,B->Bytes+16,6))continue;
                if(WcnJoinCandidate(S->D,B->Bytes,B->Length,B->Frequency,B->Rssi,Name->pElements,Name->ElementCount,Auth,Pmf,&Candidate)&&(!Selected.Valid||Candidate.Rssi>Selected.Rssi))Selected=Candidate;
            }
            if(Selected.Valid){
                S->D->Join.Ap=Selected;S->D->Join.SsidLength=(UCHAR)Name->ElementCount;RtlZeroMemory(S->D->Join.Ssid,32);RtlCopyMemory(S->D->Join.Ssid,Name->pElements,Name->ElementCount);
                S->Connect=TRUE;S->ConnectPending=TRUE;S->ConnectAt=WcnNowMs();S->ConnectHeader=H;St=STATUS_SUCCESS;
                WcnLog(S->D,"stage8: Windows selected BSSID=%02x:%02x:%02x:%02x:%02x:%02x frequency=%u SSID-bytes=%u; exact OS security choice, no downgrade",Selected.Bssid[0],Selected.Bssid[1],Selected.Bssid[2],Selected.Bssid[3],Selected.Bssid[4],Selected.Bssid[5],Selected.Frequency,Name->ElementCount);
            }
        }
        CleanupParsedWdiTaskConnect(&C);break;}
    case WDI_SET_SAE_AUTH_PARAMS:St=S8SaeCommand(S,&H,P,N);break;
    case WDI_SET_ADD_CIPHER_KEYS:St=S8ParseKeys(S,R,P,N);if(St==STATUS_PENDING)return;break;
    case WDI_SET_DEFAULT_KEY_ID:{WDI_SET_DEFAULT_KEY_ID_PARAMETERS K;auto Ns=ParseWdiSetDefaultKeyIdToIhv(N,P,&S->Tlv,&K);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;CleanupParsedWdiSetDefaultKeyId(&K);break;}
    case WDI_SET_PRIVACY_EXEMPTION_LIST:{WDI_SET_PRIVACY_EXEMPTION_LIST_PARAMETERS K;auto Ns=ParseWdiSetPrivacyExemptionListToIhv(N,P,&S->Tlv,&K);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;CleanupParsedWdiSetPrivacyExemptionList(&K);break;}
    case WDI_SET_RECEIVE_PACKET_FILTER:{WDI_SET_RECEIVE_PACKET_FILTER_PARAMETERS K;auto Ns=ParseWdiSetReceivePacketFilterToIhv(N,P,&S->Tlv,&K);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
        if(NT_SUCCESS(St)){if(K.PacketFilterType&(WDI_PACKET_FILTER_PROMISCUOUS|WDI_PACKET_FILTER_802_11_RAW_DATA))St=STATUS_NOT_SUPPORTED;else{KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);S->PacketFilter=K.PacketFilterType&15u;KeReleaseSpinLock(&S->Lock,Irql);}}
        CleanupParsedWdiSetReceivePacketFilter(&K);break;}
    case WDI_SET_MULTICAST_LIST:{WDI_SET_MULTICAST_LIST_PARAMETERS K;auto Ns=ParseWdiSetMulticastListToIhv(N,P,&S->Tlv,&K);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
        if(NT_SUCCESS(St)){if(K.MulticastList.ElementCount>32)St=STATUS_NOT_SUPPORTED;else{
            for(ULONG I=0;I<K.MulticastList.ElementCount;I++)if(!(K.MulticastList.pElements[I].Address[0]&1))St=STATUS_INVALID_PARAMETER;
            if(NT_SUCCESS(St)){KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);S->MulticastCount=K.MulticastList.ElementCount;RtlZeroMemory(S->Multicast,sizeof(S->Multicast));for(ULONG I=0;I<S->MulticastCount;I++)RtlCopyMemory(S->Multicast[I],K.MulticastList.pElements[I].Address,6);KeReleaseSpinLock(&S->Lock,Irql);}}}
        CleanupParsedWdiSetMulticastList(&K);break;}
    case WDI_SET_CONNECTION_QUALITY:{WDI_SET_CONNECTION_QUALITY_PARAMETERS K;auto Ns=ParseWdiSetConnectionQualityToIhv(N,P,&S->Tlv,&K);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;CleanupParsedWdiSetConnectionQuality(&K);break;}
    case WDI_TASK_SET_RADIO_STATE:{
        WDI_SET_RADIO_STATE_PARAMETERS K;auto Ns=ParseWdiTaskSetRadioStateToIhv(N,P,&S->Tlv,&K);
        St=Ns==NDIS_STATUS_SUCCESS&&K.SoftwareRadioState<=1&&(!S->Connect||K.SoftwareRadioState==1)?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
        UCHAR *RadioTlv=nullptr;ULONG RadioLength=0;BOOLEAN NewRadio=K.SoftwareRadioState!=0;
        if(NT_SUCCESS(St)){WDI_INDICATION_RADIO_STATUS_PARAMETERS RadioStatus;RadioStatus.RadioState.HardwareState=TRUE;RadioStatus.RadioState.SoftwareState=NewRadio;
            if(GenerateWdiIndicationRadioStatusFromIhv(&RadioStatus,0,&S->Tlv,&RadioLength,&RadioTlv)!=NDIS_STATUS_SUCCESS)St=STATUS_INSUFFICIENT_RESOURCES;}
        if(NT_SUCCESS(St))S->Radio=NewRadio;
        CleanupParsedWdiTaskSetRadioState(&K);S8M3(R,St);
        if(NT_SUCCESS(St)){S8Indicate(S,&H,WDI_INDICATION_RADIO_STATUS,St,RadioTlv,RadioLength,true);FreeGenerated(RadioTlv);S8Indicate(S,&H,WDI_INDICATION_SET_RADIO_STATE_COMPLETE,St);}
        return;}
    case WDI_TASK_DISCONNECT:
    case WDI_TASK_DOT11_RESET:
        /* v0.12.6: real bounded reset - cancel the session, stop the firmware
         * vdev and re-arm scans/reconnect; never claim a full MAC re-init. */
        S8M3(R,STATUS_SUCCESS);S->ResetPending=TRUE;S->ResetHeader=H;S->ResetMessage=Id;S8ResetSession(S);return;
    default:break;
    }
    S8M3(R,St);
    if(!NT_SUCCESS(St))WcnLog(S->D,"stage8: command id=%u rejected status=0x%x; no fabricated success",Id,St);
}
extern "C" VOID WcnStage8Service(PWCN_DEVICE D)
{
    auto S=(S8*)D->Stage8;if(!S||S->Stopped)return;
    if(!S->Fatal&&(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed)){
        WcnLog(D,"stage8: fatal hardware state latched; no further firmware commands or automatic reset loop");S8ResetSession(S);
    }
    if(D->Ce.WmiReady)KeSetEvent(&S->ReadyEvent,IO_NO_INCREMENT,FALSE);
    for(ULONG I=0;I<8;I++){KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);if(S->ReqHead==S->ReqTail){KeReleaseSpinLock(&S->Lock,Irql);break;}
        WIFIREQUEST R=S->Requests[S->ReqTail];S->ReqTail=(S->ReqTail+1)%32;KeReleaseSpinLock(&S->Lock,Irql);S8Handle(S,R);}
    S8ResetService(S);
    if(S->Fatal){S8DebugDrain(S);return;}
    if(S->Resetting){S8DebugDrain(S);return;}
    if(S->ScanAborting){
        NTSTATUS St=WcnStage6CancelRescan(D);bool Done=!D->Scan.RuntimeScan;
        if(St==STATUS_DEVICE_BUSY)St=STATUS_SUCCESS;
        if(D->Scan.Failed||WcnNowMs()-S->ScanAbortAt>5000){St=STATUS_IO_TIMEOUT;Done=true;}
        if(!NT_SUCCESS(St)||Done){WDI_MESSAGE_HEADER Old=S->ScanHeader;S->ScanAborting=FALSE;S->ScanPending=S->ScanIssued=FALSE;
            S8Indicate(S,&Old,WDI_INDICATION_SCAN_COMPLETE,NT_SUCCESS(St)?(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED:St);}
    }
    if(S->ScanPending&&!S->ScanAborting){
        NTSTATUS St=STATUS_SUCCESS;bool Finish=false;
        if(!S->Radio||D->Scan.Failed){St=STATUS_DEVICE_NOT_READY;Finish=true;}
        else if(!S->ScanIssued){
            if(!D->Scan.Passed||D->Scan.RuntimeScan){
                // Coalesce with the actual initial/ongoing hardware scan.
                if(D->Scan.ScanSent){S->ExpectedScanId=D->Scan.ScanId;S->ScanIssued=TRUE;}
            }else{
                St=WcnStage6Rescan(D);
                if(St==STATUS_DEVICE_BUSY)St=STATUS_SUCCESS;
                else if(!NT_SUCCESS(St))Finish=true;
                else{S->ExpectedScanId=D->Scan.ScanId;S->ScanIssued=TRUE;S->BssCount=0;}
            }
        }
        if(!Finish&&S->ScanIssued&&D->Scan.ScanId==S->ExpectedScanId&&D->Scan.ScanCompleted&&!D->Scan.RuntimeScan&&D->Scan.State==7){
            St=D->Scan.ScanReason?STATUS_DEVICE_NOT_READY:S8BssList(S,&S->ScanHeader,true);Finish=true;
        }
        if(!Finish&&WcnNowMs()-S->ScanAt>28000){St=STATUS_IO_TIMEOUT;Finish=true;}
        if(Finish){S->ScanPending=S->ScanIssued=FALSE;S8Indicate(S,&S->ScanHeader,WDI_INDICATION_SCAN_COMPLETE,St);}
    }
    if(S->ConnectPending&&D->Join.State>=9&&!D->Join.Failed){NTSTATUS St=S8Association(S);if(!NT_SUCCESS(St)&&St!=STATUS_DEVICE_BUSY)WcnStage7Fail(D,"WiFiCx association-result generation");}
    if(S->KeyRequest){NTSTATUS St=STATUS_SUCCESS;bool Done=true;
        for(ULONG I=0;I<S->KeyCount;I++){auto K=&S->Keys[I];if(!K->Sent){St=K->Management?WcnPmfInstall(D,K->Index,K->Bytes,K->Rsc):WcnStage8Key(D,K->Group,K->Index,K->Bytes,K->Rsc);if(St==STATUS_DEVICE_BUSY){St=STATUS_SUCCESS;Done=false;break;}if(!NT_SUCCESS(St))break;K->Sent=TRUE;RtlSecureZeroMemory(K->Bytes,16);}
            if(!K->Management&&!(K->Group?D->Join.GroupInstalled:D->Join.PairwiseInstalled))Done=false;}
        if(D->Join.Failed||WcnNowMs()-S->KeyAt>2000)St=STATUS_DEVICE_HARDWARE_ERROR;
        if(!NT_SUCCESS(St)||Done){auto R=S->KeyRequest;S->KeyRequest=nullptr;RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));S->KeyCount=0;S8M3(R,St);WcnLog(D,"stage8: Windows key request completed after firmware data-key confirmation / software PMF-key acceptance status=0x%x",St);}}
    if(S->AssocIndicated&&D->Join.PairwiseInstalled&&D->Join.GroupInstalled&&!D->Join.Authorized&&!D->Join.Failed&&
       (!D->Join.Ap.Pmf||D->Join.Igtk[0].Valid||D->Join.Igtk[1].Valid)){
        NTSTATUS St=S8Authorize(S);if(NT_SUCCESS(St))WcnLog(D,"stage8: Windows PTK/GTK both firmware-confirmed; required PMF key accepted; hardware controlled port authorized; OS networking requires separate verification");
        else if(St!=STATUS_DEVICE_BUSY)WcnStage7Fail(D,"OS controlled-port authorization transport");
    }
    if(S->ConnectPending&&WcnNowMs()-S->ConnectAt>24000)WcnStage7Fail(D,"OS connect deadline");
    if(D->Join.Failed&&!S->FailureReported&&!S->Resetting&&!S->Fatal){S->FailureReported=TRUE;WcnLog(D,"stage8: FAIL actual hardware/MLME failure; system link down");S8ResetSession(S);}
    S8QueuesService(S);S8DebugDrain(S);
}
extern "C" VOID WcnStage8Stop(PWCN_DEVICE D)
{
    auto S=(S8*)D->Stage8;if(!S)return;KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);S->Stopped=TRUE;KeReleaseSpinLock(&S->Lock,Irql);
    while(S->ReqTail!=S->ReqHead){auto R=S->Requests[S->ReqTail];S->ReqTail=(S->ReqTail+1)%32;S8M3(R,STATUS_CANCELLED,0);}
    if(S->KeyRequest){S8M3(S->KeyRequest,STATUS_CANCELLED,0);S->KeyRequest=nullptr;}
    RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));S8SetLink(S,false);
    if(S->AdapterReferenced){S->AdapterReferenced=FALSE;WdfObjectDereference(S->Adapter);}
    WcnLog(D,"stage8: stopped OS-TX=%u OS-RX=%u hardware-ACK=%u TX-failed=%u RX-dropped=%u EAPOL-TX=%u EAPOL-RX=%u",S->OsTx,S->OsRx,S->TxAck,S->TxFailed,S->RxDropped,S->EapolTx,S->EapolRx);
}
