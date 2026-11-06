/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Native WiFiCx P2P. Windows owns pairing, credentials, role selection and IP.
 * A fourth, device-only vdev uses bounded scan/ROC windows; no third radio,
 * guessed firmware ABI, active DFS probing or descriptor reuse on timeout. */
#include "stage8-internal.h"
extern "C" {
#include "stage7-wire.h"
#include "stage7-util.h"
#include "network-policy.h"
#include "hotspot.h"
}
#include "p2p-wire.h"
static const UCHAR P2pOui[]={0x50,0x6f,0x9a,9},WpsOui[]={0,0x50,0xf2,4};
struct P2P_PEER {UCHAR Mac[6],Type,Token;ULONG Frequency;LONGLONG At;};
struct P2P {
 PWCN_DEVICE D;BOOLEAN Created,Failed,OwnScan,OnChannel,Discover,Background,Cancel,Passive,Listen,TxBusy,TxPending,TxTask,TxResponse,ModePending,ModeCancel,Find,FindListen,ScanForTx,ModeQuarantined,LoActive,LoStopping,LoDisabled,LoRefresh,ResetPending;
 ULONG ScanId,Frequency,ListenFrequency,ListenPeriod,ListenInterval,TxFrequency,TxLength,TxId,TxDwell,Mode,Repeat,Delay,Sequence,PeerCount,NameLength,ExtraLength,ScanExtraLength,FilterCount;
 ULONG DiscoveryCount,DiscoveryFrequency[64],SecondaryCount,NegotiatedFrequency,RoleFrequency;UCHAR NegotiationPeer[6],NegotiationToken;LONGLONG NegotiationAt;UCHAR SecondaryTypes[32];
 UCHAR Mac[6],DeviceCap,GroupCap,DeviceType[8],Name[32],Extra[512],ScanExtra[512],Filters[4][6],Tx[1024];USHORT Methods;
 UCHAR TargetSsid[32],TargetSsidLength;WCN_DMA Dma;ULONG Wire[512];P2P_PEER Peers[32];
 WDI_MESSAGE_HEADER DiscoverHeader,TxHeader,ModeHeader,ResetHeader;LONGLONG CreatedAt,ScanAt,NextAt,TxAt,TxDeadline,ModeAt,CancelAt,LoStopAt,ResetAt;
};
static P2P *Ps(PWCN_DEVICE D){return (P2P*)D->P2p;}
static S8 *Primary(P2P *P){return (S8*)P->D->Stage8;}
static bool Pmac(const UCHAR *M){UCHAR Any=0;for(ULONG I=0;I<6;I++)Any|=M[I];return Any&&!(M[0]&1);}
static bool Plegal(P2P *P,ULONG F,bool Active=true)
{if(!WcnConnectFrequency(F))return false;for(ULONG I=0;I<P->D->Scan.ChannelCount;I++)if(P->D->Scan.Channel[I].Frequency==F){auto C=&P->D->Scan.Channel[I];if(Primary(P)->DisabledBands&(WcnIs2g(F)?1u:2u))return false;return Active?WcnProbeAllowed(F,C->Flags)!=FALSE:!(C->Flags&(1u|2u|64u|2048u));}return false;}
static ULONG Pchannel(const WDI_P2P_CHANNEL &C)
{ULONG F=WcnChannelFrequency(C.ChannelNumber);if(C.OperatingClass!=P2pOperatingClass(F)&&!(WcnIs2g(F)&&(C.OperatingClass==83||C.OperatingClass==84)))return 0;return F;}
static ULONG Poperating(P2P *P)
{ULONG F=P->D->Join.Ap.Frequency;if(Plegal(P,F))return F;return Plegal(P,P->ListenFrequency)?P->ListenFrequency:0;}
static P2P_PEER *Ppeer(P2P *P,const UCHAR Mac[6],bool Create=false)
{ULONG I;for(I=0;I<P->PeerCount;I++)if(Jeq(Mac,P->Peers[I].Mac,6))return &P->Peers[I];if(!Create)return nullptr;
 if(I==ARRAYSIZE(P->Peers)){I=0;for(ULONG K=1;K<P->PeerCount;K++)if(P->Peers[K].At<P->Peers[I].At)I=K;}else P->PeerCount++;
 auto E=&P->Peers[I];RtlZeroMemory(E,sizeof(*E));RtlCopyMemory(E->Mac,Mac,6);return E;}
static void PfinishDiscover(P2P *P,NTSTATUS St)
{if(P->Discover&&!P->Background)S8Indicate(Primary(P),&P->DiscoverHeader,WDI_INDICATION_P2P_DISCOVERY_COMPLETE,St);P->Discover=P->Background=FALSE;P->ScanExtraLength=0;}
static void PfinishTx(P2P *P,NTSTATUS St)
{
 if(!P->TxTask){P->TxPending=FALSE;return;}UCHAR *Out=nullptr;ULONG N=0;NDIS_STATUS Ns;
 auto Fill=[&](WDI_P2P_SEND_ACTION_FRAME_RESULT_CONTAINER &R){RtlCopyMemory(R.FrameParameters.PeerDeviceAddress.Address,P->Tx+4,6);R.FrameParameters.DialogToken=P->Tx[31];R.FrameIEs.SimpleAssign(P->Tx+32,P->TxLength-32);};
 if(P->TxResponse){WDI_INDICATION_P2P_SEND_RESPONSE_ACTION_FRAME_COMPLETE_PARAMETERS X;Fill(X.SendActionFrameResult);Ns=GenerateWdiIndicationP2pSendResponseActionFrameCompleteFromIhv(&X,0,&Primary(P)->Tlv,&N,&Out);}
 else{WDI_INDICATION_P2P_SEND_REQUEST_ACTION_FRAME_COMPLETE_PARAMETERS X;Fill(X.SendActionFrameResult);Ns=GenerateWdiIndicationP2pSendRequestActionFrameCompleteFromIhv(&X,0,&Primary(P)->Tlv,&N,&Out);}
 if(Ns!=NDIS_STATUS_SUCCESS)St=STATUS_INSUFFICIENT_RESOURCES;
 S8Indicate(Primary(P),&P->TxHeader,P->TxResponse?WDI_INDICATION_P2P_SEND_RESPONSE_ACTION_FRAME_COMPLETE:WDI_INDICATION_P2P_SEND_REQUEST_ACTION_FRAME_COMPLETE,St,Out,N);if(Out)FreeGenerated(Out);P->TxTask=P->TxPending=FALSE;
}
static void Pfail(P2P *P,PCSTR Why)
{if(P->Failed)return;if(P->ModePending){P->ModeQuarantined=TRUE;WcnHotspotFail(P->D,"P2P role ownership unconfirmed");}P->Failed=TRUE;P->Listen=FALSE;WcnLog(P->D,"p2p: %s; DMA/scan ownership retained until completion or physical quiesce",Why);PfinishTx(P,STATUS_DEVICE_HARDWARE_ERROR);PfinishDiscover(P,STATUS_DEVICE_HARDWARE_ERROR);if(P->ModePending){S8Indicate(Primary(P),&P->ModeHeader,WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE,STATUS_DEVICE_HARDWARE_ERROR);P->ModePending=FALSE;}if(P->ResetPending){S8Indicate(Primary(P),&P->ResetHeader,WDI_INDICATION_DOT11_RESET_COMPLETE,STATUS_DEVICE_HARDWARE_ERROR);P->ResetPending=FALSE;}}
extern "C" NTSTATUS WcnP2pCreate(PWCN_DEVICE D)
{auto P=(P2P*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(P2P),WCN_TAG);if(!P)return STATUS_INSUFFICIENT_RESOURCES;P->D=D;P->DeviceCap=4;P->ListenPeriod=150;P->ListenInterval=500;D->P2p=P;return STATUS_SUCCESS;}
extern "C" VOID WcnP2pDmaCleanup(PWCN_DEVICE D)
{auto P=Ps(D);if(P&&P->Dma.Buffer){WdfObjectDelete(P->Dma.Buffer);RtlZeroMemory(&P->Dma,sizeof(P->Dma));P->TxBusy=FALSE;D->MgmtInUse[3]=0;}}
extern "C" VOID WcnP2pDestroy(PWCN_DEVICE D)
{auto P=Ps(D);if(P){RtlSecureZeroMemory(P,sizeof(*P));ExFreePoolWithTag(P,WCN_TAG);D->P2p=nullptr;}}
extern "C" VOID WcnP2pStop(PWCN_DEVICE D)
{auto P=Ps(D);if(!P)return;P->Listen=FALSE;P->Cancel=TRUE;PfinishDiscover(P,STATUS_CANCELLED);PfinishTx(P,STATUS_CANCELLED);if(P->ModePending){S8Indicate(Primary(P),&P->ModeHeader,WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE,STATUS_CANCELLED);P->ModePending=FALSE;}if(P->ResetPending){S8Indicate(Primary(P),&P->ResetHeader,WDI_INDICATION_DOT11_RESET_COMPLETE,STATUS_CANCELLED);P->ResetPending=FALSE;}}
extern "C" BOOLEAN WcnP2pScanOwned(PWCN_DEVICE D)
{auto P=Ps(D);return P&&P->OwnScan;}
static ULONG Pdevice(P2P *P,UCHAR B[128])
{if(P->NameLength>32||P->SecondaryCount>4)return 0;RtlCopyMemory(B,P->Mac,6);P2pBe16(B+6,P->Methods);RtlCopyMemory(B+8,P->DeviceType,8);B[16]=(UCHAR)P->SecondaryCount;RtlCopyMemory(B+17,P->SecondaryTypes,P->SecondaryCount*8);ULONG At=17+P->SecondaryCount*8;P2pBe16(B+At,0x1011);P2pBe16(B+At+2,P->NameLength);RtlCopyMemory(B+At+4,P->Name,P->NameLength);return At+4+P->NameLength;}
static ULONG Pinfo(P2P *P,UCHAR *Out,ULONG Capacity,bool Wps=true,bool Response=false)
{
 if(!P->NameLength)return 0;P2pWriter W(Out,Capacity);UCHAR B[128]={P->DeviceCap,P->GroupCap};W.Attr(2,B,2);
 if(P->ListenFrequency){P2pChannelBytes(P->ListenFrequency,B);W.Attr(6,B,5);}
 ULONG DeviceLength=Pdevice(P,B);if(!DeviceLength)W.Good=false;W.Attr(13,B,DeviceLength);
 if(Wps){W.Begin(WpsOui);B[0]=0x20;W.Wps(0x104a,B,1);B[0]=Response?0:1;W.Wps(Response?0x103b:0x103a,B,1);P2pBe16(B,P->Methods);W.Wps(0x1008,B,2);W.Wps(0x1054,P->DeviceType,8);W.Wps(0x1011,P->Name,P->NameLength);}
 return W.Finish();
}
static ULONG Pchannels(P2P *P,UCHAR *Out)
{ULONG N=3;Out[0]='C';Out[1]='N';Out[2]=4;UCHAR Classes[]={81,115,124,125};for(auto C:Classes){ULONG At=N;Out[N++]=C;Out[N++]=0;for(ULONG I=0;I<P->D->Scan.ChannelCount;I++){ULONG F=P->D->Scan.Channel[I].Frequency;if(P2pOperatingClass(F)==C&&Plegal(P,F))Out[N++]=(UCHAR)WcnFrequencyChannel(F);}Out[At+1]=(UCHAR)(N-At-2);if(!Out[At+1])N=At;}return N;}
static bool Pvendor(const UCHAR *B,ULONG N)
{if(N>512||!P2pIesValid(B,N)||P2pVendor(B,N,P2pOui))return false;for(ULONG I=0;I<N;I+=B[I+1]+2)if(B[I]!=221&&B[I]!=127)return false;return true;}
static void Pgroup(P2pWriter &W,const WDI_P2P_GROUP_ID &G)
{UCHAR B[38];if(!Pmac(G.DeviceAddress.Address)||!G.GroupSSID.ElementCount||G.GroupSSID.ElementCount>32){W.Good=false;return;}RtlCopyMemory(B,G.DeviceAddress.Address,6);RtlCopyMemory(B+6,G.GroupSSID.pElements,G.GroupSSID.ElementCount);W.Attr(15,B,6+G.GroupSSID.ElementCount);}
static bool PremoteChannel(const UCHAR *Ies,ULONG N,ULONG Frequency)
{
 UCHAR B[512];ULONG L;if(!P2pAttr(Ies,N,11,B,sizeof(B),&L)||L<3)return false;
 for(ULONG I=3;I<L;){if(L-I<2||B[I+1]>L-I-2)return false;ULONG Count=B[I+1];if(B[I]==P2pOperatingClass(Frequency)||(B[I]==125&&Frequency>=5745&&Frequency<=5825))for(ULONG K=0;K<Count;K++)if(B[I+2+K]==WcnFrequencyChannel(Frequency))return true;I+=Count+2;}return false;
}
static ULONG Pchoose(P2P *P,const UCHAR *Remote,ULONG Length)
{ULONG F=Poperating(P);if(PremoteChannel(Remote,Length,F))return F;for(ULONG I=0;I<P->D->Scan.ChannelCount;I++){F=P->D->Scan.Channel[I].Frequency;if(Plegal(P,F)&&PremoteChannel(Remote,Length,F))return F;}return 0;}
static void Pcommon(P2P *P,P2pWriter &W,UCHAR Group,ULONG Operating,const UCHAR *Remote=nullptr,ULONG RemoteLength=0,bool Channels=true,bool OperatingInfo=true)
{UCHAR B[128];B[0]=P->DeviceCap;B[1]=Group;W.Attr(2,B,2);if(Channels){P2pChannelBytes(P->ListenFrequency,B);W.Attr(6,B,5);ULONG N=Pchannels(P,B);if(Remote){ULONG At=3;for(ULONG I=3;I<N;){ULONG C=B[I],Count=B[I+1],Start=At;B[At++]=(UCHAR)C;B[At++]=0;for(ULONG K=0;K<Count;K++){ULONG F=WcnChannelFrequency(B[I+2+K]);if(PremoteChannel(Remote,RemoteLength,F))B[At++]=B[I+2+K];}B[Start+1]=(UCHAR)(At-Start-2);if(!B[Start+1])At=Start;I+=Count+2;}N=At;}W.Attr(11,B,N);if(OperatingInfo){P2pChannelBytes(Operating,B);W.Attr(17,B,5);}}
 ULONG DeviceLength=Pdevice(P,B);if(!DeviceLength)W.Good=false;W.Attr(13,B,DeviceLength);}
