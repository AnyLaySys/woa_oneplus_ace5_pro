#pragma once
/* Native802.11 is the Windows-facing link type (INF MediaType16).
 * Firmware TCL/RXDMA remain Ethernet. Convert ONLY at the system boundary;
 * never reinterpret a copied Ethernet M1 as an 802.11 header. No key bytes. */
static ULONG S8NativeRx(const UCHAR *F,ULONG N,const UCHAR Bssid[6],BOOLEAN Protected,USHORT Sequence,UCHAR *Out,ULONG Capacity)
{
    if(N<14||N>1514||Capacity<N+18)return 0;
    RtlZeroMemory(Out,32);Out[0]=0x08;Out[1]=Protected?0x42:0x02;
    RtlCopyMemory(Out+4,F,6);RtlCopyMemory(Out+10,Bssid,6);RtlCopyMemory(Out+16,F+6,6);
    Out[22]=(UCHAR)(Sequence<<4);Out[23]=(UCHAR)(Sequence>>4);
    RtlCopyMemory(Out+24,"\xaa\xaa\x03\x00\x00\x00",6);
    RtlCopyMemory(Out+30,F+12,N-12);return N+18;
}
static ULONG S8NativeTx(UCHAR *F,ULONG N,const UCHAR Bssid[6],const UCHAR Sta[6])
{
    ULONG Fc,H=24,Len;UCHAR Dest[6];
    if(N<32||N>1532)return 0;Fc=F[0]|((ULONG)F[1]<<8);
    /* STA ToDS ordinary or QoS data only; no WDS, null, fragmentation,
     * A-MSDU, HT-control or multicast transmitter. HW supplies sequence/PN. */
    if((Fc&0x030f)!=0x0108||(Fc&0x8400)||(F[0]&0x70))return 0;
    if(F[0]&0x80){H=26;if(N<34||(F[24]&0x80))return 0;}
    if((F[22]&15)||RtlCompareMemory(F+4,Bssid,6)!=6||RtlCompareMemory(F+10,Sta,6)!=6)return 0;
    if(N<H+8||RtlCompareMemory(F+H,"\xaa\xaa\x03\x00\x00\x00",6)!=6)return 0;
    Len=N-H-8+14;if(Len>1514)return 0;
    RtlCopyMemory(Dest,F+16,6);RtlMoveMemory(F+12,F+H+6,N-H-6);
    RtlCopyMemory(F,Dest,6);RtlCopyMemory(F+6,Sta,6);return Len;
}
