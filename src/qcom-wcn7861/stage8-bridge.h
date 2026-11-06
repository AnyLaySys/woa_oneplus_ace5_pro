#pragma once
/* C transport boundary for the WiFiCx adapter. */
NTSTATUS WcnStage8Init(PWDFDEVICE_INIT Init);
NTSTATUS WcnStage8Create(PWCN_DEVICE Dev);
NTSTATUS WcnStage8Ready(PWCN_DEVICE Dev);
BOOLEAN WcnStage8ConnectRequested(PWCN_DEVICE Dev,PWCN_JOIN J);
VOID WcnStage8Service(PWCN_DEVICE Dev);
VOID WcnStage8Stop(PWCN_DEVICE Dev);
VOID WcnStage8Observe(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,ULONG Frequency,LONG Rssi);
VOID WcnStage8Mgmt(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Frame,ULONG Length,BOOLEAN Sent);
VOID WcnStage8Ethernet(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Frame,ULONG Length);
VOID WcnStage8SaeService(PWCN_DEVICE Dev,PWCN_JOIN J);
VOID WcnStage8SaeReceive(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Body,ULONG Length);
VOID WcnStage8TxFailed(PWCN_DEVICE Dev,ULONG Vdev);
NTSTATUS WcnStage8Key(PWCN_DEVICE Dev,PWCN_JOIN J,BOOLEAN Group,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6]);

BOOLEAN WcnStage8SecondaryIdle(PWCN_DEVICE D);