static NTSTATUS Pensure(P2P *P)
{
 auto D=P->D;if(P->Created)return WcnNowMs()-P->CreatedAt<200?STATUS_DEVICE_BUSY:STATUS_SUCCESS;
 if(!D->Scan.Passed||!P->NameLength)return STATUS_DEVICE_BUSY;auto Mac=HotspotDeviceMac(D);if(!Mac)return STATUS_DEVICE_NOT_READY;RtlCopyMemory(P->Mac,Mac,6);
 ULONG B[21]={Jtlv(86,44),3,2,1};RtlCopyMemory(B+4,Mac,6);B[6]=2;B[7]=1;B[12]=Jtlv(18,32);for(ULONG I=0;I<2;I++){B[13+I*4]=Jtlv(411,12);B[14+I*4]=I;B[15+I*4]=B[16+I*4]=D->Ce.NumRfChains;}
 NTSTATUS St=WcnWmiSend(D,0x5001,B,sizeof(B));if(NT_SUCCESS(St)){P->Created=TRUE;P->CreatedAt=WcnNowMs();return STATUS_DEVICE_BUSY;}return St;
}
static NTSTATUS PstopLo(P2P *P)
{
 if(!P->LoActive)return STATUS_SUCCESS;if(P->LoStopping)return STATUS_DEVICE_BUSY;
 if(!P->LoStopAt)P->LoStopAt=WcnNowMs();ULONG B[]={Jtlv(547,4),3};NTSTATUS St=WcnWmiSend(P->D,0xd00b,B,sizeof(B));if(NT_SUCCESS(St))P->LoStopping=TRUE;return NT_SUCCESS(St)?STATUS_DEVICE_BUSY:St;
}
static ULONG Presponse(P2P *P,const UCHAR Peer[6],ULONG Frequency,UCHAR *Out,ULONG Capacity);
static NTSTATUS PstartLo(P2P *P)
{
 UCHAR Frame[1024],Broadcast[6]={255,255,255,255,255,255};ULONG N=Presponse(P,Broadcast,P->ListenFrequency,Frame,sizeof(Frame));if(!N)return STATUS_INVALID_BUFFER_SIZE;
 ULONG *B=P->Wire,Types=(P->SecondaryCount+1)*8;RtlZeroMemory(B,sizeof(P->Wire));B[0]=Jtlv(546,40);B[1]=3;B[3]=P->ListenFrequency;B[4]=P->ListenPeriod;B[5]=P->ListenInterval;B[7]=Types;B[8]=N;RtlCopyMemory(B+9,P->Mac,6);ULONG At=11;B[At++]=Jtlv(17,Types);RtlCopyMemory(B+At,P->DeviceType,8);RtlCopyMemory((UCHAR*)(B+At)+8,P->SecondaryTypes,P->SecondaryCount*8);At+=Types/4;B[At++]=Jtlv(17,(N+3)&~3u);RtlCopyMemory(B+At,Frame,N);At+=(N+3)/4;NTSTATUS St=WcnWmiSend(P->D,0xd00a,B,At*4);
 if(NT_SUCCESS(St)){P->LoActive=TRUE;P->LoStopping=P->LoRefresh=FALSE;P->LoStopAt=0;}return St;
}
extern "C" NTSTATUS WcnP2pPauseListen(PWCN_DEVICE D)
{auto P=Ps(D);return P?PstopLo(P):STATUS_SUCCESS;}
static NTSTATUS Pscan(P2P *P,const ULONG *F,ULONG Count,bool Active,ULONG Dwell)
{
 auto D=P->D;if(P->LoActive)return PstopLo(P);if(P->OwnScan||D->Scan.RuntimeScan)return STATUS_DEVICE_BUSY;auto A=(S8*)D->Stage8;auto B=(S8*)D->SecondaryStage8;auto C=D->P2pClient?(S8*)D->RoleStage8:nullptr;if((A&&A->ScanIssued)||(B&&B->ScanIssued)||(C&&C->ScanIssued)||D->Join.KeyUpdate.Slot||D->Secondary.KeyUpdate.Slot||(D->P2pClient&&D->P2pJoin.KeyUpdate.Slot))return STATUS_DEVICE_BUSY;
 NTSTATUS St=WcnTdlsPause(D,&D->Join);if(NT_SUCCESS(St))St=WcnTdlsPause(D,&D->Secondary);if(NT_SUCCESS(St))St=WcnTwtStop(D,&D->Join);if(NT_SUCCESS(St))St=WcnTwtStop(D,&D->Secondary);if(!NT_SUCCESS(St))return St;
 WCN_SCAN_TARGET T={};RtlFillMemory(T.Bssid,6,255);T.Active=Active;T.DwellTime=Dwell;T.Flags=0x2100;
 if(Active){T.SsidLength=P->TargetSsidLength?P->TargetSsidLength:7;RtlCopyMemory(T.Ssid,P->TargetSsidLength?P->TargetSsid:(const UCHAR*)"DIRECT-",T.SsidLength);T.IeLength=Pinfo(P,T.Ies,sizeof(T.Ies),!P2pVendor(P->ScanExtra,P->ScanExtraLength,WpsOui));if(!T.IeLength)return STATUS_INVALID_DEVICE_STATE;if(P->ScanExtraLength>sizeof(T.Ies)-T.IeLength)return STATUS_INVALID_BUFFER_SIZE;RtlCopyMemory(T.Ies+T.IeLength,P->ScanExtra,P->ScanExtraLength);T.IeLength+=P->ScanExtraLength;}
 St=WcnStage6Rescan(D,3,F,Count,&T);if(NT_SUCCESS(St)){P->OwnScan=TRUE;P->ScanId=D->Scan.ScanId;P->ScanAt=WcnNowMs();P->OnChannel=FALSE;P->ScanForTx=P->TxTask&&Count==1&&F[0]==P->TxFrequency;}return St;
}
static NTSTATUS Psubmit(P2P *P)
{
 auto D=P->D;ULONG N=P->TxLength;if(P->TxBusy||!P->TxPending)return STATUS_DEVICE_BUSY;
 if(!P->OwnScan||!P->OnChannel||P->Frequency!=P->TxFrequency)return STATUS_DEVICE_BUSY;
 if(!P->Dma.Buffer){NTSTATUS St=WcnDmaAllocAligned(D,1024,0xfff,&P->Dma);if(!NT_SUCCESS(St))return St;}
 RtlCopyMemory(P->Dma.Va,P->Tx,N);RtlZeroMemory(P->Wire,sizeof(P->Wire));auto C=(wmi_mgmt_send_cmd*)P->Wire;JHDR(C,422);C->vdev_id=3;C->desc_id=WcnMgmtAllocate(D,3);if(!C->desc_id)return STATUS_DEVICE_BUSY;
 P->TxId=C->desc_id;C->chanfreq=P->TxFrequency;C->paddr_lo=(ULONG)P->Dma.La;C->paddr_hi=(ULONG)(P->Dma.La>>32);C->frame_len=C->buf_len=N;P->Wire[sizeof(*C)/4]=Jtlv(17,(N+3)&~3u);RtlCopyMemory((UCHAR*)P->Wire+sizeof(*C)+4,P->Tx,N);KeMemoryBarrier();NTSTATUS St=WcnWmiSend(D,0x7008,P->Wire,sizeof(*C)+4+((N+3)&~3u));
 if(NT_SUCCESS(St)){P->TxBusy=TRUE;P->TxAt=WcnNowMs();P->TxPending=FALSE;}else D->MgmtInUse[3]=0;return St;
}

