/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Native Windows mobile hotspot. WiFiCx owns credentials, the authenticator,
 * DHCP and NAT. AP vdev1 and its peer/keys never borrow the STA vdev0 context.
 * Independent AP PHY and peer state; no password or handshake implementation in the driver. */
#include "stage8-internal.h"
extern "C" {
#include "aggregation-policy.h"
#include "hotspot.h"
#include "stage7-wire.h"
#include "stage7-util.h"
#include "stage7-rx-protocol.h"
#include "network-policy.h"
#include "security-policy.h"
}
#include "sae-wire.h"
#include "p2p-wire.h"
struct HOTSPOT_SAE {UCHAR Tx[488];ULONG Length,Sequence,Attempts,Generation,SentGeneration;LONGLONG At;BOOLEAN Sending,PeerConfirm,ConfirmSent,Validated,Authenticated;};
struct HOTSPOT_KEY {UCHAR Bytes[16];BOOLEAN Valid;};
struct HOTSPOT_CLIENT {
    UCHAR Client[6];USHORT ClientListen;ULONG Auth;HOTSPOT_SAE Sae;ULONG64 MgmtRxPn,MgmtTxPn;BOOLEAN SaQueryPending;UCHAR SaQueryId[2];
    WCN_JOIN_AP ClientPhy;WCN_BA Ba[16];ULONG ClientStep,ClientRetry,QueueTid,PeerId,Ast,Hash;
    BOOLEAN Provisioning,P2pPeer;
    BOOLEAN Used,ClientMapped,ClientCreated,OsPeer,NotifyDisconnect,Associated,Authorized,AssocConfirmed,DeleteSent,DeleteConfirmed,Unmapped,MgmtBusy;
    ULONG MgmtStatus;LONGLONG ClientAt;WCN_DMA Reo,Retired[8];ULONG RetiredCount;
    UCHAR Assoc[1024];ULONG AssocLength;WDI_MESSAGE_HEADER AssocHeader;BOOLEAN AssocPending;
    HOTSPOT_KEY PairKey;ULONG64 Pn[17];UCHAR PnValid[17];
};
struct HOTSPOT {
    PWCN_DEVICE D;S8 *Go;WIFIDIRECTDEVICE Device;ULONG DevicePort;
    WIFI_WIFIDIRECT_CAPABILITIES Caps;DOT11_AUTH_CIPHER_PAIR AuthPairs[2];WDI_MAC_ADDRESS RoleAddress;
    UCHAR DeviceMac[6],Mac[6],Ssid[32],SsidLength,Rates[12],RateCount;WCN_JOIN_AP Radio;
    HOTSPOT_CLIENT Clients[WCN_HOTSPOT_CLIENTS],*Current,*MgmtClient,*TaskClient,*KeyClient[4];ULONG StopClient;
    BOOLEAN Coexist20,ResizeRequested;ULONG ResizeStep,ResizePeer;LONGLONG ResizeAt;
    ULONG Frequency,Power,Gain,BeaconPeriod,Dtim,Step,MacId,Sequence;BOOLEAN Created,Started,Running,Failed,SelfMapped,Stopped,BeamformSet,AggregationSet;
    ULONG ReleaseStep,SelfPeerId;BOOLEAN SelfDeleteConfirmed,SelfUnmapped,VdevDeleted;
    ULONG SelfAst,SelfHash;WCN_DMA Mgmt;ULONG Wire[512],MgmtId,MgmtStatus;BOOLEAN MgmtBusy;LONGLONG MgmtAt,At;
    UCHAR Beacon[1536],Advert[512],ProbeAdvert[512];ULONG BeaconLength,TimOffset,AdvertLength,ProbeAdvertLength;
    UINT16 Task;WDI_MESSAGE_HEADER TaskHeader;
    WIFIREQUEST KeyRequest,DefaultRequest,AdvertRequest;LONGLONG DefaultAt,AdvertAt;S8_KEY Keys[4];ULONG KeyCount,KeyStep,KeyStatus;BOOLEAN KeyDone,DefaultPending,KeyDelete;LONGLONG KeyAt;
    BOOLEAN AllowPsk,AllowSae,PmfConfigured,SaeCapable;WCN_IGTK Igtk[2];UCHAR DefaultMgmtKey;
    HOTSPOT_KEY GroupKey[4];UCHAR DefaultKey;
};
static HOTSPOT *Hs(PWCN_DEVICE D){return (HOTSPOT*)D->Hotspot;}
static HOTSPOT_CLIENT *Hfind(HOTSPOT *H,const UCHAR Mac[6])
{for(auto &C:H->Clients)if(C.Used&&Jeq(C.Client,Mac,6))return &C;return nullptr;}
static ULONG Haid(HOTSPOT *H){return (ULONG)(H->Current-H->Clients)+1;}
static NTSTATUS Hparam(HOTSPOT *H,ULONG Param,ULONG Value)
{return WcnVdevParam(H->D,1,Param,Value);}
static NTSTATUS Hpeer(HOTSPOT *H,const UCHAR *Mac,BOOLEAN Delete)
{
    auto Peer=Hfind(H,Mac);if(Delete&&Peer&&Peer->DeleteSent)return STATUS_SUCCESS;
    ULONG B[6]={Jtlv(Delete?98:97,Delete?12:16),1,0,0,0,0};RtlCopyMemory(B+2,Mac,6);
    if(!Delete)B[5]=Jtlv(18,0);NTSTATUS St=WcnWmiSend(H->D,Delete?0x6002:0x6001,B,Delete?16:24);if(Delete&&Peer&&NT_SUCCESS(St))Peer->DeleteSent=TRUE;return St;
}
static void Hfinish(HOTSPOT *H,NTSTATUS Status)
{
    if(!H->Task)return;UINT16 Id=H->Task==WDI_TASK_START_AP?WDI_INDICATION_START_AP_COMPLETE:H->Task==WDI_TASK_STOP_AP?WDI_INDICATION_STOP_AP_COMPLETE:H->Task==WDI_TASK_DISCONNECT?WDI_INDICATION_DISCONNECT_COMPLETE:WDI_INDICATION_DOT11_RESET_COMPLETE;
    H->Task=0;S8Indicate((S8*)H->D->Stage8,&H->TaskHeader,Id,Status);
}
static void HfinishAdvert(HOTSPOT *H,NTSTATUS Status)
{
    if(!H->AdvertRequest)return;
    auto R=H->AdvertRequest;H->AdvertRequest=nullptr;S8Complete(R,Status);

}
static void Hfail(HOTSPOT *H,PCSTR Reason)
{
    if(H->Failed)return;H->Failed=TRUE;H->Running=FALSE;if(H->Go)H->Go->Resetting=FALSE;HfinishAdvert(H,STATUS_DEVICE_HARDWARE_ERROR);
    WcnLog(H->D,"hotspot: FAIL step=%u client-step=%u reason=%s; STA context unchanged",H->Step,H->Current->ClientStep,Reason);
    if(H->Go){H->Go->Fatal=TRUE;S8SetLink(H->Go,false);}Hfinish(H,STATUS_DEVICE_HARDWARE_ERROR);
    if(H->DefaultRequest){auto R=H->DefaultRequest;H->DefaultRequest=nullptr;S8Complete(R,STATUS_DEVICE_HARDWARE_ERROR);}
    if(H->KeyRequest){auto R=H->KeyRequest;H->KeyRequest=nullptr;S8Complete(R,STATUS_DEVICE_HARDWARE_ERROR);}RtlSecureZeroMemory(H->Keys,sizeof(H->Keys));
}
NTSTATUS HotspotCreate(PWCN_DEVICE D)
{
    auto H=(HOTSPOT*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(HOTSPOT),WCN_TAG);if(!H)return STATUS_INSUFFICIENT_RESOURCES;
    H->D=D;H->Current=H->Clients;D->Hotspot=H;H->DevicePort=MAXULONG;return STATUS_SUCCESS;
}
NTSTATUS HotspotCapabilities(PWCN_DEVICE D)
{
    auto H=Hs(D);if(!H)return STATUS_DEVICE_NOT_READY;RtlCopyMemory(H->Mac,D->Ce.MacAddress,6);H->Mac[0]|=2;H->Mac[5]^=1;
    RtlCopyMemory(H->DeviceMac,H->Mac,6);H->DeviceMac[5]^=2;
    auto &C=H->Caps;WIFI_WIFIDIRECT_CAPABILITIES_INIT(&C);C.WFDRoleCount=C.ConcurrentGOCount=C.ConcurrentClientCount=1;
    // Native WiFi requires device/GO/client capability bits together; client MLME is not yet verified.
    C.GOClientTableSize=(UCHAR)WCN_HOTSPOT_CLIENTS;C.AssociationTableSize=WCN_HOTSPOT_CLIENTS;C.DiscoveryFilterListSize=C.DesiredSSIDListSize=C.PrivacyExemptionListSize=1;C.ScanSSIDListSize=4; // Native ExtAP attach requires at least four scan SSIDs.
    C.DefaultKeyTableSize=4;C.MaxVendorSpecificExtensionIESize=512;C.GOon5GHzBandSupported=TRUE;
    RtlCopyMemory(C.DeviceAddress,H->DeviceMac,6);RtlCopyMemory(H->RoleAddress.Address,H->Mac,6);C.NumInterfaceAddresses=1;C.InterfaceAddressList=&H->RoleAddress;
    auto S=(S8*)D->Stage8;H->SaeCapable=S->PmfAvailable&&S->Tlv.PeerVersion>=WDI_VERSION_2_0_11;H->DefaultMgmtKey=4;
    H->AuthPairs[0]={DOT11_AUTH_ALGO_RSNA_PSK,DOT11_CIPHER_ALGO_CCMP};H->AuthPairs[1]={DOT11_AUTH_ALGO_WPA3_SAE,DOT11_CIPHER_ALGO_CCMP};
    C.DefaultKeyTableSize=H->SaeCapable?6:4;C.NumSupportedUnicastAlgorithms=C.NumSupportedMulticastDataAlgorithms=H->SaeCapable?2:1;C.UnicastAlgorithms=C.MulticastDataAlgorithms=H->AuthPairs;
    NTSTATUS St=WifiDeviceSetWiFiDirectCapabilities(D->Device,&C);return St;
}
NTSTATUS HotspotDirectDevice(WDFDEVICE Device,WIFIDIRECT_DEVICE_INIT *Init)
{
    auto H=Hs(WcnGetDevice(Device));if(!H||H->Device)return STATUS_NOT_SUPPORTED;
    NTSTATUS St=WifiDirectDeviceCreate(Init,WDF_NO_OBJECT_ATTRIBUTES,&H->Device);if(!NT_SUCCESS(St))return St;
    St=WifiDirectDeviceInitialize(H->Device);if(NT_SUCCESS(St))H->DevicePort=WifiDirectDeviceGetPortId(H->Device);
    return St;
}
S8 *HotspotAdapter(PWCN_DEVICE D)
{
    auto H=Hs(D);if(!H)return nullptr;auto S=H->Go;
    if(S){bool Queued=S->RxQueue!=nullptr;for(auto Q:S->TxQueue)if(Q)Queued=true;if(!S->AdapterGone||Queued||H->Running||H->Task||(D->P2pClient&&(S->Resetting||D->P2pJoin.Started||D->P2pJoin.Resetting||D->P2pJoin.PeerCreated||D->P2pJoin.VdevStarted)))return nullptr;RtlSecureZeroMemory(S,sizeof(*S));S->J=&D->Join;}
    else{S=(S8*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(S8),WCN_TAG);if(!S)return nullptr;H->Go=S;}
    S->D=D;S->J=D->P2pClient?&D->P2pJoin:&D->Join;S->HotspotRole=!D->P2pClient;S->P2pRole=TRUE;D->RoleStage8=S;S->Radio=TRUE;S->Tlv=((S8*)D->Stage8)->Tlv;KeInitializeSpinLock(&S->Lock);RtlCopyMemory(S->Mac,H->Mac,6);return S;
}
static const WCN_JOIN_AP *Hstation(HOTSPOT *H,ULONG Frequency)
{
    PWCN_JOIN Links[]={&H->D->Join,&H->D->Secondary};
    for(auto J:Links)if(J->Ap.Valid&&J->Ap.Frequency==Frequency&&(J->Started||J->VdevStarted||WcnStage8ConnectRequested(H->D,J)))return &J->Ap;
    return nullptr;
}
static bool Hchannel(HOTSPOT *H,const WDI_TASK_START_AP_PARAMETERS &P)
{
    auto D=H->D;for(ULONG Pass=0;Pass<5;Pass++)for(ULONG I=0;I<D->Scan.ChannelCount;I++){
        auto C=&D->Scan.Channel[I];if((((S8*)D->Stage8)->DisabledBands|(H->Go?H->Go->DisabledBands:0))&(WcnIs2g(C->Frequency)?1u:2u))continue;if(C->Flags&(1u|2u|8u|64u|2048u))continue;
        if(!WcnConnectFrequency(C->Frequency)||(P2pGroupFrequency(D)&&P2pGroupFrequency(D)!=C->Frequency))continue;bool Allowed=!P.Optional.APBandChannelList_IsPresent||!P.APBandChannelList.ElementCount;
        for(ULONG J=0;J<P.APBandChannelList.ElementCount;J++){auto B=&P.APBandChannelList.pElements[J];if(B->BandID!=(WcnIs2g(C->Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000))continue;
            if(!B->Optional.ChannelList_IsPresent||!B->ChannelList.ElementCount)Allowed=true;
            for(ULONG K=0;K<B->ChannelList.ElementCount;K++)if(B->ChannelList.pElements[K]==WcnFrequencyChannel(C->Frequency))Allowed=true;}
        if(P.StaticParameters.MustUseSpecifiedChannels&&P.Optional.AdvertisedOperatingChannel_IsPresent&&P.AdvertisedOperatingChannel.ChannelNumber!=WcnFrequencyChannel(C->Frequency))Allowed=false;
        bool Preferred=P.Optional.AdvertisedOperatingChannel_IsPresent&&WcnChannelFrequency(P.AdvertisedOperatingChannel.ChannelNumber)==C->Frequency;
        if(!Allowed||(Pass<2&&!Hstation(H,C->Frequency))||((Pass==0||Pass==2)&&!Preferred)||(Pass==3&&!WcnIs2g(C->Frequency)))continue;
        H->Frequency=C->Frequency;H->Power=C->Power;H->Gain=C->Gain;return true;
    }return false;
}
static bool HwideAllowed(HOTSPOT *H,ULONG Center,ULONG Width)
{
    if(!WcnWidthAllowed(H->D,Center,Width))return false;
    auto A=Hstation(H,H->Frequency);
    if(A&&(Center-Width/2<A->CenterFrequency-A->Width/2||Center+Width/2>A->CenterFrequency+A->Width/2))return false;
    for(ULONG F=Center-Width/2+10;F<Center+Width/2;F+=20)for(ULONG I=0;I<H->D->Scan.ChannelCount;I++)if(H->D->Scan.Channel[I].Frequency==F&&(H->D->Scan.Channel[I].Flags&8))return false;
    return true;
}
static bool HcoexistSeen(HOTSPOT *H)
{
    auto S=(S8*)H->D->Stage8;
    for(ULONG I=0;I<S->BssCount;I++){auto E=&S->Bss[I];if(!WcnIs2g(E->Frequency)||WcnNowMs()-E->At>30000)continue;
        LONG Delta=(LONG)E->Frequency-(LONG)H->Frequency;if(Delta<-35||Delta>35)continue;
        for(ULONG K=36;K+2<=E->Length;){ULONG N=E->Bytes[K+1];if(N>E->Length-K-2)break;
            if(E->Bytes[K]==45&&N==26&&(Jle16(E->Bytes+K+2)&0x4000))return true;
            if(E->Bytes[K]==72&&N==1&&(E->Bytes[K+2]&6))return true;K+=N+2;}
    }return false;
}
static void Hphy(HOTSPOT *H)
{
    auto D=H->D;auto A=&H->Radio;auto C=WcnRadioCaps(D);RtlZeroMemory(A,sizeof(*A));ULONG B=WcnIs2g(H->Frequency)?0:1;
    A->Frequency=A->CenterFrequency=H->Frequency;A->Width=20;A->Nss=(UCHAR)D->Ce.NumRfChains;A->Qos=TRUE;A->Ht=(D->Ce.HtCap&1)!=0;A->Vht=A->Ht&&B&&D->Ce.VhtCap;
    A->Auth=H->AllowSae&&!H->AllowPsk?WCN_AUTH_SAE:WCN_AUTH_PSK;A->Power=H->Power;A->Gain=H->Gain;
    static const UCHAR Edca[]={3,0xa4,0,0,0x27,0xa4,0,0,0x42,0x43,94,0,0x62,0x32,47,0};RtlCopyMemory(A->Edca,Edca,16);
    if(C&&A->Ht){A->He=(C->HeMcs[B]&3)!=3&&C->HeMcs[B]!=0;A->HeMcs[0]=(USHORT)C->HeMcs[B];A->HeMcs[1]=(USHORT)(C->HeMcs[B]>>16);A->HeOps=1u<<24;
        ULONG Maximum=20u<<(C->WidthCode[B]>3?3:C->WidthCode[B]);if(!B&&Maximum>40)Maximum=40;if(!A->Vht&&!A->He&&Maximum>40)Maximum=40;
        if(Maximum>=160&&!(A->Vht&&(D->Ce.VhtCap&12))&&!(A->He&&(C->HePhy[B][0]&8)&&(A->HeMcs[1]&3)!=3))Maximum=80;
        if(!B&&H->Coexist20)Maximum=20;
        for(ULONG Width=Maximum;Width>=40;Width/=2){
            if(!B){ULONG Preferred=A->Frequency<=2447?A->Frequency+10:A->Frequency-10,Other=A->Frequency<=2447?A->Frequency-10:A->Frequency+10;
                if(A->He&&!(C->HePhy[0][0]&2))break;
                if(HwideAllowed(H,Preferred,40)){A->Width=40;A->CenterFrequency=Preferred;break;}
                if(HwideAllowed(H,Other,40)){A->Width=40;A->CenterFrequency=Other;break;}
            }else{ULONG First=A->Frequency>=5745?5745:5180,Center=First+((A->Frequency-First)/Width)*Width+Width/2-10;
                if(HwideAllowed(H,Center,Width)){A->Width=(UCHAR)Width;A->CenterFrequency=Center;break;}}
        }
        for(ULONG F=A->CenterFrequency-A->Width/2+10;F<A->CenterFrequency+A->Width/2;F+=20)for(ULONG I=0;I<D->Scan.ChannelCount;I++)if(D->Scan.Channel[I].Frequency==F){
            if(D->Scan.Channel[I].Power<A->Power)A->Power=D->Scan.Channel[I].Power;if(D->Scan.Channel[I].Gain<A->Gain)A->Gain=D->Scan.Channel[I].Gain;break;}
        A->Eht=A->He&&C->EhtValid;
        for(ULONG M=0;M<2;M++)for(ULONG I=0;I<3;I++){ULONG V=C->EhtMcs[B][M+1]>>(I*8),K=(V&15)<((V>>4)&15)?V&15:(V>>4)&15;if(K>A->Nss)K=A->Nss;A->EhtMcs[M][I]=(UCHAR)(K|(K<<4));}
        if(!A->EhtMcs[A->Width==160?1:0][0])A->Eht=FALSE;
    }
    A->Sgi=A->Vht&&A->Width>=80?!!(D->Ce.VhtCap&(A->Width==160?64:32)):!!(D->Ce.HtCap&(A->Width==40?0x800:2));
    A->HtRateCount=(UCHAR)(A->Ht?A->Nss*8:0);for(ULONG I=0;I<A->HtRateCount;I++)A->HtRates[I]=(UCHAR)I;
    A->VhtMcs=65535;for(ULONG I=0;I<A->Nss;I++)A->VhtMcs=(USHORT)((A->VhtMcs&~(3u<<(I*2)))|(2u<<(I*2)));
    A->PhyMode=WcnPhyMode(A);A->RateKbps=WcnPhyRate(A);

}
extern "C" VOID WcnHotspotFail(PWCN_DEVICE D,PCSTR Reason)
{auto H=Hs(D);if(H)Hfail(H,Reason);}
static bool HbaBusy(HOTSPOT *H)
{for(auto &C:H->Clients)for(auto &A:C.Ba)if(A.Phase)return true;return false;}
static bool HvalidIes(const UCHAR *P,ULONG N)
{while(N){if(N<2||P[1]>N-2)return false;ULONG L=P[1]+2;if(P[0]!=221&&P[0]!=127)return false;P+=L;N-=L;}return true;}
static ULONG HextraIes(HOTSPOT *H,UCHAR *Out,const UCHAR *In,ULONG N)
{
    RtlCopyMemory(Out,In,N);if(!WcnIs2g(H->Frequency)||!H->Radio.Ht)return N;
    for(ULONG I=0;I<N;I+=Out[I+1]+2)if(Out[I]==127){if(Out[I+1]){Out[I+2]|=1;return N;}RtlMoveMemory(Out+I+3,Out+I+2,N-I-2);Out[I+1]=Out[I+2]=1;return N+1;}
    Out[N]=127;Out[N+1]=Out[N+2]=1;return N+3;
}
static ULONG Hies(HOTSPOT *H,UCHAR *F,BOOLEAN Tim)
{
    ULONG N=0;F[N++]=0;F[N++]=H->SsidLength;RtlCopyMemory(F+N,H->Ssid,H->SsidLength);N+=H->SsidLength;
    ULONG First=H->RateCount>8?8:H->RateCount;F[N++]=1;F[N++]=(UCHAR)First;RtlCopyMemory(F+N,H->Rates,First);N+=First;
    if(H->RateCount>8){F[N++]=50;F[N++]=H->RateCount-8;RtlCopyMemory(F+N,H->Rates+8,H->RateCount-8);N+=H->RateCount-8;}
    F[N++]=3;F[N++]=1;F[N++]=(UCHAR)WcnFrequencyChannel(H->Frequency);
    if(Tim){H->TimOffset=36+N;F[N++]=5;F[N++]=4;F[N++]=0;F[N++]=(UCHAR)H->Dtim;F[N++]=0;F[N++]=0;}
    N+=WcnRsnBuildAp(F+N,H->AllowPsk,H->AllowSae);N+=WcnPhyBuildIes(H->D,&H->Radio,F+N,TRUE);N+=WcnPhyOperationIes(&H->Radio,F+N);
    if(WcnIs2g(H->Frequency)&&H->Radio.Ht&&H->Radio.Width==40){static const USHORT Obss[]={20,10,300,200,20,5,25};F[N++]=74;F[N++]=14;for(auto V:Obss){Jputle16(F+N,V);N+=2;}}static const UCHAR WpsOui[]={0,0x50,0xf2,4};N+=P2pGroupIes(H->D,H->Ssid,H->SsidLength,F+N,1024-N,!P2pVendor(Tim?H->Advert:H->ProbeAdvert,Tim?H->AdvertLength:H->ProbeAdvertLength,WpsOui));return N;
}
static NTSTATUS Hbeacon(HOTSPOT *H)
{
    UCHAR *F=H->Beacon;RtlZeroMemory(F,sizeof(H->Beacon));F[0]=0x80;RtlFillMemory(F+4,6,255);RtlCopyMemory(F+10,H->Mac,6);RtlCopyMemory(F+16,H->Mac,6);
    Jputle16(F+32,H->BeaconPeriod);Jputle16(F+34,0x0411);ULONG N=36+Hies(H,F+36,TRUE);if(N+H->AdvertLength+3>sizeof(H->Beacon))return STATUS_INVALID_BUFFER_SIZE;N+=HextraIes(H,F+N,H->Advert,H->AdvertLength);H->BeaconLength=N;
    ULONG *B=H->Wire;RtlZeroMemory(B,sizeof(H->Wire));B[0]=Jtlv(103,52);B[1]=1;B[2]=H->TimOffset;B[3]=N;B[14]=Jtlv(105,8);B[17]=Jtlv(17,(N+3)&~3u);RtlCopyMemory(B+18,F,N);
    return WcnWmiSend(H->D,0x7003,B,72+((N+3)&~3u));
}
static NTSTATUS Hmgmt(HOTSPOT *H,const UCHAR *F,ULONG N,HOTSPOT_CLIENT *Client=nullptr)
{
    if(H->MgmtBusy)return STATUS_DEVICE_BUSY;if(N<24||N>H->Mgmt.Length)return STATUS_INVALID_BUFFER_SIZE;
    RtlCopyMemory(H->Mgmt.Va,F,N);
    bool Robust=WcnRobustManagement(F,N)!=FALSE;
    if(Client&&Client->ClientPhy.Pmf&&Robust){if(!Client->PairKey.Valid)return STATUS_INVALID_DEVICE_STATE;NTSTATUS Pmf=WcnPmfTxUnicast(H->Mgmt.Va,&N,(ULONG)H->Mgmt.Length,&Client->MgmtTxPn);if(!NT_SUCCESS(Pmf))return Pmf;}
    RtlZeroMemory(H->Wire,sizeof(H->Wire));auto C=(wmi_mgmt_send_cmd*)H->Wire;JHDR(C,422);
    C->vdev_id=1;C->desc_id=WcnMgmtAllocate(H->D,1);if(!C->desc_id)return STATUS_DEVICE_BUSY;H->MgmtId=C->desc_id;C->paddr_lo=(ULONG)H->Mgmt.La;C->paddr_hi=(ULONG)(H->Mgmt.La>>32);C->frame_len=C->buf_len=N;
    ULONG Off=sizeof(*C)/4;H->Wire[Off++]=Jtlv(17,(N+3)&~3u);RtlCopyMemory(H->Wire+Off,F,N);KeMemoryBarrier();NTSTATUS St=WcnWmiSend(H->D,0x7008,H->Wire,Off*4+((N+3)&~3u));
    if(NT_SUCCESS(St)){H->MgmtBusy=TRUE;H->MgmtStatus=MAXULONG;H->MgmtAt=WcnNowMs();H->MgmtClient=Client;if(Client){Client->MgmtBusy=TRUE;Client->MgmtStatus=MAXULONG;}}else H->D->MgmtInUse[1]=0;return St;
}
static ULONG Hheader(HOTSPOT *H,UCHAR *F,UCHAR Kind,const UCHAR *Dest)
{
    RtlZeroMemory(F,24);F[0]=Kind;RtlCopyMemory(F+4,Dest,6);RtlCopyMemory(F+10,H->Mac,6);RtlCopyMemory(F+16,H->Mac,6);Jputle16(F+22,(++H->Sequence&4095)<<4);return 24;
}
extern "C" NTSTATUS WcnHotspotActionSend(PWCN_DEVICE D,const UCHAR *Body,ULONG N)
{auto H=Hs(D);if(!H||!H->Current->Authorized||N>32)return STATUS_DEVICE_NOT_READY;UCHAR F[56];Hheader(H,F,0xd0,H->Current->Client);RtlCopyMemory(F+24,Body,N);return Hmgmt(H,F,24+N,H->Current);}
static NTSTATUS HclientAssoc(HOTSPOT *H)
{return WcnPhyPeer(H->D,&H->Current->ClientPhy,H->Current->Client,1,Haid(H),H->Current->ClientListen,H->Wire);}
static void HremoveOsPeer(HOTSPOT *H)
{if(H->Current->OsPeer){S8_ADAPTER_REF A(H->Go);if(A.Handle){NET_EUI48_ADDRESS Mac;RtlCopyMemory(Mac.Value,H->Current->Client,6);WifiAdapterRemovePeer(A.Handle,&Mac);}H->Current->OsPeer=FALSE;}}
static void HassocComplete(HOTSPOT *H,NTSTATUS Status)
{
    if(!H->Current->AssocPending)return;H->Current->AssocPending=FALSE;S8_ADAPTER_REF Ref(H->Go);if(!Ref.Handle)return;auto S=(S8*)H->D->Stage8;
    WDI_INDICATION_SEND_AP_ASSOCIATION_RESPONSE_COMPLETE_PARAMETERS P;auto A=&P.AssocResponseResult;RtlCopyMemory(A->PeerMACAddress.Address,H->Current->Client,6);
    A->AuthAlgorithm=(WDI_AUTH_ALGORITHM)H->Current->Auth;A->UnicastCipherAlgorithm=A->MulticastCipherAlgorithm=WDI_CIPHER_ALGO_CCMP;
    if(NT_SUCCESS(Status)&&H->Current->Associated&&!H->Current->OsPeer){NET_EUI48_ADDRESS Mac;RtlCopyMemory(Mac.Value,H->Current->Client,6);WifiAdapterAddPeer(Ref.Handle,&Mac);H->Current->OsPeer=TRUE;}
    P.AssocResponseFrame.SimpleAssign(H->Current->Assoc+24,H->Current->AssocLength-24);// The Windows GO security path skips the 12-byte beacon fixed body before parsing RSN.
    P.BeaconIEs.SimpleAssign(H->Beacon+24,H->BeaconLength-24);auto C=&H->Current->ClientPhy;WDI_PHY_TYPE Type=C->Eht?WDI_PHY_TYPE_EHT:C->He?WDI_PHY_TYPE_HE:C->Vht?WDI_PHY_TYPE_VHT:C->Ht?WDI_PHY_TYPE_HT:C->PhyMode==2?WDI_PHY_TYPE_HRDSSS:WcnIs2g(H->Frequency)?WDI_PHY_TYPE_ERP:WDI_PHY_TYPE_OFDM;P.ActivePhyList.SimpleAssign(&Type,1);
    UCHAR *Out=nullptr;ULONG N=0;auto Ns=GenerateWdiIndicationSendApAssociationResponseCompleteFromIhv(&P,0,&S->Tlv,&N,&Out);
    if(Ns==NDIS_STATUS_SUCCESS){S8Indicate(S,&H->Current->AssocHeader,WDI_INDICATION_SEND_AP_ASSOCIATION_RESPONSE_COMPLETE,Status,Out,N);FreeGenerated(Out);}else Hfail(H,"AP association indication encoding");
}
static void HclientDrop(HOTSPOT *H);
static NTSTATUS HsaeIndicate(HOTSPOT *H,HOTSPOT_CLIENT *C,WDI_SAE_INDICATION_TYPE Type,const UCHAR *Body,ULONG Length)
{WDI_MESSAGE_HEADER Header={};Header.PortId=(WDI_PORT_ID)H->Go->Port;return S8SaeIndicate((S8*)H->D->Stage8,Header,C->Client,Type,Body,Length);}
static NTSTATUS HsaeCommand(HOTSPOT *H,const UCHAR *P,ULONG N)
{
    auto S=(S8*)H->D->Stage8;WDI_SET_SAE_AUTH_PARAMS_COMMAND A;auto Ns=ParseWdiSetSaeAuthParamsToIhv(N,P,&S->Tlv,&A);NTSTATUS St=STATUS_INVALID_PARAMETER;
    if(Ns!=NDIS_STATUS_SUCCESS){CleanupParsedWdiSetSaeAuthParams(&A);return St;}
    auto C=Hfind(H,A.BssId.Address);
    if(C&&H->Running&&H->AllowSae&&!H->Task&&C->Auth==WCN_AUTH_SAE&&C->ClientStep<19){
        if(A.SAERequestType==WDI_SAE_REQUEST_TYPE_FAILURE){H->Current=C;HclientDrop(H);St=STATUS_SUCCESS;}
        else if(A.SAERequestType==WDI_SAE_REQUEST_TYPE_SUCCESS){
            if(C->Sae.PeerConfirm&&(!A.Optional.SAEStatus_IsPresent||A.SAEStatus==WDI_SAE_STATUS_SUCCESS)){
                C->Sae.Validated=TRUE;C->Sae.Authenticated=C->Sae.ConfirmSent;St=STATUS_SUCCESS;}}
        else{
            ULONG Length=S8SaeEncode(A,C->Sae.Tx,19,true);
            if(Length){C->Sae.Length=Length;C->Sae.Generation++;C->Sae.Attempts=0;C->Sae.At=WcnNowMs();
                if(Jle16(C->Sae.Tx+2)==2){if(C->Sae.PeerConfirm)C->Sae.Validated=TRUE;}
                else{C->Sae.ConfirmSent=C->Sae.Validated=C->Sae.Authenticated=FALSE;}
                St=STATUS_SUCCESS;}}
    }
    CleanupParsedWdiSetSaeAuthParams(&A);return St;
}
static void HsaeService(HOTSPOT *H)
{
    auto C=H->Current;if(C->Auth!=WCN_AUTH_SAE||C->ClientStep>=19||C->Associated)return;auto A=&C->Sae;
    if(A->Sending){if(C->MgmtBusy)return;A->Sending=FALSE;
        if(!C->MgmtStatus){if(A->Sequence==2){A->ConfirmSent=TRUE;A->Authenticated=A->Validated&&A->PeerConfirm;}
            if(A->SentGeneration==A->Generation){RtlSecureZeroMemory(A->Tx,sizeof(A->Tx));A->Length=0;}}
        else if(++A->Attempts>=3){HclientDrop(H);return;}}
    if(A->Authenticated)return;
    if(A->At&&WcnNowMs()-A->At>5000){HsaeIndicate(H,C,WDI_SAE_INDICATION_TYPE_ERROR,nullptr,0);HclientDrop(H);return;}
    if(!A->Length||C->ClientStep||!C->ClientMapped||H->Task)return;
    UCHAR F[512];Hheader(H,F,0xb0,C->Client);RtlCopyMemory(F+24,A->Tx,A->Length);
    NTSTATUS St=Hmgmt(H,F,24+A->Length,C);if(St==STATUS_DEVICE_BUSY)return;
    if(!NT_SUCCESS(St)){HclientDrop(H);return;}
    A->Sending=TRUE;A->Sequence=Jle16(A->Tx+2);A->SentGeneration=A->Generation;
}
static NTSTATUS Hkeys(HOTSPOT *H,WIFIREQUEST R,const UCHAR *P,ULONG N)
{
    if(H->KeyRequest||!H->Running)return STATUS_DEVICE_BUSY;WDI_SET_ADD_CIPHER_KEYS_PARAMETERS K;auto S=(S8*)H->D->Stage8;
    auto Ns=ParseWdiSetAddCipherKeysToIhv(N,P,&S->Tlv,&K);NTSTATUS St=STATUS_SUCCESS;
    if(Ns!=NDIS_STATUS_SUCCESS||!K.SetCipherKey.ElementCount||K.SetCipherKey.ElementCount>4)St=STATUS_INVALID_PARAMETER;
    for(ULONG I=0;NT_SUCCESS(St)&&I<K.SetCipherKey.ElementCount;I++){
        auto A=&K.SetCipherKey.pElements[I];auto T=&H->Keys[I];auto Type=A->CipherKeyTypeInfo.KeyType;
        bool Mgmt=Type==WDI_CIPHER_KEY_TYPE_IGTK,Group=Type==WDI_CIPHER_KEY_TYPE_GROUP_KEY;ULONG Id=A->Optional.CipherKeyID_IsPresent?A->CipherKeyID.CipherKeyID:0;
        auto Direction=A->CipherKeyTypeInfo.Direction;
        if((A->Optional.LinkId_IsPresent&&A->LinkId!=0)||(Direction!=WDI_CIPHER_KEY_DIRECTION_BOTH&&(!(Group||Mgmt)||Direction!=WDI_CIPHER_KEY_DIRECTION_OUTBOUND))){St=STATUS_NOT_SUPPORTED;break;}
        auto Peer=Group||Mgmt?nullptr:(A->Optional.PeerMacAddress_IsPresent?Hfind(H,A->PeerMacAddress.Address):nullptr);
        if(Mgmt){if(!H->AllowSae||Id<4||Id>5||A->CipherKeyTypeInfo.CipherAlgorithm!=WDI_CIPHER_ALGO_BIP||!A->Optional.BIPKey_IsPresent||A->BIPKey.ElementCount!=16){St=STATUS_NOT_SUPPORTED;break;}RtlCopyMemory(T->Bytes,A->BIPKey.pElements,16);}
        else{if((!Group&&Type!=WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY)||Id>3||(!Group&&Id)||A->CipherKeyTypeInfo.CipherAlgorithm!=WDI_CIPHER_ALGO_CCMP||!A->Optional.CCMPKey_IsPresent||A->CCMPKey.ElementCount!=16||(!Group&&(!Peer||!Peer->Associated))){St=STATUS_NOT_SUPPORTED;break;}RtlCopyMemory(T->Bytes,A->CCMPKey.pElements,16);}
        for(ULONG J=0;J<I;J++)if(H->Keys[J].Management==(BOOLEAN)Mgmt&&H->Keys[J].Group==(BOOLEAN)Group&&H->Keys[J].Index==Id&&H->KeyClient[J]==Peer)St=STATUS_INVALID_PARAMETER;
        H->KeyClient[I]=Peer;T->Management=Mgmt;T->Group=Group;T->Index=(UCHAR)Id;T->Sent=FALSE;
        if(A->Optional.ReceiveSequenceCount_IsPresent)RtlCopyMemory(T->Rsc,A->ReceiveSequenceCount.ReceiveSequenceCount,6);
    }
    if(NT_SUCCESS(St)){H->KeyDelete=FALSE;H->KeyCount=K.SetCipherKey.ElementCount;H->KeyStep=0;H->KeyRequest=R;H->KeyAt=WcnNowMs();St=STATUS_PENDING;}else RtlSecureZeroMemory(H->Keys,sizeof(H->Keys));
    CleanupParsedWdiSetAddCipherKeys(&K);return St;
}
static NTSTATUS HdeleteKeys(HOTSPOT *H,WIFIREQUEST R,const UCHAR *P,ULONG N)
{
    if(H->KeyRequest)return STATUS_DEVICE_BUSY;WDI_SET_DELETE_CIPHER_KEYS_PARAMETERS K;auto S=(S8*)H->D->Stage8;auto Ns=ParseWdiSetDeleteCipherKeysToIhv(N,P,&S->Tlv,&K);NTSTATUS St=STATUS_SUCCESS;
    if(Ns!=NDIS_STATUS_SUCCESS||!K.CipherKeyInfo.ElementCount||K.CipherKeyInfo.ElementCount>4)St=STATUS_INVALID_PARAMETER;
    for(ULONG I=0;NT_SUCCESS(St)&&I<K.CipherKeyInfo.ElementCount;I++){
        auto A=&K.CipherKeyInfo.pElements[I];auto T=&H->Keys[I];auto Type=A->CipherKeyTypeInfo.KeyType;bool Mgmt=Type==WDI_CIPHER_KEY_TYPE_IGTK,Group=Type==WDI_CIPHER_KEY_TYPE_GROUP_KEY;
        ULONG Id=A->Optional.CipherKeyID_IsPresent?A->CipherKeyID.CipherKeyID:0;auto Peer=Group||Mgmt?nullptr:(A->Optional.PeerMacAddress_IsPresent?Hfind(H,A->PeerMacAddress.Address):nullptr);
        if((A->Optional.LinkId_IsPresent&&A->LinkId!=0)||(Mgmt?(Id<4||Id>5):Id>3||(!Group&&(Type!=WDI_CIPHER_KEY_TYPE_PAIRWISE_KEY||Id||!A->Optional.PeerMacAddress_IsPresent)))){St=STATUS_INVALID_PARAMETER;break;}
        H->KeyClient[I]=Peer;T->Management=Mgmt;T->Group=Group;T->Index=(UCHAR)Id;
    }
    if(NT_SUCCESS(St)){for(ULONG I=0;I<K.CipherKeyInfo.ElementCount;I++)for(auto &C:H->Clients)if(H->KeyClient[I]==&C||H->Keys[I].Group||(H->Keys[I].Management&&C.ClientPhy.Pmf))C.Authorized=FALSE;
        H->KeyDelete=TRUE;H->KeyCount=K.CipherKeyInfo.ElementCount;H->KeyStep=0;H->KeyRequest=R;H->KeyAt=WcnNowMs();St=STATUS_PENDING;}else RtlSecureZeroMemory(H->Keys,sizeof(H->Keys));
    CleanupParsedWdiSetDeleteCipherKeys(&K);return St;
}
bool HotspotCommand(S8 *S,WIFIREQUEST R,const WDI_MESSAGE_HEADER &Header,UINT16 Id,const UCHAR *P,ULONG N)
{
    auto H=Hs(S->D);if(!H)return false;bool Role=H->Go&&H->Go->Adapter&&Header.PortId==H->Go->Port;
    if(!Role&&Header.PortId!=H->DevicePort)return false;
    if(Role&&S->D->P2pClient&&Id!=WDI_TASK_CHANGE_OPERATION_MODE&&Id!=WDI_SET_ADVERTISEMENT_INFORMATION)return false;NTSTATUS St=STATUS_NOT_SUPPORTED;
    if(H->Failed){S8Complete(R,STATUS_DEVICE_HARDWARE_ERROR);return true;}
    switch(Id){
    case WDI_SET_ADAPTER_CONFIGURATION:St=S8Configure(Role?H->Go:S,P,N,Role?H->Mac:H->DeviceMac);break;
    case WDI_TASK_START_AP:{
        WDI_TASK_START_AP_PARAMETERS A;auto Ns=ParseWdiTaskStartApToIhv(N,P,&S->Tlv,&A);St=STATUS_INVALID_PARAMETER;
        bool Psk=false,Sae=false,Security=Ns==NDIS_STATUS_SUCCESS&&A.AuthenticationAlgorithms.ElementCount>0&&A.AuthenticationAlgorithms.ElementCount<=2;
        if(Security)for(ULONG I=0;I<A.AuthenticationAlgorithms.ElementCount;I++){auto Auth=A.AuthenticationAlgorithms.pElements[I];if(Auth==WDI_AUTH_ALGO_RSNA_PSK&&!Psk)Psk=true;else if(Auth==WDI_AUTH_ALGO_WPA3_SAE&&H->SaeCapable&&!Sae)Sae=true;else Security=false;}
        if(Ns==NDIS_STATUS_SUCCESS&&Role&&!H->Task&&!H->Running&&A.DesiredSSID.ElementCount&&A.DesiredSSID.ElementCount<=32&&Security&&A.UnicastCipherAlgorithms.ElementCount==1&&A.UnicastCipherAlgorithms.pElements[0]==WDI_CIPHER_ALGO_CCMP&&A.MulticastCipherAlgorithms.ElementCount==1&&A.MulticastCipherAlgorithms.pElements[0]==WDI_CIPHER_ALGO_CCMP&&Hchannel(H,A)){
            H->AllowPsk=Psk;H->AllowSae=Sae;H->Coexist20=WcnIs2g(H->Frequency)&&HcoexistSeen(H);Hphy(H);H->SsidLength=(UCHAR)A.DesiredSSID.ElementCount;RtlCopyMemory(H->Ssid,A.DesiredSSID.pElements,H->SsidLength);H->BeaconPeriod=A.StaticParameters.BeaconPeriod?A.StaticParameters.BeaconPeriod:100;H->Dtim=A.StaticParameters.DTIMPeriod?A.StaticParameters.DTIMPeriod:2;
            static const UCHAR Rates[]={0x82,0x84,0x8b,0x96,0x8c,18,0x98,36,0xb0,72,96,108};ULONG First=WcnIs2g(H->Frequency)&&A.StaticParameters.DataRateSupport802_11b?0:4;
            H->RateCount=(UCHAR)(sizeof(Rates)-First);RtlCopyMemory(H->Rates,Rates+First,H->RateCount);if(!First)for(ULONG I=4;I<H->RateCount;I++)H->Rates[I]&=127;
            St=H->BeaconPeriod>65535||H->Dtim>255?STATUS_INVALID_PARAMETER:STATUS_SUCCESS;
            if(NT_SUCCESS(St)){H->Task=Id;H->TaskHeader=Header;H->Step=H->Created?2:0;H->At=WcnNowMs();H->Started=H->Stopped=H->Go->Connect=H->BeamformSet=FALSE;H->Go->Stopped=H->Go->Fatal=H->Go->Resetting=FALSE;}
        }CleanupParsedWdiTaskStartAp(&A);break;}
    case WDI_TASK_STOP_AP:case WDI_TASK_DOT11_RESET:
        if(!Role){St=STATUS_SUCCESS;S8Complete(R,St);S8Indicate(S,&Header,WDI_INDICATION_DOT11_RESET_COMPLETE,St);return true;}
        if(H->Task)St=STATUS_DEVICE_BUSY;else{H->Task=Id;H->TaskHeader=Header;H->Step=19;H->At=WcnNowMs();H->Running=FALSE;H->StopClient=0;for(auto &C:H->Clients){C.NotifyDisconnect=FALSE;C.ClientStep=0;}H->Go->AssocIndicated=FALSE;S8SetLink(H->Go,false);St=STATUS_SUCCESS;}break;
    case WDI_SET_ADVERTISEMENT_INFORMATION:{
        WDI_SET_ADVERTISEMENT_INFORMATION_PARAMETERS A;auto Ns=ParseWdiSetAdvertisementInformationToIhv(N,P,&S->Tlv,&A);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;bool Shared=A.Optional.DeviceInformation_IsPresent||A.Optional.DeviceCapability_IsPresent||A.Optional.GroupOwnerCapability_IsPresent||A.Optional.SecondaryDeviceTypeList_IsPresent;bool Changed=Shared;
        if(NT_SUCCESS(St)&&A.Optional.AdditionalIEs_IsPresent){auto B=&A.AdditionalIEs;
            if(B->AdditionalBeaconIEs.ElementCount>sizeof(H->Advert)||B->AdditionalProbeResponseIEs.ElementCount>sizeof(H->ProbeAdvert))St=STATUS_INVALID_BUFFER_SIZE;
            else if(!HvalidIes(B->AdditionalBeaconIEs.pElements,B->AdditionalBeaconIEs.ElementCount)||!HvalidIes(B->AdditionalProbeResponseIEs.pElements,B->AdditionalProbeResponseIEs.ElementCount))St=STATUS_NOT_SUPPORTED;
            else if(Role&&H->AdvertRequest)St=STATUS_DEVICE_BUSY;
            else if(Role){
                Changed=Changed||H->AdvertLength!=B->AdditionalBeaconIEs.ElementCount||!Jeq(H->Advert,B->AdditionalBeaconIEs.pElements,H->AdvertLength);
                H->AdvertLength=B->AdditionalBeaconIEs.ElementCount;H->ProbeAdvertLength=B->AdditionalProbeResponseIEs.ElementCount;
                if(H->AdvertLength)RtlCopyMemory(H->Advert,B->AdditionalBeaconIEs.pElements,H->AdvertLength);
                if(H->ProbeAdvertLength)RtlCopyMemory(H->ProbeAdvert,B->AdditionalProbeResponseIEs.pElements,H->ProbeAdvertLength);
            }}
        CleanupParsedWdiSetAdvertisementInformation(&A);
        // Role-specific IEs stay port-scoped; shared P2P identity refreshes the GO.
        if(NT_SUCCESS(St)&&(Role||Shared)&&H->Running&&Changed){
            H->AdvertRequest=R;H->AdvertAt=WcnNowMs();return true;}
        break;}
    case WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE:{
        WDI_TASK_SEND_AP_ASSOCIATION_RESPONSE_PARAMETERS A;auto Ns=ParseWdiTaskSendApAssociationResponseToIhv(N,P,&S->Tlv,&A);St=STATUS_INVALID_PARAMETER;auto Peer=Ns==NDIS_STATUS_SUCCESS?Hfind(H,A.IncomingRequestInfo.AssocRequestParams.PeerMacAddress.Address):nullptr;if(Peer)H->Current=Peer;
        if(Peer&&Role&&H->Running&&H->Current->ClientCreated&&H->Current->ClientMapped&&!H->Current->AssocPending&&Jeq(A.IncomingRequestInfo.AssocRequestParams.PeerMacAddress.Address,H->Current->Client,6)&&!A.IncomingRequestInfo.AssocRequestParams.IsReassociationRequest&&A.VendorIEs.ElementCount<=512&&HvalidIes(A.VendorIEs.pElements,A.VendorIEs.ElementCount)){
            H->Current->AssocLength=Hheader(H,H->Current->Assoc,0x10,H->Current->Client);Jputle16(H->Current->Assoc+24,0x0411);Jputle16(H->Current->Assoc+26,A.AssocResponseParameters.AcceptRequest?0:A.AssocResponseParameters.ReasonCode);Jputle16(H->Current->Assoc+28,(USHORT)(0xc000|Haid(H)));H->Current->AssocLength=30;
            H->Current->Assoc[H->Current->AssocLength++]=1;ULONG C=H->RateCount>8?8:H->RateCount;H->Current->Assoc[H->Current->AssocLength++]=(UCHAR)C;RtlCopyMemory(H->Current->Assoc+H->Current->AssocLength,H->Rates,C);H->Current->AssocLength+=C;
            if(H->RateCount>C){H->Current->Assoc[H->Current->AssocLength++]=50;H->Current->Assoc[H->Current->AssocLength++]=H->RateCount-(UCHAR)C;RtlCopyMemory(H->Current->Assoc+H->Current->AssocLength,H->Rates+C,H->RateCount-C);H->Current->AssocLength+=H->RateCount-C;}
            if(!H->Current->Provisioning)H->Current->AssocLength+=WcnRsnBuild(H->Current->Assoc+H->Current->AssocLength,H->Current->Auth,H->Current->ClientPhy.Pmf,H->Current->Auth==WCN_AUTH_SAE);
            H->Current->AssocLength+=WcnPhyBuildIes(H->D,&H->Current->ClientPhy,H->Current->Assoc+H->Current->AssocLength,TRUE);H->Current->AssocLength+=WcnPhyOperationIes(&H->Radio,H->Current->Assoc+H->Current->AssocLength);
            static const UCHAR P2pOui[]={0x50,0x6f,0x9a,9};if(H->Current->P2pPeer&&!P2pVendor(A.VendorIEs.pElements,A.VendorIEs.ElementCount,P2pOui)){UCHAR Status[]={221,8,0x50,0x6f,0x9a,9,0,1,0,(UCHAR)(A.AssocResponseParameters.AcceptRequest?0:1)};RtlCopyMemory(H->Current->Assoc+H->Current->AssocLength,Status,sizeof(Status));H->Current->AssocLength+=sizeof(Status);}
            RtlCopyMemory(H->Current->Assoc+H->Current->AssocLength,A.VendorIEs.pElements,A.VendorIEs.ElementCount);H->Current->AssocLength+=A.VendorIEs.ElementCount;
            H->Current->AssocHeader=Header;H->Current->AssocPending=TRUE;H->Current->AssocConfirmed=H->Current->Associated=H->Current->Authorized=H->Current->PairKey.Valid=FALSE;H->Current->ClientStep=A.AssocResponseParameters.AcceptRequest?10:12;H->Current->ClientAt=WcnNowMs();St=STATUS_SUCCESS;}
        CleanupParsedWdiTaskSendApAssociationResponse(&A);break;}
    case WDI_SET_SAE_AUTH_PARAMS:St=Role?HsaeCommand(H,P,N):STATUS_NOT_SUPPORTED;break;
    case WDI_SET_ADD_CIPHER_KEYS:St=Hkeys(H,R,P,N);if(St==STATUS_PENDING)return true;break;
    case WDI_SET_DELETE_CIPHER_KEYS:St=HdeleteKeys(H,R,P,N);if(St==STATUS_PENDING)return true;break;
    case WDI_SET_DEFAULT_KEY_ID:{WDI_SET_DEFAULT_KEY_ID_PARAMETERS A;auto Ns=ParseWdiSetDefaultKeyIdToIhv(N,P,&S->Tlv,&A);ULONG KeyId=A.DefaultKeyIdParameters.KeyID;
        St=Ns==NDIS_STATUS_SUCCESS&&KeyId<=5&&(KeyId<=3||H->AllowSae)?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
        if(NT_SUCCESS(St)){if(KeyId>=4)H->DefaultMgmtKey=(UCHAR)KeyId;
            else if(H->DefaultRequest)St=STATUS_DEVICE_BUSY;
            else{H->DefaultKey=(UCHAR)KeyId;H->DefaultPending=TRUE;H->DefaultRequest=R;H->DefaultAt=WcnNowMs();for(auto &C:H->Clients)C.Authorized=FALSE;}}
        CleanupParsedWdiSetDefaultKeyId(&A);if(NT_SUCCESS(St)&&KeyId<=3)return true;break;}
    case WDI_TASK_DISCONNECT:{WDI_TASK_DISCONNECT_PARAMETERS A;auto Ns=ParseWdiTaskDisconnectToIhv(N,P,&S->Tlv,&A);auto Peer=Ns==NDIS_STATUS_SUCCESS?Hfind(H,A.DisconnectParameters.MacAddress.Address):nullptr;if(Peer)H->Current=Peer;St=Ns==NDIS_STATUS_SUCCESS&&Role&&!H->Task&&H->Running&&Peer?STATUS_SUCCESS:STATUS_INVALID_DEVICE_STATE;
        if(NT_SUCCESS(St)){H->Task=Id;H->TaskHeader=Header;H->TaskClient=H->Current;H->Current->NotifyDisconnect=FALSE;H->Step=H->Current->ClientStep>=20?H->Current->ClientStep+10:30;H->Current->ClientStep=0;H->At=WcnNowMs();H->Current->Authorized=H->Current->Associated=FALSE;}CleanupParsedWdiTaskDisconnect(&A);break;}
    case WDI_SET_RECEIVE_PACKET_FILTER:{WDI_SET_RECEIVE_PACKET_FILTER_PARAMETERS A;auto Ns=ParseWdiSetReceivePacketFilterToIhv(N,P,&S->Tlv,&A);St=Ns==NDIS_STATUS_SUCCESS&&!(A.PacketFilterType&(WDI_PACKET_FILTER_PROMISCUOUS|WDI_PACKET_FILTER_802_11_RAW_DATA))?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;if(NT_SUCCESS(St)&&Role)H->Go->PacketFilter=A.PacketFilterType&15u;CleanupParsedWdiSetReceivePacketFilter(&A);break;}
    case WDI_SET_MULTICAST_LIST:{WDI_SET_MULTICAST_LIST_PARAMETERS A;auto Ns=ParseWdiSetMulticastListToIhv(N,P,&S->Tlv,&A);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;CleanupParsedWdiSetMulticastList(&A);break;}
    case WDI_SET_PRIVACY_EXEMPTION_LIST:{WDI_SET_PRIVACY_EXEMPTION_LIST_PARAMETERS A;auto Ns=ParseWdiSetPrivacyExemptionListToIhv(N,P,&S->Tlv,&A);St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;CleanupParsedWdiSetPrivacyExemptionList(&A);break;}

    }
    S8Complete(R,St);if(!NT_SUCCESS(St))WcnLog(S->D,"hotspot: command=%u rejected status=0x%x",Id,St);return true;
}
static NTSTATUS HstartRequest(HOTSPOT *H,BOOLEAN Restart)
{
    auto D=H->D;auto B=H->Wire;RtlZeroMemory(B,sizeof(H->Wire));auto C=(wmi_vdev_start_request_cmd*)B;JHDR(C,88);C->vdev_id=1;C->beacon_interval=H->BeaconPeriod;C->dtim_period=H->Dtim;C->ssid.ssid_len=H->SsidLength;RtlCopyMemory(C->ssid.ssid,H->Ssid,H->SsidLength);C->preferred_tx_streams=C->preferred_rx_streams=D->Ce.NumRfChains;C->mbssid_flags=1;
        auto Ch=(ath12k_wmi_channel_params*)(C+1);JHDR(Ch,80);Ch->mhz=H->Frequency;Ch->band_center_freq1=H->Radio.CenterFrequency;Ch->info=H->Radio.PhyMode|(H->Radio.Ht?1u<<11:0)|(H->Radio.Vht?1u<<12:0)|(H->Radio.He?1u<<17:0)|((H->Radio.Width==40&&H->Radio.CenterFrequency>H->Frequency)?1u<<6:0);C->he_ops=H->Radio.HeOps&0xffffff;if(H->Radio.Eht)C->punct_bitmap=0xffffffffu;ULONG Power=H->Radio.Power*2,Gain=H->Radio.Gain*2;if(Power>127)Power=127;if(Gain>127)Gain=127;Ch->reg_info_1=(Power<<8)|(Power<<16);Ch->reg_info_2=Gain|(Power<<8);auto E=(ULONG*)(Ch+1);*E=Jtlv(18,0);return WcnWmiSend(D,Restart?0x5004u:0x5003u,B,(ULONG)((UCHAR*)(E+1)-(UCHAR*)B));
}
static NTSTATUS HstartStep(HOTSPOT *H)
{
    auto D=H->D;ULONG *B=H->Wire;
    switch(H->Step){
    case 0:{NTSTATUS Stop=WcnTdlsStop(D,&D->Join);if(!NT_SUCCESS(Stop))return Stop;Stop=WcnTdlsStop(D,&D->Secondary);if(!NT_SUCCESS(Stop))return Stop;Stop=WcnTwtStop(D,&D->Join);if(!NT_SUCCESS(Stop))return Stop;Stop=WcnTwtStop(D,&D->Secondary);if(!NT_SUCCESS(Stop))return Stop;RtlZeroMemory(B,sizeof(H->Wire));B[0]=Jtlv(86,44);B[1]=1;B[2]=1;B[3]=3;RtlCopyMemory(B+4,H->Mac,6);B[6]=2;B[7]=1;B[12]=Jtlv(18,32);for(ULONG I=0;I<2;I++){B[13+I*4]=Jtlv(411,12);B[14+I*4]=I;B[15+I*4]=B[16+I*4]=D->Ce.NumRfChains;}NTSTATUS St=WcnWmiSend(D,0x5001,B,84);if(NT_SUCCESS(St))H->Created=TRUE;return St;}
    case 1:if(WcnNowMs()-H->At<200)return STATUS_PENDING;return STATUS_SUCCESS;
    case 2:if(H->AllowSae&&!H->PmfConfigured){ULONG Param[]={Jtlv(82,12),1,34,1};NTSTATUS St=WcnWmiSend(D,0x4003,Param,sizeof(Param));if(NT_SUCCESS(St))H->PmfConfigured=TRUE;return NT_SUCCESS(St)?STATUS_PENDING:St;}return Hparam(H,48,2);
    case 3:return Hparam(H,91,2);
    case 4:return Hparam(H,34,D->Ce.NumRfChains);
    case 5:return H->SelfMapped?STATUS_SUCCESS:Hpeer(H,H->Mac,FALSE);
    case 6:if(!H->SelfMapped)return STATUS_PENDING;return STATUS_SUCCESS;
    case 7:return HstartRequest(H,FALSE);
    case 8:if(!H->Started)return STATUS_PENDING;if(!H->AggregationSet){NTSTATUS St=WcnAggregationConfigure(D,1);if(NT_SUCCESS(St))H->AggregationSet=TRUE;return St;}return STATUS_SUCCESS;
    case 9:return Hbeacon(H);
    case 10:if(!H->Go->Connect){NTSTATUS St=WcnPhyWmm(D,&H->Radio,1);if(!NT_SUCCESS(St))return St;H->Go->Connect=TRUE;return STATUS_PENDING;}
        if(!H->BeamformSet){NTSTATUS St=WcnPhyBeamform(D,&H->Radio,1);if(!NT_SUCCESS(St))return St;H->BeamformSet=TRUE;return STATUS_PENDING;}
        {wmi_vdev_up_cmd C={};JHDR(&C,92);C.vdev_id=1;RtlCopyMemory(C.vdev_bssid.addr,H->Mac,6);return WcnWmiSend(D,S7_WMI_VDEV_UP_CMDID,&C,sizeof(C));}
    case 11:
        WcnWrite(D,0xa44094,(1u<<1)|(1u<<10)|(1u<<14)|(H->MacId<<15));WcnWrite(D,0xa44098,(2u<<1)|(1u<<10)|(1u<<14)|(H->MacId<<15));
        H->Running=TRUE;H->Go->AssocIndicated=TRUE;S8SetLink(H->Go,true);P2pChannel(H->Go,H->Frequency);Hfinish(H,STATUS_SUCCESS);return STATUS_PENDING;
    default:return STATUS_INVALID_DEVICE_STATE;
    }
}
static void Hdisconnected(HOTSPOT *H)
{
    if(!H->Current->NotifyDisconnect)return;H->Current->NotifyDisconnect=FALSE;auto S=(S8*)H->D->Stage8;WDI_INDICATION_DISASSOCIATION_PARAMETERS P;
    RtlCopyMemory(P.DisconnectIndicationParameters.MacAddress.Address,H->Current->Client,6);P.DisconnectIndicationParameters.DisassociationWABIReason=WDI_ASSOC_STATUS_DISASSOCIATE_UNABLE_TO_MAINTAIN;
    P.Optional.NeedPeerStateCleanup_IsPresent=TRUE;P.NeedPeerStateCleanup=FALSE;UCHAR *Out=nullptr;ULONG N=0;
    if(GenerateWdiIndicationDisassociationFromIhv(&P,0,&S->Tlv,&N,&Out)==NDIS_STATUS_SUCCESS){WDI_MESSAGE_HEADER Header={};Header.PortId=(WDI_PORT_ID)H->Go->Port;S8Indicate(S,&Header,WDI_INDICATION_DISASSOCIATION,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
}
static void HclientDrop(HOTSPOT *H)
{H->Current->Associated=H->Current->Authorized=FALSE;HassocComplete(H,STATUS_UNSUCCESSFUL);if(!H->Current->DeleteSent)H->Current->DeleteConfirmed=H->Current->Unmapped=FALSE;H->Current->ClientStep=H->Current->ClientPhy.Pmf&&H->Current->PairKey.Valid?19:20;H->Current->ClientAt=WcnNowMs();}
static NTSTATUS HclientStep(HOTSPOT *H)
{
    auto D=H->D;switch(H->Current->ClientStep){
    case 1:{NTSTATUS St=Hpeer(H,H->Current->Client,FALSE);if(NT_SUCCESS(St))H->Current->ClientCreated=TRUE;return St;}
    case 2:if(!H->Current->ClientMapped)return STATUS_PENDING;return STATUS_SUCCESS;
    case 3:return WcnPeerParam(D,1,H->Current->Client,19,1|((H->MacId+1)<<1));
    case 4:{if(H->Current->QueueTid==17)return STATUS_SUCCESS;wmi_peer_reorder_queue_setup_cmd C={};JHDR(&C,S7_WMI_TAG_REORDER_QUEUE_SETUP_CMD);C.vdev_id=1;RtlCopyMemory(C.peer_macaddr.addr,H->Current->Client,6);C.tid=C.queue_no=H->Current->QueueTid;ULONG64 Pa=H->Current->Reo.La+H->Current->QueueTid*512;C.queue_ptr_lo=(ULONG)Pa;C.queue_ptr_hi=(ULONG)(Pa>>32);C.ba_window_size_valid=C.ba_window_size=1;NTSTATUS St=WcnWmiSend(D,S7_WMI_PEER_REORDER_QUEUE_SETUP_CMDID,&C,sizeof(C));if(NT_SUCCESS(St))H->Current->QueueTid++;return NT_SUCCESS(St)?STATUS_PENDING:St;}
    case 5:{if(H->Current->Auth==WCN_AUTH_SAE){H->Current->ClientStep=0;return STATUS_PENDING;}UCHAR F[30];Hheader(H,F,0xb0,H->Current->Client);Jputle16(F+24,0);Jputle16(F+26,2);Jputle16(F+28,0);return Hmgmt(H,F,30,H->Current);}
    case 6:if(H->Current->MgmtBusy)return STATUS_PENDING;if(H->Current->MgmtStatus){if(++H->Current->ClientRetry<3)H->Current->ClientStep=5;else HclientDrop(H);return STATUS_PENDING;}H->Current->ClientRetry=0;H->Current->ClientStep=0;return STATUS_PENDING;
    case 10:return HclientAssoc(H);
    case 11:if(!H->Current->AssocConfirmed)return STATUS_PENDING;return STATUS_SUCCESS;
    case 12:return Hmgmt(H,H->Current->Assoc,H->Current->AssocLength,H->Current);
    case 13:if(H->Current->MgmtBusy)return STATUS_PENDING;if(H->Current->MgmtStatus){if(++H->Current->ClientRetry<3)H->Current->ClientStep=12;else HclientDrop(H);return STATUS_PENDING;}H->Current->Associated=Jle16(H->Current->Assoc+26)==0;HassocComplete(H,STATUS_SUCCESS);H->Current->ClientStep=0;return STATUS_PENDING;
    case 19:{if(H->Current->MgmtBusy)return STATUS_PENDING;UCHAR F[48];Hheader(H,F,0xc0,H->Current->Client);Jputle16(F+24,3);return Hmgmt(H,F,26,H->Current);}
    case 20:if(H->Current->MgmtBusy)return STATUS_PENDING;if(!H->Current->ClientCreated){H->Current->DeleteConfirmed=H->Current->Unmapped=TRUE;return STATUS_SUCCESS;}if(H->KeyRequest)for(ULONG I=0;I<H->KeyCount;I++)if(H->KeyClient[I]==H->Current)return STATUS_PENDING;if(H->D->Runtime.ReoPending&&H->D->Runtime.ReoOwner==1&&H->D->Runtime.ReoBase==H->Current->Reo.La){if(!H->D->Runtime.ReoDone)return STATUS_PENDING;H->D->Runtime.ReoPending=H->D->Runtime.ReoDone=FALSE;}RtlZeroMemory(H->Current->Ba,sizeof(H->Current->Ba));return Hpeer(H,H->Current->Client,TRUE);
    case 21:if(!H->Current->DeleteConfirmed||!H->Current->Unmapped)return STATUS_PENDING;HremoveOsPeer(H);H->Current->ClientCreated=H->Current->ClientMapped=H->Current->PairKey.Valid=FALSE;RtlSecureZeroMemory(H->Current->PairKey.Bytes,16);H->Current->ClientStep=0;Hdisconnected(H);RtlSecureZeroMemory(&H->Current->Sae,sizeof(H->Current->Sae));H->Current->Used=FALSE;return STATUS_PENDING;
    default:return STATUS_PENDING;
    }
}
static void HkeysService(HOTSPOT *H)
{
    if(!H->KeyRequest)return;NTSTATUS St=STATUS_SUCCESS;
    if(WcnNowMs()-H->KeyAt>2500)St=STATUS_IO_TIMEOUT;
    else if(H->KeyStep<H->KeyCount&&H->Keys[H->KeyStep].Management){auto K=&H->Keys[H->KeyStep];
        if(H->KeyDelete)RtlSecureZeroMemory(&H->Igtk[K->Index-4],sizeof(WCN_IGTK));
        else{St=WcnPmfInstallKey(H->Igtk,K->Index,K->Bytes,K->Rsc);if(NT_SUCCESS(St))H->DefaultMgmtKey=K->Index;}
        if(NT_SUCCESS(St)){RtlSecureZeroMemory(K->Bytes,16);H->KeyStep++;}
    }
    else if(H->KeyStep<H->KeyCount){auto K=&H->Keys[H->KeyStep];auto Peer=H->KeyClient[H->KeyStep];if(!K->Group&&!Peer){H->KeyStep++;return;}auto Slot=K->Group?&H->GroupKey[K->Index]:&Peer->PairKey;
        if(!K->Sent){if((H->KeyDelete&&!Slot->Valid)||(!H->KeyDelete&&Slot->Valid&&Jeq(Slot->Bytes,K->Bytes,16))){K->Sent=TRUE;H->KeyDone=TRUE;H->KeyStatus=0;}
            else{RtlZeroMemory(H->Wire,sizeof(H->Wire));auto C=(wmi_vdev_install_key_cmd*)H->Wire;JHDR(C,S7_WMI_TAG_VDEV_INSTALL_KEY_CMD);C->vdev_id=1;RtlCopyMemory(C->peer_macaddr.addr,K->Group?H->Mac:Peer->Client,6);C->key_idx=K->Index;C->key_flags=K->Group?1:0;C->key_cipher=H->KeyDelete?0:4;C->key_len=H->KeyDelete?0:16;RtlCopyMemory(&C->key_rsc_counter,K->Rsc,6);ULONG Off=sizeof(*C)/4;H->Wire[Off++]=Jtlv(17,H->KeyDelete?0:16);if(!H->KeyDelete)RtlCopyMemory(H->Wire+Off,K->Bytes,16);H->KeyDone=FALSE;St=WcnWmiSend(H->D,0x5009,H->Wire,(Off+(H->KeyDelete?0:4))*4);RtlSecureZeroMemory(H->Wire,sizeof(H->Wire));if(NT_SUCCESS(St))K->Sent=TRUE;}}
        if(K->Sent&&H->KeyDone){if(H->KeyStatus)St=STATUS_DEVICE_HARDWARE_ERROR;else{if(H->KeyDelete){Slot->Valid=FALSE;RtlSecureZeroMemory(Slot->Bytes,16);}
                else{if(!Slot->Valid||!Jeq(Slot->Bytes,K->Bytes,16)){if(K->Group&&!H->GroupKey[0].Valid&&!H->GroupKey[1].Valid&&!H->GroupKey[2].Valid&&!H->GroupKey[3].Valid){H->DefaultKey=K->Index;H->DefaultPending=TRUE;}RtlCopyMemory(Slot->Bytes,K->Bytes,16);
                        if(Peer){ULONG64 Pn=Jle32(K->Rsc)|((ULONG64)Jle16(K->Rsc+4)<<32);for(ULONG I=0;I<17;I++)Peer->Pn[I]=Pn;RtlZeroMemory(Peer->PnValid,sizeof(Peer->PnValid));}}Slot->Valid=TRUE;}
                RtlSecureZeroMemory(K->Bytes,16);H->KeyStep++;}}}
    if(St!=STATUS_DEVICE_BUSY&&(!NT_SUCCESS(St)||H->KeyStep==H->KeyCount)){auto R=H->KeyRequest;H->KeyRequest=nullptr;S8Complete(R,St);RtlSecureZeroMemory(H->Keys,sizeof(H->Keys));if(!NT_SUCCESS(St))Hfail(H,"key installation failed");}
}
static void HclientService(HOTSPOT *H)
{
    auto D=H->D;auto C=H->Current;
    if(C->ClientStep){NTSTATUS St=HclientStep(H);if(St==STATUS_SUCCESS){C->ClientStep++;C->ClientAt=WcnNowMs();}else if(St!=STATUS_PENDING&&St!=STATUS_DEVICE_BUSY){Hfail(H,"client peer/authentication/association transport");return;}
        if(C->ClientStep&&WcnNowMs()-C->ClientAt>5000){if(C->ClientStep<19)HclientDrop(H);else Hfail(H,"client deletion ownership confirmation timeout");}}
    if(C->Auth==WCN_AUTH_SAE)HsaeService(H);
    if(C->SaQueryPending&&C->Authorized&&!C->ClientStep&&!H->Task){UCHAR B[]={8,1,0,0};RtlCopyMemory(B+2,C->SaQueryId,2);NTSTATUS St=WcnHotspotActionSend(D,B,sizeof(B));if(NT_SUCCESS(St))C->SaQueryPending=FALSE;else if(St!=STATUS_DEVICE_BUSY)HclientDrop(H); }
    if(H->Running&&C->Associated&&!C->Provisioning&&C->PairKey.Valid&&H->GroupKey[H->DefaultKey].Valid&&!H->DefaultPending&&(!C->ClientPhy.Pmf||H->Igtk[0].Valid||H->Igtk[1].Valid)&&!C->Authorized){NTSTATUS St=WcnPeerParam(D,1,C->Client,3,1);if(NT_SUCCESS(St)){C->Authorized=TRUE;}else if(St!=STATUS_DEVICE_BUSY)Hfail(H,"controlled port authorize");}
    if(H->Running&&C->Authorized&&!C->ClientStep&&!H->Task)WcnAggregationServicePeer(D,1,&C->ClientPhy,C->Ba,C->Client,C->Reo.La,C->MgmtBusy,C->MgmtStatus);
}
static void HadvertService(HOTSPOT *H)
{
    if(!H->AdvertRequest)return;
    NTSTATUS St=H->Running?Hbeacon(H):STATUS_CANCELLED;
    if(St==STATUS_DEVICE_BUSY){if(WcnNowMs()-H->AdvertAt<1000)return;St=STATUS_IO_TIMEOUT;}
    HfinishAdvert(H,St);
}
static bool HresizeService(HOTSPOT *H)
{
    if(!H->ResizeRequested)return false;auto D=H->D;NTSTATUS St=STATUS_SUCCESS;
    if(!H->ResizeStep){
        if(H->Task||!H->Running){H->ResizeRequested=FALSE;return false;}
        if(H->MgmtBusy||H->KeyRequest||H->AdvertRequest||HbaBusy(H))return false;
        H->Radio.Width=20;H->Radio.CenterFrequency=H->Frequency;H->Radio.Sgi=!!(D->Ce.HtCap&2);H->Radio.PhyMode=WcnPhyMode(&H->Radio);H->Radio.RateKbps=WcnPhyRate(&H->Radio);
        H->Go->Resetting=TRUE;H->Started=FALSE;H->ResizeStep=1;H->ResizePeer=0;H->ResizeAt=WcnNowMs();
    }
    if(WcnNowMs()-H->ResizeAt>5000){Hfail(H,"20/40 coexistence width-change confirmation timeout");return true;}
    switch(H->ResizeStep){
    case 1:St=HstartRequest(H,TRUE);break;
    case 2:if(!H->Started)return true;if(H->Task){H->ResizeRequested=FALSE;H->ResizeStep=0;H->Go->Resetting=FALSE;return true;}break;
    case 3:
        if(H->ResizePeer<WCN_HOTSPOT_CLIENTS*2){auto C=&H->Clients[H->ResizePeer/2];if(C->Associated&&C->ClientPhy.Width>20){
                auto Next=C->ClientPhy;Next.Width=20;Next.CenterFrequency=H->Frequency;Next.Sgi=!!((D->Ce.HtCap&2)&&(Jle16(Next.HtCap)&32));Next.PhyMode=WcnPhyMode(&Next);Next.RateKbps=WcnPhyRate(&Next);
                // Downgrade CHWIDTH before PHYMODE, as required by firmware sanity checks.
                St=WcnPeerParam(D,1,C->Client,(H->ResizePeer&1)?13:4,(H->ResizePeer&1)?Next.PhyMode:0);
                if(NT_SUCCESS(St)){if(H->ResizePeer&1)C->ClientPhy=Next;H->ResizePeer++;}}
            else H->ResizePeer=(H->ResizePeer/2+1)*2;
            if(St==STATUS_DEVICE_BUSY)return true;if(!NT_SUCCESS(St))Hfail(H,"peer coexistence width/mode update");return true;}break;
    case 4:St=Hbeacon(H);break;
    case 5:{ULONG B[]={Jtlv(92,16),1,0,0,0};RtlCopyMemory(B+3,H->Mac,6);St=WcnWmiSend(D,0x5005,B,sizeof(B));break;}
    case 6:H->ResizeRequested=FALSE;H->ResizeStep=0;H->Go->Resetting=FALSE;S8SetLink(H->Go,true);return true;
    default:Hfail(H,"invalid coexistence state");return true;
    }
    if(NT_SUCCESS(St))H->ResizeStep++;else if(St!=STATUS_DEVICE_BUSY)Hfail(H,"coexistence restart transport");return true;
}
static void Hrequest20(HOTSPOT *H)
{if(H->Running&&WcnIs2g(H->Frequency)&&H->Radio.Width==40){H->Coexist20=H->ResizeRequested=TRUE;}}
void HotspotService(PWCN_DEVICE D)
{
    auto H=Hs(D);if(!H||H->Failed)return;if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Dp.Failed||D->Ce.SourceFailed){if(H->Task||H->Running)Hfail(H,"shared physical device failed");return;}
    if(H->MgmtBusy&&WcnNowMs()-H->MgmtAt>1500){Hfail(H,"management DMA completion timeout");return;}
    HkeysService(H);if(H->Failed||HresizeService(H))return;HadvertService(H);
    if(H->Task==WDI_TASK_START_AP){auto Secondary=(S8*)D->SecondaryStage8;
        if(!WcnStage8SecondaryIdle(D)&&((Secondary&&Secondary->Resetting)||!WcnHotspotTriple(D,&D->Join.Ap,&D->Secondary.Ap))){if(WcnNowMs()-H->At>15000)Hfail(H,"secondary STA resource handoff timeout");return;}if(!D->Scan.Passed||!D->Scan.CountryReady){if(WcnNowMs()-H->At>5000)Hfail(H,"regulatory discovery not ready");return;}
        if(!H->Mgmt.Buffer){NTSTATUS A=WcnDmaAllocAligned(D,1536,0xfff,&H->Mgmt);if(!NT_SUCCESS(A)){Hfail(H,"management DMA allocation");return;}}
        NTSTATUS St=WcnStage7DpInit(D,&D->Join);if(!NT_SUCCESS(St)){Hfail(H,"shared data path preparation");return;}
        St=HstartStep(H);if(St==STATUS_SUCCESS){H->Step++;H->At=WcnNowMs();}else if(St!=STATUS_PENDING&&St!=STATUS_DEVICE_BUSY){Hfail(H,"AP start command transport");return;}if(H->Task&&WcnNowMs()-H->At>5000)Hfail(H,"AP start hardware confirmation timeout");}
    else if(H->Task==WDI_TASK_DISCONNECT){H->Current=H->TaskClient;auto C=H->Current;if(C&&C->Used){if(C->ClientStep<19)HclientDrop(H);HclientService(H);}if(!C||!C->Used)Hfinish(H,STATUS_SUCCESS);}
    else if(H->Task){NTSTATUS St=STATUS_SUCCESS;switch(H->Step){
        case 19:{if(H->MgmtBusy)return;bool Pmf=false;for(auto &C:H->Clients)if(C.Authorized&&C.ClientPhy.Pmf)Pmf=true;
            if(Pmf&&H->Started){UCHAR Index=H->DefaultMgmtKey;if(Index<4||Index>5||!H->Igtk[Index-4].Valid)Index=H->Igtk[0].Valid?4:5;
                if(H->Igtk[Index-4].Valid){UCHAR F[64],Broadcast[6]={255,255,255,255,255,255};Hheader(H,F,0xc0,Broadcast);Jputle16(F+24,3);ULONG N=26;
                    St=WcnPmfTxGroup(D->Join.PmfAlgorithm,H->Igtk,Index,F,&N,sizeof(F));if(NT_SUCCESS(St))St=Hmgmt(H,F,N);}}break;}
        case 20:if(H->MgmtBusy)return;HfinishAdvert(H,STATUS_CANCELLED);H->DefaultPending=FALSE;if(H->DefaultRequest){auto R=H->DefaultRequest;H->DefaultRequest=nullptr;S8Complete(R,STATUS_CANCELLED);}if(H->KeyRequest){auto R=H->KeyRequest;H->KeyRequest=nullptr;S8Complete(R,STATUS_CANCELLED);}RtlSecureZeroMemory(H->Keys,sizeof(H->Keys));if(H->Started){ULONG B[]={Jtlv(94,4),1};St=WcnWmiSend(D,0x5007,B,sizeof(B));}break;
        case 21:if(H->StopClient<WCN_HOTSPOT_CLIENTS){H->Current=&H->Clients[H->StopClient];if(H->Current->Used){if(H->Current->ClientStep<19)HclientDrop(H);HclientService(H);}else H->StopClient++;H->At=WcnNowMs();return;}break;
        case 22:if(H->Started){ULONG B[]={Jtlv(93,4),1};St=WcnWmiSend(D,0x5006,B,sizeof(B));}else H->Stopped=TRUE;break;
        case 23:if(!H->Stopped)St=STATUS_PENDING;else{for(ULONG I=0;I<WCN_TX_SLOTS;I++)if(D->Runtime.TxUsed[I]&0x80)return;S8ClearStaging(H->Go);P2pRoleStopped(D);H->Go->Connect=FALSE;H->Started=FALSE;RtlSecureZeroMemory(H->Igtk,sizeof(H->Igtk));RtlSecureZeroMemory(H->GroupKey,sizeof(H->GroupKey));Hfinish(H,STATUS_SUCCESS);return;}break;
        default:St=STATUS_INVALID_DEVICE_STATE;break;}
        if(St==STATUS_SUCCESS){H->Step++;H->At=WcnNowMs();}else if(St!=STATUS_PENDING&&St!=STATUS_DEVICE_BUSY){Hfail(H,"AP stop transport");return;}if(WcnNowMs()-H->At>5000)Hfail(H,"AP stop ownership confirmation timeout");}
    if(H->DefaultPending){NTSTATUS St=Hparam(H,33,H->DefaultKey);if(St==STATUS_DEVICE_BUSY&&H->DefaultRequest&&WcnNowMs()-H->DefaultAt>2500)St=STATUS_IO_TIMEOUT;if(NT_SUCCESS(St)){H->DefaultPending=FALSE;if(H->DefaultRequest){auto R=H->DefaultRequest;H->DefaultRequest=nullptr;S8Complete(R,STATUS_SUCCESS);}}else if(St!=STATUS_DEVICE_BUSY){Hfail(H,"default GTK selection");return;}}
    for(auto &C:H->Clients)if(C.Used){H->Current=&C;if(!H->Task)HclientService(H);}
    if(H->Go&&H->Running){S8QueuesService(H->Go);}
}
extern "C" VOID WcnHotspotHtt(PWCN_DEVICE D,const UCHAR *P,ULONG N)
{
    auto H=Hs(D);if(!H||D->P2pClient||N<4)return;ULONG Info=Jle32(P),T=Info&255;
    if((T==3||T==0x1e||T==0x2b)&&N>=16&&((Info>>8)&255)==1){ULONG Ast=T==0x2b?Jle32(P+12)&65535:Jle32(P+8)>>16,Hash=T==0x2b?Jle32(P+12)>>16:Jle32(P+12)&65535;
        if(Jeq(P+4,H->Mac,6)){H->SelfAst=Ast;H->SelfHash=Hash;H->SelfPeerId=Info>>16;H->SelfMapped=TRUE;}
        else if(auto C=Hfind(H,P+4)){C->PeerId=Info>>16;C->Ast=Ast;C->Hash=Hash;C->ClientMapped=TRUE;}

    }else if((T==4||T==0x1f)&&H->ReleaseStep&&H->SelfMapped&&(Info>>16)==H->SelfPeerId)H->SelfUnmapped=TRUE;
    else if(T==4||T==0x1f)for(auto &C:H->Clients)if(C.Used&&C.ClientMapped&&(Info>>16)==C.PeerId){C.Unmapped=TRUE;C.Authorized=FALSE;}
}
static bool HinitClient(HOTSPOT *H,const UCHAR Mac[6],ULONG Auth)
{
    auto C=H->Current;
    if(C->Reo.Buffer){if(C->RetiredCount==8){Hfail(H,"retired client REO ownership exhausted");return false;}C->Retired[C->RetiredCount++]=C->Reo;RtlZeroMemory(&C->Reo,sizeof(C->Reo));}
    NTSTATUS St=WcnDmaAllocAligned(H->D,17*512,0xfff,&C->Reo);if(!NT_SUCCESS(St)){Hfail(H,"client REO allocation");return false;}
    for(ULONG I=0;I<17;I++){auto Q=(ULONG*)(C->Reo.Va+I*512);Q[0]=(0xddbeefu<<8)|(8u<<4)|4;Q[1]=I;ULONG Ac=(I==0||I==3)?0:(I==1||I==2)?1:(I==4||I==5)?2:3;
        Q[2]=3|(Ac<<5)|(1u<<21)|(1u<<25)|(1u<<27)|(I==16?1u<<8:1u<<11);if(I<16)for(ULONG K=0;K<3;K++)Q[(128+K*128)/4]=((0xadbeefu+K*0x100000u)<<8)|(9u<<4)|4;}
    C->Used=TRUE;C->Auth=Auth;RtlCopyMemory(C->Client,Mac,6);RtlZeroMemory(C->Ba,sizeof(C->Ba));RtlSecureZeroMemory(&C->Sae,sizeof(C->Sae));RtlSecureZeroMemory(&C->PairKey,sizeof(C->PairKey));
    C->MgmtRxPn=C->MgmtTxPn=0;C->SaQueryPending=FALSE;C->QueueTid=C->ClientRetry=0;C->ClientStep=1;C->ClientAt=C->Sae.At=WcnNowMs();
    C->Provisioning=C->P2pPeer=FALSE;C->DeleteSent=C->DeleteConfirmed=C->Unmapped=C->Associated=C->Authorized=C->AssocConfirmed=FALSE;return true;
}
static void HmgmtRx(HOTSPOT *H,const UCHAR *P,ULONG N)
{
    ULONG L;const UCHAR *B=Jfind(P,N,44,&L);if(!B||L<68||(Jle32(B+20)&0x39))return;ULONG Len=Jle32(B+16);const UCHAR *F=Jfind(P,N,17,&L);if(!F||Len>L||Len<24||(Jle16(F)&0x030f))return;ULONG Type=F[0]&0xfc;
    if(Type==0x40){if(!H->Running||H->ResizeRequested||H->Task||H->Current->ClientStep||H->Current->AssocPending||HbaBusy(H)||H->MgmtBusy||(Jle32(B+64)&&Jle32(B+64)!=H->Frequency))return;
        bool Ssid=false;for(ULONG I=24;I<Len;){if(Len-I<2||F[I+1]>Len-I-2)return;if(F[I]==0){if(Ssid)return;Ssid=TRUE;if(F[I+1]&&(F[I+1]!=H->SsidLength||!Jeq(F+I+2,H->Ssid,H->SsidLength)))return;}I+=F[I+1]+2;}if(!Ssid)return;UCHAR R[1536];Hheader(H,R,0x50,F+10);RtlCopyMemory(R+24,H->Beacon+24,12);ULONG Size=36+Hies(H,R+36,FALSE);if(Size+H->ProbeAdvertLength+3>sizeof(R))return;Size+=HextraIes(H,R+Size,H->ProbeAdvert,H->ProbeAdvertLength);Hmgmt(H,R,Size);return;}
    if(!Jeq(F+4,H->Mac,6)||!Jeq(F+16,H->Mac,6)||(F[10]&1))return;
    bool AuthStart=Type==0xb0&&Len>=30&&Jle16(F+26)==1&&(Jle16(F+24)==0||Jle16(F+24)==3)&&H->Running&&!H->Task;
    ULONG Auth=AuthStart&&Jle16(F+24)==3?WCN_AUTH_SAE:WCN_AUTH_PSK;
    if(AuthStart&&((Auth==WCN_AUTH_SAE&&!H->AllowSae)||(Auth==WCN_AUTH_PSK&&!H->AllowPsk)))return;
    auto Peer=Hfind(H,F+10);if(!Peer&&AuthStart)for(auto &C:H->Clients)if(!C.Used){Peer=&C;break;}
    if(!Peer)return;H->Current=Peer;
    if(AuthStart&&!Peer->Used){if(!HinitClient(H,F+10,Auth))return;}
    if(Type==0xb0&&Len>=30&&Jle16(F+24)==3&&Peer->Auth==WCN_AUTH_SAE&&H->AllowSae&&!H->Task&&Peer->ClientStep<19&&!Peer->Associated){
        ULONG Seq=Jle16(F+26),Status=Jle16(F+28);if(Len>512)return;
        if(Seq==1){if(Len<32||(Status!=0&&Status!=126))return;Peer->Sae.PeerConfirm=FALSE;}
        else if(Seq==2){if(Len<64||Status!=0)return;Peer->Sae.PeerConfirm=TRUE;}else return;
        Peer->Sae.At=WcnNowMs();if(!NT_SUCCESS(HsaeIndicate(H,Peer,Seq==1?WDI_SAE_INDICATION_TYPE_COMMIT_FRAME:WDI_SAE_INDICATION_TYPE_CONFIRM_FRAME,F+24,Len-24)))HclientDrop(H);return;
    }
    if(AuthStart&&Auth==WCN_AUTH_PSK){if(Peer->Auth==Auth&&Peer->ClientCreated&&!Peer->ClientStep)Peer->ClientStep=5;return;}
    ULONG Off=24;bool Robust=WcnRobustManagement(F,Len)!=FALSE;
    if(Robust&&Peer->ClientPhy.Pmf){if(!Peer->PairKey.Valid||!WcnPmfReceiveFrame(H->D->Join.PmfAlgorithm,H->Igtk,&Peer->MgmtRxPn,F,Len,&Off))return;}
    else if(F[1]&0x40)return;
    if(Type==0xd0&&Len>=28&&F[24]==4&&F[25]==0){
        for(ULONG I=26;I<Len;){if(Len-I<2||F[I+1]>Len-I-2)return;if(F[I]==72&&F[I+1]==1&&(F[I+2]&6))Hrequest20(H);I+=F[I+1]+2;}return;}
    if(Type==0xd0&&Peer->Authorized){
        if(Len>=Off+4&&F[Off]==8&&F[Off+1]==0){Peer->SaQueryPending=TRUE;RtlCopyMemory(Peer->SaQueryId,F+Off+2,2);}
        else WcnAggregationReceivePeer(H->D,&Peer->ClientPhy,Peer->Ba,F+Off,Len-Off);return;
    }
    if(!H->Task&&H->Current->ClientStep<19&&Type==0&&Len>=28&&H->Current->ClientCreated&&H->Current->ClientMapped&&Jeq(H->Current->Client,F+10,6)&&!H->Current->AssocPending){
        if(H->Current->Auth==WCN_AUTH_SAE&&!H->Current->Sae.Authenticated)return;
        if(H->ResizeRequested)return;
        if(WcnIs2g(H->Frequency)&&H->Radio.Width==40){for(ULONG I=28;I<Len;){if(Len-I<2||F[I+1]>Len-I-2)return;if(F[I]==45&&F[I+1]==26&&(Jle16(F+I+2)&0x4000)){Hrequest20(H);return;}I+=F[I+1]+2;}}
        RtlZeroMemory(&H->Current->ClientPhy,sizeof(H->Current->ClientPhy));H->Current->ClientPhy.Capability=Jle16(F+24);H->Current->ClientListen=Jle16(F+26);if(!H->Current->ClientListen||(!(H->Current->ClientPhy.Capability&16)&&!H->Go->WpsEnabled))return;
        bool Rsn=false,Ssid=false;ULONG RateMask=0;H->Current->ClientPhy.RateCount=0;
        for(ULONG I=28;I<Len;){if(Len-I<2||F[I+1]>Len-I-2)return;ULONG K=F[I+1];
            if(F[I]==0){if(Ssid||K!=H->SsidLength||!Jeq(F+I+2,H->Ssid,K))return;Ssid=true;}
            if(F[I]==1||F[I]==50)for(ULONG J=0;J<K;J++)for(ULONG A=0;A<H->RateCount;A++)if((F[I+2+J]&127)==(H->Rates[A]&127))RateMask|=1u<<A;
            if(F[I]==48){BOOLEAN Pmf=H->AllowSae;if(Rsn||!WcnRsnAccept(F+I+2,K,H->Current->Auth,&Pmf))return;H->Current->ClientPhy.Pmf=Pmf;Rsn=true;}I+=K+2;}if(!Ssid)return;
        static const UCHAR P2pOui[]={0x50,0x6f,0x9a,9},WpsOui[]={0,0x50,0xf2,4};H->Current->P2pPeer=P2pVendor(F+28,Len-28,P2pOui);
        H->Current->Provisioning=!Rsn&&H->Go->WpsEnabled&&H->AllowPsk&&H->Current->P2pPeer&&P2pVendor(F+28,Len-28,WpsOui);if(!Rsn&&!H->Current->Provisioning)return;
        for(ULONG I=0;I<H->RateCount;I++){if((H->Rates[I]&128)&&!(RateMask&(1u<<I)))return;if(RateMask&(1u<<I)){UCHAR Rate=H->Rates[I]&127;H->Current->ClientPhy.Rates[H->Current->ClientPhy.RateCount++]=Rate;}}if(!H->Current->ClientPhy.RateCount)return;
        auto Phy=&H->Current->ClientPhy;Phy->Frequency=H->Frequency;Phy->Auth=H->Current->Provisioning?WCN_AUTH_OPEN:H->Current->Auth;
        if(Len>1536)return;auto Synthetic=(UCHAR*)H->Wire;RtlZeroMemory(Synthetic,36);RtlCopyMemory(Synthetic+36,F+28,Len-28);ULONG SizePhy=Len+8;SizePhy+=WcnPhyOperationIes(&H->Radio,Synthetic+SizePhy);
        if(!WcnPhySelect(H->D,Synthetic,SizePhy,Phy))return;

        WDI_INDICATION_AP_ASSOCIATION_REQUEST_PARAMETERS A;RtlCopyMemory(A.IncomingRequestInfo.AssocRequestParams.PeerMacAddress.Address,H->Current->Client,6);A.IncomingRequestInfo.AssocRequestFrame.SimpleAssign((UCHAR*)F+24,Len-24);auto S=(S8*)H->D->Stage8;WDI_MESSAGE_HEADER Header={};Header.PortId=(WDI_PORT_ID)H->Go->Port;UCHAR *Out=nullptr;ULONG Size=0;
        if(GenerateWdiIndicationApAssociationRequestReceivedFromIhv(&A,0,&S->Tlv,&Size,&Out)==NDIS_STATUS_SUCCESS){S8Indicate(S,&Header,WDI_INDICATION_AP_ASSOCIATION_REQUEST_RECEIVED,STATUS_SUCCESS,Out,Size,true);FreeGenerated(Out);}return;}
    if((Type==0xa0||Type==0xc0)&&Len>=26&&H->Current->ClientCreated&&Jeq(H->Current->Client,F+10,6)&&!H->Task&&H->Current->ClientStep<19){H->Current->NotifyDisconnect=H->Current->OsPeer;HclientDrop(H);}
}
extern "C" VOID WcnHotspotWmi(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N)
{
    auto H=Hs(D);if(!H||!Jtlvs(P,N))return;ULONG L;const UCHAR *F;
    if(Event==0x5006){F=Jfind(P,N,450,&L);if(F&&L>=4&&Jle32(F)==1)H->VdevDeleted=TRUE;return;}
    if(Event==0x6007){F=Jfind(P,N,451,&L);if(F&&L>=12&&Jle32(F)==1&&Jeq(F+4,H->Mac,6))H->SelfDeleteConfirmed=TRUE;}
    if(H->Failed||D->P2pClient)return;
    if(Event==0x7001){if(H->Running)HmgmtRx(H,P,N);return;}
    if(Event==0x7006){F=Jfind(P,N,423,&L);if(F&&L>=12&&H->MgmtBusy&&Jle32(F)==H->MgmtId&&D->MgmtInUse[1]==H->MgmtId&&Jle32(F+8)>=1&&Jle32(F+8)<=2){H->MgmtStatus=Jle32(F+4);H->MgmtBusy=FALSE;if(H->MgmtClient){H->MgmtClient->MgmtStatus=H->MgmtStatus;H->MgmtClient->MgmtBusy=FALSE;auto A=&H->MgmtClient->Sae;if(A->Sending&&A->Sequence==2&&!H->MgmtStatus&&A->Validated&&A->PeerConfirm){A->ConfirmSent=A->Authenticated=TRUE;}H->MgmtClient=nullptr;}D->MgmtInUse[1]=0;}return;}
    if(Event==0x5001){F=Jfind(P,N,40,&L);if(!F||L<36||Jle32(F)!=1)return;if(Jle32(F+12)||Jle32(F+24)>1||!Jle32(F+28)||!Jle32(F+32)){Hfail(H,"firmware AP vdev start rejected");return;}H->MacId=Jle32(F+24);ULONG Nss=Jle32(F+28)<Jle32(F+32)?Jle32(F+28):Jle32(F+32);if(H->Radio.Nss>Nss)H->Radio.Nss=(UCHAR)Nss;H->Radio.RateKbps=WcnPhyRate(&H->Radio);H->Started=TRUE;}
    else if(Event==0x5002){F=Jfind(P,N,41,&L);if(F&&L>=4&&Jle32(F)==1)H->Stopped=TRUE;}
    else if(Event==0x6007){F=Jfind(P,N,451,&L);if(F&&L>=12&&Jle32(F)==1)if(auto C=Hfind(H,F+4))C->DeleteConfirmed=TRUE;}
    else if(Event==S7_WMI_PEER_ASSOC_CONF_EVENTID){F=Jfind(P,N,S7_WMI_TAG_PEER_ASSOC_CONF_EVENT,&L);if(F&&L>=12&&Jle32(F)==1)if(auto C=Hfind(H,F+4))C->AssocConfirmed=TRUE;}
    else if(Event==S7_WMI_VDEV_INSTALL_KEY_COMPLETE_EVENTID){F=Jfind(P,N,42,&L);if(F&&L>=24&&Jle32(F)==1&&H->KeyRequest&&H->KeyStep<H->KeyCount){auto K=&H->Keys[H->KeyStep];if(K->Sent&&Jeq(F+4,K->Group?H->Mac:H->KeyClient[H->KeyStep]->Client,6)&&Jle32(F+12)==K->Index&&Jle32(F+16)==(K->Group?1u:0u)){H->KeyStatus=Jle32(F+20);H->KeyDone=TRUE;}}}
}
bool HotspotAuthorized(PWCN_DEVICE D,const UCHAR *Peer)
{auto H=Hs(D);if(!H||!H->Running)return false;if(Peer[0]&1)return H->GroupKey[H->DefaultKey].Valid;auto C=Hfind(H,Peer);return C&&C->Authorized;}
extern "C" BOOLEAN WcnHotspotRx(PWCN_DEVICE D,const UCHAR *B,ULONG N,ULONG Pad,BOOLEAN Error,const ULONG *Desc)
{
    auto H=Hs(D);if(!H||D->P2pClient||!Jeq(B+204,H->Mac,6))return FALSE;auto C=Hfind(H,B+210);if(!H->Running||!C||!C->Associated)return TRUE;
    ULONG Fc=Jle16(B+200),Valid=Jle32(B+176),I2=Jle32(B+148),Meta=Error?Desc[4]:Desc[3],Mpdu=Error?Desc[3]:Desc[2],Msdu=Error?Desc[5]:Desc[4];
    if((Fc&0x030c)!=0x0108||D->Ce.PeerMetadataVersion>1||((Meta>>16)&255)!=1||(Meta&(D->Ce.PeerMetadataVersion==1?0x3fffu:0xffffu))!=C->PeerId)return TRUE;
    const UCHAR *F=B+392+Pad;bool Eapol=Jbe16(F+12)==0x888e;
    if(Fc&0x4000){ULONG Tid=(Fc&0x80)?((I2>>15)&15):16;ULONG64 Pn=Jle32(B+160)|((ULONG64)(Jle32(B+164)&65535)<<32);
        if(!C->PairKey.Valid||(!Eapol&&!C->Authorized)||(Valid&0x4020du)!=0x4020du||!(Mpdu&(1u<<12))||((I2>>2)&15)!=6||(Jle32(B+192)&255)||((Fc&0x80)&&!(Valid&128)))return TRUE;
        if(!WcnRxPnAccept(Pn,&C->Pn[Tid],&C->PnValid[Tid],Msdu,!Error&&(Fc&0x80)))return TRUE;
    }else if(!Eapol||(Msdu&3)!=3)return TRUE;
    if(!Jeq(F+6,C->Client,6)||(!Eapol&&!C->Authorized))return TRUE;S8ReceiveEthernet(H->Go,F,N);return TRUE;
}
extern "C" NTSTATUS WcnHotspotTx(PWCN_DEVICE D,const UCHAR *F,ULONG N,BOOLEAN Eapol)
{
    auto H=Hs(D);if(!H||!H->Running)return STATUS_DEVICE_NOT_READY;bool Group=(F[0]&1)!=0;auto C=Group?nullptr:Hfind(H,F);
    if(!Group&&(!C||!C->Associated||!C->ClientMapped||(!Eapol&&!C->Authorized)))return STATUS_DEVICE_NOT_READY;
    if(!Eapol&&!H->GroupKey[H->DefaultKey].Valid)return STATUS_DEVICE_NOT_READY;
    NTSTATUS St=WcnDataTx(D,F,N,Eapol,1,H->MacId,H->Mac,C?C->Client:F,Group?H->SelfAst:C->Ast,Group?H->SelfHash:C->Hash,TRUE);return St;
}
extern "C" ULONG WcnHotspotRate(PWCN_DEVICE D)
{auto H=Hs(D);return H&&H->Radio.RateKbps?H->Radio.RateKbps:54000;}
extern "C" VOID WcnHotspotTxFailed(PWCN_DEVICE D){auto H=Hs(D);if(H&&H->Go)H->Go->TxFailed++;}
void HotspotStop(PWCN_DEVICE D)
{
    auto H=Hs(D);if(!H)return;H->Running=H->DefaultPending=FALSE;HfinishAdvert(H,STATUS_CANCELLED);Hfinish(H,STATUS_CANCELLED);if(H->DefaultRequest){auto R=H->DefaultRequest;H->DefaultRequest=nullptr;S8Complete(R,STATUS_CANCELLED);}if(H->KeyRequest){auto R=H->KeyRequest;H->KeyRequest=nullptr;S8Complete(R,STATUS_CANCELLED);}RtlSecureZeroMemory(H->Keys,sizeof(H->Keys));RtlSecureZeroMemory(H->GroupKey,sizeof(H->GroupKey));RtlSecureZeroMemory(H->Igtk,sizeof(H->Igtk));for(auto &C:H->Clients){RtlSecureZeroMemory(&C.PairKey,sizeof(C.PairKey));RtlSecureZeroMemory(&C.Sae,sizeof(C.Sae));}
    if(H->Go){H->Go->Stopped=TRUE;S8SetLink(H->Go,false);}
}
extern "C" VOID WcnHotspotDmaCleanup(PWCN_DEVICE D)
{
    auto H=Hs(D);if(!H)return;WcnP2pDmaCleanup(D);if(H->Mgmt.Buffer){WdfObjectDelete(H->Mgmt.Buffer);RtlZeroMemory(&H->Mgmt,sizeof(H->Mgmt));}
    for(auto &C:H->Clients){if(C.Reo.Buffer){WdfObjectDelete(C.Reo.Buffer);RtlZeroMemory(&C.Reo,sizeof(C.Reo));}for(ULONG I=0;I<C.RetiredCount;I++)if(C.Retired[I].Buffer)WdfObjectDelete(C.Retired[I].Buffer);C.RetiredCount=0;}
}
void HotspotDestroy(PWCN_DEVICE D)
{auto H=Hs(D);if(!H)return;if(H->Go){RtlSecureZeroMemory(H->Go,sizeof(S8));ExFreePoolWithTag(H->Go,WCN_TAG);}RtlSecureZeroMemory(H,sizeof(*H));ExFreePoolWithTag(H,WCN_TAG);D->Hotspot=nullptr;}

extern "C" BOOLEAN WcnHotspotActive(PWCN_DEVICE D)
{auto H=Hs(D);return (D->P2pClient&&H&&H->Go&&(H->Go->Connect||H->Go->ScanPending||H->Go->ScanIssued||H->Go->Resetting||D->P2pJoin.Started||D->P2pJoin.PeerCreated||D->P2pJoin.VdevStarted))||(H&&(H->Running||H->Task==WDI_TASK_START_AP||H->Task==WDI_TASK_STOP_AP||(H->Started&&!H->Stopped)));}
extern "C" ULONG WcnHotspotFrequency(PWCN_DEVICE D)
{auto H=Hs(D);return H&&!D->P2pClient&&WcnHotspotActive(D)?H->Frequency:0;}
extern "C" BOOLEAN WcnHotspotTriple(PWCN_DEVICE D,const WCN_JOIN_AP *Primary,const WCN_JOIN_AP *Secondary)
{
    ULONG F=WcnHotspotFrequency(D);if(!WcnTripleChannel(Primary->Frequency,Secondary?Secondary->Frequency:0,F))return FALSE;
    if(!F)return TRUE;auto H=Hs(D);auto A=F==Primary->Frequency?Primary:Secondary;if(!A)return TRUE;
    // Same primary, nested channel geometry; never opposite HT40 extensions.
    ULONG Low=H->Radio.CenterFrequency-H->Radio.Width/2,High=H->Radio.CenterFrequency+H->Radio.Width/2;
    ULONG PeerLow=A->CenterFrequency-A->Width/2,PeerHigh=A->CenterFrequency+A->Width/2;
    return (Low>=PeerLow&&High<=PeerHigh)||(PeerLow>=Low&&PeerHigh<=High);
}

ULONG HotspotDevicePort(PWCN_DEVICE D){auto H=Hs(D);return H?H->DevicePort:MAXULONG;}
const UCHAR *HotspotDeviceMac(PWCN_DEVICE D){auto H=Hs(D);return H?H->DeviceMac:nullptr;}
/* A role vdev cannot change STA/AP type while any old peer or DMA is owned. */
NTSTATUS HotspotReleaseRole(PWCN_DEVICE D)
{
 auto H=Hs(D);auto S=H?H->Go:nullptr;if(!H||!S)return STATUS_DEVICE_NOT_READY;
 if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Ce.SourceFailed||D->Dp.Failed)return STATUS_DEVICE_HARDWARE_ERROR;
 if(H->Task||H->Running||H->MgmtBusy||S->Connect||S->Resetting||H->KeyRequest||H->AdvertRequest)return STATUS_DEVICE_BUSY;
 for(auto &C:H->Clients)if(C.Used)return STATUS_DEVICE_BUSY;
 for(ULONG I=0;I<WCN_TX_SLOTS;I++)if(D->Runtime.TxUsed[I]&&(D->Runtime.TxUsed[I]&0x80))return STATUS_DEVICE_BUSY;
 if(D->P2pClient&&(D->P2pJoin.Started||D->P2pJoin.PeerCreated||D->P2pJoin.VdevStarted||D->P2pJoin.MgmtBusy))return STATUS_DEVICE_BUSY;
 bool Created=D->P2pClient?D->P2pJoin.Created!=FALSE:H->Created!=FALSE;
 if(!Created){H->Failed=S->Fatal=FALSE;H->ReleaseStep=0;return STATUS_SUCCESS;}
 NTSTATUS St;
 switch(H->ReleaseStep){
 case 0:H->SelfDeleteConfirmed=H->SelfUnmapped=H->VdevDeleted=FALSE;
     if(!D->P2pClient&&H->SelfMapped){St=Hpeer(H,H->Mac,TRUE);if(!NT_SUCCESS(St))return St;H->ReleaseStep=1;return STATUS_PENDING;}H->ReleaseStep=2;return STATUS_PENDING;
 case 1:if(!H->SelfDeleteConfirmed||!H->SelfUnmapped)return STATUS_PENDING;H->SelfMapped=FALSE;H->ReleaseStep=2;return STATUS_PENDING;
 case 2:{ULONG B[]={Jtlv(87,4),1};St=WcnWmiSend(D,0x5002,B,sizeof(B));if(!NT_SUCCESS(St))return St;H->ReleaseStep=3;return STATUS_PENDING;}
 case 3:if(!H->VdevDeleted)return STATUS_PENDING;H->Created=D->P2pJoin.Created=FALSE;H->Started=H->SelfMapped=H->AggregationSet=H->BeamformSet=FALSE;H->Stopped=TRUE;H->ReleaseStep=0;H->Failed=S->Fatal=FALSE;RtlSecureZeroMemory(H->GroupKey,sizeof(H->GroupKey));RtlSecureZeroMemory(H->Igtk,sizeof(H->Igtk));H->DefaultKey=0;H->DefaultMgmtKey=4;return STATUS_SUCCESS;
 default:return STATUS_INVALID_DEVICE_STATE;
 }
}

bool HotspotAdvertBusy(PWCN_DEVICE D){auto H=Hs(D);return H&&H->AdvertRequest;}
