#pragma once
/* CN STA primary channels. DFS remains passive-discovery-only and bonded
 * widths intersect every constituent channel and firmware rule. */
static __inline BOOLEAN WcnIs2g(ULONG F){return F>=2412&&F<=2472&&(F-2412)%5==0;}
static __inline BOOLEAN WcnConnectFrequency(ULONG F)
{return WcnIs2g(F)||(F>=5180&&F<=5320&&(F-5180)%20==0)||(F>=5745&&F<=5825&&(F-5745)%20==0);}
static __inline ULONG WcnChannelFrequency(ULONG C){return C>=1&&C<=13?2407+C*5:5000+C*5;}
static __inline ULONG WcnFrequencyChannel(ULONG F){return (F-(WcnIs2g(F)?2407:5000))/5;}
/* DBS supplies two radios, not a third independent channel. Keep both STAs
 * only when the hotspot shares one uplink's primary channel (SCC + DBS). */
static __inline BOOLEAN WcnTripleChannel(ULONG Primary,ULONG Secondary,ULONG Hotspot)
{return !Hotspot||(Primary&&(!Secondary||WcnIs2g(Primary)!=WcnIs2g(Secondary))&&
 (Hotspot==Primary||(WcnIs2g(Hotspot)!=WcnIs2g(Primary)&&(!Secondary||Hotspot==Secondary))));}
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

/* Off-channel scans are not evidence of an unreachable peer. This clock is
 * separate from real beacon/RX/ACK timestamps and never changes reported RSSI. */
static __inline BOOLEAN WcnPeerExpired(LONGLONG Now,LONGLONG Seen,ULONG Limit,BOOLEAN Scanning,LONGLONG *HomeAt)
{
    if(Scanning){*HomeAt=Now;return FALSE;}
    if(*HomeAt>Seen)Seen=*HomeAt;
    return Limit&&Seen&&Now-Seen>=Limit;
}

/* Reconstitute only the header around a real OS-supplied beacon/probe body.
 * This is connection input, never a scan indication or a fresh RSSI sample. */
static __inline ULONG WcnBssFrame(const UCHAR Mac[6],const UCHAR *Body,ULONG N,ULONG Frequency,BOOLEAN Probe,UCHAR *Out,ULONG Capacity)
{
    UCHAR Any=0;ULONG I;
    if(!Body||!Out||N<12||N>2024||Capacity<N+24||(Mac[0]&1)||!WcnConnectFrequency(Frequency))return 0;
    for(I=0;I<6;I++)Any|=Mac[I];
    if(!Any)return 0;
    RtlZeroMemory(Out,24);Out[0]=Probe?0x50:0x80;
    for(I=0;I<6;I++)Out[4+I]=255;
    RtlCopyMemory(Out+10,Mac,6);RtlCopyMemory(Out+16,Mac,6);RtlCopyMemory(Out+24,Body,N);
    return WcnPrimaryFrequency(Out,N+24,Frequency)==Frequency?N+24:0;
}

/* SSIDs are 0..32 raw bytes, not C strings. A hidden-name exception is valid
 * only for an OS-selected BSSID with HiddenNetwork explicitly set. */
static __inline const UCHAR *WcnBssSsid(const UCHAR *F,ULONG N,ULONG *Length)
{
    const UCHAR *S=NULL;ULONG I,L;*Length=0;if(N<36)return NULL;
    for(I=36;I<N;I+=L+2){if(N-I<2||(L=F[I+1])>N-I-2)return NULL;
        if(F[I]==0){if(S||L>32)return NULL;S=F+I+2;*Length=L;}}
    return S;
}
static __inline BOOLEAN WcnSsidHidden(const UCHAR *S,ULONG N)
{ULONG I;if(!S||N>32)return FALSE;for(I=0;I<N;I++)if(S[I])return FALSE;return TRUE;}
static __inline BOOLEAN WcnBssSsidMatches(const UCHAR *F,ULONG N,const UCHAR *Name,ULONG Length)
{ULONG L,I;const UCHAR *S=WcnBssSsid(F,N,&L);if(!S||!Name||!Length||Length>32||L!=Length)return FALSE;
 for(I=0;I<L;I++)if(S[I]!=Name[I])return FALSE;return TRUE;}
/* Never initiate probes on disabled, NO_IR, DFS or unsupported CN channels. */
static __inline BOOLEAN WcnProbeAllowed(ULONG Frequency,ULONG Flags)
{return WcnConnectFrequency(Frequency)&&!(Flags&(1u|2u|8u|64u|(1u<<11)))&&!(Frequency>=5260&&Frequency<=5320);}