static NTSTATUS Padvert(P2P *P,const UCHAR *B,ULONG N)
{
 WDI_SET_ADVERTISEMENT_INFORMATION_PARAMETERS A;auto Ns=ParseWdiSetAdvertisementInformationToIhv(N,B,&Primary(P)->Tlv,&A);NTSTATUS St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
 if(NT_SUCCESS(St)&&A.Optional.DeviceInformation_IsPresent){auto I=&A.DeviceInformation;auto M=HotspotDeviceMac(P->D);if(!M||!Jeq(I->DeviceInfoParameters.DeviceAddress,M,6)||!I->DeviceName.ElementCount||I->DeviceName.ElementCount>32)St=STATUS_INVALID_PARAMETER;}
 if(NT_SUCCESS(St)&&A.Optional.SecondaryDeviceTypeList_IsPresent&&A.SecondaryDeviceTypeList.ElementCount>4)St=STATUS_NOT_SUPPORTED;
 if(NT_SUCCESS(St)&&A.Optional.AdditionalIEs_IsPresent&&(!Pvendor(A.AdditionalIEs.AdditionalProbeResponseIEs.pElements,A.AdditionalIEs.AdditionalProbeResponseIEs.ElementCount)||!Pvendor(A.AdditionalIEs.AdditionalBeaconIEs.pElements,A.AdditionalIEs.AdditionalBeaconIEs.ElementCount)))St=STATUS_NOT_SUPPORTED;
 if(NT_SUCCESS(St)&&A.Optional.AdvertisedServices_IsPresent)St=STATUS_NOT_SUPPORTED; /* ASP service publication is not generic device discovery. */
 if(NT_SUCCESS(St)){
  if(A.Optional.DeviceInformation_IsPresent){auto I=&A.DeviceInformation;RtlCopyMemory(P->Mac,I->DeviceInfoParameters.DeviceAddress,6);P->Methods=I->DeviceInfoParameters.ConfigurationMethods;P->NameLength=I->DeviceName.ElementCount;RtlCopyMemory(P->Name,I->DeviceName.pElements,P->NameLength);P2pBe16(P->DeviceType,I->DeviceInfoParameters.DeviceType.CategoryID);RtlCopyMemory(P->DeviceType+2,I->DeviceInfoParameters.DeviceType.OUI,4);P2pBe16(P->DeviceType+6,I->DeviceInfoParameters.DeviceType.SubcategoryID);}
  if(A.Optional.DeviceCapability_IsPresent)P->DeviceCap=(A.DeviceCapability.DeviceCapabilityBitmap&A.DeviceCapability.OSSetDeviceCapabilities&0x30)|4;
  if(A.Optional.GroupOwnerCapability_IsPresent)P->GroupCap=A.GroupOwnerCapability.GroupCapabilityBitmap&A.GroupOwnerCapability.OSSetGroupCapabilities&0x77;
  if(A.Optional.AdditionalIEs_IsPresent){P->ExtraLength=A.AdditionalIEs.AdditionalProbeResponseIEs.ElementCount;if(P->ExtraLength)RtlCopyMemory(P->Extra,A.AdditionalIEs.AdditionalProbeResponseIEs.pElements,P->ExtraLength);}
  if(A.Optional.SecondaryDeviceTypeList_IsPresent){P->SecondaryCount=A.SecondaryDeviceTypeList.ElementCount;for(ULONG I=0;I<P->SecondaryCount;I++){auto T=&A.SecondaryDeviceTypeList.pElements[I];P2pBe16(P->SecondaryTypes+I*8,T->CategoryID);RtlCopyMemory(P->SecondaryTypes+I*8+2,T->OUI,4);P2pBe16(P->SecondaryTypes+I*8+6,T->SubcategoryID);}}
  P->LoRefresh=TRUE;
  if(!P->ListenFrequency){for(ULONG I=0;I<P->D->Scan.ChannelCount;I++)if(P->D->Scan.Channel[I].Frequency<2500&&Plegal(P,P->D->Scan.Channel[I].Frequency)){P->ListenFrequency=P->D->Scan.Channel[I].Frequency;break;}}
 }
 CleanupParsedWdiSetAdvertisementInformation(&A);return St;
}
static NTSTATUS Pdiscover(P2P *P,const WDI_MESSAGE_HEADER &H,const UCHAR *B,ULONG N,bool Background)
{
 if(!Primary(P)->Radio)return STATUS_DEVICE_NOT_READY;if(P->Discover)return STATUS_DEVICE_BUSY;WDI_TASK_P2P_DISCOVER_PARAMETERS A;WDI_SET_P2P_START_BACKGROUND_DISCOVERY_PARAMETERS Bg;
 auto Ns=Background?ParseWdiSetP2pStartBackgroundDiscoveryToIhv(N,B,&Primary(P)->Tlv,&Bg):ParseWdiTaskP2pDiscoverToIhv(N,B,&Primary(P)->Tlv,&A);NTSTATUS St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
 auto Type=Background?Bg.DiscoverMode.DiscoveryType:A.DiscoverMode.DiscoveryType;auto Service=Background?Bg.DiscoverMode.ServiceDiscoveryType:A.DiscoverMode.ServiceDiscoveryType;
 auto &Settings=Background?Bg.DiscoveryChannelSettings:A.DiscoveryChannelSettings;auto &Vendor=Background?Bg.VendorIEs:A.VendorIEs;
 if(NT_SUCCESS(St)&&(Type<WDI_P2P_DISCOVER_TYPE_SCAN_ONLY||Type>WDI_P2P_DISCOVER_TYPE_SCAN_SOCIAL_CHANNELS||Service!=WDI_P2P_SERVICE_DISCOVERY_TYPE_NO_SERVICE_DISCOVERY||!Pvendor(Vendor.pElements,Vendor.ElementCount)))St=STATUS_NOT_SUPPORTED;
 if(NT_SUCCESS(St)&&!Background&&(A.SSIDList.ElementCount>1||A.ServiceNameHash.ElementCount||A.ServiceInformationDiscoveryEntry.ElementCount))St=STATUS_NOT_SUPPORTED;
 if(NT_SUCCESS(St)&&Background&&(Bg.DeviceFilterList.ElementCount>4||Bg.ServiceNameHash.ElementCount))St=STATUS_NOT_SUPPORTED;
 auto Name=!Background&&A.SSIDList.ElementCount?&A.SSIDList.pElements[0]:nullptr;if(NT_SUCCESS(St)&&Name&&Name->ElementCount>32)St=STATUS_INVALID_PARAMETER;
 ULONG Count=0,F[64];bool Passive=!Background&&A.DiscoverMode.ScanType==WDI_P2P_SCAN_TYPE_PASSIVE;
 if(NT_SUCCESS(St))for(ULONG I=0;I<P->D->Scan.ChannelCount;I++){ULONG V=P->D->Scan.Channel[I].Frequency;bool Allowed=Settings.ElementCount==0;if(!Plegal(P,V,!Passive))continue;
  if((Type==WDI_P2P_DISCOVER_TYPE_FIND_ONLY||Type==WDI_P2P_DISCOVER_TYPE_SCAN_SOCIAL_CHANNELS)&&V!=2412&&V!=2437&&V!=2462&&!(!Background&&A.Optional.bIncludeListenChannel_IsPresent&&A.bIncludeListenChannel&&V==P->ListenFrequency))continue;
  for(ULONG K=0;K<Settings.ElementCount;K++)for(ULONG J=0;J<Settings.pElements[K].BandChannelList.ElementCount;J++){auto C=&Settings.pElements[K].BandChannelList.pElements[J];if(C->BandID!=(WcnIs2g(V)?WDI_BAND_ID_2400:WDI_BAND_ID_5000))continue;if(!C->ChannelList.ElementCount)Allowed=true;for(ULONG Q=0;Q<C->ChannelList.ElementCount;Q++)if(C->ChannelList.pElements[Q]==WcnFrequencyChannel(V))Allowed=true;}
  if(!Background&&A.Optional.bIncludeListenChannel_IsPresent&&A.bIncludeListenChannel&&V==P->ListenFrequency)Allowed=true;
  if(Allowed&&Count<ARRAYSIZE(F))F[Count++]=V;
 }
 if(NT_SUCCESS(St)&&!Count)St=STATUS_NOT_SUPPORTED;
 if(NT_SUCCESS(St)){P->TargetSsidLength=(UCHAR)(Name?Name->ElementCount:0);if(P->TargetSsidLength)RtlCopyMemory(P->TargetSsid,Name->pElements,P->TargetSsidLength);P->Discover=TRUE;P->Background=Background;P->Find=Type==WDI_P2P_DISCOVER_TYPE_FIND_ONLY||Type==WDI_P2P_DISCOVER_TYPE_AUTO;P->FindListen=FALSE;P->Passive=Passive;P->DiscoverHeader=H;P->DiscoveryCount=Count;RtlCopyMemory(P->DiscoveryFrequency,F,Count*sizeof(ULONG));P->ScanExtraLength=Vendor.ElementCount;if(Vendor.ElementCount)RtlCopyMemory(P->ScanExtra,Vendor.pElements,Vendor.ElementCount);P->Repeat=Background?0:A.DiscoverMode.ScanRepeatCount;P->Delay=Background?1000:A.DiscoverMode.TimeBetweenScans;if(P->Delay<100)P->Delay=100;P->FilterCount=Background?Bg.DeviceFilterList.ElementCount:0;if(P->FilterCount)for(ULONG I=0;I<P->FilterCount;I++)RtlCopyMemory(P->Filters[I],Bg.DeviceFilterList.pElements[I].Address,6);P->NextAt=0;P->Cancel=FALSE;}
 if(Background)CleanupParsedWdiSetP2pStartBackgroundDiscovery(&Bg);else CleanupParsedWdiTaskP2pDiscover(&A);return St;
}
static NTSTATUS Paction(P2P *P,const WDI_MESSAGE_HEADER &H,const UCHAR *B,ULONG N,bool Response)
{
 if(!Primary(P)->Radio)return STATUS_DEVICE_NOT_READY;if(P->TxPending||P->TxBusy||P->TxTask||!P->NameLength)return STATUS_DEVICE_BUSY;
 WDI_TASK_P2P_SEND_REQUEST_ACTION_FRAME_PARAMETERS A;WDI_TASK_P2P_SEND_RESPONSE_ACTION_FRAME_PARAMETERS R;
 auto Ns=Response?ParseWdiTaskP2pSendResponseActionFrameToIhv(N,B,&Primary(P)->Tlv,&R):ParseWdiTaskP2pSendRequestActionFrameToIhv(N,B,&Primary(P)->Tlv,&A);NTSTATUS St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
 ULONG Type=Response?(ULONG)R.ResponseParams.ResponseFrameType:(ULONG)A.RequestParams.RequestFrameType;const UCHAR *Peer=Response?R.ResponseParams.PeerDeviceAddress.Address:A.RequestParams.PeerDeviceAddress.Address;
 UCHAR Token=Response?R.ResponseParams.DialogToken:A.RequestParams.DialogToken;ULONG Timeout=Response?R.ResponseParams.SendTimeout:A.RequestParams.SendTimeout,Dwell=Response?R.ResponseParams.PostACKDwellTime:A.RequestParams.PostACKDwellTime;auto &Vendor=Response?R.VendorIEs:A.VendorIEs;auto Cached=Ppeer(P,Peer);ULONG F=Cached?Cached->Frequency:0;
 if(!Response){F=A.DeviceDescriptor.ChannelInfo.ChannelNumber?WcnChannelFrequency(A.DeviceDescriptor.ChannelInfo.ChannelNumber):Cached?Cached->Frequency:0;
  auto Q=&A.DeviceDescriptor;auto Body=Q->Optional.ProbeResponseFrame_IsPresent?&Q->ProbeResponseFrame:Q->Optional.BeaconFrame_IsPresent?&Q->BeaconFrame:nullptr;
  if(Body&&Body->ElementCount>=12&&!(Jle16(Body->pElements+10)&1)){UCHAR Channel[5];ULONG L;if(P2pAttr(Body->pElements+12,Body->ElementCount-12,6,Channel,sizeof(Channel),&L)&&L==5){ULONG V=WcnChannelFrequency(Channel[4]);if(Plegal(P,V))F=V;}}
  if(Type==1&&A.GONegotiationRequestInfo.Optional.RemoteListenChannel_IsPresent)F=Pchannel(A.GONegotiationRequestInfo.RemoteListenChannel);}
 if(NT_SUCCESS(St)&&(!Pmac(Peer)||!Token||P2pSubtype(Type)==255||!Plegal(P,F)||!Timeout||Timeout>30000||Dwell>500||!Pvendor(Vendor.pElements,Vendor.ElementCount)))St=STATUS_INVALID_PARAMETER;
 if(NT_SUCCESS(St)&&Response){auto C=&R.PrecedingFrameInformation;UCHAR Expected=Type==2?1:Type==3?2:Type==5?4:Type==7?6:0;if(!Expected||!Cached||Cached->Type!=Expected||Cached->Token!=Token||WcnNowMs()-Cached->At>30000||C->FrameParams.ActionFrameType!=Expected||C->FrameParams.DialogToken!=Token||!Jeq(C->FrameParams.PeerDeviceAddress.Address,Peer,6))St=STATUS_INVALID_PARAMETER;}
 if(NT_SUCCESS(St)&&((!Response&&Type!=1&&Type!=4&&Type!=6)||(Response&&Type!=2&&Type!=3&&Type!=5&&Type!=7)))St=STATUS_INVALID_PARAMETER;
 if(NT_SUCCESS(St)){ULONG Count=Response?(ULONG)R.Optional.GONegotiationResponseInfo_IsPresent+R.Optional.GONegotiationConfirmationInfo_IsPresent+R.Optional.InvitationResponseInfo_IsPresent+R.Optional.ProvisionDiscoveryResponseInfo_IsPresent:(ULONG)A.Optional.GONegotiationRequestInfo_IsPresent+A.Optional.InvitationRequestInfo_IsPresent+A.Optional.ProvisionDiscoveryRequestInfo_IsPresent;if(Count!=1)St=STATUS_INVALID_PARAMETER;}
 ULONG Operating=Poperating(P);const UCHAR *Remote=nullptr;ULONG RemoteLength=0;
 if(NT_SUCCESS(St)&&Response&&(Type==2||Type==3)){Remote=R.PrecedingFrameInformation.FrameIEs.pElements;RemoteLength=R.PrecedingFrameInformation.FrameIEs.ElementCount;Operating=Pchoose(P,Remote,RemoteLength);
  bool LocalGo=Type==2?R.GONegotiationResponseInfo.Optional.GroupID_IsPresent!=0:R.GONegotiationConfirmationInfo.Optional.GroupID_IsPresent!=0;UCHAR Op[5];ULONG L;
  if(!LocalGo&&P2pAttr(Remote,RemoteLength,17,Op,sizeof(Op),&L)&&L==5){ULONG Fg=WcnChannelFrequency(Op[4]);if(Plegal(P,Fg)&&PremoteChannel(Remote,RemoteLength,Fg))Operating=Fg;else Operating=0;}}
 if(NT_SUCCESS(St)&&!Operating)St=STATUS_NOT_SUPPORTED;
 UCHAR Frame[1024]={0};ULONG Length=0;
 if(NT_SUCCESS(St)){
  Frame[0]=0xd0;RtlCopyMemory(Frame+4,Peer,6);RtlCopyMemory(Frame+10,P->Mac,6);RtlFillMemory(Frame+16,6,255);Jputle16(Frame+22,(++P->Sequence&4095)<<4);UCHAR Public[]={4,9,0x50,0x6f,0x9a,9,P2pSubtype(Type),Token};RtlCopyMemory(Frame+24,Public,8);P2pWriter W(Frame+32,sizeof(Frame)-32);UCHAR T[128];UCHAR Group=P->GroupCap;
  if(Type==1&&A.Optional.GONegotiationRequestInfo_IsPresent){auto Q=&A.GONegotiationRequestInfo.RequestParams;if(Q->GOIntent>15||Q->GOConfigTimeout>2550||Q->ClientConfigTimeout>2550||!Pmac(Q->IntendedInterfaceAddress.Address))W.Good=false;T[0]=(Q->GOIntent<<1)|(Q->TieBreaker?1:0);W.Attr(4,T,1);T[0]=(UCHAR)((Q->GOConfigTimeout+9)/10);T[1]=(UCHAR)((Q->ClientConfigTimeout+9)/10);W.Attr(5,T,2);W.Attr(9,Q->IntendedInterfaceAddress.Address,6);Group=Q->GroupCapability&0x77;}
  else if(Type==2&&R.Optional.GONegotiationResponseInfo_IsPresent){auto Q=&R.GONegotiationResponseInfo.ResponseParams;if(Q->GOIntent>15||!Pmac(Q->IntendedInterfaceAddress.Address))W.Good=false;T[0]=Q->StatusCode;W.Attr(0,T,1);T[0]=(Q->GOIntent<<1)|(Q->TieBreaker?1:0);W.Attr(4,T,1);T[0]=(UCHAR)((Q->GOConfigTimeout+9)/10);T[1]=(UCHAR)((Q->ClientConfigTimeout+9)/10);W.Attr(5,T,2);W.Attr(9,Q->IntendedInterfaceAddress.Address,6);Group=Q->GroupCapability&0x77;if(R.GONegotiationResponseInfo.Optional.GroupID_IsPresent)Pgroup(W,R.GONegotiationResponseInfo.GroupID);}
  else if(Type==3&&R.Optional.GONegotiationConfirmationInfo_IsPresent){T[0]=R.GONegotiationConfirmationInfo.ConfirmationParams.StatusCode;W.Attr(0,T,1);Group=R.GONegotiationConfirmationInfo.ConfirmationParams.GroupCapability&0x77;if(R.GONegotiationConfirmationInfo.Optional.GroupID_IsPresent)Pgroup(W,R.GONegotiationConfirmationInfo.GroupID);}
  else if(Type==4&&A.Optional.InvitationRequestInfo_IsPresent){auto Q=&A.InvitationRequestInfo;T[0]=(UCHAR)((Q->RequestParams.GOConfigTimeout+9)/10);T[1]=(UCHAR)((Q->RequestParams.ClientConfigTimeout+9)/10);W.Attr(5,T,2);T[0]=Q->RequestParams.InvitationFlags;W.Attr(18,T,1);Pgroup(W,Q->GroupID);if(Q->Optional.GroupBSSID_IsPresent)W.Attr(7,Q->GroupBSSID.Address,6);if(Q->Optional.OperatingChannel_IsPresent){Operating=Pchannel(Q->OperatingChannel);if(!Plegal(P,Operating))W.Good=false;}}
  else if(Type==5&&R.Optional.InvitationResponseInfo_IsPresent){auto Q=&R.InvitationResponseInfo;T[0]=Q->ResponseParams.StatusCode;W.Attr(0,T,1);if(Q->Optional.GroupBSSID_IsPresent)W.Attr(7,Q->GroupBSSID.Address,6);if(Q->Optional.OperatingChannel_IsPresent){Operating=Pchannel(Q->OperatingChannel);if(!Plegal(P,Operating))W.Good=false;}}
  else if(Type==6&&A.Optional.ProvisionDiscoveryRequestInfo_IsPresent){auto Q=&A.ProvisionDiscoveryRequestInfo;if(Q->Optional.ServiceAttributes_IsPresent||Q->Optional.PersistentGroupID_IsPresent||Q->Optional.ServiceSessionInfo_IsPresent)W.Good=false;Group=Q->RequestParams.GroupCapability&0x77;if(Q->Optional.GroupID_IsPresent)Pgroup(W,Q->GroupID);}
  else if(Type==7&&R.Optional.ProvisionDiscoveryResponseInfo_IsPresent)Group=R.ProvisionDiscoveryResponseInfo.ResponseParams.GroupCapability&0x77;
  else W.Good=false;
  bool IncludeOp=Type<=3||(Type==4&&(A.InvitationRequestInfo.Optional.OperatingChannel_IsPresent||A.InvitationRequestInfo.RequestParams.IsLocalGO))||(Type==5&&R.InvitationResponseInfo.Optional.OperatingChannel_IsPresent);Pcommon(P,W,Group,Operating,Remote,RemoteLength,Type<=5,IncludeOp);ULONG Ies=W.Finish();if(!Ies||Vendor.ElementCount>sizeof(Frame)-32-Ies)St=STATUS_INVALID_PARAMETER;
  else{Length=32+Ies;if(Vendor.ElementCount)RtlCopyMemory(Frame+Length,Vendor.pElements,Vendor.ElementCount);Length+=Vendor.ElementCount;if(!P2pVendor(Frame+32,Length-32,WpsOui)){P2pWriter V(Frame+Length,sizeof(Frame)-Length);V.Begin(WpsOui);T[0]=0x20;V.Wps(0x104a,T,1);P2pBe16(T,P->Methods);V.Wps(0x1008,T,2);ULONG M=V.Finish();if(!M)St=STATUS_INVALID_BUFFER_SIZE;else Length+=M;}}
 }
 if(NT_SUCCESS(St)){RtlCopyMemory(P->Tx,Frame,Length);P->TxLength=Length;P->TxFrequency=F;P->TxPending=P->TxTask=TRUE;P->TxResponse=Response;P->TxHeader=H;P->TxDeadline=WcnNowMs()+Timeout;P->TxDwell=Dwell<100?100:Dwell;if(Type<=3){RtlCopyMemory(P->NegotiationPeer,Peer,6);P->NegotiationToken=Token;P->NegotiationAt=WcnNowMs();if(Type==2||Type==3)P->NegotiatedFrequency=Operating;}}
 if(Response)CleanupParsedWdiTaskP2pSendResponseActionFrame(&R);else CleanupParsedWdiTaskP2pSendRequestActionFrame(&A);return St;
}

