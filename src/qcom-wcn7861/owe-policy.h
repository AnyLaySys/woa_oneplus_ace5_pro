#pragma once
/* RFC8110: Windows owns ephemeral secrets, public-point validation, PMK and
 * four-way handshake. Only bounded, exact OS DH IEs are transported here. */
static __inline ULONG WcnOweDhGroup(const UCHAR *P,ULONG N)
{
    ULONG G,L;if(!P||N<5||P[0]!=255||P[1]!=N-2||P[2]!=32)return 0;
    G=P[3]|((ULONG)P[4]<<8);L=G==19?32:G==20?48:G==21?66:0;
    return L&&N==L+5?G:0;
}
static __inline BOOLEAN WcnOweResponse(const UCHAR *P,ULONG N,ULONG Group,const UCHAR *Pmkid)
{
    const UCHAR *Rsn=NULL,*Dh=NULL;ULONG RsnLength=0,DhLength=0,I,L,K;BOOLEAN Pmf=TRUE,Cached=FALSE;
    for(I=0;I<N;I+=L+2){if(N-I<2||(L=P[I+1])>N-I-2)return FALSE;
        if(P[I]==48){if(Rsn)return FALSE;Rsn=P+I+2;RsnLength=L;}
        if(P[I]==255&&L&&P[I+2]==32){if(Dh)return FALSE;Dh=P+I;DhLength=L+2;}}
    if(!Rsn||!WcnRsnAccept(Rsn,RsnLength,WCN_AUTH_OWE,&Pmf))return FALSE;
    /* WcnRsnAccept already checked these nested lengths. Cache reuse requires
     * the exact OS-requested PMKID, not merely any PMKID in the response. */
    if(Pmkid){const UCHAR *Q=Rsn+6;ULONG Left=RsnLength-6;K=Q[0]|((ULONG)Q[1]<<8);Q+=2+4*K;Left-=2+4*K;
        K=Q[0]|((ULONG)Q[1]<<8);Q+=2+4*K;Left-=2+4*K;
        if(Left>=4){Q+=2;K=Q[0]|((ULONG)Q[1]<<8);Q+=2;
            for(I=0;I<K;I++)if(RtlCompareMemory(Q+I*16,Pmkid,16)==16)Cached=TRUE;}}
    return Cached||(Group&&Dh&&WcnOweDhGroup(Dh,DhLength)==Group);
}
