#pragma once
/* CN station policy: 20MHz 2.4GHz channels1-13 and existing non-DFS5GHz.
 * Firmware regulatory rules are an additional mandatory intersection. */
static __inline BOOLEAN WcnIs2g(ULONG F){return F>=2412&&F<=2472&&(F-2412)%5==0;}
static __inline BOOLEAN WcnConnectFrequency(ULONG F)
{return WcnIs2g(F)||(F>=5180&&F<=5240&&(F-5180)%20==0)||(F>=5745&&F<=5825&&(F-5745)%20==0);}
static __inline ULONG WcnChannelFrequency(ULONG C){return C>=1&&C<=13?2407+C*5:5000+C*5;}
static __inline ULONG WcnFrequencyChannel(ULONG F){return (F-(WcnIs2g(F)?2407:5000))/5;}
/* HT-operation overrides scan RX channel; DS and HT must agree. Legacy APs
 * without either IE use the actual receive channel, never invent channel36. */
static __inline ULONG WcnPrimaryFrequency(const UCHAR *F,ULONG N,ULONG Rx)
{
    ULONG I,K,Ds=0,Ht=0;
    if(N<36)return 0;
    for(I=36;I<N;I+=K+2){
        if(N-I<2)return 0;K=F[I+1];if(K>N-I-2)return 0;
        if(F[I]==3){if(K!=1||Ds)return 0;Ds=F[I+2];}
        if(F[I]==61){if(K!=22||Ht)return 0;Ht=F[I+2];}
    }
    if(Ds&&Ht&&Ds!=Ht)return 0;
    return Ht||Ds?WcnChannelFrequency(Ht?Ht:Ds):Rx;
}
