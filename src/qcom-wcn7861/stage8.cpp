/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * WiFiCx station: Windows owns network choice, authentication, keys and IP.
 * Firmware MLME negotiates board/regulatory-limited 2.4/5GHz HT/VHT/HE/EHT.
 * Microsoft WDK TLV parser/generator is used; never sample synthetic BSS data. */
#include "stage8-internal.h"
extern "C" {

#include "stage7-wire.h"
#include "stage7-util.h"
#include "network-policy.h"
#include "security-policy.h"
#include "hotspot.h"
}
#include "p2p-wire.h"
NTSTATUS S8Configure(S8 *S,const UCHAR *P,ULONG N,const UCHAR ExpectedMac[6])
{
    WDI_SET_FIRMWARE_CONFIGURATION_PARAMETERS C;auto Ns=ParseWdiSetAdapterConfigurationToIhv(N,P,&S->Tlv,&C);NTSTATUS St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;ULONG Mask=0;
    if(NT_SUCCESS(St)&&C.Optional.MacAddress_IsPresent&&!Jeq(C.MacAddress.Address,ExpectedMac,6))St=STATUS_NOT_SUPPORTED;
    if(NT_SUCCESS(St)&&C.Optional.DisallowedBandList_IsPresent)for(ULONG I=0;I<C.DisallowedBandList.ElementCount;I++){
        auto B=C.DisallowedBandList.pElements[I];if(B==WDI_BAND_ID_2400)Mask|=1;else if(B==WDI_BAND_ID_5000)Mask|=2;else{St=STATUS_NOT_SUPPORTED;break;}}
    if(NT_SUCCESS(St)&&S->Connect&&(Mask&(WcnIs2g(S->J->Ap.Frequency)?1:2)))St=STATUS_DEVICE_BUSY;
    // The driver reports continuous 0..100 quality; Windows owns the supplied bar map.
    // PLDR/GO-reset permissions never trigger an unsolicited reset in this driver.
    if(NT_SUCCESS(St)){S->DisabledBands=Mask;if(C.Optional.UnreachableDetectionThreshold_IsPresent)S->UnreachableMs=C.UnreachableDetectionThreshold;}

    CleanupParsedWdiSetAdapterConfigurationToIhv(&C);return St;
}
static bool S8CipherAllowed(const WDI_CIPHER_ALGORITHM_LIST_CONTAINER &List,ULONG Cipher)
{for(ULONG I=0;I<List.ElementCount;I++)if((ULONG)List.pElements[I]==Cipher)return true;return false;}
static UCHAR S8Quality(LONG Rssi){LONG Q=(Rssi+100)*2;return (UCHAR)(Q<0?0:Q>100?100:Q);}
// The WDK TLV library calls these operators; bounded allocations are zeroed
// before release, including parsed key material. No exceptions or C++ runtime.
// Keep a 16-byte prefix so generated TLV buffers retain allocation alignment.
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
    if(S->AdapterGone&&Original->PortId!=WDI_PORT_ID_ADAPTER)return STATUS_DELETE_PENDING;
    if(N>1024*1024)return STATUS_INVALID_BUFFER_SIZE;
    WDFMEMORY M;UCHAR *P;WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT(&A);A.ParentObject=S->D->Device;
    NTSTATUS St=WdfMemoryCreate(&A,NonPagedPoolNx,WCN_TAG,sizeof(WDI_MESSAGE_HEADER)+N,&M,(void**)&P);if(!NT_SUCCESS(St))return St;
    RtlZeroMemory(P,sizeof(WDI_MESSAGE_HEADER)+N);auto H=(WDI_MESSAGE_HEADER*)P;
    *H=*Original;H->Status=Status;if(Unsolicited)H->TransactionId=0;
    if(N)RtlCopyMemory(P+sizeof(*H),Tlv,N);
    WifiDeviceReceiveIndication(S->D->Device,Id,M);WdfObjectDelete(M);return STATUS_SUCCESS;
}
void S8Complete(WIFIREQUEST R,NTSTATUS St,ULONG Bytes)
{
    UINT In,Out;auto P=(WDI_MESSAGE_HEADER*)WifiRequestGetInOutBuffer(R,&In,&Out);
    if(P&&Out>=sizeof(*P))P->Status=St;else Bytes=0;
    WifiRequestComplete(R,St,Bytes);
}
static NTSTATUS S8Caps(S8 *S)
{
    WIFI_DEVICE_CAPABILITIES D;WIFI_DEVICE_CAPABILITIES_INIT(&D);D.HardwareRadioState=D.SoftwareRadioState=TRUE;D.ActionFramesSupported=TRUE;D.FastTransitionSupported=TRUE;
    RtlCopyMemory(D.FirmwareVersion,"WLAN.GNG.1.0-03342",sizeof("WLAN.GNG.1.0-03342"));D.NumRxStreams=D.NumTxStreams=S->D->Ce.NumRfChains;D.NumRadios=2;D.SAEAuthenticationSupported=S->PmfAvailable;
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
    WIFI_STA_BANDS_COMBINATION Dual={};Dual.NumStaBands=2;Dual.BandIDs[0]=WDI_BAND_ID_2400;Dual.BandIDs[1]=WDI_BAND_ID_5000;C.NumSecondaryStaBandCombinations=1;C.SecondaryStaBandsCombinations=&Dual;
    St=WifiDeviceSetStationCapabilities(S->D->Device,&C);if(!NT_SUCCESS(St))return St;
    St=HotspotCapabilities(S->D);if(!NT_SUCCESS(St))return St;
    const WCN_RADIO_CAP *Radio=WcnRadioCaps(S->D);
    bool Ht=(S->D->Ce.HtCap&1)!=0,Vht=Ht&&S->D->Ce.VhtCap!=0;
    bool He=Ht&&Radio&&Radio->HeMcs[1]!=0,Eht=He&&Radio->EhtValid&&Radio->EhtMcs[1][1]!=0;
    WDI_PHY_TYPE Types[]={WDI_PHY_TYPE_HRDSSS,WDI_PHY_TYPE_ERP,WDI_PHY_TYPE_OFDM,WDI_PHY_TYPE_HT,WDI_PHY_TYPE_VHT,WDI_PHY_TYPE_HE,WDI_PHY_TYPE_EHT};
    WDI_PHY_TYPE Types2[]={WDI_PHY_TYPE_HRDSSS,WDI_PHY_TYPE_ERP,WDI_PHY_TYPE_HT,WDI_PHY_TYPE_HE,WDI_PHY_TYPE_EHT},Types5[]={WDI_PHY_TYPE_OFDM,WDI_PHY_TYPE_HT,WDI_PHY_TYPE_VHT,WDI_PHY_TYPE_HE,WDI_PHY_TYPE_EHT};
    UINT32 Widths[]={20,40,80,160};ULONG PhyCount=3+(Ht?1:0)+(Vht?1:0)+(He?1:0)+(Eht?1:0);
    WDI_CHANNEL_MAPPING_ENTRY Channels2[13],Channels5[]={{36,5180},{40,5200},{44,5220},{48,5240},{52,5260},{56,5280},{60,5300},{64,5320},{149,5745},{153,5765},{157,5785},{161,5805},{165,5825}};
    for(ULONG I=0;I<13;I++){Channels2[I].ChannelNumber=I+1;Channels2[I].ChannelCenterFrequency=2412+I*5;}
    WIFI_BAND_INFO Info[2]={};
    for(ULONG I=0;I<2;I++){
        Info[I].BandID=I?WDI_BAND_ID_5000:WDI_BAND_ID_2400;Info[I].BandState=TRUE;
        Info[I].NumValidPhyTypes=(I?1:2)+(Ht?1:0)+(I&&Vht?1:0)+(He?1:0)+(Eht?1:0);Info[I].ValidPhyTypeList=I?Types5:Types2;
        Info[I].NumValidChannelTypes=I?ARRAYSIZE(Channels5):ARRAYSIZE(Channels2);Info[I].ValidChannelTypes=I?Channels5:Channels2;
        Info[I].NumChannelWidths=1+(Ht?1:0)+(I&&Vht?1:0)+(I&&Radio&&Radio->WidthCode[1]>=3?1:0);Info[I].ChannelWidthList=Widths;
    }
    WIFI_BAND_CAPABILITIES Bands;WIFI_BAND_CAPABILITIES_INIT(&Bands);Bands.NumBands=2;Bands.BandInfoList=Info;
    St=WifiDeviceSetBandCapabilities(S->D->Device,&Bands);if(!NT_SUCCESS(St))return St;
    WIFI_PHY_INFO Phy[7]={};USHORT Rates[]={2,4,11,22,12,18,24,36,48,72,96,108};
    for(ULONG I=0;I<PhyCount;I++){
        Phy[I].PhyType=Types[I];ULONG First=I>=2?4:0;Phy[I].NumberDataRateEntries=I?ARRAYSIZE(Rates)-First:4;
        for(ULONG K=First;K<First+Phy[I].NumberDataRateEntries;K++){Phy[I].DataRateList[K-First].DataRateValue=Rates[K];Phy[I].DataRateList[K-First].DataRateFlag=WDI_DATA_RATE_RX_RATE|WDI_DATA_RATE_TX_RATE;}
    }
    WIFI_PHY_CAPABILITIES P;WIFI_PHY_CAPABILITIES_INIT(&P);P.NumPhyTypes=PhyCount;P.PhyInfoList=Phy;
    return WifiDeviceSetPhyCapabilities(S->D->Device,&P);
}
extern "C" NTSTATUS WcnStage8Init(PWDFDEVICE_INIT I)
{NTSTATUS St=NetDeviceInitConfig(I);return NT_SUCCESS(St)?WifiDeviceInitConfig(I):St;}
extern "C" NTSTATUS WcnStage8Create(PWCN_DEVICE D)
{
    WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,S8_HOLDER);A.EvtDestroyCallback=S8Cleanup;S8_HOLDER *H;
    NTSTATUS St=WdfObjectAllocateContext(D->Device,&A,(void**)&H);if(!NT_SUCCESS(St))return St;
    auto S=(S8*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(S8),WCN_TAG);if(!S)return STATUS_INSUFFICIENT_RESOURCES;
    H->S=S;S->D=D;D->Stage8=S;S->Bss=(S8_BSS*)ExAllocatePool2(POOL_FLAG_NON_PAGED,128*sizeof(S8_BSS),WCN_TAG);if(!S->Bss)return STATUS_INSUFFICIENT_RESOURCES;S->J=&D->Join;D->Secondary.Vdev=2;D->P2pJoin.Vdev=1;S->Radio=TRUE;KeInitializeSpinLock(&S->Lock);KeInitializeEvent(&S->ReadyEvent,NotificationEvent,FALSE);
    WIFI_DEVICE_CONFIG C;WIFI_DEVICE_CONFIG_INIT(&C,WDI_VERSION_LATEST,S8Command,S8Adapter,HotspotDirectDevice);
    St=HotspotCreate(D);if(!NT_SUCCESS(St))return St;
    St=WcnP2pCreate(D);if(!NT_SUCCESS(St))return St;
    St=WcnTdlsCreate(D);if(!NT_SUCCESS(St))return St;
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
    S->PmfAvailable=NT_SUCCESS(WcnPmfInit(D,S->J));

    St=S8Caps(S);return St;
}
static void S8Cleanup(WDFOBJECT Device)
{
    auto H=S8GetHolder(Device);if(!H->S)return;
    // WDF destroys queue children first; worker joined during D0 exit.
    WcnP2pDestroy(H->S->D);HotspotDestroy(H->S->D);
    WcnTdlsDestroy(H->S->D);
    auto D=H->S->D;auto Secondary=(S8*)D->SecondaryStage8;if(Secondary){RtlSecureZeroMemory(Secondary,sizeof(S8));ExFreePoolWithTag(Secondary,WCN_TAG);D->SecondaryStage8=nullptr;}
    if(H->S->Bss)ExFreePoolWithTag(H->S->Bss,WCN_TAG);RtlSecureZeroMemory(H->S,sizeof(S8));ExFreePoolWithTag(H->S,WCN_TAG);H->S=nullptr;
}
extern "C" BOOLEAN WcnStage8ConnectRequested(PWCN_DEVICE D,PWCN_JOIN J){auto S=S8Station(D,J);return S&&S->Connect&&!S->Stopped&&!S->Resetting&&!S->Fatal;}
void S8SetLink(S8 *S,bool Up)
{
    S8_ADAPTER_REF A(S);if(!A.Handle)return;NET_ADAPTER_LINK_STATE L;
    if(Up)NET_ADAPTER_LINK_STATE_INIT(&L,(ULONG64)(S->HotspotRole?WcnHotspotRate(S->D):S->J->Ap.RateKbps)*1000,MediaConnectStateConnected,MediaDuplexStateHalf,NetAdapterPauseFunctionTypeUnsupported,NetAdapterAutoNegotiationFlagNone);
    else NET_ADAPTER_LINK_STATE_INIT_DISCONNECTED(&L);
    if(Up&&!S->LinkUp)S->ReachabilityAt=WcnNowMs();NetAdapterSetLinkState(A.Handle,&L);S->LinkUp=Up;
}
static void S8AdapterCleanup(WDFOBJECT A)
{
    auto S=S8GetAdapter(A)->S;if(S){KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);S->AdapterGone=TRUE;S->Stopped=TRUE;S->LinkUp=FALSE;KeReleaseSpinLock(&S->Lock,Irql);
        KeSetEvent(&S->D->WorkEvent,IO_NO_INCREMENT,FALSE);}
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
    auto D=WcnGetDevice(Device);auto Type=WifiAdapterInitGetType(Init);auto S=(S8*)D->Stage8;
    if(Type==WIFI_ADAPTER_WIFI_DIRECT_ROLE)S=HotspotAdapter(D);
    else if(Type==WIFI_ADAPTER_SECONDARY_STATION){
        S=(S8*)D->SecondaryStage8;
        if(S&&S->Adapter){bool Idle=S->AdapterGone&&!S->RxQueue&&!S->J->Started&&!S->J->Resetting;for(auto Q:S->TxQueue)if(Q)Idle=false;if(!Idle)return STATUS_DEVICE_BUSY;
            S->Adapter=nullptr;S8ClearStaging(S);S->Stopped=S->Fatal=S->AdapterGone=S->FailureReported=S->ResetPending=FALSE;}
        if(!S){S=(S8*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(S8),WCN_TAG);if(!S)return STATUS_INSUFFICIENT_RESOURCES;
            S->D=D;S->J=&D->Secondary;S->Secondary=S->Radio=TRUE;S->Tlv=((S8*)D->Stage8)->Tlv;KeInitializeSpinLock(&S->Lock);KeInitializeEvent(&S->ReadyEvent,NotificationEvent,FALSE);S->PmfAvailable=NT_SUCCESS(WcnPmfInit(D,S->J));D->SecondaryStage8=S;}
        RtlCopyMemory(S->Mac,D->Ce.MacAddress,6);S->Mac[0]|=2;S->Mac[5]^=14;
    }else if(Type!=WIFI_ADAPTER_EXTENSIBLE_STATION)return STATUS_NOT_SUPPORTED;
    if(!S||S->Adapter)return STATUS_NOT_SUPPORTED;if(!D->Ce.WmiReady)return STATUS_DEVICE_NOT_READY;
    if(!S->HotspotRole){if(!S->Secondary)RtlCopyMemory(S->Mac,D->Ce.MacAddress,6);RtlCopyMemory(S->J->Mac,S->Mac,6);}
    return S8CreateAdapter(S,Init);
}
NTSTATUS S8CreateAdapter(S8 *S,NETADAPTER_INIT *Init)
{
    if(S->P2pRole){WIFI_ADAPTER_TX_DEMUX Peer;WIFI_ADAPTER_TX_PEER_ADDRESS_DEMUX_INIT(&Peer,WCN_HOTSPOT_CLIENTS);WifiAdapterInitAddTxDemux(Init,&Peer);}
    NET_ADAPTER_DATAPATH_CALLBACKS C;NET_ADAPTER_DATAPATH_CALLBACKS_INIT(&C,S8CreateTx,S8CreateRx);NetAdapterInitSetDatapathCallbacks(Init,&C);
    WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,S8_ADAPTER);A.EvtCleanupCallback=S8AdapterCleanup;
    NETADAPTER Adapter;NTSTATUS St=NetAdapterCreate(Init,&A,&Adapter);if(!NT_SUCCESS(St))return St;
    S8GetAdapter(Adapter)->S=S;S->Adapter=Adapter;
    St=WifiAdapterInitialize(Adapter);if(!NT_SUCCESS(St))return St;S->Port=WifiAdapterGetPortId(Adapter);
    NET_ADAPTER_TX_CAPABILITIES Tx;NET_ADAPTER_TX_CAPABILITIES_INIT(&Tx,S->P2pRole?WCN_HOTSPOT_CLIENTS+1:1);Tx.MaximumNumberOfFragments=16;
    NET_ADAPTER_RX_CAPABILITIES Rx;NET_ADAPTER_RX_CAPABILITIES_INIT_SYSTEM_MANAGED(&Rx,1536,1);
    ULONG64 MaxRate=(ULONG64)WcnMaximumRate(S->D)*1000;
    NET_ADAPTER_LINK_LAYER_CAPABILITIES L;NET_ADAPTER_LINK_LAYER_CAPABILITIES_INIT(&L,MaxRate,MaxRate);
    NetAdapterSetLinkLayerCapabilities(Adapter,&L);NetAdapterSetLinkLayerMtuSize(Adapter,1500);NetAdapterSetDataPathCapabilities(Adapter,&Tx,&Rx);
    NET_ADAPTER_LINK_LAYER_ADDRESS Mac;NET_ADAPTER_LINK_LAYER_ADDRESS_INIT(&Mac,6,S->Mac);
    NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES Filters;NET_ADAPTER_RECEIVE_FILTER_CAPABILITIES_INIT(&Filters,S8ReceiveFilter);
    Filters.SupportedPacketFilters=(NET_PACKET_FILTER_FLAGS)(NDIS_PACKET_TYPE_DIRECTED|NDIS_PACKET_TYPE_MULTICAST|NDIS_PACKET_TYPE_ALL_MULTICAST|NDIS_PACKET_TYPE_BROADCAST);Filters.MaximumMulticastAddresses=32;
    NetAdapterSetReceiveFilterCapabilities(Adapter,&Filters);
    NetAdapterSetPermanentLinkLayerAddress(Adapter,&Mac);NetAdapterSetCurrentLinkLayerAddress(Adapter,&Mac);S8SetLink(S,false);
    St=NetAdapterStart(Adapter);return St;
}
static void S8Command(WDFDEVICE Device,WIFIREQUEST Request)
{
    auto S=(S8*)WcnGetDevice(Device)->Stage8;KIRQL Irql;if(!S){S8Complete(Request,STATUS_DEVICE_NOT_READY,0);return;}
    KeAcquireSpinLock(&S->Lock,&Irql);ULONG Next=(S->ReqHead+1)%32;
    if(S->Stopped||Next==S->ReqTail){KeReleaseSpinLock(&S->Lock,Irql);S8Complete(Request,STATUS_DEVICE_BUSY,0);return;}
    S->Requests[S->ReqHead]=Request;S->ReqHead=Next;KeReleaseSpinLock(&S->Lock,Irql);KeSetEvent(&S->D->WorkEvent,IO_NO_INCREMENT,FALSE);
}
extern "C" VOID WcnStage8Observe(PWCN_DEVICE D,const UCHAR *F,ULONG N,ULONG Frequency,LONG Rssi)
{
    auto S=(S8*)D->Stage8;if(!S||N<36||N>2048)return;ULONG NameLength;auto Name=WcnBssSsid(F,N,&NameLength);if(!Name)return;
    ULONG Primary=WcnPrimaryFrequency(F,N,Frequency);
    if(!WcnConnectFrequency(Primary))return;
    ULONG I;for(I=0;I<S->BssCount;I++)if(S->Bss[I].Frequency==Primary&&Jeq(S->Bss[I].Bytes+16,F+16,6))break;
    bool Existing=I<S->BssCount;
    if(I==S->BssCount){
        if(I<128)S->BssCount++;
        else{I=0;for(ULONG K=1;K<S->BssCount;K++)if(S->Bss[K].At<S->Bss[I].At)I=K;}
    }
    auto B=&S->Bss[I];LONGLONG Now=WcnNowMs();
    if(!Existing){B->Rssi=-127;B->SignalAt=0;B->PrimarySample=FALSE;}
    bool Direct=Frequency==Primary;
    // Prefer primary-channel measurements. A short peak window is only a fallback for passive scan misses.
    if(Rssi<0&&Rssi>-127&&(Direct||!B->PrimarySample||Now-B->SignalAt>=2000)){
        if(Direct||!Existing||Now-B->SignalAt>=2000||Rssi>B->Rssi){
            B->Rssi=Direct&&B->PrimarySample&&Now-B->SignalAt<5000?(B->Rssi*3+Rssi)/4:Rssi;B->SignalAt=Now;B->PrimarySample=Direct;
        }
        S8 *Links[]={(S8*)D->Stage8,(S8*)D->SecondaryStage8};if(Direct)for(auto Link:Links)if(Link&&Link->Connect&&Link->J->Ap.Frequency==Primary&&Jeq(Link->J->Ap.Bssid,F+16,6)){Link->J->Ap.Rssi=B->Rssi;Link->SignalAt=Now;}
    }
    B->ScanId=D->Scan.ScanSent?D->Scan.ScanId:0;
    // Keep a real, recently named probe response when a hidden beacon follows;
    // never synthesize an SSID or refresh the age of the old named frame.
    if(Existing&&Name&&WcnSsidHidden(Name,NameLength)&&Now-B->At<=30000){ULONG OldLength;auto Old=WcnBssSsid(B->Bytes,B->Length,&OldLength);if(Old&&!WcnSsidHidden(Old,OldLength))return;}
    B->Length=N;B->Frequency=Primary;B->At=Now;RtlCopyMemory(B->Bytes,F,N);
}
extern "C" VOID WcnStage8Mgmt(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N,BOOLEAN Sent)
{
    auto S=S8Station(D,J);if(!S||N<24)return;
    if(Sent&&((F[0]&0xfc)==0||(F[0]&0xfc)==0x20)&&N<=sizeof(S->AssocRequest)){RtlCopyMemory(S->AssocRequest,F,N);S->AssocRequestSize=N;}
    else if(!Sent&&((F[0]&0xfc)==0x10||(F[0]&0xfc)==0x30)&&N<=sizeof(S->AssocResponse)){RtlCopyMemory(S->AssocResponse,F,N);S->AssocResponseSize=N;}
    else if(!Sent&&(F[0]&0xfc)==0xb0&&N<=sizeof(S->AuthResponse)){RtlCopyMemory(S->AuthResponse,F,N);S->AuthResponseSize=N;}
}
static S8 *S8Cache(S8 *S){return (S8*)S->D->Stage8;}
static NTSTATUS S8BssList(S8 *S,const WDI_MESSAGE_HEADER *H,bool EmptyAllowed=false)
{
    ULONG Count=0;for(ULONG I=0;I<S8Cache(S)->BssCount;I++){
        auto B=&S8Cache(S)->Bss[I];if((S->Secondary&&WcnIs2g(B->Frequency)==WcnIs2g(S->D->Join.Ap.Frequency))||WcnNowMs()-B->At>30000||(S->DisabledBands&(WcnIs2g(B->Frequency)?1u:2u)))continue;
        if(S->ScanPending&&(B->ScanId!=S->ExpectedScanId||
           ((S->ScanTarget.Bssid[0]!=255)&&!Jeq(S->ScanTarget.Bssid,B->Bytes+16,6))||
           (S->ScanTarget.SsidLength&&!WcnBssSsidMatches(B->Bytes,B->Length,S->ScanTarget.Ssid,S->ScanTarget.SsidLength))))continue;
        if(S->ScanPending){ULONG K;for(K=0;K<S->ScanFrequencyCount;K++)if(S->ScanFrequency[K]==B->Frequency)break;if(K==S->ScanFrequencyCount)continue;}
        WDI_BSS_ENTRY_CONTAINER E;RtlCopyMemory(E.BSSID.Address,B->Bytes+16,6);
        // WDI requires the management frame body (timestamp through IEs).
        if((B->Bytes[0]&0xfc)==0x80){E.Optional.BeaconFrame_IsPresent=TRUE;E.BeaconFrame.SimpleAssign(B->Bytes+24,B->Length-24);}
        else{E.Optional.ProbeResponseFrame_IsPresent=TRUE;E.ProbeResponseFrame.SimpleAssign(B->Bytes+24,B->Length-24);}
        E.SignalInfo.RSSI=B->Rssi;E.SignalInfo.LinkQuality=S8Quality(B->Rssi);
        E.ChannelInfo.BandId=WcnIs2g(B->Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;E.ChannelInfo.ChannelNumber=WcnFrequencyChannel(B->Frequency);
        WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS P;P.Optional.DeviceDescriptor_IsPresent=TRUE;P.DeviceDescriptor.SimpleAssign(&E,1);
        UCHAR *Out=nullptr;ULONG N=0;NDIS_STATUS Ns=GenerateWdiIndicationBssEntryListFromIhv(&P,0,&S->Tlv,&N,&Out);
        if(Ns!=NDIS_STATUS_SUCCESS)return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS St=S8Indicate(S,H,WDI_INDICATION_BSS_ENTRY_LIST,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);if(!NT_SUCCESS(St))return St;Count++;
    }

    return Count||EmptyAllowed?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
static NTSTATUS S8Authorize(S8 *S)
{if(S->Provisioning)return STATUS_DEVICE_BUSY;NTSTATUS St=WcnPeerParam(S->D,S->J->Vdev,S->J->Ap.Bssid,S7_WMI_PEER_AUTHORIZE,1);if(NT_SUCCESS(St))S->J->Authorized=TRUE;return St;}
static NTSTATUS S8Association(S8 *S)
{
    auto J=S->J;if(!S->AssocRequestSize||!S->AssocResponseSize)return STATUS_DEVICE_NOT_READY;
    if(J->Ap.Auth==WCN_AUTH_OPEN&&!J->Authorized&&!S->Provisioning){NTSTATUS St=S8Authorize(S);if(!NT_SUCCESS(St))return St;}
    WDI_ASSOCIATION_RESULT_CONTAINER A;RtlCopyMemory(A.BSSID.Address,J->Ap.Bssid,6);
    auto P=&A.AssociationResultParameters;P->AssociationStatus=WDI_ASSOC_STATUS_SUCCESS;P->ReAssociation=J->Reassociate;P->AuthAlgorithm=(WDI_AUTH_ALGORITHM)J->Ap.Auth;
    P->UnicastCipherAlgorithm=P->MulticastDataCipherAlgorithm=J->Ap.Auth==WCN_AUTH_OPEN?WDI_CIPHER_ALGO_NONE:WDI_CIPHER_ALGO_CCMP;P->MulticastMgmtCipherAlgorithm=J->Ap.Pmf?WDI_CIPHER_ALGO_BIP:WDI_CIPHER_ALGO_NONE;P->BandID=WcnIs2g(J->Ap.Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;P->PortAuthorized=J->Authorized;P->WMMQoSEnabled=J->Ap.Qos;
    A.Optional.AssociationRequestFrame_IsPresent=A.Optional.AssociationResponseFrame_IsPresent=TRUE;
    A.AssociationRequestFrame.SimpleAssign(S->AssocRequest+24,S->AssocRequestSize-24);A.AssociationResponseFrame.SimpleAssign(S->AssocResponse+24,S->AssocResponseSize-24);
    if(S->AuthResponseSize>=30){A.Optional.AuthenticationResponseFrame_IsPresent=TRUE;A.AuthenticationResponseFrame.SimpleAssign(S->AuthResponse+24,S->AuthResponseSize-24);}
    if(S->ConnectBssSize){A.Optional.BeaconProbeResponse_IsPresent=TRUE;A.BeaconProbeResponse.SimpleAssign(S->ConnectBss,S->ConnectBssSize);}

    WDI_PHY_TYPE Type=J->Ap.Eht?WDI_PHY_TYPE_EHT:J->Ap.He?WDI_PHY_TYPE_HE:J->Ap.Vht?WDI_PHY_TYPE_VHT:J->Ap.Ht?WDI_PHY_TYPE_HT:J->Ap.PhyMode==2?WDI_PHY_TYPE_HRDSSS:WcnIs2g(J->Ap.Frequency)?WDI_PHY_TYPE_ERP:WDI_PHY_TYPE_OFDM;A.ActivePhyTypeList.SimpleAssign(&Type,1);
    WDI_INDICATION_ASSOCIATION_RESULT_LIST List;List.AssociationResults.SimpleAssign(&A,1);UCHAR *Out=nullptr;ULONG N;
    NDIS_STATUS Ns=GenerateWdiIndicationAssociationResultFromIhv(&List,0,&S->Tlv,&N,&Out);if(Ns!=NDIS_STATUS_SUCCESS)return STATUS_INSUFFICIENT_RESOURCES;
    /* One shared TX queue: no WifiAdapterInitAddTxDemux was registered.
     * WifiAdapterAddPeer is ONLY for configured peer-address demultiplexing,
     * not the hardware peer/association already confirmed by WMI/HTT.
     * Calling it here exceeds the configured (zero) demux-peer limit. */

    NTSTATUS St=S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_ASSOCIATION_RESULT,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);
    if(!NT_SUCCESS(St))return St;

    if(S->P2pRole&&!S->AdapterPeer){S8_ADAPTER_REF Ref(S);if(!Ref.Handle)return STATUS_DELETE_PENDING;NET_EUI48_ADDRESS Mac;RtlCopyMemory(Mac.Value,J->Ap.Bssid,6);WifiAdapterAddPeer(Ref.Handle,&Mac);S->AdapterPeer=TRUE;}
    S->AssocIndicated=TRUE;S8SetLink(S,true);S->ConnectPending=FALSE;if(S->P2pRole)P2pChannel(S,J->Ap.Frequency);

    S8Indicate(S,&S->ConnectHeader,S->Roaming?WDI_INDICATION_ROAM_COMPLETE:WDI_INDICATION_CONNECT_COMPLETE,STATUS_SUCCESS);S->Roaming=FALSE;
WcnStage8RxReady(S->D,S->J);
    return STATUS_SUCCESS;
}
/* Clear software staging only under its lock. Never zero/reuse firmware
 * descriptors or drop ownership merely because a control request arrived. */
void S8ClearStaging(S8 *S)
{
    KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);
    RtlSecureZeroMemory(S->Tx,sizeof(S->Tx));RtlSecureZeroMemory(S->Rx,sizeof(S->Rx));
    S->TxHead=S->TxTail=S->RxHead=S->RxTail=0;S->TxGeneration++;
    S->AssocRequestSize=S->AssocResponseSize=S->AuthResponseSize=0;if(!S->RoamPending){S->ConnectBssSize=0;RtlSecureZeroMemory(&S->Ft,sizeof(S->Ft));}
    if(!S->RoamPending){RtlSecureZeroMemory(S->AssocVendor,sizeof(S->AssocVendor));S->AssocVendorLength=0;S->Provisioning=FALSE;}
    S->SignalAt=S->SignalAttemptAt=S->ReachabilityAt=0;S->AssocIndicated=FALSE;S->SaeRequested=S->SaeAwaitOs=S->SaeConfirmSent=S->SaePeerConfirm=FALSE;S->SaeTxLength=0;RtlSecureZeroMemory(S->SaeTx,sizeof(S->SaeTx));KeReleaseSpinLock(&S->Lock,Irql);
}
static void S8ScanFinish(S8 *S,NTSTATUS Status);
static void S8FinishReset(S8 *S,NTSTATUS Status)
{
    auto D=S->D;auto J=S->J;
    if(!NT_SUCCESS(Status)){
        S->Fatal=TRUE;J->Failed=TRUE;S->FailureReported=TRUE;
        if(S->ScanPending)S8ScanFinish(S,Status);
    }
    else{
        J->Failed=J->Started=J->Resetting=FALSE;J->State=J->Attempts=J->ReoSetupTid=0;J->RoutingSet=FALSE;
        J->Authenticated=J->Associated=J->AssocConfirmed=J->Authorized=FALSE;
        J->PairwiseInstalled=J->GroupInstalled=FALSE;
        J->PeerCreated=J->PeerMapped=J->MgmtBusy=J->VdevStarted=FALSE;J->MacId=J->NssLimit=0;
        J->H2e=J->Reassociate=FALSE;J->TwtState=J->CsaState=J->CsaStep=0;J->CsaAck=J->CsaBlock=FALSE;J->Traffic=0;J->TrafficAt=J->TrafficBusyAt=J->TwtAt=0;RtlZeroMemory(&J->NextChannel,sizeof(J->NextChannel));
        J->WmmSet=J->BeamformSet=J->AggregationSet=FALSE;J->TxFrames=0;RtlZeroMemory(J->Ba,sizeof(J->Ba));
        WcnStationKeysClear(J);
        S->FailureReported=FALSE;
    }
    WcnPmfClear(D,S->J);S->Connect=S->Resetting=FALSE;
    if(S->RoamPending){S->RoamPending=FALSE;
        if(NT_SUCCESS(Status)&&!S->Stopped&&!S->AdapterGone&&!S->ResetPending&&S->Radio){RtlCopyMemory(J->PreviousBssid,J->Ap.Bssid,6);J->Reassociate=!S->Ft.Enabled||S->Ft.Reassoc;J->Ap=S->RoamAp;J->TxMcs=J->RxMcs=MAXULONG;J->PeerSeenAt=0;S->ConnectHeader=S->RoamHeader;S->Connect=S->ConnectPending=S->Roaming=TRUE;S->ConnectAt=WcnNowMs();}
        else{S8SetLink(S,false);S8Indicate(S,&S->RoamHeader,WDI_INDICATION_ROAM_COMPLETE,NT_SUCCESS(Status)?STATUS_CANCELLED:Status);}
        RtlZeroMemory(&S->RoamAp,sizeof(S->RoamAp));
    }
    if(S->ResetPending){S->ResetPending=FALSE;S8Indicate(S,&S->ResetHeader,S->ResetMessage==WDI_TASK_DISCONNECT?WDI_INDICATION_DISCONNECT_COMPLETE:WDI_INDICATION_DOT11_RESET_COMPLETE,Status);}
    if(!NT_SUCCESS(Status))WcnLog(D,"stage8: teardown failed status=0x%x; ownership retained, reconnect blocked",Status);
}
static void S8CancelPendingRoam(S8 *S,NTSTATUS Status)
{if(S->RoamPending){S->RoamPending=FALSE;S->ConnectBssSize=0;RtlSecureZeroMemory(&S->Ft,sizeof(S->Ft));RtlZeroMemory(&S->RoamAp,sizeof(S->RoamAp));S8Indicate(S,&S->RoamHeader,WDI_INDICATION_ROAM_COMPLETE,Status);S8SetLink(S,false);}}
static void S8ConnectFailure(S8 *S,const WDI_MESSAGE_HEADER &H,const UCHAR Mac[6],WDI_BAND_ID Band,WDI_ASSOC_STATUS Reason)
{
    WDI_ASSOCIATION_RESULT_CONTAINER A;RtlCopyMemory(A.BSSID.Address,Mac,6);
    A.AssociationResultParameters.AssociationStatus=Reason;A.AssociationResultParameters.BandID=Band;
    WDI_INDICATION_ASSOCIATION_RESULT_LIST List;List.AssociationResults.SimpleAssign(&A,1);UCHAR *Out=nullptr;ULONG N=0;
    if(GenerateWdiIndicationAssociationResultFromIhv(&List,0,&S->Tlv,&N,&Out)==NDIS_STATUS_SUCCESS){
        S8Indicate(S,&H,WDI_INDICATION_ASSOCIATION_RESULT,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
}
static void S8ResetSession(S8 *S,bool Aborted=false)
{
    auto D=S->D;auto J=S->J;
    if(S->Resetting)return;S8ActionCancel(S,STATUS_CANCELLED);
    if(S->AdapterPeer){S8_ADAPTER_REF Ref(S);if(Ref.Handle){NET_EUI48_ADDRESS Mac;RtlCopyMemory(Mac.Value,J->Ap.Bssid,6);WifiAdapterRemovePeer(Ref.Handle,&Mac);}S->AdapterPeer=FALSE;}
    if(S->AssocIndicated){
        WDI_INDICATION_DISASSOCIATION_PARAMETERS P;RtlCopyMemory(P.DisconnectIndicationParameters.MacAddress.Address,J->Ap.Bssid,6);
        P.DisconnectIndicationParameters.DisassociationWABIReason=S->ResetPending?
            (S->ResetMessage==WDI_TASK_DISCONNECT?WDI_ASSOC_STATUS_DISASSOCIATED_BY_HOST:WDI_ASSOC_STATUS_DISASSOCIATE_BY_DEVICE_RESET):
            S->RoamPending?WDI_ASSOC_STATUS_ROAMING_BETTER_AP_FOUND:S->Limited?WDI_ASSOC_STATUS_DISASSOCIATE_NEEDED_REASSOC:WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN;
        P.Optional.NeedPeerStateCleanup_IsPresent=TRUE;P.NeedPeerStateCleanup=FALSE;UCHAR *Out=nullptr;ULONG N=0;
        if(GenerateWdiIndicationDisassociationFromIhv(&P,0,&S->Tlv,&N,&Out)==NDIS_STATUS_SUCCESS){S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_DISASSOCIATION,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
    }
    if(!S->RoamPending)S8SetLink(S,false);S8ClearStaging(S);
    if(S->KeyRequest){S8Complete(S->KeyRequest,STATUS_CANCELLED,0);S->KeyRequest=nullptr;}
    RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));S->KeyCount=S->KeyNext=0;S->KeyDelete=FALSE;
    if(S->ConnectPending){S->ConnectPending=FALSE;bool Cancel=Aborted||S->Limited||S->ResetPending;
        S8ConnectFailure(S,S->ConnectHeader,J->Ap.Bssid,WcnIs2g(J->Ap.Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000,Cancel?WDI_ASSOC_STATUS_ABORTED:WDI_ASSOC_STATUS_FAILURE);
        S8Indicate(S,&S->ConnectHeader,S->Roaming?WDI_INDICATION_ROAM_COMPLETE:WDI_INDICATION_CONNECT_COMPLETE,Cancel?(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED:STATUS_CONNECTION_DISCONNECTED);}
    S->Connect=S->Roaming=FALSE;
    if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed||D->Runtime.RuntimeErrors){S8FinishReset(S,STATUS_DEVICE_HARDWARE_ERROR);return;}
    if(!J->Started&&!J->PeerCreated&&!J->VdevStarted){S8FinishReset(S,STATUS_SUCCESS);return;}
    S->Resetting=J->Resetting=TRUE;S->ResetStep=J->ReoReleaseStep=0;J->ReoReleaseAt=0;S->ResetAt=WcnNowMs();
    J->VdevStopConfirmed=J->PeerDeleteConfirmed=J->PeerUnmapped=FALSE;

}
static void S8ResetService(S8 *S)
{
    auto D=S->D;auto J=S->J;if(!S->Resetting)return;
    if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed||D->Runtime.RuntimeErrors){S8FinishReset(S,STATUS_DEVICE_HARDWARE_ERROR);return;}
    if(WcnNowMs()-S->ResetAt>15000){S8FinishReset(S,STATUS_IO_TIMEOUT);return;}
    NTSTATUS St=STATUS_SUCCESS;
    switch(S->ResetStep){
    case 0:if(J->MgmtBusy)return;if(J->VdevStarted)St=WcnStage7TeardownCommand(D,S->J,0);break;
    case 1:if(J->PeerCreated)St=WcnStage7TeardownCommand(D,S->J,1);else J->PeerDeleteConfirmed=J->PeerUnmapped=TRUE;break;
    case 2:if(!J->PeerDeleteConfirmed||(!J->PeerUnmapped&&J->PeerMapped))return;break;
    case 3:if(J->VdevStarted)St=WcnStage7TeardownCommand(D,S->J,2);else J->VdevStopConfirmed=TRUE;break;
    case 4:
        if(!J->VdevStopConfirmed)return;
        if(!WcnStage7DataIdle(D,S->J))return;break;
    case 5:
        St=WcnStage7ReoRelease(D,J);if(St==STATUS_PENDING||St==STATUS_DEVICE_BUSY)return;
        S8FinishReset(S,St);return;
    default:S8FinishReset(S,STATUS_INVALID_DEVICE_STATE);return;
    }
    if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St)){S8FinishReset(S,St);return;}
    S->ResetStep++;
}