void P2pChannel(S8 *S,ULONG Frequency)
{
 WDI_INDICATION_P2P_GROUP_OPERATING_CHANNEL_PARAMETERS C;auto F=&C.Channel;F->CountryOrRegionString[0]='C';F->CountryOrRegionString[1]='N';F->CountryOrRegionString[2]=4;F->OperatingClass=P2pOperatingClass(Frequency);F->ChannelNumber=WcnFrequencyChannel(Frequency);C.IndicateReason=WDI_P2P_CHANNEL_INDICATE_REASON_NEW_CONNECTION;C.APBandInformation.BandID=WcnIs2g(Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;
 UCHAR *Out=nullptr;ULONG N=0;if(GenerateWdiIndicationP2pGroupOperatingChannelFromIhv(&C,0,&S->Tlv,&N,&Out)==NDIS_STATUS_SUCCESS){WDI_MESSAGE_HEADER H={};H.PortId=(WDI_PORT_ID)S->Port;S8Indicate(S,&H,WDI_INDICATION_P2P_GROUP_OPERATING_CHANNEL,STATUS_SUCCESS,Out,N,true);FreeGenerated(Out);}
}
ULONG P2pGroupFrequency(PWCN_DEVICE D){auto P=Ps(D);return P?P->RoleFrequency:0;}
NTSTATUS P2pConnectIes(S8 *S,const WDI_BYTE_BLOB &General,const WDI_BYTE_BLOB *Selected,bool Provisioning)
{
 auto P=Ps(S->D);if(!P||!P->NameLength)return STATUS_DEVICE_NOT_READY;ULONG N=0;UCHAR B[sizeof(S->AssocVendor)];
 if(!P2pIesValid(General.pElements,General.ElementCount)||General.ElementCount>sizeof(B))return STATUS_INVALID_PARAMETER;
 if(General.ElementCount)RtlCopyMemory(B,General.pElements,General.ElementCount);N=General.ElementCount;
 if(Selected){if(!P2pIesValid(Selected->pElements,Selected->ElementCount)||Selected->ElementCount>sizeof(B)-N)return STATUS_INVALID_PARAMETER;if(Selected->ElementCount)RtlCopyMemory(B+N,Selected->pElements,Selected->ElementCount);N+=Selected->ElementCount;}
 if(!P2pAttributesValid(B,N))return STATUS_INVALID_PARAMETER;
 UCHAR Info[128];ULONG L;P2pWriter W(B+N,sizeof(B)-N);
 if(P2pAttr(B,N,2,Info,sizeof(Info),&L)){if(L!=2||(Info[1]&1))return STATUS_INVALID_PARAMETER;}else{UCHAR Capability[]={P->DeviceCap,0};W.Attr(2,Capability,2);}
 if(P2pAttr(B,N,13,Info,sizeof(Info),&L,TRUE)){if(L<21||!Jeq(Info,P->Mac,6)||Info[16]>4)return STATUS_INVALID_PARAMETER;ULONG At=17+Info[16]*8;if(L<At+4||Jbe16(Info+At)!=0x1011||Jbe16(Info+At+2)>32||L!=At+4+Jbe16(Info+At+2))return STATUS_INVALID_PARAMETER;}else{ULONG Size=Pdevice(P,Info);if(!Size)return STATUS_INVALID_DEVICE_STATE;W.Attr(13,Info,Size);}
 if(Provisioning&&!P2pVendor(B,N,WpsOui)){W.Begin(WpsOui);Info[0]=0x20;W.Wps(0x104a,Info,1);Info[0]=1;W.Wps(0x103a,Info,1);P2pBe16(Info,P->Methods);W.Wps(0x1008,Info,2);}
 ULONG More=W.Finish();if(!W.Good)return STATUS_INVALID_BUFFER_SIZE;N+=More;
 RtlCopyMemory(S->AssocVendor,B,N);S->AssocVendorLength=N;return STATUS_SUCCESS;
}
bool P2pCommand(S8 *S,WIFIREQUEST R,const WDI_MESSAGE_HEADER &H,UINT16 Id,const UCHAR *B,ULONG N)
{
 auto D=S->D;auto P=Ps(D);if(!P)return false;bool Device=H.PortId==HotspotDevicePort(D),Role=D->RoleStage8&&((S8*)D->RoleStage8)->Adapter&&!((S8*)D->RoleStage8)->AdapterGone&&H.PortId==((S8*)D->RoleStage8)->Port;
 if(!Device&&!Role)return false;NTSTATUS St=STATUS_NOT_SUPPORTED;
 if(P->ResetPending&&Device&&Id!=WDI_ABORT_TASK){S8Complete(R,STATUS_DEVICE_BUSY);return true;}
 if(P->Failed&&Device&&Id!=WDI_TASK_DOT11_RESET){S8Complete(R,STATUS_DEVICE_HARDWARE_ERROR);return true;}
 if(Id==WDI_ABORT_TASK){WDI_TASK_ABORT_PARAMETERS A;auto Ns=ParseWdiAbortTaskToIhv(N,B,&Primary(P)->Tlv,&A);bool Discovery=Ns==NDIS_STATUS_SUCCESS&&P->Discover&&!P->Background&&A.CancelParameters.OriginalTaskOID==OID_WDI_TASK_P2P_DISCOVER&&A.CancelParameters.OriginalPortId==P->DiscoverHeader.PortId&&A.CancelParameters.OriginalTransactionId==P->DiscoverHeader.TransactionId;
 bool Action=Ns==NDIS_STATUS_SUCCESS&&P->TxTask&&A.CancelParameters.OriginalTaskOID==(P->TxResponse?OID_WDI_TASK_P2P_SEND_RESPONSE_ACTION_FRAME:OID_WDI_TASK_P2P_SEND_REQUEST_ACTION_FRAME)&&A.CancelParameters.OriginalPortId==P->TxHeader.PortId&&A.CancelParameters.OriginalTransactionId==P->TxHeader.TransactionId;
 bool Mode=Ns==NDIS_STATUS_SUCCESS&&P->ModePending&&A.CancelParameters.OriginalTaskOID==OID_WDI_TASK_CHANGE_OPERATION_MODE&&A.CancelParameters.OriginalPortId==P->ModeHeader.PortId&&A.CancelParameters.OriginalTransactionId==P->ModeHeader.TransactionId;CleanupParsedWdiAbortTaskToIhv(&A);if(!Discovery&&!Action&&!Mode)return false;
 if(Discovery){if(P->OwnScan&&!P->ScanForTx){P->Cancel=TRUE;P->CancelAt=WcnNowMs();}else PfinishDiscover(P,STATUS_CANCELLED);}if(Action)PfinishTx(P,STATUS_CANCELLED);if(Mode)P->ModeCancel=TRUE;S8Complete(R,STATUS_SUCCESS);return true;}
 if(P->ModePending&&Role){S8Complete(R,STATUS_DEVICE_BUSY);return true;}
 switch(Id){
 case WDI_GET_NEXT_ACTION_FRAME_DIALOG_TOKEN:return S8ActionCommand(S,R,H,Id,B,N);
 case WDI_SET_ADVERTISEMENT_INFORMATION:if(HotspotAdvertBusy(D)){St=STATUS_DEVICE_BUSY;break;}St=Padvert(P,B,N);if(NT_SUCCESS(St))return false;break;
 case WDI_SET_P2P_WPS_ENABLED:{WDI_SET_P2P_WPS_ENABLED_PARAMETERS A;auto Ns=ParseWdiSetP2pWpsEnabledToIhv(N,B,&Primary(P)->Tlv,&A);St=Ns==NDIS_STATUS_SUCCESS&&A.WPSEnabled<=1?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;if(NT_SUCCESS(St)&&Role)S->WpsEnabled=A.WPSEnabled?TRUE:FALSE;CleanupParsedWdiSetP2pWpsEnabled(&A);break;}
 case WDI_TASK_CHANGE_OPERATION_MODE:{WDI_TASK_CHANGE_OPERATION_MODE_PARAMETERS A;auto Ns=ParseWdiTaskChangeOperationModeToIhv(N,B,&Primary(P)->Tlv,&A);ULONG Mode=A.OperationMode;St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
 if(NT_SUCCESS(St)&&Device&&Mode==WDI_OPERATION_MODE_P2P_DEVICE){CleanupParsedWdiTaskChangeOperationMode(&A);S8Complete(R,St);S8Indicate(S,&H,WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE,St);return true;}
 if(NT_SUCCESS(St)&&(!Role||(Mode!=WDI_OPERATION_MODE_P2P_GO&&Mode!=WDI_OPERATION_MODE_P2P_CLIENT)||S->Connect||S->Resetting||S->AdapterGone||P->TxBusy||P->OwnScan))St=STATUS_DEVICE_BUSY;
 if(NT_SUCCESS(St)){P->ModePending=TRUE;P->ModeCancel=FALSE;P->Mode=Mode;P->ModeHeader=H;P->ModeAt=WcnNowMs();}CleanupParsedWdiTaskChangeOperationMode(&A);break;}
 case WDI_TASK_P2P_DISCOVER:St=Device?Pdiscover(P,H,B,N,false):STATUS_NOT_SUPPORTED;break;
 case WDI_SET_P2P_START_BACKGROUND_DISCOVERY:St=Device?Pdiscover(P,H,B,N,true):STATUS_NOT_SUPPORTED;break;
 case WDI_SET_P2P_STOP_BACKGROUND_DISCOVERY:if(!Device)return false;if(P->Background){if(P->OwnScan&&!P->ScanForTx){P->Cancel=TRUE;P->CancelAt=WcnNowMs();}else PfinishDiscover(P,STATUS_CANCELLED);}St=STATUS_SUCCESS;break;
 case WDI_SET_P2P_LISTEN_STATE:{WDI_SET_P2P_LISTEN_STATE_PARAMETERS A;auto Ns=ParseWdiSetP2pListenStateToIhv(N,B,&Primary(P)->Tlv,&A);ULONG F=A.Optional.ListenChannel_IsPresent?Pchannel(A.ListenChannel):P->ListenFrequency;St=Ns==NDIS_STATUS_SUCCESS?STATUS_SUCCESS:STATUS_INVALID_PARAMETER;
 if(NT_SUCCESS(St)&&A.ListenState!=WDI_P2P_LISTEN_STATE_OFF&&!Primary(P)->Radio)St=STATUS_DEVICE_NOT_READY;
 if(NT_SUCCESS(St)&&A.ListenState!=WDI_P2P_LISTEN_STATE_OFF&&!D->Scan.Passed)St=STATUS_DEVICE_BUSY;
 if(NT_SUCCESS(St)&&A.ListenState!=WDI_P2P_LISTEN_STATE_OFF&&(!Plegal(P,F)||(A.ListenState!=WDI_P2P_LISTEN_STATE_PASSIVE_AVAILABILITY&&A.ListenState!=WDI_P2P_LISTEN_STATE_AUTOMATIC_AVAILABILITY&&A.ListenState!=WDI_P2P_LISTEN_STATE_HIGH_AVAILABILITY)||(A.ListenState==WDI_P2P_LISTEN_STATE_PASSIVE_AVAILABILITY&&(A.ListenDuration.ListenActiveDuration>(WcnService(D,106)?30000u:500u)||A.ListenDuration.CycleDuration<A.ListenDuration.ListenActiveDuration))))St=STATUS_NOT_SUPPORTED;
 if(NT_SUCCESS(St)){P->Listen=A.ListenState!=WDI_P2P_LISTEN_STATE_OFF;P->ListenFrequency=F;P->ListenPeriod=A.ListenDuration.ListenActiveDuration?A.ListenDuration.ListenActiveDuration:150;P->ListenInterval=A.ListenDuration.CycleDuration?A.ListenDuration.CycleDuration:500;P->LoRefresh=TRUE;if(A.ListenState==WDI_P2P_LISTEN_STATE_HIGH_AVAILABILITY){P->ListenPeriod=300;P->ListenInterval=400;}else if(A.ListenState==WDI_P2P_LISTEN_STATE_AUTOMATIC_AVAILABILITY){P->ListenPeriod=500;P->ListenInterval=5000;}else if(A.ListenState==WDI_P2P_LISTEN_STATE_PASSIVE_AVAILABILITY){P->ListenPeriod=A.ListenDuration.ListenActiveDuration;P->ListenInterval=A.ListenDuration.CycleDuration;P->Listen=P->ListenPeriod!=0;}if(P->ListenInterval<P->ListenPeriod+50)P->ListenInterval=P->ListenPeriod+50;P->NextAt=0;}CleanupParsedWdiSetP2pListenState(&A);break;}
 case WDI_TASK_P2P_SEND_REQUEST_ACTION_FRAME:St=Device?Paction(P,H,B,N,false):STATUS_NOT_SUPPORTED;break;
 case WDI_TASK_P2P_SEND_RESPONSE_ACTION_FRAME:St=Device?Paction(P,H,B,N,true):STATUS_NOT_SUPPORTED;break;
 case WDI_TASK_DOT11_RESET:if(!Device)return false;if(P->ModePending){St=STATUS_DEVICE_BUSY;break;}
  if(P->Failed&&!P->TxBusy&&!P->OwnScan&&!P->LoActive&&!P->ModeQuarantined&&!D->Mhi.SysErr&&!D->Mhi.RddmSaved&&!D->Ce.SourceFailed&&!D->Dp.Failed){P->Failed=FALSE;P->PeerCount=0;}if(P->Failed){St=STATUS_DEVICE_HARDWARE_ERROR;break;}
  P->Listen=FALSE;P->Cancel=P->OwnScan;P->CancelAt=WcnNowMs();PfinishTx(P,STATUS_CANCELLED);PfinishDiscover(P,STATUS_CANCELLED);P->ResetPending=TRUE;P->ResetHeader=H;P->ResetAt=WcnNowMs();St=STATUS_SUCCESS;break;
 default:return false;
 }
 S8Complete(R,St);return true;
}
extern "C" VOID WcnP2pService(PWCN_DEVICE D)
{
 auto P=Ps(D);if(!P||P->Failed)return;LONGLONG Now=WcnNowMs();NTSTATUS St;
 if(D->Mhi.SysErr||D->Mhi.RddmSaved||D->Ce.SourceFailed||D->Dp.Failed){Pfail(P,"physical device failed");return;}
 if(!Primary(P)->Radio){P->Listen=FALSE;PfinishDiscover(P,STATUS_DEVICE_NOT_READY);PfinishTx(P,STATUS_DEVICE_NOT_READY);if(P->OwnScan&&!P->Cancel){P->Cancel=TRUE;P->CancelAt=Now;}}
 if(P->ResetPending&&Now-P->ResetAt>5000){Pfail(P,"device reset ownership timeout");return;}
 if(P->LoActive){if((P->LoStopping||P->LoStopAt)&&Now-P->LoStopAt>5000){Pfail(P,"listen-stop confirmation timeout");return;}if(!P->Listen||P->Discover||P->TxPending||P->ModePending||P->LoRefresh||P->ResetPending){St=PstopLo(P);if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St)){Pfail(P,"listen-stop transport");}return;}}
 if(P->ModePending){auto S=(S8*)D->RoleStage8;if(!S){Pfail(P,"role adapter unavailable");return;}St=HotspotReleaseRole(D);
  if(St==STATUS_SUCCESS){if(!P->ModeCancel){bool Client=P->Mode==WDI_OPERATION_MODE_P2P_CLIENT;BOOLEAN Pmf=S->PmfAvailable;
    if(Client){D->P2pJoin.Vdev=1;RtlCopyMemory(D->P2pJoin.Mac,S->Mac,6);Pmf=NT_SUCCESS(WcnPmfInit(D,&D->P2pJoin));}S8ClearStaging(S);KIRQL Irql;KeAcquireSpinLock(&S->Lock,&Irql);D->P2pClient=Client;S->HotspotRole=!Client;S->J=Client?&D->P2pJoin:&D->Join;S->Stopped=S->Fatal=S->FailureReported=FALSE;S->Radio=Primary(P)->Radio;S->PmfAvailable=Pmf;KeReleaseSpinLock(&S->Lock,Irql);
    P->RoleFrequency=P->NegotiatedFrequency;P->NegotiatedFrequency=0;}
   S8Indicate(S,&P->ModeHeader,WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE,P->ModeCancel?STATUS_CANCELLED:STATUS_SUCCESS);P->ModePending=FALSE;
  }else if(St!=STATUS_PENDING&&St!=STATUS_DEVICE_BUSY){S8Indicate(S,&P->ModeHeader,WDI_INDICATION_CHANGE_OPERATION_MODE_COMPLETE,St);P->ModePending=FALSE;}
  else if(Now-P->ModeAt>15000){Pfail(P,"role delete/unmap confirmation timeout");return;}
 }
 if(P->TxBusy&&Now-P->TxAt>5000){Pfail(P,"management DMA completion timeout");return;}
 if(P->TxTask&&Now>P->TxDeadline)PfinishTx(P,STATUS_IO_TIMEOUT);
 if(!P->TxTask&&P->TxPending&&Now>P->TxDeadline)P->TxPending=FALSE;
 if(P->OwnScan){
  if(P->Cancel&&Now-P->CancelAt>5000){Pfail(P,"scan cancel confirmation timeout");return;}
  if(D->Scan.ScanId!=P->ScanId||D->Scan.ScanVdev!=3){Pfail(P,"scan ownership changed");return;}
  if(D->Scan.Failed||Now-P->ScanAt>28000){Pfail(P,"scan completion timeout");return;}
  if(P->Cancel||(P->TxTask&&!P->ScanForTx)){St=WcnStage6CancelRescan(D);if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St)){Pfail(P,"scan cancellation transport");return;}}
  if(D->Scan.ScanCompleted&&!D->Scan.RuntimeScan&&D->Scan.State==7){P->OwnScan=P->OnChannel=FALSE;
   if(P->Cancel){P->Cancel=FALSE;PfinishDiscover(P,STATUS_CANCELLED);}
   else if(P->Discover){if(D->Scan.ScanReason)PfinishDiscover(P,STATUS_DEVICE_NOT_READY);else if(P->Find&&!P->FindListen)P->FindListen=TRUE;else{P->FindListen=FALSE;if(P->Repeat&&--P->Repeat==0)PfinishDiscover(P,STATUS_SUCCESS);}}
   P->NextAt=Now+(P->Discover?(P->FindListen?0:P->Delay):P->ListenInterval>P->ListenPeriod?P->ListenInterval-P->ListenPeriod:0);
  }
 }
 if(P->ResetPending){if(!P->OwnScan&&!P->TxBusy&&!P->LoActive){P->ResetPending=FALSE;P->Cancel=FALSE;P->PeerCount=0;S8Indicate(Primary(P),&P->ResetHeader,WDI_INDICATION_DOT11_RESET_COMPLETE,STATUS_SUCCESS);}return;}
 if(P->TxPending){St=Pensure(P);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St)){PfinishTx(P,St);return;}
  if(!P->OwnScan){St=Pscan(P,&P->TxFrequency,1,false,P->TxDwell?P->TxDwell:150);if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St))PfinishTx(P,St);return;}
  St=Psubmit(P);if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St))PfinishTx(P,St);return;
 }
 if(P->OwnScan||P->TxBusy||P->ModePending||Now<P->NextAt||(!P->Discover&&!P->Listen))return;
 St=Pensure(P);if(St==STATUS_DEVICE_BUSY)return;if(!NT_SUCCESS(St)){Pfail(P,"P2P device creation failed");return;}
 if(P->Listen&&!P->Discover&&WcnService(D,106)&&!P->LoDisabled){auto A=(S8*)D->Stage8;auto B=(S8*)D->SecondaryStage8;auto C=D->P2pClient?(S8*)D->RoleStage8:nullptr;if(D->Scan.RuntimeScan||(A&&A->ScanPending)||(B&&B->ScanPending)||(C&&C->ScanPending))return;if(P->LoActive)return;St=PstartLo(P);if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St))Pfail(P,"listen offload transport");return;}
 if(P->Discover&&!P->FindListen)St=Pscan(P,P->DiscoveryFrequency,P->DiscoveryCount,!P->Passive,50);
 else if(Plegal(P,P->ListenFrequency))St=Pscan(P,&P->ListenFrequency,1,false,P->ListenPeriod);else{P->Listen=FALSE;return;}
 if(St!=STATUS_DEVICE_BUSY&&!NT_SUCCESS(St)){if(P->Discover)PfinishDiscover(P,St);else Pfail(P,"listen window setup failed");}
}

