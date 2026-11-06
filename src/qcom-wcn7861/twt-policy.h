/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
/* A few background ARP/DNS/health packets must not defeat radio power saving.
 * Burst traffic exits immediately; quiet time is measured per physical link. */
static __inline BOOLEAN WcnTwtTrafficBusy(ULONG Count,ULONG *Seen,LONGLONG Now,LONGLONG *Window,LONGLONG *Busy)
{
    if(!*Window){*Seen=Count;*Window=*Busy=Now;}
    if((ULONG)(Count-*Seen)>=32){*Seen=Count;*Window=*Busy=Now;}
    else if(Now-*Window>=1000){*Seen=Count;*Window=Now;}
    return Now-*Busy<3000;
}