static NTSTATUS S8Connect(S8 *S,const WDI_MESSAGE_HEADER &H,const UCHAR *P,ULONG N,bool Roam=false)
{
    WDI_TASK_CONNECT_PARAMETERS C;WDI_TASK_ROAM_PARAMETERS Rc;
    auto Ns=Roam?ParseWdiTaskRoamToIhv(N,P,&S->Tlv,&Rc):ParseWdiTaskConnectToIhv(N,P,&S->Tlv,&C);NTSTATUS St=STATUS_NOT_SUPPORTED;
    auto &Qp=Roam?Rc.ConnectParameters:C.ConnectParameters;auto &Entries=Roam?Rc.PreferredBSSEntryList:C.PreferredBSSEntryList;
    auto Cleanup=[&](){if(Roam)CleanupParsedWdiTaskRoam(&Rc);else CleanupParsedWdiTaskConnect(&C);};
    if(Ns!=NDIS_STATUS_SUCCESS){Cleanup();return STATUS_INVALID_PARAMETER;}
    if(!S->Radio||S->Limited||S->Resetting||S->RoamPending||S->Roaming||(!Roam&&S->Connect)||(Roam&&(!S->Connect||!S->LinkUp||!S->J->Authorized))||S->ScanPending||S->D->Scan.RuntimeScan){
        if(Entries.ElementCount){auto B=&Entries.pElements[0];S8ConnectFailure(S,H,B->BSSID.Address,B->ChannelInfo.BandId,WDI_ASSOC_STATUS_ABORTED);}
        Cleanup();
        return (NTSTATUS)NDIS_STATUS_REQUEST_ABORTED;
    }
    if(S->D->Scan.Passed&&Qp.SSIDList.ElementCount==1){
        auto Q=&Qp;auto Name=&Q->SSIDList.pElements[0];WCN_JOIN_AP Selected={};WDI_CONNECT_BSS_ENTRY_CONTAINER *SelectedEntry=nullptr;UCHAR Frame[2048];
        ULONG Auth=Q->AuthenticationAlgorithms.ElementCount==1?(ULONG)Q->AuthenticationAlgorithms.pElements[0]:0;
        BOOLEAN Pmf=Q->ConnectionSettings.MFPEnabled;ULONG Cipher=Auth==WCN_AUTH_OPEN?WDI_CIPHER_ALGO_NONE:WDI_CIPHER_ALGO_CCMP;
        bool Good=Name->ElementCount>=1&&Name->ElementCount<=32&&WcnAuthSupported(Auth)&&(!Pmf||S->PmfAvailable)&&(!WcnAuthNeedsPmf(Auth)||Pmf)&&
            S8CipherAllowed(Q->UnicastCipherAlgorithms,Cipher)&&S8CipherAllowed(Q->MulticastCipherAlgorithms,Cipher);
        if(Roam&&(Name->ElementCount!=S->J->SsidLength||!Jeq(Name->pElements,S->J->Ssid,Name->ElementCount)||Auth!=S->J->Ap.Auth||Pmf!=S->J->Ap.Pmf))Good=false;
        S->ConnectBssSize=0;
        bool Wps=S->P2pRole&&S->WpsEnabled&&(Auth==WCN_AUTH_OPEN||Auth==WCN_AUTH_PSK);
        if(S->P2pRole&&Auth!=WCN_AUTH_PSK&&Auth!=WCN_AUTH_SAE&&!Wps)Good=false;
        if(Good)for(ULONG I=0;I<Entries.ElementCount;I++){
            auto B=&Entries.pElements[I];if(Roam&&Jeq(B->BSSID.Address,S->J->Ap.Bssid,6))continue;
            if(B->Optional.PMKID_IsPresent&&B->PMKID.ElementCount!=16)continue;const UCHAR *F=nullptr;ULONG Length=0,Frequency=0;LONG Rssi=B->SignalInfo.RSSI;
            auto Body=B->Optional.ProbeResponseFrame_IsPresent&&B->ProbeResponseFrame.ElementCount?&B->ProbeResponseFrame:
                B->Optional.BeaconFrame_IsPresent&&B->BeaconFrame.ElementCount?&B->BeaconFrame:nullptr;

            if(Body){
                Frequency=WcnChannelFrequency(B->ChannelInfo.ChannelNumber);
                if(B->ChannelInfo.BandId!=(WcnIs2g(Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000))continue;
                Length=WcnBssFrame(B->BSSID.Address,Body->pElements,Body->ElementCount,Frequency,Body==&B->ProbeResponseFrame,Frame,sizeof(Frame));
                if(!Length)continue;F=Frame;
            }else for(ULONG K=0;K<S8Cache(S)->BssCount;K++){
                auto E=&S8Cache(S)->Bss[K];if(Jeq(B->BSSID.Address,E->Bytes+16,6)&&WcnNowMs()-E->At<=30000){F=E->Bytes;Length=E->Length;Frequency=E->Frequency;Rssi=E->Rssi;break;}}
            if(!F||Rssi>=0||Rssi<=-127||(S->Secondary&&(WcnIs2g(Frequency)==WcnIs2g(S->D->Join.Ap.Frequency)||!WcnTripleChannel(S->D->Join.Ap.Frequency,Frequency,WcnHotspotFrequency(S->D)))))continue;
            WCN_JOIN_AP Candidate;UCHAR Akm,Mode;if(!S8FtCandidate(S,*B,F,Length,Auth,&Akm,&Mode))continue;
            if(!(S->DisabledBands&(WcnIs2g(Frequency)?1u:2u))&&WcnJoinCandidate(S->D,F,Length,Frequency,Rssi,Name->pElements,Name->ElementCount,Wps?WCN_AUTH_PSK:Auth,Pmf,Wps?2:Akm,Q->ConnectionSettings.HiddenNetwork,&Candidate)){
                if(S->P2pRole){UCHAR Capability[2];ULONG L;if(!P2pAttributesValid(F+36,Length-36)||!P2pAttr(F+36,Length-36,2,Capability,sizeof(Capability),&L)||L!=2||!(Capability[1]&1))continue;}
                if(Wps){static const UCHAR WpsOui[]={0,0x50,0xf2,4};if(Length<36||!P2pVendor(F+36,Length-36,WpsOui))continue;Candidate.Auth=Auth;Candidate.Pmf=FALSE;Candidate.Akm=Auth==WCN_AUTH_OPEN?0:2;}
                if(S->Secondary&&!WcnHotspotTriple(S->D,&S->D->Join.Ap,&Candidate))continue;
                Candidate.FtMode=Mode;Candidate.HasPmkid=B->Optional.PMKID_IsPresent&&Auth!=WCN_AUTH_OPEN&&Auth!=WCN_AUTH_SAE;if(Candidate.HasPmkid)RtlCopyMemory(Candidate.Pmkid,B->PMKID.pElements,16);
                if(!Selected.Valid||WcnPhyScore(&Candidate)>WcnPhyScore(&Selected)){Selected=Candidate;SelectedEntry=B;S->ConnectBssSize=Length-24;RtlCopyMemory(S->ConnectBss,F+24,S->ConnectBssSize);}}
        }

        if(Selected.Valid){
            if(S->P2pRole){if(Roam){Cleanup();return STATUS_NOT_SUPPORTED;}St=P2pConnectIes(S,Q->AssociationRequestVendorIE,SelectedEntry->Optional.AssociationRequestVendorIE_IsPresent?&SelectedEntry->AssociationRequestVendorIE:nullptr,Wps);if(!NT_SUCCESS(St)){Cleanup();return St;}}
            St=S8FtPrepare(S,*SelectedEntry,Selected);if(!NT_SUCCESS(St)){Cleanup();return St;}
            if(Roam){S->RoamAp=Selected;S->RoamHeader=H;S->RoamPending=TRUE;S8ResetSession(S);St=STATUS_SUCCESS;Cleanup();return St;}
            S->J->PeerSeenAt=0;S->J->TxMcs=S->J->RxMcs=MAXULONG;S->J->Ap=Selected;S->Provisioning=Wps;S->J->SsidLength=(UCHAR)Name->ElementCount;
            RtlZeroMemory(S->J->Ssid,32);RtlCopyMemory(S->J->Ssid,Name->pElements,Name->ElementCount);
            S->Connect=TRUE;S->ConnectPending=TRUE;S->ConnectAt=WcnNowMs();S->ConnectHeader=H;St=STATUS_SUCCESS;
        }
    }
    Cleanup();return St;
}
static bool S8OwnScan(S8 *S)
{return S->ScanIssued&&S->ExpectedScanId==S->D->Scan.ScanId&&S->J->Vdev==S->D->Scan.ScanVdev;}
static void S8ScanFinish(S8 *S,NTSTATUS Status)
{
    S->ScanPending=S->ScanIssued=S->ScanAborting=FALSE;S->ScanAbortStatus=STATUS_SUCCESS;RtlSecureZeroMemory(&S->ScanTarget,sizeof(S->ScanTarget));
    S8Indicate(S,&S->ScanHeader,WDI_INDICATION_SCAN_COMPLETE,Status);
}
static NTSTATUS S8Scan(S8 *S,const WDI_MESSAGE_HEADER &H,const UCHAR *P,ULONG N)
{
    if(!S->Radio||S->ScanPending||(S->Secondary&&S->Limited))return STATUS_DEVICE_BUSY;
    WDI_SCAN_PARAMETERS Params;auto Ns=ParseWdiTaskScanToIhv(N,P,&S->Tlv,&Params);NTSTATUS St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
    WCN_SCAN_TARGET Target={};ULONG Count=0,Frequency[WCN_SCAN_MAX_CHANNELS];
    if(NT_SUCCESS(St)){
        auto Type=Params.ScanModeParameters.AllowedScanType;
        if(Type!=WDI_SCAN_TYPE_ACTIVE_ONLY&&Type!=WDI_SCAN_TYPE_PASSIVE_ONLY&&Type!=WDI_SCAN_TYPE_AUTO)St=STATUS_INVALID_PARAMETER;
        if(Params.SSIDList.ElementCount>1)St=STATUS_NOT_SUPPORTED; // Matches advertised ScanSSIDListSize.
        else if(Params.SSIDList.ElementCount){auto Name=&Params.SSIDList.pElements[0];if(Name->ElementCount>32||(Name->ElementCount&&!Name->pElements))St=STATUS_INVALID_PARAMETER;
            else{Target.SsidLength=(UCHAR)Name->ElementCount;if(Name->ElementCount)RtlCopyMemory(Target.Ssid,Name->pElements,Name->ElementCount);}}
        Target.Active=Type==WDI_SCAN_TYPE_ACTIVE_ONLY||(Type==WDI_SCAN_TYPE_AUTO&&Target.SsidLength);
        RtlCopyMemory(Target.Bssid,Params.BSSID.Address,6);UCHAR Any=0;bool Broadcast=true;
        for(ULONG I=0;I<6;I++){Any|=Target.Bssid[I];if(Target.Bssid[I]!=255)Broadcast=false;}
        if(!Any)RtlFillMemory(Target.Bssid,6,255);else if(!Broadcast&&(Target.Bssid[0]&1))St=STATUS_INVALID_PARAMETER;
        if(NT_SUCCESS(St))for(ULONG I=0;I<S->D->Scan.ChannelCount;I++){
            auto C=&S->D->Scan.Channel[I];ULONG F=C->Frequency;
            if((S->DisabledBands&(WcnIs2g(F)?1u:2u))||(S->Secondary&&(WcnIs2g(F)==WcnIs2g(S->D->Join.Ap.Frequency)||!WcnTripleChannel(S->D->Join.Ap.Frequency,F,WcnHotspotFrequency(S->D))))||(Target.Active&&!WcnProbeAllowed(F,C->Flags)))continue;
            bool Allowed=!Params.Optional.BandChannelList_IsPresent||!Params.BandChannelList.ElementCount;
            for(ULONG K=0;K<Params.BandChannelList.ElementCount;K++){auto B=&Params.BandChannelList.pElements[K];if(B->BandID!=(WcnIs2g(F)?WDI_BAND_ID_2400:WDI_BAND_ID_5000))continue;
                if(!B->ChannelList.ElementCount)Allowed=true;for(ULONG Cn=0;Cn<B->ChannelList.ElementCount;Cn++)if(B->ChannelList.pElements[Cn]==WcnFrequencyChannel(F))Allowed=true;}
            if(Allowed&&Count<WCN_SCAN_MAX_CHANNELS)Frequency[Count++]=F;
        }
        if(NT_SUCCESS(St)&&!Count)St=STATUS_NOT_SUPPORTED;
    }
    CleanupParsedWdiTaskScan(&Params);
    if(NT_SUCCESS(St)){S->ScanTarget=Target;S->ScanFrequencyCount=Count;RtlCopyMemory(S->ScanFrequency,Frequency,Count*sizeof(ULONG));
        S->ScanHeader=H;S->ScanPending=TRUE;S->ScanIssued=FALSE;S->ScanAbortStatus=(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED;S->ExpectedScanId=0;S->ScanAt=WcnNowMs();}
    return St;
}

static void S8Handle(S8 *S,WIFIREQUEST R)
{
    UINT In=0,Out=0;auto Raw=(UCHAR*)WifiRequestGetInOutBuffer(R,&In,&Out);UINT16 Id=WifiRequestGetMessageId(R);
    if(!Raw||In<sizeof(WDI_MESSAGE_HEADER)||Out<sizeof(WDI_MESSAGE_HEADER)||In>65536){S8Complete(R,STATUS_INVALID_PARAMETER,0);return;}
    WDI_MESSAGE_HEADER H=*(WDI_MESSAGE_HEADER*)Raw;auto P=Raw+sizeof(H);ULONG N=In-sizeof(H);NTSTATUS St=STATUS_NOT_SUPPORTED;
    auto Secondary=(S8*)S->D->SecondaryStage8;if(Secondary&&Secondary->Adapter&&!Secondary->AdapterGone&&H.PortId==Secondary->Port)S=Secondary;
    auto Role=(S8*)S->D->RoleStage8;if(Role&&Role->Adapter&&!Role->AdapterGone&&H.PortId==Role->Port)S=Role;
    if(P2pCommand(S,R,H,Id,P,N)||HotspotCommand(S,R,H,Id,P,N))return;
    if(S->Fatal){S8Complete(R,STATUS_DEVICE_HARDWARE_ERROR);return;}
    if(S->Resetting&&Id!=WDI_GET_STATISTICS&&Id!=WDI_ABORT_TASK&&Id!=WDI_TASK_DISCONNECT&&Id!=WDI_TASK_DOT11_RESET&&Id!=WDI_TASK_CONNECT){S8Complete(R,STATUS_DEVICE_BUSY);return;}
    if(S->Adapter&&H.PortId!=WDI_PORT_ID_ADAPTER&&H.PortId!=S->Port){S8Complete(R,STATUS_NOT_SUPPORTED);return;}

    if((Id==WDI_TASK_SCAN||Id==WDI_TASK_CONNECT||Id==WDI_TASK_ROAM||Id==WDI_SET_ADD_CIPHER_KEYS)&&(!S->Adapter||H.PortId!=S->Port)){S8Complete(R,STATUS_NOT_SUPPORTED);return;}
    if(S8ActionCommand(S,R,H,Id,P,N))return;
    switch(Id){
    case WDI_SET_ADAPTER_CONFIGURATION:{St=S8Configure(S,P,N,S->Mac);S8Complete(R,St);
        if(NT_SUCCESS(St)){WDI_INDICATION_RADIO_STATUS_PARAMETERS Radio;Radio.RadioState.HardwareState=S->D->Ce.WmiReady&&!S->D->Mhi.SysErr;Radio.RadioState.SoftwareState=S->Radio;UCHAR *B=nullptr;ULONG Len=0;
            auto Ns=GenerateWdiIndicationRadioStatusFromIhv(&Radio,0,&S->Tlv,&Len,&B);if(Ns==NDIS_STATUS_SUCCESS){auto DeviceHeader=H;DeviceHeader.PortId=WDI_PORT_ID_ADAPTER;S8Indicate(S,&DeviceHeader,WDI_INDICATION_RADIO_STATUS,STATUS_SUCCESS,B,Len,true);FreeGenerated(B);}}return;}
    case WDI_TASK_CHANGE_OPERATION_MODE:{WDI_TASK_CHANGE_OPERATION_MODE_PARAMETERS Pm;auto Ns=ParseWdiTaskChangeOperationModeToIhv(N,P,&S->Tlv,&Pm);St=Ns==NDIS_STATUS_SUCCESS&&Pm.OperationMode==WDI_OPERATION_MODE_STA?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;CleanupParsedWdiTaskChangeOperationMode(&Pm);S8Complete(R,St);if(NT_SUCCESS(St))S8Indicate(S,&H,WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE,St);return;}
    case WDI_TASK_SCAN:St=S8Scan(S,H,P,N);break;
    case WDI_ABORT_TASK:{
        WDI_TASK_ABORT_PARAMETERS K;auto Ns=ParseWdiAbortTaskToIhv(N,P,&S->Tlv,&K);St=STATUS_NOT_SUPPORTED;
        bool Match=Ns==NDIS_STATUS_SUCCESS&&K.CancelParameters.OriginalTaskOID==OID_WDI_TASK_SCAN&&S->ScanPending&&
            K.CancelParameters.OriginalTransactionId==S->ScanHeader.TransactionId&&K.CancelParameters.OriginalPortId==S->ScanHeader.PortId;
        auto ConnectTask=S->RoamPending?&S->RoamHeader:&S->ConnectHeader;
        bool CancelConnect=Ns==NDIS_STATUS_SUCCESS&&(S->ConnectPending||S->RoamPending)&&K.CancelParameters.OriginalTaskOID==(S->Roaming||S->RoamPending?OID_WDI_TASK_ROAM:OID_WDI_TASK_CONNECT)&&
            K.CancelParameters.OriginalTransactionId==ConnectTask->TransactionId&&K.CancelParameters.OriginalPortId==ConnectTask->PortId;
        bool CancelAction=Ns==NDIS_STATUS_SUCCESS&&S->ActionPending&&K.CancelParameters.OriginalTaskOID==(S->ActionKind==WDI_TASK_SEND_REQUEST_ACTION_FRAME?OID_WDI_TASK_SEND_REQUEST_ACTION_FRAME:OID_WDI_TASK_SEND_RESPONSE_ACTION_FRAME)&&
            K.CancelParameters.OriginalTransactionId==S->ActionHeader.TransactionId&&K.CancelParameters.OriginalPortId==S->ActionHeader.PortId;
        // WDI_ABORT_TASK is a signal/property, not a task: acknowledge it
        // immediately and complete the ORIGINAL scan only after HW stops.
        St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
        CleanupParsedWdiAbortTaskToIhv(&K);
        if(CancelAction){S8Complete(R,St);S8ActionCancel(S,(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED);return;}
        if(CancelConnect){S8Complete(R,St);S8CancelPendingRoam(S,(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED);S8ResetSession(S,true);return;}
        if(Match&&S->ScanAborting){S8Complete(R,St);return;}
        if(Match&&S8OwnScan(S)&&S->D->Scan.RuntimeScan){if(!S->ScanAborting){S->ScanAborting=TRUE;S->ScanAbortAt=WcnNowMs();}S8Complete(R,St);return;}
        S8Complete(R,St);
        if(Match){S8ScanFinish(S,(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED);
            }return;}
    case WDI_TASK_CONNECT:
    case WDI_TASK_ROAM:
        S8Complete(R,STATUS_SUCCESS);St=S8Connect(S,H,P,N,Id==WDI_TASK_ROAM);
        if(!NT_SUCCESS(St))S8Indicate(S,&H,Id==WDI_TASK_ROAM?WDI_INDICATION_ROAM_COMPLETE:WDI_INDICATION_CONNECT_COMPLETE,St);return;
    case WDI_SET_FAST_BSS_TRANSITION_PARAMETERS:St=S8FtCommand(S,H,P,N);break;
    case WDI_SET_SAE_AUTH_PARAMS:St=S8SaeCommand(S,&H,P,N);break;
    case WDI_SET_ADD_CIPHER_KEYS:
    case WDI_SET_DELETE_CIPHER_KEYS:St=S8ParseKeys(S,R,P,N,Id==WDI_SET_DELETE_CIPHER_KEYS);if(St==STATUS_PENDING)return;break;
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
        CleanupParsedWdiTaskSetRadioState(&K);S8Complete(R,St);
        if(NT_SUCCESS(St)){S8Indicate(S,&H,WDI_INDICATION_RADIO_STATUS,St,RadioTlv,RadioLength,true);FreeGenerated(RadioTlv);S8Indicate(S,&H,WDI_INDICATION_SET_RADIO_STATE_COMPLETE,St);}
        return;}
    case WDI_TASK_DISCONNECT:
    case WDI_TASK_DOT11_RESET:
        /* real bounded reset - cancel the session, stop the firmware
         * vdev and re-arm scans/reconnect; never claim a full MAC re-init. */
        if(S->ResetPending){S8Complete(R,STATUS_DEVICE_BUSY);return;}
        S8CancelPendingRoam(S,(NTSTATUS)NDIS_STATUS_REQUEST_ABORTED);S8Complete(R,STATUS_SUCCESS);S->ResetPending=TRUE;S->ResetHeader=H;S->ResetMessage=Id;S8ResetSession(S);return;

    }
    S8Complete(R,St);
}
static void S8Signal(S8 *S)
{
    LONGLONG Now=WcnNowMs();if(!S->LinkUp||!S->SignalAt||Now-S->SignalAt>5000||Now-S->SignalAttemptAt<1000)return;
    S->SignalAttemptAt=Now;
    auto J=S->J;auto A=&J->Ap;WDI_INDICATION_LINK_STATE_CHANGE_PARAMETERS P;auto L=&P.LinkStateChangeParameters;
    RtlCopyMemory(L->PeerMACAddress.Address,A->Bssid,6);L->TxLinkSpeed=L->RxLinkSpeed=A->RateKbps;L->LinkQuality=S8Quality(A->Rssi);
    // LinkInfo is mandatory from WDI 2.0.3 onward, including single-link STA.
    WDI_LINK_INFO_CONTAINER Info;RtlCopyMemory(Info.LocalLinkMACAddress.Address,S->Mac,6);RtlCopyMemory(Info.PeerLinkMACAddress.Address,A->Bssid,6);
    Info.BandId=WcnIs2g(A->Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;Info.ChannelNumber=WcnFrequencyChannel(A->Frequency);
    Info.RSSI=A->Rssi;Info.Bandwidth=A->Width;Info.TxMCS=J->TxMcs;Info.RxMCS=J->RxMcs;P.LinkInfo.SimpleAssign(&Info,1);
    UCHAR *Out=nullptr;ULONG N=0;auto Ns=GenerateWdiIndicationLinkStateChangeFromIhv(&P,0,&S->Tlv,&N,&Out);
    if(Ns==NDIS_STATUS_SUCCESS){S8Indicate(S,&S->ConnectHeader,WDI_INDICATION_LINK_STATE_CHANGE,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
}
static void S8SecondaryPolicy(S8 *S)
{
    if(!S->Secondary)return;auto D=S->D;auto Primary=(S8*)D->Stage8;auto Target=Primary->RoamPending?&Primary->RoamAp:&D->Join.Ap;
    ULONG PrimaryBand=Target->Frequency?(WcnIs2g(Target->Frequency)?1u:2u):0;
    bool Busy=S->Connect||S->J->Started||S->LinkUp;
    // Before a BSS is selected, scan/connect constrain the shared channel.
    BOOLEAN Limited=(D->P2pClient&&WcnHotspotActive(D))||!WcnHotspotTriple(D,Target,Busy?&S->J->Ap:nullptr)||S->Resetting||!PrimaryBand||Primary->Radio==FALSE||(Busy&&(Primary->Connect||Primary->RoamPending)&&WcnIs2g(S->J->Ap.Frequency)==WcnIs2g(Target->Frequency));
    if(S->Limited!=Limited||S->SecondaryBand!=PrimaryBand){S->Limited=Limited;S->SecondaryBand=PrimaryBand;
        WDI_INDICATION_SECONDARY_STA_CONNECTIVITY_PARAMETERS P;P.LimitedConnectivity=Limited;
        UCHAR *Out=nullptr;ULONG N=0;if(GenerateWdiIndicationSecondaryStaConnectivityFromIhv(&P,0,&S->Tlv,&N,&Out)==NDIS_STATUS_SUCCESS){WDI_MESSAGE_HEADER H={};H.PortId=WDI_PORT_ID_ADAPTER;S8Indicate(S,&H,WDI_INDICATION_SECONDARY_STA_CONNECTIVITY,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
        }
    if((Limited||S->AdapterGone)&&(S->Connect||S->J->Started)&&!S->Resetting&&!S->Fatal){if(S->AdapterGone)S->AssocIndicated=FALSE;S8ResetSession(S);}
}
extern "C" BOOLEAN WcnStage8SecondaryIdle(PWCN_DEVICE D)
{auto S=(S8*)D->SecondaryStage8;return !S||(!S->J->Started&&!S->J->PeerCreated&&!S->J->VdevStarted&&!S->Resetting);}
static void S8ScanService(S8 *S)
{
    auto D=S->D;bool OtherScan=false;S8 *Owners[]={(S8*)D->Stage8,(S8*)D->SecondaryStage8,D->P2pClient?(S8*)D->RoleStage8:nullptr};for(auto O:Owners)if(O&&O!=S&&O->ScanIssued)OtherScan=true;
    if(S->ScanAborting){
        if(!S8OwnScan(S)){S8ScanFinish(S,S->ScanAbortStatus);return;}
        NTSTATUS St=WcnStage6CancelRescan(D);bool Done=!D->Scan.RuntimeScan;
        if(St==STATUS_DEVICE_BUSY)St=STATUS_SUCCESS;
        if(D->Scan.Failed||WcnNowMs()-S->ScanAbortAt>5000){St=STATUS_IO_TIMEOUT;Done=true;}
        if(!NT_SUCCESS(St)||Done)S8ScanFinish(S,NT_SUCCESS(St)?S->ScanAbortStatus:St);
    }
    if(S->ScanPending&&!S->ScanAborting){
        NTSTATUS St=STATUS_SUCCESS;bool Finish=false;
        if(!S->Radio||D->Scan.Failed){St=STATUS_DEVICE_NOT_READY;Finish=true;}
        else if(!S->ScanIssued&&(S->J->CsaState||S->ActionPending||S->RoamPending||S->ConnectPending||S->KeyRequest||(S->Connect&&!S->J->Authorized))){}
        else if(!S->ScanIssued&&(OtherScan||WcnP2pScanOwned(D))){}
        else if(!S->ScanIssued&&S->P2pRole&&!WcnStage8SecondaryIdle(D)){}
        else if(!S->ScanIssued&&!S->J->Created){St=WcnStage7EnsureVdev(D,S->J);if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St))Finish=true;}
        else if(!S->ScanIssued&&S->J->VdevAt&&WcnNowMs()-S->J->VdevAt<200){}
        else if(!S->ScanIssued){
            if(!D->Scan.Passed||D->Scan.RuntimeScan){
                // Only an initial untargeted passive scan can satisfy this request.
                if(!D->Scan.Passed&&!S->ScanTarget.Active&&!S->ScanTarget.SsidLength&&S->ScanTarget.Bssid[0]==255&&D->Scan.ScanSent&&D->Scan.ScanVdev==S->J->Vdev){S->ExpectedScanId=D->Scan.ScanId;S->ScanIssued=TRUE;}
            }else{
                // Topology may change while this request waits behind the other STA.
                ULONG Count=0;for(ULONG I=0;I<S->ScanFrequencyCount;I++){ULONG F=S->ScanFrequency[I];
                    if(!S->Secondary||(WcnIs2g(F)!=WcnIs2g(D->Join.Ap.Frequency)&&WcnTripleChannel(D->Join.Ap.Frequency,F,WcnHotspotFrequency(D))))S->ScanFrequency[Count++]=F;}
                S->ScanFrequencyCount=Count;St=Count?WcnTdlsPause(D,S->J):STATUS_NOT_SUPPORTED;
                if(NT_SUCCESS(St))St=WcnTwtStop(D,S->J);if(NT_SUCCESS(St))St=WcnStage6Rescan(D,S->J->Vdev,S->ScanFrequency,Count,&S->ScanTarget);
                if(St==STATUS_DEVICE_BUSY)St=STATUS_SUCCESS;
                else if(!NT_SUCCESS(St))Finish=true;
                else{S->ExpectedScanId=D->Scan.ScanId;S->ScanIssued=TRUE;if(!S->Secondary)S->BssCount=0;}
            }
        }
        if(!Finish&&S->ScanIssued&&D->Scan.ScanId==S->ExpectedScanId&&D->Scan.ScanCompleted&&!D->Scan.RuntimeScan&&D->Scan.State==7){
            St=D->Scan.ScanReason?STATUS_DEVICE_NOT_READY:S8BssList(S,&S->ScanHeader,true);Finish=true;
        }
        if(!Finish&&WcnNowMs()-S->ScanAt>28000){
            if(S8OwnScan(S)&&D->Scan.RuntimeScan){S->ScanAborting=TRUE;S->ScanAbortStatus=STATUS_IO_TIMEOUT;S->ScanAbortAt=WcnNowMs();}
            else{St=STATUS_IO_TIMEOUT;Finish=true;}
        }
        if(Finish)S8ScanFinish(S,St);
    }
}
static void S8Service(S8 *S)
{
    if(!S)return;auto D=S->D;if(S->Stopped&&!S->Secondary&&!S->P2pRole)return;S8SecondaryPolicy(S);
    if(S->P2pRole&&!S->HotspotRole&&S->AdapterGone&&(S->Connect||S->J->Started)&&!S->Resetting&&!S->Fatal){S->AssocIndicated=FALSE;S8ResetSession(S);}
    if(!S->Fatal&&(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed)){
        WcnLog(D,"stage8: fatal hardware state latched; no further firmware commands or automatic reset loop");S8ResetSession(S);
    }
    if(D->Ce.WmiReady)KeSetEvent(&S->ReadyEvent,IO_NO_INCREMENT,FALSE);
    for(ULONG I=0;I<8;I++){KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);if(S->ReqHead==S->ReqTail){KeReleaseSpinLock(&S->Lock,Irql);break;}
        WIFIREQUEST R=S->Requests[S->ReqTail];S->ReqTail=(S->ReqTail+1)%32;KeReleaseSpinLock(&S->Lock,Irql);S8Handle(S,R);}
    S8ResetService(S);
    if(S->Fatal){return;}
    if(S->Resetting){return;}
    S8ScanService(S);
    if(S->ConnectPending&&S->J->State>=9&&!S->J->Failed){NTSTATUS St=S8Association(S);if(!NT_SUCCESS(St)&&St!=STATUS_DEVICE_BUSY)WcnStage7Fail(D,S->J,"WiFiCx association-result generation");}
    S8KeysService(S);
    if(S->AssocIndicated&&S->J->PairwiseInstalled&&S->J->GroupInstalled&&!S->J->Authorized&&!S->J->Failed&&
       (!S->J->Ap.Pmf||S->J->Igtk[0].Valid||S->J->Igtk[1].Valid)){
        NTSTATUS St=S8Authorize(S);if(!NT_SUCCESS(St)&&St!=STATUS_DEVICE_BUSY)WcnStage7Fail(D,S->J,"OS controlled-port authorization transport");
    }
    if(S->LinkUp&&S->UnreachableMs){LONGLONG Last=S->J->PeerSeenAt>S->SignalAt?S->J->PeerSeenAt:S->SignalAt;
        if(WcnPeerExpired(WcnNowMs(),Last,S->UnreachableMs,S->J->CsaState||(D->Scan.ScanSent&&!D->Scan.ScanCompleted&&!D->Scan.Failed),&S->ReachabilityAt))
            WcnStage7Fail(D,S->J,"OS-configured peer reachability deadline while on channel; no recent beacon, RX or TX ACK");}
    if(S->ConnectPending&&WcnNowMs()-S->ConnectAt>24000)WcnStage7Fail(D,S->J,"OS connect deadline");
    if(S->J->Failed&&!S->FailureReported&&!S->Resetting&&!S->Fatal){S->FailureReported=TRUE;WcnLog(D,"stage8: FAIL actual hardware/MLME failure; system link down");S8ResetSession(S);}
    S8ActionService(S);S8QueuesService(S);S8Signal(S);

}
extern "C" VOID WcnStage8ChannelChanged(PWCN_DEVICE D,PWCN_JOIN J)
{auto S=S8Station(D,J);if(S&&S->LinkUp)S8SetLink(S,true);}
extern "C" VOID WcnStage8Service(PWCN_DEVICE D)
{WcnP2pService(D);HotspotService(D);if(D->Stage8)S8Service((S8*)D->Stage8);if(D->SecondaryStage8)S8Service((S8*)D->SecondaryStage8);if(D->P2pClient&&D->RoleStage8)S8Service((S8*)D->RoleStage8);}
static void S8StopState(S8 *S)
{
    if(!S)return;S8ActionCancel(S,STATUS_CANCELLED);if(S->RoamPending){S8Indicate(S,&S->RoamHeader,WDI_INDICATION_ROAM_COMPLETE,STATUS_CANCELLED);S->RoamPending=FALSE;}KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);S->Stopped=TRUE;KeReleaseSpinLock(&S->Lock,Irql);
    while(S->ReqTail!=S->ReqHead){auto R=S->Requests[S->ReqTail];S->ReqTail=(S->ReqTail+1)%32;S8Complete(R,STATUS_CANCELLED,0);}
    if(S->KeyRequest){S8Complete(S->KeyRequest,STATUS_CANCELLED,0);S->KeyRequest=nullptr;}
    RtlSecureZeroMemory(S->Keys,sizeof(S->Keys));S8SetLink(S,false);

}

extern "C" VOID WcnStage8Stop(PWCN_DEVICE D)
{WcnP2pStop(D);if(D->P2pClient)S8StopState((S8*)D->RoleStage8);HotspotStop(D);S8StopState((S8*)D->SecondaryStage8);S8StopState((S8*)D->Stage8);}

extern "C" ULONG WcnStage8AssocIes(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *Out,ULONG Capacity)
{auto S=S8Station(D,J);if(!S||!S->P2pRole)return 0;if(S->AssocVendorLength>Capacity)return MAXULONG;RtlCopyMemory(Out,S->AssocVendor,S->AssocVendorLength);return S->AssocVendorLength;}

extern "C" BOOLEAN WcnStage8Provisioning(PWCN_DEVICE D,PWCN_JOIN J)
{auto S=S8Station(D,J);return S&&S->Provisioning;}