static ULONG Presponse(P2P *P,const UCHAR Peer[6],ULONG Frequency,UCHAR *Out,ULONG Capacity)
{
 UCHAR *B=Out;RtlZeroMemory(B,Capacity);B[0]=0x50;RtlCopyMemory(B+4,Peer,6);RtlCopyMemory(B+10,P->Mac,6);RtlCopyMemory(B+16,P->Mac,6);Jputle16(B+22,(++P->Sequence&4095)<<4);Jputle16(B+32,100);Jputle16(B+34,0x400);ULONG L=36;
 const UCHAR Base[]={0,7,'D','I','R','E','C','T','-',1,8,0x8c,18,0x98,36,0xb0,72,96,108,3,1};RtlCopyMemory(B+L,Base,sizeof(Base));L+=sizeof(Base);B[L++]=(UCHAR)WcnFrequencyChannel(Frequency);
 ULONG M=Pinfo(P,B+L,Capacity-L,!P2pVendor(P->Extra,P->ExtraLength,WpsOui),true);if(!M||P->ExtraLength>Capacity-L-M)return 0;L+=M;RtlCopyMemory(B+L,P->Extra,P->ExtraLength);L+=P->ExtraLength;
 return L;
}
static void Pprobe(P2P *P,const UCHAR *F,ULONG N,ULONG Frequency)
{
 if((!P->Listen&&!(P->Discover&&P->FindListen))||!P->OwnScan||!P->OnChannel||P->Frequency!=Frequency||!Plegal(P,Frequency)||P->TxTask||P->TxBusy||P->TxPending||!Pmac(F+10))return;
 bool Direct=false;for(ULONG I=24;I<N;I+=F[I+1]+2){if(N-I<2||F[I+1]>N-I-2)return;if(F[I]==0&&F[I+1]==7&&Jeq(F+I+2,(const UCHAR*)"DIRECT-",7))Direct=true;}if(!Direct)return;
 ULONG L=Presponse(P,F+10,Frequency,P->Tx,sizeof(P->Tx));if(!L)return;
 P->TxLength=L;P->TxFrequency=Frequency;P->TxPending=TRUE;P->TxTask=FALSE;P->TxDeadline=WcnNowMs()+200;
}
static void Pobserve(P2P *P,const UCHAR *F,ULONG N,ULONG Frequency,LONG Rssi)
{
 if(!P->Discover||!P->OwnScan||!P->OnChannel||P->Frequency!=Frequency||N<36||!P2pAttributesValid(F+36,N-36)||!P2pVendor(F+36,N-36,P2pOui))return;
 ULONG NameLength;auto Name=WcnBssSsid(F,N,&NameLength);if(!Name)return;
 if(P->TargetSsidLength){bool Wild=P->TargetSsidLength==7&&Jeq(P->TargetSsid,(const UCHAR*)"DIRECT-",7);if(Wild?(NameLength<7||!Jeq(Name,P->TargetSsid,7)):(NameLength!=P->TargetSsidLength||!Jeq(Name,P->TargetSsid,NameLength)))return;}
 UCHAR Device[6];ULONG L=0;bool Info=P2pAttr(F+36,N-36,13,Device,sizeof(Device),&L,TRUE)!=FALSE;const UCHAR *Mac=Info&&L>=6?Device:F+10;if(!Pmac(Mac))return;
 if(P->FilterCount){bool Match=false;for(ULONG I=0;I<P->FilterCount;I++)if(Jeq(Mac,P->Filters[I],6))Match=true;if(!Match)return;}
 auto E=Ppeer(P,Mac,true);E->Frequency=Frequency;E->At=WcnNowMs();
 if(Rssi>0||Rssi<-127)Rssi=-127;WDI_BSS_ENTRY_CONTAINER C;RtlCopyMemory(C.BSSID.Address,F+16,6);C.SignalInfo.RSSI=Rssi;LONG Q=(Rssi+100)*2;C.SignalInfo.LinkQuality=(UCHAR)(Q<0?0:Q>100?100:Q);C.ChannelInfo.BandId=WcnIs2g(Frequency)?WDI_BAND_ID_2400:WDI_BAND_ID_5000;C.ChannelInfo.ChannelNumber=WcnFrequencyChannel(Frequency);
 if((F[0]&0xfc)==0x80){C.Optional.BeaconFrame_IsPresent=TRUE;C.BeaconFrame.SimpleAssign((UCHAR*)F+24,N-24);}else{C.Optional.ProbeResponseFrame_IsPresent=TRUE;C.ProbeResponseFrame.SimpleAssign((UCHAR*)F+24,N-24);}
 WDI_INDICATION_BSS_ENTRY_LIST_PARAMETERS List;List.Optional.DeviceDescriptor_IsPresent=TRUE;List.DeviceDescriptor.SimpleAssign(&C,1);UCHAR *Out=nullptr;ULONG Size=0;if(GenerateWdiIndicationBssEntryListFromIhv(&List,0,&Primary(P)->Tlv,&Size,&Out)==NDIS_STATUS_SUCCESS){S8Indicate(Primary(P),&P->DiscoverHeader,WDI_INDICATION_BSS_ENTRY_LIST,STATUS_SUCCESS,Out,Size,true);FreeGenerated(Out);}
}
extern "C" VOID WcnP2pWmi(PWCN_DEVICE D,ULONG Event,const UCHAR *Data,ULONG N)
{
 auto P=Ps(D);if(!P||!Jtlvs(Data,N))return;ULONG L,K;const UCHAR *V;
 if(Event==0xd003){V=Jfind(Data,N,548,&L);if(V&&L>=8&&Jle32(V)==3){P->LoActive=P->LoStopping=FALSE;P->LoStopAt=0;if(Jle32(V+4)>1)P->LoDisabled=TRUE;}return;}
 if(Event==0x7006){V=Jfind(Data,N,423,&L);if(!V||L<12||!P->TxBusy||Jle32(V)!=P->TxId||D->MgmtInUse[3]!=P->TxId||Jle32(V+8)<1||Jle32(V+8)>2)return;
  ULONG Status=Jle32(V+4);P->TxBusy=FALSE;D->MgmtInUse[3]=0;if(P->TxTask)PfinishTx(P,Status?STATUS_UNSUCCESSFUL:STATUS_SUCCESS);return;
 }
 if(Event==0x3001&&P->OwnScan){V=Jfind(Data,N,36,&L);if(!V||L<24||Jle32(V+16)!=P->ScanId||Jle32(V+20)!=3)return;ULONG Type=Jle32(V);if(Type&8){P->Frequency=Jle32(V+8);P->OnChannel=TRUE;}if(Type&(2|4|64|256))P->OnChannel=FALSE;if(P->Failed&&(Type&(2|64)))P->OwnScan=FALSE;return;}
 if(P->Failed||!Primary(P)->Radio||Event!=0x7001)return;V=Jfind(Data,N,44,&L);auto F=Jfind(Data,N,17,&K);if(!V||L<68||!F||(Jle32(V+20)&0x29))return;ULONG Size=Jle32(V+16),Frequency=Jle32(V+64),Pdev=Jle32(V+60);if(Size<24||Size>K||Size>2048||(Pdev!=1&&Pdev!=2)||!Pmac(F+10))return;
 if(!Frequency)Frequency=WcnChannelFrequency(Jle32(V));if(!Plegal(P,Frequency,false))return;
 if(Jeq(F+10,P->Mac,6)||Jeq(F+10,D->Ce.MacAddress,6))return;
 ULONG Type=F[0]&0xfc;if(Type==0x80||Type==0x50){Pobserve(P,F,Size,Frequency,(LONG)Jle32(V+44));return;}
 if(Type==0x40){Pprobe(P,F,Size,Frequency);return;}
 if(Type!=0xd0||(F[1]&0x40)||Size<32||F[24]!=4||F[25]!=9||!Jeq(F+26,P2pOui,4)||!P2pAttributesValid(F+32,Size-32)||!P2pVendor(F+32,Size-32,P2pOui))return;
 if(!Jeq(F+4,P->Mac,6))return;ULONG Action=P2pFrameType(F[30]);if(!Action||!F[31])return;auto E=Ppeer(P,F+10,true);E->Frequency=Frequency;E->Type=(UCHAR)Action;E->Token=F[31];E->At=WcnNowMs();
 UCHAR Op[5];ULONG Ol;if((Action==2||Action==3)&&Jeq(P->NegotiationPeer,F+10,6)&&P->NegotiationToken==F[31]&&WcnNowMs()-P->NegotiationAt<30000&&P2pAttr(F+32,Size-32,17,Op,sizeof(Op),&Ol)&&Ol==5){ULONG Of=WcnChannelFrequency(Op[4]);if(Op[3]==P2pOperatingClass(Of)&&Plegal(P,Of))P->NegotiatedFrequency=Of;}
 WDI_INDICATION_P2P_ACTION_FRAME_RECEIVED_PARAMETERS R;R.FrameInfo.FrameParams.ActionFrameType=(WDI_P2P_ACTION_FRAME_TYPE)Action;RtlCopyMemory(R.FrameInfo.FrameParams.PeerDeviceAddress.Address,F+10,6);R.FrameInfo.FrameParams.DialogToken=F[31];R.FrameInfo.FrameIEs.SimpleAssign((UCHAR*)F+32,Size-32);UCHAR *Out=nullptr;ULONG Len=0;
 if(GenerateWdiIndicationP2pActionFrameReceivedFromIhv(&R,0,&Primary(P)->Tlv,&Len,&Out)==NDIS_STATUS_SUCCESS){WDI_MESSAGE_HEADER H={};H.PortId=(WDI_PORT_ID)HotspotDevicePort(D);S8Indicate(Primary(P),&H,WDI_INDICATION_P2P_ACTION_FRAME_RECEIVED,STATUS_SUCCESS,Out,Len,true);FreeGenerated(Out);}
}

