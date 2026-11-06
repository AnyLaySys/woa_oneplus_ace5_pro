/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
/* Worker-only firmware hooks; queue callbacks only access their own S8 state. */
NTSTATUS WcnHotspotTx(PWCN_DEVICE D,const UCHAR *F,ULONG N,BOOLEAN Eapol);
VOID WcnHotspotTxFailed(PWCN_DEVICE D);
BOOLEAN WcnHotspotRx(PWCN_DEVICE D,const UCHAR *Buffer,ULONG Length,ULONG Pad,BOOLEAN Error,const ULONG *Desc);
VOID WcnHotspotWmi(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N);
VOID WcnHotspotHtt(PWCN_DEVICE D,const UCHAR *P,ULONG N);
VOID WcnHotspotDmaCleanup(PWCN_DEVICE D);

NTSTATUS WcnHotspotActionSend(PWCN_DEVICE D,const UCHAR *Body,ULONG N);
VOID WcnHotspotFail(PWCN_DEVICE D,PCSTR Reason);

ULONG WcnHotspotRate(PWCN_DEVICE D);

BOOLEAN WcnHotspotActive(PWCN_DEVICE D);
ULONG WcnHotspotFrequency(PWCN_DEVICE D);
BOOLEAN WcnHotspotTriple(PWCN_DEVICE D,const WCN_JOIN_AP *Primary,const WCN_JOIN_AP *Secondary);
