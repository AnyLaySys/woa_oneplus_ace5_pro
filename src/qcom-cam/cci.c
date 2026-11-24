#include "cam.h"

static VOID Clear(CAM *c,ULONG bits) {
    CamWrite(c->Base[CamCci],0xc08,bits);
    CamWrite(c->Base[CamCci],0xc00,1);
}

static NTSTATUS Recover(CAM *c) {
    PUCHAR b=c->Base[CamCci];
    CamWrite(b,0x34,2);
    NTSTATUS status=CamWait(b,0xc0c,0x04000000,0x04000000,20000);
    if(!NT_SUCCESS(status)){c->Faulted=TRUE;return status;}
    Clear(c,0x04000000);
    CamWrite(b,4,0x3f001);
    status=CamWait(b,0xc0c,0x01000000,0x01000000,20000);
    if(!NT_SUCCESS(status))c->Faulted=TRUE;
    Clear(c,0x61fff000);CamWrite(b,0x34,0);return status;
}

static NTSTATUS Execute(CAM *c,const ULONG *words,ULONG count,ULONG done) {
    PUCHAR b=c->Base[CamCci];
    if(c->Faulted)return STATUS_DEVICE_HARDWARE_ERROR;
    if(CamRead(b,0x504))return STATUS_DEVICE_BUSY;
    Clear(c,0x60fff000);
    for(ULONG i=0;i<count;i++)CamWrite(b,0x510,words[i]);
    if(CamRead(b,0x504)!=count){Recover(c);return STATUS_IO_DEVICE_ERROR;}
    CamWrite(b,0x500,count);CamWrite(b,8,4);
    ULONGLONG deadline=KeQueryInterruptTime()+200000;
    do {
        ULONG irq=CamRead(b,0xc0c);c->Info.CciIrq=irq;
        if(irq&0x60ee6000){Recover(c);return STATUS_IO_DEVICE_ERROR;}
        if(irq&done){Clear(c,irq&0x60fff000);return STATUS_SUCCESS;}
        CamDelay(50);
    } while(KeQueryInterruptTime()<deadline);
    Recover(c);return STATUS_IO_TIMEOUT;
}

NTSTATUS CamCciInit(CAM *c) {
    PUCHAR b=c->Base[CamCci];
    c->Info.CciVersion=CamRead(b,0);
    if(c->Info.CciVersion<0x10000000 || c->Info.CciVersion==MAXULONG)return STATUS_REVISION_MISMATCH;
    if(CamRead(b,0x504)||CamRead(b,0x604))return STATUS_DEVICE_BUSY;
    Clear(c,0x01000000);
    CamWrite(b,4,0x3f001);
    NTSTATUS status=CamWait(b,0xc0c,0x01000000,0x01000000,20000);
    if(!NT_SUCCESS(status))return status;
    Clear(c,0x61fff000);
    CamWrite(b,0x200,(0x26u<<16)|0x38u);
    CamWrite(b,0x204,(0x28u<<16)|0x28u);
    CamWrite(b,0x208,(0x16u<<16)|0x23u);
    CamWrite(b,0x20c,0x3e);
    CamWrite(b,0x210,0x63);
    return STATUS_SUCCESS;
}

static NTSTATUS Read(CAM *c,UCHAR slave,USHORT address,ULONG addressBytes,PUCHAR buffer,ULONG bytes) {
    if(!buffer||!bytes||bytes>32||slave>0x7f)return STATUS_INVALID_PARAMETER;
    PUCHAR b=c->Base[CamCci];
    ULONG level=CamRead(b,0x21c);
    if(level>16)return STATUS_IO_DEVICE_ERROR;
    while(level--)CamRead(b,0x218);
    ULONG words[]={6,1|((ULONG)slave<<4)|(3u<<16),addressBytes==2?0x2b|((ULONG)(address>>8)<<8)|((ULONG)(address&255)<<16):0x1b|((ULONG)address<<8),10|(bytes<<4),7};
    NTSTATUS status=Execute(c,words,RTL_NUMBER_OF(words),0x1000);
    if(!NT_SUCCESS(status))return status;
    level=CamRead(b,0x21c);
    if(level!=(bytes/4)+1)return STATUS_DATA_ERROR;
    ULONG offset=0;
    for(ULONG i=0;i<level;i++) {
        ULONG value=CamRead(b,0x218);
        for(ULONG j=0;j<4;j++) {
            if(!i&&!j)continue;
            if(offset<bytes)buffer[offset++]=(UCHAR)(value>>(8*j));
        }
    }
    return offset==bytes?STATUS_SUCCESS:STATUS_DATA_ERROR;
}

static NTSTATUS Write(CAM *c,UCHAR slave,USHORT address,ULONG addressBytes,const UCHAR *buffer,ULONG bytes) {
    if(!buffer||!bytes||bytes>8||slave>0x7f)return STATUS_INVALID_PARAMETER;
    UCHAR payload[12]={0};ULONG words[8]={0};
    words[0]=6;words[1]=1|((ULONG)slave<<4)|(3u<<16);
    payload[0]=(UCHAR)(9|((bytes+addressBytes)<<4));
    if(addressBytes==2)payload[1]=(UCHAR)(address>>8);
    payload[addressBytes]=(UCHAR)address;
    RtlCopyMemory(payload+addressBytes+1,buffer,bytes);
    ULONG count=(bytes+addressBytes+4)/4;
    RtlCopyMemory(words+2,payload,count*4);
    words[2+count]=7;words[3+count]=0x308;
    return Execute(c,words,count+4,0x10000);
}

NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes) {return Read(c,slave,address,2,buffer,bytes);}
NTSTATUS CamCciWrite(CAM *c,UCHAR slave,USHORT address,const UCHAR *buffer,ULONG bytes) {return Write(c,slave,address,2,buffer,bytes);}
NTSTATUS CamCciReadByte(CAM *c,UCHAR slave,UCHAR address,PUCHAR buffer,ULONG bytes) {return Read(c,slave,address,1,buffer,bytes);}
NTSTATUS CamCciWriteByte(CAM *c,UCHAR slave,UCHAR address,const UCHAR *buffer,ULONG bytes) {return Write(c,slave,address,1,buffer,bytes);}
