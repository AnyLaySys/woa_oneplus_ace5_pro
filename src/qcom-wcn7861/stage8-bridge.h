#pragma once
/* C transport boundary. Existing host regression builds leave WCN_STAGE8 off. */
#ifdef WCN_STAGE8
NTSTATUS WcnStage8Init(PWDFDEVICE_INIT Init);
NTSTATUS WcnStage8Create(PWCN_DEVICE Dev);
NTSTATUS WcnStage8Ready(PWCN_DEVICE Dev);
BOOLEAN WcnStage8ConnectRequested(PWCN_DEVICE Dev);
VOID WcnStage8Service(PWCN_DEVICE Dev);
VOID WcnStage8Stop(PWCN_DEVICE Dev);
VOID WcnStage8Observe(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,ULONG Frequency,LONG Rssi);
VOID WcnStage8Mgmt(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,BOOLEAN Sent);
VOID WcnStage8Ethernet(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length);
VOID WcnStage8SaeService(PWCN_DEVICE Dev);
VOID WcnStage8SaeReceive(PWCN_DEVICE Dev,const UCHAR *Body,ULONG Length);
VOID WcnStage8TxDone(PWCN_DEVICE Dev,UCHAR Purpose,BOOLEAN Ack);
NTSTATUS WcnStage8Key(PWCN_DEVICE Dev,BOOLEAN Group,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6]);
#else
#define WcnStage8ConnectRequested(D) TRUE
#define WcnStage8Observe(D,F,N,C,R) ((void)0)
#define WcnStage8Mgmt(D,F,N,S) ((void)0)
#define WcnStage8Ethernet(D,F,N) ((void)0)
#define WcnStage8SaeService(D) ((void)0)
#define WcnStage8SaeReceive(D,P,N) ((void)0)
#define WcnStage8TxDone(D,P,A) ((void)0)
#endif
