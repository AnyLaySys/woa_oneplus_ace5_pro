#include "stage8-internal.h"
#include <ws2ipdef.h>
#include <netioapi.h>
#include <wsk.h>
#include <fwpsk.h>
#include <initguid.h>
#include <fwpmk.h>
extern "C" {
#include "balance-packet.h"
}

static const GUID Bkeys[]={
    {0x8e50388a,0xcaf6,0x4656,{0xa6,0xd1,0x77,0x2e,0xee,0x88,0x9e,0x71}},
    {0x8e50388b,0xcaf6,0x4656,{0xa6,0xd1,0x77,0x2e,0xee,0x88,0x9e,0x71}},
    {0x8e50388d,0xcaf6,0x4656,{0xa6,0xd1,0x77,0x2e,0xee,0x88,0x9e,0x71}},
    {0x8e50388e,0xcaf6,0x4656,{0xa6,0xd1,0x77,0x2e,0xee,0x88,0x9e,0x71}},
    {0x8e50388c,0xcaf6,0x4656,{0xa6,0xd1,0x77,0x2e,0xee,0x88,0x9e,0x71}}
};
static const GUID *Blayers[]={&FWPM_LAYER_ALE_CONNECT_REDIRECT_V4,&FWPM_LAYER_ALE_CONNECT_REDIRECT_V6,&FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4,&FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6};
struct BALANCE;
struct BFLOW {
    BALANCE *B;UINT64 Endpoint,Flow;PWSK_SOCKET Socket;PIRP Close;LONGLONG At;
    ULONG Generation;UCHAR Family,Protocol,Path;BOOLEAN Busy;
    USHORT LocalPort,RemotePort,AlternatePort;UCHAR Remote[16],Original[16],Alternate[16],Gateway[6];
};
struct BFRAGMENT {UCHAR Source[16],Destination[16],Family,Protocol;ULONG Id,Generation,Index;LONGLONG At;};
struct BPENDING {USHORT Length;UCHAR Owner;BOOLEAN Ready;UCHAR Bytes[1514];LONGLONG At;};
struct BALANCE {
    PWCN_DEVICE D;WDFTIMER Timer;WDFWORKITEM Work;KSPIN_LOCK Lock;EX_RUNDOWN_REF Rundown;HANDLE Engine;
    UINT32 Callout[4];volatile BOOLEAN Stopping;BOOLEAN Registered,Captured;
    WSK_REGISTRATION Registration;WSK_PROVIDER_NPI Provider;
    NET_LUID Luid[2];SOCKADDR_INET Gateway[2][2];LONGLONG At;
    UCHAR Primary[16][16],PrimaryFamily[16];ULONG PrimaryCount,Serial[2],Generation;volatile LONG Mapped;
    BFLOW Flow[2048];BFRAGMENT Fragment[128];BPENDING Pending[128];
};
struct BCONTEXT {BALANCE *B;};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(BCONTEXT,Bcontext);
static ULONG Bsize(UCHAR Family){return Family==4?4u:16u;}
static ADDRESS_FAMILY Bfamily(UCHAR Family){return Family==4?AF_INET:AF_INET6;}
static UCHAR *Bbytes(SOCKADDR_INET *A)
{return A->si_family==AF_INET?(UCHAR*)&A->Ipv4.sin_addr:(UCHAR*)&A->Ipv6.sin6_addr;}
static BOOLEAN Bsame(const UCHAR *A,const UCHAR *B,ULONG N){return RtlCompareMemory(A,B,N)==N;}
static BOOLEAN Bzero(const UCHAR *A,ULONG N){UCHAR V=0;for(ULONG I=0;I<N;I++)V|=A[I];return V==0;}
static BOOLEAN Bpublic(UCHAR Family,const UCHAR *A)
{
    if(Family==6)return (A[0]&0xe0)==0x20;
    return A[0]&&A[0]<224&&A[0]!=10&&A[0]!=127&&!(A[0]==169&&A[1]==254)&&!(A[0]==172&&(A[1]&0xf0)==16)&&
        !(A[0]==192&&A[1]==168)&&!(A[0]==100&&(A[1]&0xc0)==64)&&!(A[0]==198&&(A[1]&0xfe)==18);
}
static void Bsockaddr(SOCKADDR_INET *A,UCHAR Family,const UCHAR *Bytes)
{
    RtlZeroMemory(A,sizeof(*A));A->si_family=Bfamily(Family);RtlCopyMemory(Bbytes(A),Bytes,Bsize(Family));
}
static BOOLEAN Bprimary(BALANCE *B,const UCHAR *Address,UCHAR Family)
{for(ULONG I=0;I<B->PrimaryCount;I++)if(B->PrimaryFamily[I]==Family&&Bsame(B->Primary[I],Address,Bsize(Family)))return TRUE;return FALSE;}
struct BREQUEST {
    KEVENT Event;PIRP Irp;
    static NTSTATUS Complete(PDEVICE_OBJECT Device,PIRP Irp,PVOID Context){UNREFERENCED_PARAMETER(Device);UNREFERENCED_PARAMETER(Irp);KeSetEvent((PKEVENT)Context,IO_NO_INCREMENT,FALSE);return STATUS_MORE_PROCESSING_REQUIRED;}
    BREQUEST():Irp(IoAllocateIrp(1,FALSE)){KeInitializeEvent(&Event,NotificationEvent,FALSE);if(Irp)IoSetCompletionRoutine(Irp,Complete,&Event,TRUE,TRUE,TRUE);}
    ~BREQUEST(){if(Irp)IoFreeIrp(Irp);}
    NTSTATUS Wait(NTSTATUS St){if(St==STATUS_PENDING)KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,nullptr);return St==STATUS_PENDING?Irp->IoStatus.Status:St;}
};
static void BsocketClose(PWSK_SOCKET Socket,PIRP Irp)
{
    if(!Irp)return;if(Socket){KEVENT Event;KeInitializeEvent(&Event,NotificationEvent,FALSE);IoReuseIrp(Irp,STATUS_UNSUCCESSFUL);
        IoSetCompletionRoutine(Irp,BREQUEST::Complete,&Event,TRUE,TRUE,TRUE);
        if(((PWSK_PROVIDER_BASIC_DISPATCH)Socket->Dispatch)->WskCloseSocket(Socket,Irp)==STATUS_PENDING)KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,nullptr);}
    IoFreeIrp(Irp);
}
static NTSTATUS Boption(PWSK_SOCKET Socket,ULONG Option,ULONG Level)
{
    BREQUEST R;if(!R.Irp)return STATUS_INSUFFICIENT_RESOURCES;ULONG Value=1;
    return R.Wait(((PWSK_PROVIDER_BASIC_DISPATCH)Socket->Dispatch)->WskControlSocket(Socket,WskSetOption,Option,Level,sizeof(Value),&Value,0,nullptr,nullptr,R.Irp));
}
static PWSK_SOCKET Bsocket(BALANCE *B,BFLOW *F,SOCKADDR_INET *Address,PIRP *Close)
{
    *Close=nullptr;if(!B->Captured)return nullptr;*Close=IoAllocateIrp(1,FALSE);if(!*Close)return nullptr;BREQUEST R;
    if(!R.Irp){BsocketClose(nullptr,*Close);*Close=nullptr;return nullptr;}
    bool Tcp=F->Protocol==6;NTSTATUS St=R.Wait(B->Provider.Dispatch->WskSocket(B->Provider.Client,Bfamily(F->Family),Tcp?SOCK_STREAM:SOCK_DGRAM,F->Protocol,
        Tcp?WSK_FLAG_CONNECTION_SOCKET:WSK_FLAG_DATAGRAM_SOCKET,nullptr,nullptr,nullptr,nullptr,nullptr,R.Irp));
    if(!NT_SUCCESS(St)){BsocketClose(nullptr,*Close);*Close=nullptr;return nullptr;}auto Socket=(PWSK_SOCKET)R.Irp->IoStatus.Information;
    St=Boption(Socket,(ULONG)SO_EXCLUSIVEADDRUSE,SOL_SOCKET);if(NT_SUCCESS(St)&&F->Family==6)St=Boption(Socket,IPV6_V6ONLY,IPPROTO_IPV6);
    if(NT_SUCCESS(St)){
        BREQUEST Bind;if(!Bind.Irp)St=STATUS_INSUFFICIENT_RESOURCES;
        else{if(F->Family==4)Address->Ipv4.sin_port=0;else Address->Ipv6.sin6_port=0;
            St=Bind.Wait((Tcp?((PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch)->WskBind:((PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch)->WskBind)(Socket,(PSOCKADDR)Address,0,Bind.Irp));}
    }
    if(NT_SUCCESS(St)){BREQUEST Local;if(!Local.Irp)St=STATUS_INSUFFICIENT_RESOURCES;
        else St=Local.Wait((Tcp?((PWSK_PROVIDER_CONNECTION_DISPATCH)Socket->Dispatch)->WskGetLocalAddress:((PWSK_PROVIDER_DATAGRAM_DISPATCH)Socket->Dispatch)->WskGetLocalAddress)(Socket,(PSOCKADDR)Address,Local.Irp));}
    if(NT_SUCCESS(St))F->AlternatePort=RtlUshortByteSwap(F->Family==4?Address->Ipv4.sin_port:Address->Ipv6.sin6_port);
    if(!NT_SUCCESS(St)){BsocketClose(Socket,*Close);*Close=nullptr;return nullptr;}return Socket;
}
static BFLOW *Bfind(BALANCE *B,const WCN_BALANCE_PACKET &P,BOOLEAN Incoming)
{
    for(auto &F:B->Flow)if(F.B&&F.Family==P.Family&&F.Protocol==P.Protocol&&(Incoming?F.AlternatePort:F.LocalPort)==(Incoming?P.DestinationPort:P.SourcePort)&&
        F.RemotePort==(Incoming?P.SourcePort:P.DestinationPort)&&Bsame(F.Remote,Incoming?P.Source:P.Destination,Bsize(P.Family))&&
        (!Incoming||(F.Path==2&&Bsame(F.Alternate,P.Destination,Bsize(P.Family)))))return &F;
    return nullptr;
}
struct BGUARD {
    EX_RUNDOWN_REF *R;
    explicit BGUARD(BALANCE *B):R(ExAcquireRundownProtection(&B->Rundown)?&B->Rundown:nullptr){}
    ~BGUARD(){if(R)ExReleaseRundownProtection(R);}
};
static void Bclassify(const FWPS_INCOMING_VALUES0 *V,const FWPS_INCOMING_METADATA_VALUES0 *M,void *Data,const void *Context,const FWPS_FILTER1 *Filter,UINT64 FlowContext,FWPS_CLASSIFY_OUT0 *Out)
{
    UNREFERENCED_PARAMETER(Data);UNREFERENCED_PARAMETER(Context);UNREFERENCED_PARAMETER(FlowContext);
    if(!(Out->rights&FWPS_RIGHT_ACTION_WRITE))return;Out->actionType=FWP_ACTION_CONTINUE;
    auto B=(BALANCE*)(ULONG_PTR)Filter->context;BGUARD Guard(B);if(!Guard.R)return;
    bool Established=V->layerId==FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4||V->layerId==FWPS_LAYER_ALE_FLOW_ESTABLISHED_V6;
    bool V6=V->layerId==FWPS_LAYER_ALE_CONNECT_REDIRECT_V6||V->layerId==FWPS_LAYER_ALE_FLOW_ESTABLISHED_V6;
    ULONG Flags=V->incomingValue[Established?(V6?FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_FLAGS:FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_FLAGS):(V6?FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_FLAGS:FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_FLAGS)].value.uint32;
    ULONG Protocol=V->incomingValue[Established?(V6?FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_PROTOCOL:FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_PROTOCOL):(V6?FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_PROTOCOL:FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_PROTOCOL)].value.uint8;
    if((Protocol!=6&&Protocol!=17)||(Flags&(FWP_CONDITION_FLAG_IS_LOOPBACK|FWP_CONDITION_FLAG_IS_IPSEC_SECURED|FWP_CONDITION_FLAG_IS_REAUTHORIZE|FWP_CONDITION_FLAG_IS_PROXY_CONNECTION))||
        (!Established&&!FWPS_IS_METADATA_FIELD_PRESENT(M,FWPS_METADATA_FIELD_TRANSPORT_ENDPOINT_HANDLE)))return;
    UCHAR Local[16]={},Remote[16]={};ULONG LocalIndex=Established?(V6?FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_LOCAL_ADDRESS:FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_LOCAL_ADDRESS):(V6?FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_LOCAL_ADDRESS:FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_LOCAL_ADDRESS);
    ULONG RemoteIndex=Established?(V6?FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_REMOTE_ADDRESS:FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_ADDRESS):(V6?FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_REMOTE_ADDRESS:FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_REMOTE_ADDRESS);
    if(V6){RtlCopyMemory(Local,V->incomingValue[LocalIndex].value.byteArray16->byteArray16,16);RtlCopyMemory(Remote,V->incomingValue[RemoteIndex].value.byteArray16->byteArray16,16);}
    else{ULONG A=RtlUlongByteSwap(V->incomingValue[LocalIndex].value.uint32),C=RtlUlongByteSwap(V->incomingValue[RemoteIndex].value.uint32);RtlCopyMemory(Local,&A,4);RtlCopyMemory(Remote,&C,4);}
    UCHAR Family=V6?6:4;if(!Bpublic(Family,Remote)||(!Established&&!Bzero(Local,Bsize(Family))))return;
    USHORT LocalPort=V->incomingValue[Established?(V6?FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_LOCAL_PORT:FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_LOCAL_PORT):(V6?FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_LOCAL_PORT:FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_LOCAL_PORT)].value.uint16;
    USHORT RemotePort=V->incomingValue[Established?(V6?FWPS_FIELD_ALE_FLOW_ESTABLISHED_V6_IP_REMOTE_PORT:FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_PORT):(V6?FWPS_FIELD_ALE_CONNECT_REDIRECT_V6_IP_REMOTE_PORT:FWPS_FIELD_ALE_CONNECT_REDIRECT_V4_IP_REMOTE_PORT)].value.uint16;
    if(!LocalPort||!RemotePort)return;KIRQL Irql;KeAcquireSpinLock(&B->Lock,&Irql);BFLOW *Found=nullptr,*Free=nullptr;
    if(!B->Stopping)for(auto &F:B->Flow){if(!F.B){if(!Free)Free=&F;continue;}
        if(Established?(F.Family==Family&&F.Protocol==Protocol&&F.LocalPort==LocalPort&&F.RemotePort==RemotePort&&Bsame(F.Remote,Remote,Bsize(Family))&&
            (Bzero(F.Original,Bsize(Family))||Bsame(F.Original,Local,Bsize(Family)))):F.Endpoint==M->transportEndpointHandle){Found=&F;break;}}
    if(!Established&&!Found&&Free){Found=Free;Found->B=B;Found->Endpoint=M->transportEndpointHandle;Found->Family=Family;Found->Protocol=(UCHAR)Protocol;Found->LocalPort=LocalPort;Found->RemotePort=RemotePort;
        RtlCopyMemory(Found->Remote,Remote,Bsize(Family));Found->Generation=++B->Generation;Found->At=WcnNowMs();}
    BOOLEAN Associate=Established&&Found&&!Found->Flow&&FWPS_IS_METADATA_FIELD_PRESENT(M,FWPS_METADATA_FIELD_FLOW_HANDLE);
    if(Associate){Found->Flow=M->flowHandle;RtlCopyMemory(Found->Original,Local,Bsize(Family));}
    KeReleaseSpinLock(&B->Lock,Irql);
    if(Associate&&!NT_SUCCESS(FwpsFlowAssociateContext0(M->flowHandle,V->layerId,B->Callout[V6?3:2],(UINT64)(ULONG_PTR)Found))){KeAcquireSpinLock(&B->Lock,&Irql);Found->Flow=0;KeReleaseSpinLock(&B->Lock,Irql);}
}
static void BflowDelete(UINT16 Layer,UINT32 Callout,UINT64 Context)
{UNREFERENCED_PARAMETER(Layer);UNREFERENCED_PARAMETER(Callout);auto F=(BFLOW*)(ULONG_PTR)Context;KIRQL Irql;KeAcquireSpinLock(&F->B->Lock,&Irql);F->Flow=0;F->At=WcnNowMs();KeReleaseSpinLock(&F->B->Lock,Irql);}
static NTSTATUS Bnotify(FWPS_CALLOUT_NOTIFY_TYPE Type,const GUID *Key,FWPS_FILTER1 *Filter)
{UNREFERENCED_PARAMETER(Type);UNREFERENCED_PARAMETER(Key);UNREFERENCED_PARAMETER(Filter);return STATUS_SUCCESS;}
static void Bclose(BALANCE *B){if(B->Engine){FwpmEngineClose0(B->Engine);B->Engine=nullptr;}}
static NTSTATUS Bopen(BALANCE *B)
{
    if(B->Engine)return STATUS_SUCCESS;
    for(ULONG I=0;I<4;I++)if(!B->Callout[I]){FWPS_CALLOUT1 C={};C.calloutKey=Bkeys[I];C.classifyFn=Bclassify;C.notifyFn=Bnotify;if(I>=2)C.flowDeleteFn=BflowDelete;
        NTSTATUS St=FwpsCalloutRegister1(WdfDeviceWdmGetDeviceObject(B->D->Device),&C,&B->Callout[I]);if(!NT_SUCCESS(St))return St;}
    FWPM_SESSION0 Session={};Session.flags=FWPM_SESSION_FLAG_DYNAMIC;NTSTATUS St=FwpmEngineOpen0(nullptr,RPC_C_AUTHN_WINNT,nullptr,&Session,&B->Engine);if(!NT_SUCCESS(St)){B->Engine=nullptr;return St;}
    St=FwpmTransactionBegin0(B->Engine,0);if(!NT_SUCCESS(St)){Bclose(B);return St;}
    FWPM_SUBLAYER0 Sub={};Sub.subLayerKey=Bkeys[4];Sub.displayData.name=(wchar_t*)L"WCN7861 STA balance";Sub.weight=0x100;St=FwpmSubLayerAdd0(B->Engine,&Sub,nullptr);
    for(ULONG I=0;I<4&&NT_SUCCESS(St);I++){
        FWPM_CALLOUT0 C={};C.calloutKey=Bkeys[I];C.applicableLayer=*Blayers[I];C.displayData.name=Sub.displayData.name;St=FwpmCalloutAdd0(B->Engine,&C,nullptr,nullptr);if(!NT_SUCCESS(St))break;
        FWPM_FILTER0 F={};F.displayData.name=Sub.displayData.name;F.layerKey=*Blayers[I];F.subLayerKey=Sub.subLayerKey;F.action.type=I<2?FWP_ACTION_CALLOUT_TERMINATING:FWP_ACTION_CALLOUT_INSPECTION;F.action.calloutKey=C.calloutKey;
        F.rawContext=(UINT64)(ULONG_PTR)B;F.weight.type=FWP_UINT8;F.weight.uint8=0;FWPM_FILTER_CONDITION0 Condition={};Condition.fieldKey=FWPM_CONDITION_COMPARTMENT_ID;Condition.matchType=FWP_MATCH_EQUAL;
        Condition.conditionValue.type=FWP_UINT32;Condition.conditionValue.uint32=1;F.numFilterConditions=1;F.filterCondition=&Condition;St=FwpmFilterAdd0(B->Engine,&F,nullptr,nullptr);
    }
    if(NT_SUCCESS(St))St=FwpmTransactionCommit0(B->Engine);else FwpmTransactionAbort0(B->Engine);if(!NT_SUCCESS(St))Bclose(B);return St;
}

static void Bwork(WDFWORKITEM Work)
{
    auto B=Bcontext(Work)->B;if(B->Stopping)return;
    if(FwpmBfeStateGet0()!=FWPM_SERVICE_RUNNING)Bclose(B);else Bopen(B);
    if(B->Registered&&!B->Captured){WSK_PROVIDER_NPI Provider={};if(NT_SUCCESS(WskCaptureProviderNPI(&B->Registration,WSK_NO_WAIT,&Provider))){KIRQL L;KeAcquireSpinLock(&B->Lock,&L);B->Provider=Provider;B->Captured=TRUE;KeReleaseSpinLock(&B->Lock,L);}}
    NET_LUID Luid[2]={};SOCKADDR_INET Gateway[2][2]={};BOOLEAN Usable[2][2]={};
    S8_ADAPTER_REF Primary((S8*)B->D->Stage8),Secondary((S8*)B->D->SecondaryStage8);
    if(Primary.Handle)Luid[0]=NetAdapterGetNetLuid(Primary.Handle);if(Secondary.Handle)Luid[1]=NetAdapterGetNetLuid(Secondary.Handle);
    for(ULONG I=0;I<2;I++)if(Luid[I].Value)for(ULONG F=0;F<2;F++){MIB_IPINTERFACE_ROW Row={};Row.InterfaceLuid=Luid[I];Row.Family=F?AF_INET6:AF_INET;
        Usable[I][F]=NT_SUCCESS(GetIpInterfaceEntry(&Row))&&Row.Connected&&!Row.DisableDefaultRoutes;}
    PMIB_IPFORWARD_TABLE2 Routes=nullptr;
    if(NT_SUCCESS(GetIpForwardTable2(AF_UNSPEC,&Routes))){
        ULONG Metric[2][2]={{MAXULONG,MAXULONG},{MAXULONG,MAXULONG}};
        for(ULONG K=0;K<Routes->NumEntries;K++){auto &R=Routes->Table[K];if(R.DestinationPrefix.PrefixLength||!R.ValidLifetime||R.Loopback)continue;
            ULONG F=R.DestinationPrefix.Prefix.si_family==AF_INET6?1u:0u;
            for(ULONG I=0;I<2;I++)if(Luid[I].Value&&R.InterfaceLuid.Value==Luid[I].Value&&Usable[I][F]&&R.Metric<Metric[I][F]){Gateway[I][F]=R.NextHop;Metric[I][F]=R.Metric;}}
        FreeMibTable(Routes);
    }
    UCHAR Addresses[16][16]={},Family[16]={};ULONG Count=0;PMIB_UNICASTIPADDRESS_TABLE Table=nullptr;
    if(NT_SUCCESS(GetUnicastIpAddressTable(AF_UNSPEC,&Table))){for(ULONG K=0;K<Table->NumEntries&&Count<16;K++){auto &A=Table->Table[K];
        if(A.InterfaceLuid.Value!=Luid[0].Value||A.DadState!=IpDadStatePreferred||!A.ValidLifetime)continue;Family[Count]=A.Address.si_family==AF_INET6?6:4;
        RtlCopyMemory(Addresses[Count],Bbytes(&A.Address),Bsize(Family[Count]));Count++;}FreeMibTable(Table);}
    for(ULONG F=0;F<2;F++)if(Gateway[1][F].si_family&&!Bzero(Bbytes(&Gateway[1][F]),F?16:4)){MIB_IPNET_ROW2 Neighbor={};Neighbor.InterfaceLuid=Luid[1];Neighbor.Address=Gateway[1][F];
        if(!NT_SUCCESS(GetIpNetEntry2(&Neighbor))||Neighbor.PhysicalAddressLength!=6||Neighbor.State<=NlnsIncomplete)ResolveIpNetEntry2(&Neighbor,nullptr);}
    KIRQL Irql;KeAcquireSpinLock(&B->Lock,&Irql);RtlCopyMemory(B->Luid,Luid,sizeof(Luid));RtlCopyMemory(B->Gateway,Gateway,sizeof(Gateway));
    RtlCopyMemory(B->Primary,Addresses,sizeof(Addresses));RtlCopyMemory(B->PrimaryFamily,Family,sizeof(Family));B->PrimaryCount=Count;B->At=WcnNowMs();
    KeReleaseSpinLock(&B->Lock,Irql);
    for(auto &F:B->Flow){PWSK_SOCKET Socket=nullptr;PIRP Close=nullptr;KeAcquireSpinLock(&B->Lock,&Irql);
        if(F.B&&!F.Flow&&!F.Busy&&WcnNowMs()-F.At>240000){Socket=F.Socket;Close=F.Close;if(F.Path==2)InterlockedDecrement(&B->Mapped);RtlSecureZeroMemory(&F,sizeof(F));}
        KeReleaseSpinLock(&B->Lock,Irql);if(Close)BsocketClose(Socket,Close);}
}
static void Btimer(WDFTIMER Timer){auto B=Bcontext(Timer)->B;if(!B->Stopping)WdfWorkItemEnqueue(B->Work);}
static void Bcleanup(WDFOBJECT Object)
{
    auto B=Bcontext(Object)->B;if(!B)return;KIRQL Irql;KeAcquireSpinLock(&B->Lock,&Irql);B->Stopping=TRUE;KeReleaseSpinLock(&B->Lock,Irql);
    if(B->Timer)WdfTimerStop(B->Timer,TRUE);if(B->Work)WdfWorkItemFlush(B->Work);ExWaitForRundownProtectionRelease(&B->Rundown);Bclose(B);
    for(auto &F:B->Flow)if(F.Flow)FwpsFlowRemoveContext0(F.Flow,(UINT16)(F.Family==4?FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4:FWPS_LAYER_ALE_FLOW_ESTABLISHED_V6),B->Callout[F.Family==4?2:3]);
    for(auto Id:B->Callout)if(Id)FwpsCalloutUnregisterById0(Id);
    for(auto &F:B->Flow)if(F.Close)BsocketClose(F.Socket,F.Close);
    if(B->Captured)WskReleaseProviderNPI(&B->Registration);if(B->Registered)WskDeregister(&B->Registration);
    B->D->Balance=nullptr;RtlSecureZeroMemory(B,sizeof(*B));ExFreePoolWithTag(B,WCN_TAG);Bcontext(Object)->B=nullptr;
}
extern "C" NTSTATUS WcnBalanceCreate(PWCN_DEVICE D)
{
    WDF_OBJECT_ATTRIBUTES A;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,BCONTEXT);A.EvtCleanupCallback=Bcleanup;BCONTEXT *C;
    NTSTATUS St=WdfObjectAllocateContext(D->Device,&A,(void**)&C);if(!NT_SUCCESS(St))return St;
    auto B=(BALANCE*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(BALANCE),WCN_TAG);if(!B)return STATUS_INSUFFICIENT_RESOURCES;
    B->D=D;C->B=B;D->Balance=B;KeInitializeSpinLock(&B->Lock);ExInitializeRundownProtection(&B->Rundown);
    static const WSK_CLIENT_DISPATCH Dispatch={MAKE_WSK_VERSION(1,0),0,nullptr};WSK_CLIENT_NPI Npi={B,&Dispatch};
    B->Registered=NT_SUCCESS(WskRegister(&Npi,&B->Registration));
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&A,BCONTEXT);A.ParentObject=D->Device;
    WDF_WORKITEM_CONFIG Work;WDF_WORKITEM_CONFIG_INIT(&Work,Bwork);Work.AutomaticSerialization=FALSE;
    St=WdfWorkItemCreate(&Work,&A,&B->Work);if(!NT_SUCCESS(St))return St;Bcontext(B->Work)->B=B;
    WDF_TIMER_CONFIG Timer;WDF_TIMER_CONFIG_INIT_PERIODIC(&Timer,Btimer,1000);Timer.AutomaticSerialization=FALSE;
    St=WdfTimerCreate(&Timer,&A,&B->Timer);if(!NT_SUCCESS(St))return St;Bcontext(B->Timer)->B=B;WdfTimerStart(B->Timer,WDF_REL_TIMEOUT_IN_MS(1000));return STATUS_SUCCESS;
}
static BOOLEAN Bframe(UCHAR *F,ULONG N,WCN_BALANCE_PACKET *P)
{
    if(N<34)return FALSE;USHORT Type=Bget16(F+12);if(Type!=0x800&&Type!=0x86dd)return FALSE;
    return Bpacket(F+14,N-14,TRUE,P)&&((Type==0x800&&P->Family==4)||(Type==0x86dd&&P->Family==6));
}
static BFLOW *Bfragment(BALANCE *B,const WCN_BALANCE_PACKET &P)
{
    for(auto &G:B->Fragment)if(G.At&&WcnNowMs()-G.At<30000&&G.Family==P.Family&&G.Protocol==P.Protocol&&G.Id==P.Id&&
        Bsame(G.Source,P.Source,Bsize(P.Family))&&Bsame(G.Destination,P.Destination,Bsize(P.Family))){
        auto F=&B->Flow[G.Index];if(F->B&&F->Generation==G.Generation)return F;break;}
    return nullptr;
}
static void Bremember(BALANCE *B,const WCN_BALANCE_PACKET &P,BFLOW *F)
{
    BFRAGMENT *Slot=nullptr;for(auto &G:B->Fragment){if(G.Family==P.Family&&G.Protocol==P.Protocol&&G.Id==P.Id&&Bsame(G.Source,P.Source,Bsize(P.Family))&&Bsame(G.Destination,P.Destination,Bsize(P.Family))){Slot=&G;break;}
        if(!Slot||G.At<Slot->At)Slot=&G;}
    Slot->Family=P.Family;Slot->Protocol=P.Protocol;Slot->Id=P.Id;Slot->At=WcnNowMs();Slot->Index=(ULONG)(F-B->Flow);Slot->Generation=F->Generation;
    RtlCopyMemory(Slot->Source,P.Source,Bsize(P.Family));RtlCopyMemory(Slot->Destination,P.Destination,Bsize(P.Family));
}
static PWSK_SOCKET Bchoose(BALANCE *B,BFLOW *F,const WCN_BALANCE_PACKET &P,NET_LUID Luid[2],UCHAR Mac[6],SOCKADDR_INET *Source,PIRP *Close)
{
    SOCKADDR_INET Destination,Original;Bsockaddr(&Destination,P.Family,P.Destination);Bsockaddr(&Original,P.Family,P.Source);
    MIB_IPFORWARD_ROW2 PrimaryRoute={},Route={};SOCKADDR_INET PrimarySource;
    if(!NT_SUCCESS(GetBestRoute2(&Luid[0],0,&Original,&Destination,0,&PrimaryRoute,&PrimarySource))||PrimaryRoute.DestinationPrefix.PrefixLength||
        !NT_SUCCESS(GetBestRoute2(&Luid[1],0,nullptr,&Destination,0,&Route,Source))||Route.DestinationPrefix.PrefixLength)return nullptr;
    if(Source->si_family!=Bfamily(P.Family)||Bsame(P.Source,Bbytes(Source),Bsize(P.Family))||(P.Family==6&&!Bpublic(6,Bbytes(Source))))return nullptr;
    MIB_IPNET_ROW2 Neighbor={};Neighbor.InterfaceLuid=Luid[1];Neighbor.Address=Route.NextHop;
    if(!NT_SUCCESS(GetIpNetEntry2(&Neighbor))||Neighbor.PhysicalAddressLength!=6||Neighbor.State<=NlnsIncomplete||(Neighbor.PhysicalAddress[0]&1))return nullptr;
    RtlCopyMemory(Mac,Neighbor.PhysicalAddress,6);return Bsocket(B,F,Source,Close);
}
static PWCN_JOIN BerrorTx(BALANCE *B,UCHAR *Frame,WCN_BALANCE_PACKET *P)
{
    if(!((P->Protocol==1&&(P->Payload[0]==3||P->Payload[0]==11||P->Payload[0]==12))||(P->Protocol==58&&P->Payload[0]<128)))return &B->D->Join;
    WCN_BALANCE_PACKET Q;if(!Bpacket(P->Payload+8,P->Length-8,FALSE,&Q)||!Q.First||Q.Family!=P->Family||(Q.Protocol!=6&&Q.Protocol!=17))return &B->D->Join;
    KIRQL L;KeAcquireSpinLock(&B->Lock,&L);BFLOW *Found=nullptr;
    for(auto &F:B->Flow)if(F.B&&F.Path==2&&F.Family==Q.Family&&F.Protocol==Q.Protocol&&F.LocalPort==Q.DestinationPort&&F.RemotePort==Q.SourcePort&&
        Bsame(F.Original,Q.Destination,Bsize(Q.Family))&&Bsame(F.Remote,Q.Source,Bsize(Q.Family))&&Bsame(F.Original,P->Source,Bsize(P->Family))){Found=&F;break;}
    BFLOW Copy={};if(Found){Found->At=WcnNowMs();Copy=*Found;}KeReleaseSpinLock(&B->Lock,L);if(!Found)return &B->D->Join;
    if(!B->D->Secondary.Authorized||B->D->Secondary.Failed||B->D->Secondary.Resetting)return nullptr;
    Bquote(P,&Q,FALSE,Copy.Alternate);BquotePort(P,&Q,FALSE,Copy.LocalPort,Copy.AlternatePort);Baddress(P,TRUE,Copy.Alternate);
    RtlCopyMemory(Frame,Copy.Gateway,6);RtlCopyMemory(Frame+6,B->D->Secondary.Mac,6);return &B->D->Secondary;
}
extern "C" PWCN_JOIN WcnBalanceTx(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *Frame,ULONG N)
{
    auto B=(BALANCE*)D->Balance;if(!B||J!=&D->Join)return J;WCN_BALANCE_PACKET P;if(!Bframe(Frame,N,&P))return J;
    if(P.Protocol!=6&&P.Protocol!=17)return BerrorTx(B,Frame,&P);
    KIRQL Irql;KeAcquireSpinLock(&B->Lock,&Irql);BFLOW *F=P.First?Bfind(B,P,FALSE):Bfragment(B,P);
    if(!F||B->Stopping||(F->Path&& !Bsame(F->Original,P.Source,Bsize(P.Family)))){KeReleaseSpinLock(&B->Lock,Irql);return J;}
    F->At=WcnNowMs();NET_LUID Luid[2];RtlCopyMemory(Luid,B->Luid,sizeof(Luid));bool Select=false;
    if(!F->Path&&!F->Busy){
        RtlCopyMemory(F->Original,P.Source,Bsize(P.Family));F->Path=1;
        if(P.First&&(P.Protocol==17||(P.Length>=14&&(P.Payload[13]&0x12)==2))&&Bprimary(B,P.Source,P.Family)&&Luid[0].Value&&Luid[1].Value&&
            B->Gateway[0][P.Family==6].si_family&&B->Gateway[1][P.Family==6].si_family&&WcnNowMs()-B->At<3000&&B->Captured&&D->Secondary.Authorized&&!D->Secondary.Failed&&
            (B->Serial[P.Family==6]++&1)){F->Busy=TRUE;Select=true;}
    }
    KeReleaseSpinLock(&B->Lock,Irql);
    if(Select){SOCKADDR_INET Address={};UCHAR Mac[6];PIRP Close=nullptr;auto Socket=Bchoose(B,F,P,Luid,Mac,&Address,&Close);
        KeAcquireSpinLock(&B->Lock,&Irql);if(Socket){F->Socket=Socket;F->Close=Close;F->Path=2;InterlockedIncrement(&B->Mapped);RtlCopyMemory(F->Alternate,Bbytes(&Address),Bsize(P.Family));RtlCopyMemory(F->Gateway,Mac,6);}
        F->Busy=FALSE;KeReleaseSpinLock(&B->Lock,Irql);}
    KeAcquireSpinLock(&B->Lock,&Irql);auto Copy=*F;if(P.Fragment&&P.First)Bremember(B,P,F);KeReleaseSpinLock(&B->Lock,Irql);
    if(Copy.Path!=2)return J;if(!D->Secondary.Authorized||D->Secondary.Failed||D->Secondary.Resetting)return nullptr;
    Baddress(&P,TRUE,Copy.Alternate);Bport(&P,TRUE,Copy.LocalPort,Copy.AlternatePort);
    RtlCopyMemory(Frame,Copy.Gateway,6);RtlCopyMemory(Frame+6,D->Secondary.Mac,6);return &D->Secondary;
}
static BOOLEAN Btranslate(BALANCE *B,UCHAR *Frame,WCN_BALANCE_PACKET &P)
{
    if(P.Family==6&&P.Protocol==17&&P.Checksum&&!Bget16(P.Checksum))return FALSE;
    WCN_BALANCE_PACKET Inner={};BOOLEAN Error=(P.Protocol==1&&(P.Payload[0]==3||P.Payload[0]==11||P.Payload[0]==12))||(P.Protocol==58&&P.Payload[0]<128);
    if(Error){if(!Bpacket(P.Payload+8,P.Length-8,FALSE,&Inner)||!Inner.First||Inner.Family!=P.Family||(Inner.Protocol!=6&&Inner.Protocol!=17))return FALSE;}
    KIRQL Irql;KeAcquireSpinLock(&B->Lock,&Irql);BFLOW *F=nullptr;
    if(Error){for(auto &E:B->Flow)if(E.B&&E.Path==2&&E.Family==Inner.Family&&E.Protocol==Inner.Protocol&&E.AlternatePort==Inner.SourcePort&&E.RemotePort==Inner.DestinationPort&&
        Bsame(E.Alternate,Inner.Source,Bsize(Inner.Family))&&Bsame(E.Alternate,P.Destination,Bsize(P.Family))&&Bsame(E.Remote,Inner.Destination,Bsize(Inner.Family))){F=&E;break;}}
    else F=P.First?Bfind(B,P,TRUE):Bfragment(B,P);
    if(!F||F->Path!=2){KeReleaseSpinLock(&B->Lock,Irql);return FALSE;}
    F->At=WcnNowMs();auto Copy=*F;if(P.Fragment&&P.First)Bremember(B,P,F);KeReleaseSpinLock(&B->Lock,Irql);
    if(Error){Bquote(&P,&Inner,TRUE,Copy.Original);BquotePort(&P,&Inner,TRUE,Copy.AlternatePort,Copy.LocalPort);}
    Baddress(&P,FALSE,Copy.Original);if(!Error)Bport(&P,FALSE,Copy.AlternatePort,Copy.LocalPort);
    RtlCopyMemory(Frame,B->D->Join.Mac,6);return TRUE;
}
extern "C" BOOLEAN WcnBalanceRx(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Frame,ULONG N)
{
    auto B=(BALANCE*)D->Balance;if(!B||J!=&D->Secondary||N>1514)return FALSE;UCHAR Copy[1514];RtlCopyMemory(Copy,Frame,N);
    WCN_BALANCE_PACKET P;if(!Bframe(Copy,N,&P))return FALSE;UCHAR Source[16],Destination[16];ULONG Id=P.Id;UCHAR Family=P.Family,Protocol=P.Protocol;
    RtlCopyMemory(Source,P.Source,Bsize(Family));RtlCopyMemory(Destination,P.Destination,Bsize(Family));
    BOOLEAN Translated=Btranslate(B,Copy,P);
    if(Translated){J->PeerSeenAt=WcnNowMs();S8ReceiveEthernet((S8*)D->Stage8,Copy,N,true);}
    if(P.Fragment&&P.First)for(auto &Q:B->Pending)if(Q.Length&&!Q.Ready){WCN_BALANCE_PACKET T;
        if(Bframe(Q.Bytes,Q.Length,&T)&&T.Family==Family&&T.Protocol==Protocol&&T.Id==Id&&Bsame(T.Source,Source,Bsize(Family))&&Bsame(T.Destination,Destination,Bsize(Family))&&
            (!Translated||(WcnNowMs()-Q.At<30000&&Btranslate(B,Q.Bytes,T)))){Q.Ready=TRUE;Q.Owner=(UCHAR)(Translated?0:2);}}
    if(Translated){RtlSecureZeroMemory(Copy,sizeof(Copy));return TRUE;}
    if(P.Fragment&&!P.First){BOOLEAN Reserved=FALSE;KIRQL L;KeAcquireSpinLock(&B->Lock,&L);
        for(auto &F:B->Flow)if(F.B&&F.Path==2&&F.Family==Family&&Bsame(F.Alternate,Destination,Bsize(Family))&&Bsame(F.Remote,Source,Bsize(Family))){Reserved=TRUE;break;}
        KeReleaseSpinLock(&B->Lock,L);
        if(Reserved){J->PeerSeenAt=WcnNowMs();for(auto &Q:B->Pending)if(!Q.Length||WcnNowMs()-Q.At>=30000){RtlSecureZeroMemory(&Q,sizeof(Q));Q.Length=(USHORT)N;Q.At=WcnNowMs();RtlCopyMemory(Q.Bytes,Frame,N);return TRUE;}return TRUE;}}
    return FALSE;
}
extern "C" BOOLEAN WcnBalanceRxUsesPrimary(PWCN_DEVICE D)
{auto B=(BALANCE*)D->Balance;return B&&B->Mapped>0;}
extern "C" VOID WcnBalanceService(PWCN_DEVICE D)
{
    auto B=(BALANCE*)D->Balance;if(!B)return;for(auto &Q:B->Pending)if(Q.Length){
        if(WcnNowMs()-Q.At>=30000){RtlSecureZeroMemory(&Q,sizeof(Q));continue;}
        if(Q.Ready&&!WcnStage8RxBlocked(D,Q.Owner)){S8ReceiveEthernet((S8*)(Q.Owner==0?D->Stage8:D->SecondaryStage8),Q.Bytes,Q.Length,Q.Owner==0);RtlSecureZeroMemory(&Q,sizeof(Q));}}
}
