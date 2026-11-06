/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
/* Hardware has already authenticated this MPDU and validated its peer/key/TID.
 * A-MSDU continuation shares a PN, but cannot open its own replay window.
 * Bit 1 exists only between an accepted first and its last REO MSDU. */
static __inline BOOLEAN WcnRxPnAccept(ULONG64 Pn,ULONG64 *Last,UCHAR *Valid,ULONG Msdu,BOOLEAN Continuation)
{
    if(Continuation&&!(Msdu&1)){
        if(!(*Valid&2)||!Pn||Pn!=*Last)return FALSE;
        if(Msdu&2)*Valid&=1;return TRUE;
    }
    if(!Pn||Pn<=*Last){*Valid&=1;return FALSE;}
    *Last=Pn;*Valid=(UCHAR)(1|(Continuation&&!(Msdu&2)?2:0));return TRUE;
}
