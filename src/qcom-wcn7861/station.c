/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Lifetime of the firmware station DMA. Called only after endpoint DMA stops. */
#include "wcn7861.h"
#include <bcrypt.h>
VOID WcnStage7Cleanup(PWCN_DEVICE Dev)
{
    PWCN_JOIN J=&Dev->Join;ULONG I;
    WcnPmfClear(Dev);if(J->PmfAlgorithm)BCryptCloseAlgorithmProvider(J->PmfAlgorithm,0);
    if(J->MgmtDma.Buffer)WdfObjectDelete(J->MgmtDma.Buffer);
    if(J->TxDma.Buffer){RtlSecureZeroMemory(J->TxDma.Va,J->TxDma.Length);WdfObjectDelete(J->TxDma.Buffer);}
    if(J->ReoQueue.Buffer)WdfObjectDelete(J->ReoQueue.Buffer);
    for(I=0;I<J->RetiredReoCount;I++)if(J->RetiredReo[I].Buffer)WdfObjectDelete(J->RetiredReo[I].Buffer);
    RtlSecureZeroMemory(J,sizeof(*J));
}
