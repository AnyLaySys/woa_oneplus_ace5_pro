/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
static __inline BOOLEAN WcnRxPnAccept(ULONG64 Pn,ULONG64 *Last,ULONG64 Floor,BOOLEAN Checked)
{
    if(Pn<=Floor||(!Checked&&Pn<=*Last))return FALSE;
    if(Pn>*Last)*Last=Pn;
    return TRUE;
}
