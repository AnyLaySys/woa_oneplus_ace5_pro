#pragma once
static ULONG QchLe32(const UCHAR *P){return (ULONG)P[0]|((ULONG)P[1]<<8)|((ULONG)P[2]<<16)|((ULONG)P[3]<<24);}
static ULONGLONG QchLe64(const UCHAR *P){return QchLe32(P)|((ULONGLONG)QchLe32(P+4)<<32);}
static BOOLEAN QchUfcsCaps(const UCHAR *Data,ULONG Length,QCH_UFCS_CAPS *Caps)
{
    QCH_UFCS_CAPS C={0};
    if(Length!=152 || QchLe32(Data)!=32778 || QchLe32(Data+4)!=1 || QchLe32(Data+8)!=0x10005 || QchLe32(Data+148)!=0) return FALSE;
    ULONG Bytes=QchLe32(Data+144);
    if(Bytes==0 || Bytes>128 || Bytes%8) return FALSE;
    BOOLEAN End=FALSE;
    for(ULONG I=0;I<Bytes/8;I++) {
        ULONGLONG P=QchLe64(Data+16+I*8);
        if(!P){End=TRUE;continue;}
        if(End || I>=QCH_UFCS_CAP_COUNT) return FALSE;
        ULONG Index=(ULONG)(P>>60);
        ULONG VMax=(ULONG)((P>>40)&0xffff)*10,VMin=(ULONG)((P>>24)&0xffff)*10;
        ULONG IMax=(ULONG)((P>>8)&0xffff)*10,IMin=(ULONG)(P&0xff)*10;
        if(Index!=I+1 || !VMin || VMin>VMax || IMin>IMax) return FALSE;
        C.Pdo[I]=P;C.Count++;

    }
    *Caps=C;return TRUE;
}
