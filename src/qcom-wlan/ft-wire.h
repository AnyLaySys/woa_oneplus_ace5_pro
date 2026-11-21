/* SPDX-License-Identifier: BSD-3-Clause-Clear */
#pragma once
/* Full IE, never a body-only blob; reject duplicate security elements. */
static __inline const UCHAR *WcnFtIe(const UCHAR *P,ULONG N,UCHAR Id,ULONG *Length)
{
    const UCHAR *Found=NULL;ULONG L;*Length=0;
    while(N){if(N<2||(L=P[1]+2u)>N)return NULL;if(P[0]==Id){if(Found)return NULL;Found=P;*Length=L;}P+=L;N-=L;}return Found;
}
static __inline BOOLEAN WcnFtElement(const UCHAR *P,ULONG N,UCHAR Id,ULONG Min,ULONG Max)
{return P&&N>=Min&&N<=Max&&P[0]==Id&&(ULONG)P[1]+2==N;}
static __inline BOOLEAN WcnFtNonce(const UCHAR *Fte)
{ULONG I;UCHAR Any=0;for(I=52;I<84;I++)Any|=Fte[I];return Any!=0;}
