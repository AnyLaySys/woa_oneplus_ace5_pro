#pragma once
/* Windows owns WPS PBC/PIN, overlap detection, credentials and EAP-WSC.
 * Only explicit OS request IEs permit a temporary EAPOL-only association. */
static __inline BOOLEAN WcnExtraIesValid(const UCHAR *P,ULONG N)
{
    ULONG I,L;if(N>768||(N&&!P))return FALSE;
    for(I=0;I<N;I+=L+2){if(N-I<2||(L=P[I+1])>N-I-2)return FALSE;
        if((P[I]!=221&&P[I]!=127)||(P[I]==221&&L<4))return FALSE;}
    return TRUE;
}
static __inline ULONG WcnExtraIesMerge(const UCHAR *A,ULONG Na,const UCHAR *B,ULONG Nb,UCHAR *Out,ULONG Capacity)
{
    if(!WcnExtraIesValid(A,Na)||!WcnExtraIesValid(B,Nb)||Na>Capacity||Nb>Capacity-Na||Na+Nb>768)return MAXULONG;
    if(Na)RtlCopyMemory(Out,A,Na);if(Nb)RtlCopyMemory(Out+Na,B,Nb);return Na+Nb;
}
/* WPS attributes are network-order TLVs, possibly split across vendor IEs.
 * Return -1 for malformed WPS, 0 for no WPS, 1 for a valid attribute stream. */
static __inline LONG WcnWpsInfo(const UCHAR *P,ULONG N,UCHAR *RequestType)
{
    UCHAR Bytes[768];ULONG I,L,Used=0;BOOLEAN Seen=FALSE,Version=FALSE,Request=FALSE;
    *RequestType=255;if(N>2048||(N&&!P))return -1;
    for(I=0;I<N;I+=L+2){if(N-I<2||(L=P[I+1])>N-I-2)return -1;
        if(P[I]==221&&L>=4&&RtlCompareMemory(P+I+2,"\x00\x50\xf2\x04",4)==4){
            Seen=TRUE;if(L-4>sizeof(Bytes)-Used)return -1;RtlCopyMemory(Bytes+Used,P+I+6,L-4);Used+=L-4;}}
    if(!Seen)return 0;
    for(I=0;I<Used;I+=L+4){ULONG Id;if(Used-I<4||(L=((ULONG)Bytes[I+2]<<8)|Bytes[I+3])>Used-I-4)return -1;
        Id=((ULONG)Bytes[I]<<8)|Bytes[I+1];if(Id==0x104a){if(Version||L!=1||(Bytes[I+4]!=0x10&&Bytes[I+4]!=0x20))return -1;Version=TRUE;}
        if(Id==0x103a){if(Request||L!=1||Bytes[I+4]>3)return -1;Request=TRUE;*RequestType=Bytes[I+4];}}
    return Version?1:-1;
}
