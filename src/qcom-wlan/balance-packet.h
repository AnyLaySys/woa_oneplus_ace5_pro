#pragma once

typedef struct _WCN_BALANCE_PACKET {
    UCHAR *Ip,*Source,*Destination,*Payload,*Checksum;
    ULONG Length,Offset,Id;USHORT SourcePort,DestinationPort;
    UCHAR Family,Protocol;BOOLEAN Fragment,First;
} WCN_BALANCE_PACKET;

static __inline USHORT Bget16(const UCHAR *P){return (USHORT)((P[0]<<8)|P[1]);}
static __inline ULONG Bget32(const UCHAR *P){return ((ULONG)Bget16(P)<<16)|Bget16(P+2);}
static __inline VOID Bput16(UCHAR *P,USHORT V){P[0]=(UCHAR)(V>>8);P[1]=(UCHAR)V;}
static __inline VOID Bsum(UCHAR *Checksum,const UCHAR *Before,const UCHAR *After,ULONG N)
{
    ULONG S=(~Bget16(Checksum))&0xffff;
    for(ULONG I=0;I<N;I+=2)S+=((~Bget16(Before+I))&0xffff)+Bget16(After+I);
    while(S>>16)S=(S&0xffff)+(S>>16);Bput16(Checksum,(USHORT)~S);
}
static __inline BOOLEAN Bpacket(UCHAR *Ip,ULONG N,BOOLEAN Complete,WCN_BALANCE_PACKET *P)
{
    RtlZeroMemory(P,sizeof(*P));P->Ip=Ip;P->First=TRUE;if(N<20)return FALSE;
    ULONG H,L,End;UCHAR Next;
    if((Ip[0]>>4)==4){
        H=(Ip[0]&15)*4;L=Bget16(Ip+2);if(H<20||H>N||L<H||(Complete&&L>N))return FALSE;
        End=L<N?L:N;P->Family=4;P->Source=Ip+12;P->Destination=Ip+16;Next=Ip[9];
        USHORT F=Bget16(Ip+6);P->Offset=(F&0x1fff)*8u;P->Fragment=(F&0x3fff)!=0;P->First=P->Offset==0;P->Id=Bget16(Ip+4);
    }else if((Ip[0]>>4)==6){
        if(N<40||!Bget16(Ip+4))return FALSE;H=40;L=40+Bget16(Ip+4);if(Complete&&L>N)return FALSE;
        End=L<N?L:N;P->Family=6;P->Source=Ip+8;P->Destination=Ip+24;Next=Ip[6];
        for(ULONG I=0;I<8&&(Next==0||Next==60||Next==44);I++){
            if(End-H<8)return FALSE;ULONG K=Next==44?8u:((ULONG)Ip[H+1]+1)*8;
            if(K>End-H)return FALSE;
            if(Next==44){if(P->Fragment)return FALSE;USHORT F=Bget16(Ip+H+2);P->Offset=F&0xfff8;P->Fragment=(F&0xfff9)!=0;P->First=P->Offset==0;P->Id=Bget32(Ip+H+4);}
            Next=Ip[H];H+=K;if(!P->First)break;
        }
    }else return FALSE;
    P->Protocol=Next;P->Payload=Ip+H;P->Length=End-H;
    if(Next==6||Next==17){
        if(P->First){if(P->Length<4)return FALSE;P->SourcePort=Bget16(P->Payload);P->DestinationPort=Bget16(P->Payload+2);}
        ULONG C=Next==6?16u:6u;if(P->Offset<=C&&C-P->Offset+2<=P->Length)P->Checksum=P->Payload+C-P->Offset;
        if(Complete&&!P->Fragment&&P->Length<(Next==6?20u:8u))return FALSE;
    }else if(Next==1||Next==58){if(P->Fragment||P->Length<8)return FALSE;P->Checksum=P->Payload+2;}
    else return FALSE;
    return TRUE;
}
static __inline VOID Baddress(WCN_BALANCE_PACKET *P,BOOLEAN Source,const UCHAR *Address)
{
    ULONG N=P->Family==4?4u:16u;UCHAR *Before=Source?P->Source:P->Destination;
    if(P->Checksum&&(P->Protocol==6||P->Protocol==17||P->Protocol==58)){
        BOOLEAN Disabled=P->Family==4&&P->Protocol==17&&!Bget16(P->Checksum);
        if(!Disabled){Bsum(P->Checksum,Before,Address,N);if(P->Protocol==17&&!Bget16(P->Checksum))Bput16(P->Checksum,0xffff);}
    }
    if(P->Family==4)Bsum(P->Ip+10,Before,Address,N);RtlCopyMemory(Before,Address,N);
}
static __inline VOID Bquote(WCN_BALANCE_PACKET *Outer,WCN_BALANCE_PACKET *Inner,BOOLEAN Source,const UCHAR *Address)
{
    UCHAR Before[16],IpSum[2],TransportSum[2];ULONG N=Inner->Family==4?4u:16u;
    UCHAR *Field=Source?Inner->Source:Inner->Destination;RtlCopyMemory(Before,Field,N);
    if(Inner->Family==4)RtlCopyMemory(IpSum,Inner->Ip+10,2);
    if(Inner->Checksum)RtlCopyMemory(TransportSum,Inner->Checksum,2);
    Baddress(Inner,Source,Address);Bsum(Outer->Checksum,Before,Field,N);
    if(Inner->Family==4)Bsum(Outer->Checksum,IpSum,Inner->Ip+10,2);
    if(Inner->Checksum)Bsum(Outer->Checksum,TransportSum,Inner->Checksum,2);
}
static __inline VOID Bport(WCN_BALANCE_PACKET *P,BOOLEAN Source,USHORT Before,USHORT After)
{
    UCHAR A[2],B[2];Bput16(A,Before);Bput16(B,After);
    if(P->Checksum&&!(P->Family==4&&P->Protocol==17&&!Bget16(P->Checksum))){Bsum(P->Checksum,A,B,2);if(P->Protocol==17&&!Bget16(P->Checksum))Bput16(P->Checksum,0xffff);}
    if(P->First)Bput16(P->Payload+(Source?0:2),After);
}
static __inline VOID BquotePort(WCN_BALANCE_PACKET *Outer,WCN_BALANCE_PACKET *Inner,BOOLEAN Source,USHORT Before,USHORT After)
{
    UCHAR A[2],B[2],C[2];Bput16(A,Before);Bput16(B,After);if(Inner->Checksum)RtlCopyMemory(C,Inner->Checksum,2);
    Bport(Inner,Source,Before,After);Bsum(Outer->Checksum,A,B,2);if(Inner->Checksum)Bsum(Outer->Checksum,C,Inner->Checksum,2);
}
