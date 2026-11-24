#include "cam.h"

static VOID Clear(CAM *c,ULONG bits) {
    CamWrite(CamCciBase(c),0xc08,bits);
    CamWrite(CamCciBase(c),0xc00,1);
}

static NTSTATUS Recover(CAM *c) {
    PUCHAR b=CamCciBase(c);
    ULONG master=CamCciMaster(c),halt=master?0x04000000:0x02000000;
    CamWrite(b,0x34,1u<<master);
    NTSTATUS status=CamWait(b,0xc0c,halt,halt,20000);
    if(!NT_SUCCESS(status)){c->Faulted=TRUE;return status;}
    Clear(c,halt);
    CamWrite(b,4,CamCciMaster(c)?0x3f001:0x3f1);
    status=CamWait(b,0xc0c,0x01000000,0x01000000,20000);
    if(!NT_SUCCESS(status))c->Faulted=TRUE;
    Clear(c,CamCciMaster(c)?0x61fff000:0x19000fff);CamWrite(b,0x34,0);return status;
}

static NTSTATUS Execute(CAM *c,const ULONG *words,ULONG count,ULONG done) {
    PUCHAR b=CamCciBase(c);
    if(c->Faulted)return STATUS_DEVICE_HARDWARE_ERROR;
    ULONG master=CamCciMaster(c),queue=0x300+master*0x200,mask=master?0x60fff000:0x18000fff;
    if(CamRead(b,queue+4))return STATUS_DEVICE_BUSY;
    Clear(c,mask);
    for(ULONG i=0;i<count;i++)CamWrite(b,queue+0x10,words[i]);
    if(CamRead(b,queue+4)!=count){Recover(c);return STATUS_IO_DEVICE_ERROR;}
    CamWrite(b,queue,count);CamWrite(b,8,1u<<(master*2));
    ULONGLONG deadline=KeQueryInterruptTime()+200000;
    do {
        ULONG irq=CamRead(b,0xc0c);c->Info.CciIrq=irq;
        if(irq&(master?0x60ee6000:0x18000ee6)){Recover(c);return STATUS_IO_DEVICE_ERROR;}
        if(irq&done){Clear(c,irq&mask);return STATUS_SUCCESS;}
        CamDelay(50);
    } while(KeQueryInterruptTime()<deadline);
    Recover(c);return STATUS_IO_TIMEOUT;
}

NTSTATUS CamCciInit(CAM *c) {
    PUCHAR b=CamCciBase(c);
    c->Info.CciVersion=CamRead(b,0);
    if(c->Info.CciVersion<0x10000000 || c->Info.CciVersion==MAXULONG)return STATUS_REVISION_MISMATCH;
    ULONG queue=0x300+CamCciMaster(c)*0x200;
    if(CamRead(b,queue+4)||CamRead(b,queue+0x104))return STATUS_DEVICE_BUSY;
    Clear(c,0x01000000);
    CamWrite(b,4,CamCciMaster(c)?0x3f001:0x3f1);
    NTSTATUS status=CamWait(b,0xc0c,0x01000000,0x01000000,20000);
    if(!NT_SUCCESS(status))return status;
    Clear(c,CamCciMaster(c)?0x61fff000:0x19000fff);
    ULONG control=0x100+CamCciMaster(c)*0x100;
    CamWrite(b,control,(0x26u<<16)|0x38u);
    CamWrite(b,control+4,(0x28u<<16)|0x28u);
    CamWrite(b,control+8,(0x16u<<16)|0x23u);
    CamWrite(b,control+12,0x3e);
    CamWrite(b,control+16,0x63);
    return STATUS_SUCCESS;
}

static NTSTATUS Read(CAM *c,UCHAR slave,USHORT address,ULONG addressBytes,PUCHAR buffer,ULONG bytes) {
    if(!buffer||!bytes||bytes>32||slave>0x7f)return STATUS_INVALID_PARAMETER;
    PUCHAR b=CamCciBase(c);
    ULONG control=0x100+CamCciMaster(c)*0x100;
    ULONG level=CamRead(b,control+0x1c);
    if(level>16)return STATUS_IO_DEVICE_ERROR;
    while(level--)CamRead(b,control+0x18);
    ULONG words[]={6,1|((ULONG)slave<<4)|(3u<<16),addressBytes==2?0x2b|((ULONG)(address>>8)<<8)|((ULONG)(address&255)<<16):0x1b|((ULONG)address<<8),10|(bytes<<4),7};
    NTSTATUS status=Execute(c,words,RTL_NUMBER_OF(words),CamCciMaster(c)?0x1000:1);
    if(!NT_SUCCESS(status))return status;
    level=CamRead(b,control+0x1c);
    if(level!=(bytes/4)+1)return STATUS_DATA_ERROR;
    ULONG offset=0;
    for(ULONG i=0;i<level;i++) {
        ULONG value=CamRead(b,control+0x18);
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
    return Execute(c,words,count+4,CamCciMaster(c)?0x10000:0x10);
}

NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes) {return Read(c,slave,address,2,buffer,bytes);}
NTSTATUS CamCciWrite(CAM *c,UCHAR slave,USHORT address,const UCHAR *buffer,ULONG bytes) {return Write(c,slave,address,2,buffer,bytes);}
NTSTATUS CamCciReadByte(CAM *c,UCHAR slave,UCHAR address,PUCHAR buffer,ULONG bytes) {return Read(c,slave,address,1,buffer,bytes);}
NTSTATUS CamCciWriteByte(CAM *c,UCHAR slave,UCHAR address,const UCHAR *buffer,ULONG bytes) {return Write(c,slave,address,1,buffer,bytes);}
