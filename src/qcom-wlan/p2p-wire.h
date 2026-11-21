/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * Bounded Wi-Fi Direct/WPS IE codec. No authentication decisions or secrets. */
#pragma once
static __inline BOOLEAN P2pIesValid(const UCHAR *P,ULONG N)
{if(N&&!P)return FALSE;while(N){ULONG L;if(N<2||(L=P[1]+2)>N)return FALSE;P+=L;N-=L;}return TRUE;}
static __inline BOOLEAN P2pVendor(const UCHAR *P,ULONG N,const UCHAR Oui[4])
{ULONG I;if(!P2pIesValid(P,N))return FALSE;for(I=0;I+2<=N;I+=P[I+1]+2)if(P[I+1]>=4&&P[I]==221&&RtlCompareMemory(P+I+2,Oui,4)==4)return TRUE;return FALSE;}
static __inline BOOLEAN P2pAttr(const UCHAR *Ies,ULONG N,ULONG Id,UCHAR *Out,ULONG Capacity,ULONG *Length,BOOLEAN Prefix=FALSE)
{
    UCHAR Bytes[2048];ULONG I,L,Used=0,Seen=0;BOOLEAN Found=Id>255;static const UCHAR Oui[]={0x50,0x6f,0x9a,9};*Length=0;
    if(!P2pIesValid(Ies,N)||N>sizeof(Bytes))return FALSE;
    for(I=0;I<N;I+=Ies[I+1]+2)if(Ies[I]==221&&Ies[I+1]>=4&&RtlCompareMemory(Ies+I+2,Oui,4)==4){L=Ies[I+1]-4;if(L>sizeof(Bytes)-Used)return FALSE;RtlCopyMemory(Bytes+Used,Ies+I+6,L);Used+=L;}
    for(I=0;I<Used;I+=L+3){if(Used-I<3||(L=Bytes[I+1]|((ULONG)Bytes[I+2]<<8))>Used-I-3)return FALSE;
        if(Bytes[I]<=18){ULONG Bit=1u<<Bytes[I];if(Seen&Bit)return FALSE;Seen|=Bit;}
        if(Bytes[I]==Id){if(Found||(L>Capacity&&!Prefix))return FALSE;Found=TRUE;*Length=L;ULONG Copy=L<Capacity?L:Capacity;if(Copy)RtlCopyMemory(Out,Bytes+I+3,Copy);}}
    return Found;
}
static __inline VOID P2pBe16(UCHAR *P,ULONG V){P[0]=(UCHAR)(V>>8);P[1]=(UCHAR)V;}
struct P2pWriter {
    UCHAR *P;ULONG Capacity,N,Start;bool Good;
    P2pWriter(UCHAR *Out,ULONG C):P(Out),Capacity(C),N(0),Start(MAXULONG),Good(true){}
    void Raw(const void *Data,ULONG L){if(!Good||L>Capacity-N){Good=false;return;}if(L)RtlCopyMemory(P+N,Data,L);N+=L;}
    void Byte(ULONG V){UCHAR B=(UCHAR)V;Raw(&B,1);}
    void End(){if(Start!=MAXULONG&&Good)P[Start+1]=(UCHAR)(N-Start-2);Start=MAXULONG;}
    void Begin(const UCHAR Oui[4]){End();Start=N;Byte(221);Byte(0);Raw(Oui,4);}
    void Attr(UCHAR Id,const void *Data,ULONG L){static const UCHAR Oui[]={0x50,0x6f,0x9a,9};
        if(L>248){Good=false;return;}if(Start==MAXULONG||N-Start-2+3+L>255)Begin(Oui);Byte(Id);Byte(L);Byte(L>>8);Raw(Data,L);}
    void Wps(ULONG Id,const void *Data,ULONG L){if(L>251||Start==MAXULONG||N-Start-2+4+L>255){Good=false;return;}UCHAR H[4];P2pBe16(H,Id);P2pBe16(H+2,L);Raw(H,4);Raw(Data,L);}
    ULONG Finish(){End();return Good?N:0;}
};
static __inline UCHAR P2pOperatingClass(ULONG F)
{return F<2500?81:F<=5240?115:F<=5320?118:F<=5805?124:125;}
static __inline VOID P2pChannelBytes(ULONG F,UCHAR P[5])
{P[0]='C';P[1]='N';P[2]=4;P[3]=P2pOperatingClass(F);P[4]=(UCHAR)WcnFrequencyChannel(F);}
static __inline UCHAR P2pSubtype(ULONG Type)
{return Type>=1&&Type<=5?(UCHAR)(Type-1):Type==6?7:Type==7?8:255;}
static __inline ULONG P2pFrameType(UCHAR Subtype)
{return Subtype<=4?Subtype+1:Subtype==7?6:Subtype==8?7:0;}

static __inline BOOLEAN P2pAttributesValid(const UCHAR *P,ULONG N)
{ULONG L;return P2pAttr(P,N,256,NULL,0,&L);}