ULONG P2pGroupIes(PWCN_DEVICE D,const UCHAR *Ssid,ULONG SsidLength,UCHAR *Out,ULONG Capacity,bool Wps)
{
 auto P=Ps(D);auto S=(S8*)D->RoleStage8;if(!P||!P->NameLength||!S||SsidLength>32)return 0;
 P2pWriter W(Out,Capacity);UCHAR B[128]={P->DeviceCap,(UCHAR)(P->GroupCap|1)};W.Attr(2,B,2);
 ULONG DeviceLength=Pdevice(P,B);if(!DeviceLength)W.Good=false;W.Attr(13,B,DeviceLength);
 RtlCopyMemory(B,P->Mac,6);RtlCopyMemory(B+6,Ssid,SsidLength);W.Attr(15,B,6+SsidLength);
 if(S->WpsEnabled&&Wps){W.Begin(WpsOui);B[0]=0x20;W.Wps(0x104a,B,1);B[0]=3;W.Wps(0x103b,B,1);P2pBe16(B,P->Methods);W.Wps(0x1008,B,2);W.Wps(0x1054,P->DeviceType,8);W.Wps(0x1011,P->Name,P->NameLength);}return W.Finish();
}

void P2pRoleStopped(PWCN_DEVICE D){auto P=Ps(D);if(P)P->RoleFrequency=0;}
