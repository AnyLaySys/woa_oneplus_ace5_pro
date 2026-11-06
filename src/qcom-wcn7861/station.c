/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Per-link DMA is retained until physical endpoint DMA has stopped. */
#include "wcn7861.h"
#include "hotspot.h"
#include "stage7-util.h"
#include "stage7-wire.h"
#include <bcrypt.h>
VOID WcnStage7Cleanup(PWCN_DEVICE D)
{
    WcnHotspotDmaCleanup(D);PWCN_JOIN Links[]={&D->Join,&D->Secondary,&D->P2pJoin};
    for(ULONG K=0;K<ARRAYSIZE(Links);K++){PWCN_JOIN J=Links[K];WcnPmfClear(D,J);if(J->PmfAlgorithm)BCryptCloseAlgorithmProvider(J->PmfAlgorithm,0);
        if(J->MgmtDma.Buffer)WdfObjectDelete(J->MgmtDma.Buffer);if(J->ReoQueue.Buffer)WdfObjectDelete(J->ReoQueue.Buffer);
        RtlSecureZeroMemory(J,sizeof(*J));}
    if(D->Runtime.TxDma.Buffer){RtlSecureZeroMemory(D->Runtime.TxDma.Va,D->Runtime.TxDma.Length);WdfObjectDelete(D->Runtime.TxDma.Buffer);}RtlSecureZeroMemory(&D->Runtime,sizeof(D->Runtime));
}

/* Shared serialization only; callers retain retry and ownership policy. */
NTSTATUS WcnVdevParam(PWCN_DEVICE D,ULONG Vdev,ULONG Param,ULONG Value)
{ULONG B[]={Jtlv(95,12),Vdev,Param,Value};return WcnWmiSend(D,0x5008,B,sizeof(B));}
NTSTATUS WcnPeerParam(PWCN_DEVICE D,ULONG Vdev,const UCHAR Peer[6],ULONG Param,ULONG Value)
{struct wmi_peer_set_param_cmd B={0};JHDR(&B,100);B.vdev_id=Vdev;RtlCopyMemory(B.peer_macaddr.addr,Peer,6);B.param_id=Param;B.param_value=Value;return WcnWmiSend(D,0x6004,&B,sizeof(B));}
NTSTATUS WcnStationPower(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Enable)
{ULONG B[]={Jtlv(108,8),J->Vdev,Enable?1u:0u};return WcnWmiSend(D,0x9001,B,sizeof(B));}
