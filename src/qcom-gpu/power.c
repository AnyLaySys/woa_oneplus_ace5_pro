#include "gpu.h"
#include "../platform/lock.h"

static USHORT Half(GPU *g,ULONG offset) {
    return (USHORT)(GpuRead(g->Db,offset&~3u)>>((offset&3)*8));
}
NTSTATUS GpuDb(GPU *g,PCSTR name,PULONG address,PULONG auxiliary,PULONG length) {
    ULONG key[2]={0};
    for(ULONG i=0;i<8 && name[i];i++)key[i/4]|=(ULONG)(UCHAR)name[i]<<((i&3)*8);
    if(GpuRead(g->Db,4)!=0x0C0330DB)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG layout=0;layout<4;layout++) {
        ULONG sbase=(layout&1)?16:8,hbase=(layout&2)?144:136;
        for(ULONG slave=0;slave<8;slave++) {
            ULONG s=sbase+slave*16;
            if(!Half(g,s))break;
            ULONG entries=hbase+Half(g,s+2),data=hbase+Half(g,s+4),count=Half(g,s+6);
            if((entries&3) || entries>0x20000 || count>(0x20000-entries)/24)continue;
            for(ULONG i=0;i<count;i++) {
                ULONG e=entries+i*24;
                if(GpuRead(g->Db,e)!=key[0] || GpuRead(g->Db,e+4)!=key[1])continue;
                ULONG a=data+Half(g,e+22),n=Half(g,e+20);
                if(a>0x20000 || n>0x20000-a)return STATUS_DEVICE_CONFIGURATION_ERROR;
                *address=GpuRead(g->Db,e+16);*auxiliary=a;*length=n;
                return *address?STATUS_SUCCESS:STATUS_NOT_FOUND;
            }
        }
    }
    return STATUS_NOT_FOUND;
}
NTSTATUS GpuArc(GPU *g,PCSTR primary,PCSTR secondary,ULONG level,PULONG vote) {
    ULONG address=0,p=0,n=0,s=0,m=0,pi=MAXULONG,si=0;
    NTSTATUS status=GpuDb(g,primary,&address,&p,&n);
    if(NT_SUCCESS(status))status=GpuDb(g,secondary,&address,&s,&m);
    if(!NT_SUCCESS(status) || !n || !m || (n|m)&1)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG i=0;i<n/2;i++)if(Half(g,p+i*2)>=level){pi=i;break;}
    if(pi==MAXULONG)return STATUS_NOT_SUPPORTED;
    for(ULONG i=0;i<m/2;i++) {
        USHORT v=Half(g,s+i*2);
        if(v>=level){si=i;break;}
        if(v)si=i;
    }
    *vote=((ULONG)Half(g,p+pi*2)<<16)|(si<<8)|pi;
    return STATUS_SUCCESS;
}
static NTSTATUS Vote(GPU *g,PCSTR name,ULONG level) {
    ULONG address,aux,length,index=MAXULONG;
    NTSTATUS status=GpuDb(g,name,&address,&aux,&length);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<length/2;i++) {
        USHORT v=Half(g,aux+i*2);
        if(level==MAXULONG){if(v)index=i;}
        else if(v>=level){index=i;break;}
    }
    if(index==MAXULONG || ((GpuRead(g->Rsc,0)>>16)&255)!=3)return STATUS_NOT_SUPPORTED;
    return GpuRscWrite(g,address,index);
}
static NTSTATUS RscWriteLocked(GPU *g,ULONG address,ULONG value) {
    NTSTATUS status;
    ULONG irq=GpuRead(g->Rsc,0xD00),slot=MAXULONG,base=0;
    for(ULONG i=0;i<3;i++) {
        ULONG b=0xD00+i*0x2A0;
        if(!GpuRead(g->Rsc,b+0x24) && !GpuRead(g->Rsc,b+0x2C) && !GpuRead(g->Rsc,b+0x20) && !(GpuRead(g->Rsc,0xD04)&(1u<<i))) {
            slot=i;base=b;break;
        }
    }
    if(slot==MAXULONG)return STATUS_DEVICE_BUSY;
    ULONG bit=1u<<slot;
    GpuWrite(g->Rsc,0xD00,irq&~bit);
    GpuWrite(g->Rsc,base+0x34,0x10108);
    GpuWrite(g->Rsc,base+0x38,address);
    GpuWrite(g->Rsc,base+0x3C,value);
    GpuWrite(g->Rsc,base+0x20,1);
    GpuWrite(g->Rsc,base+0x2C,1);
    GpuWrite(g->Rsc,base+0x24,1u<<16);
    GpuWrite(g->Rsc,base+0x24,(1u<<16)|(1u<<24));
    status=GpuPoll(g->Rsc,base+0x40,0x10100,0x10100,100000);
    GpuWrite(g->Rsc,base+0x24,1u<<16);
    GpuWrite(g->Rsc,base+0x24,0);
    if(NT_SUCCESS(status)) {
        GpuWrite(g->Rsc,base+0x2C,0);GpuWrite(g->Rsc,base+0x20,0);
        GpuWrite(g->Rsc,0xD08,bit);GpuWrite(g->Rsc,0xD00,irq);
    }
    return status;
}
NTSTATUS GpuRscWrite(GPU *g,ULONG address,ULONG value) {
    if(!g || !g->Rsc)return STATUS_INVALID_PARAMETER;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,FALSE);
    if(status!=STATUS_SUCCESS)return status;
    __try {status=RscWriteLocked(g,address,value);}
    __finally {
        NTSTATUS released=PlatformLockRelease(&guard);
        if(NT_SUCCESS(status) && !NT_SUCCESS(released))status=released;
    }
    return status;
}
static const ULONG Clocks[]={0x9004,0x9008,0x9064,0x90CC,0x93A4,0x93A8,0x93E8,0x90BC,0x90E4,0x90E8,0x90D4,0x7000};
static NTSTATUS Rcg(PUCHAR cc,ULONG command,ULONG cfg) {
    GpuWrite(cc,command+4,cfg);GpuWrite(cc,command,GpuRead(cc,command)|1);
    return GpuPoll(cc,command,1,0,10000);
}
NTSTATUS GpuPower(GPU *g,BOOLEAN enable) {
    PUCHAR cc=g->Registers+0x90000;
    if(!enable) {
        for(ULONG i=0;i<RTL_NUMBER_OF(Clocks);i++)if(g->ClockMask&(1u<<i))
            GpuWrite(cc,Clocks[i],GpuRead(cc,Clocks[i])&~1u);
        if(g->ClockMask&(1u<<16)) {
            GpuWrite(cc,0x9080,g->Gdscr|1);
            GpuPoll(cc,0x9094,0x80000000,0,10000);
        }
        if(g->ClockMask&(1u<<17))GpuWrite(g->Gcc,0x71150,GpuRead(g->Gcc,0x71150)&~1u);
        if(g->ClockMask&(1u<<18))GpuWrite(g->Gcc,0x71010,GpuRead(g->Gcc,0x71010)&~1u);
        g->ClockMask=0;
        return STATUS_SUCCESS;
    }
    NTSTATUS status=Vote(g,"lcx.lvl",MAXULONG);
    if(NT_SUCCESS(status))status=Vote(g,"lmx.lvl",MAXULONG);
    if(!NT_SUCCESS(status))return status;
    if(!(GpuRead(g->Gcc,0x71150)&1))g->ClockMask|=1u<<17;
    if(!(GpuRead(g->Gcc,0x71010)&1))g->ClockMask|=1u<<18;
    GpuWrite(g->Gcc,0x71150,GpuRead(g->Gcc,0x71150)|1);
    GpuWrite(g->Gcc,0x71010,GpuRead(g->Gcc,0x71010)|1);
    g->Gdscr=GpuRead(cc,0x9080);
    g->ClockMask|=1u<<16;
    GpuWrite(cc,0x9080,(g->Gdscr&~0xFFFFF001u)|0x228800);
    status=GpuPoll(cc,0x9094,0x80000000,0x80000000,10000);
    if(!NT_SUCCESS(status))return status;
    if(!(GpuRead(cc,0)&0x80000000)) {
        GpuWrite(cc,0x10,0x34);GpuWrite(cc,0x14,0x1555);
        GpuWrite(cc,0x20,0x19660387);GpuWrite(cc,0x24,0x098060A0);GpuWrite(cc,0x28,0xB416CB20);
        GpuWrite(cc,0x18,0x400);GpuWrite(cc,0x1C,2);
        GpuWrite(cc,4,0);GpuWrite(cc,0,GpuRead(cc,0)|4);
        GpuWrite(cc,4,1);
        status=GpuPoll(cc,0,0x80000000,0x80000000,10000);
        if(!NT_SUCCESS(status))return status;
        GpuWrite(cc,0x18,GpuRead(cc,0x18)|3);
        GpuWrite(cc,0,GpuRead(cc,0)|1);
    }
    status=Rcg(cc,0x93EC,0x505);
    if(NT_SUCCESS(status))status=Rcg(cc,0x9318,0x201);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<RTL_NUMBER_OF(Clocks);i++) {
        ULONG v=GpuRead(cc,Clocks[i]);if(!(v&1))g->ClockMask|=1u<<i;
        GpuWrite(cc,Clocks[i],v|1);
    }
    KeStallExecutionProcessor(20);
    ULONGLONG registers[8]={0xC2002801,1,9};
    GpuSmc(registers);
    return registers[0] || registers[1]?STATUS_DEVICE_HARDWARE_ERROR:STATUS_SUCCESS;
}
