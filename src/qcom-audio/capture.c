#include "sia9177.h"

struct _SIA_MIC {
    PUCHAR Bus, Va, Tx, Pins;
    ULONG Pin[4], Reset[2], TxClock[2], VaClock[4], BusConfig;
    VOID (*Service)(PSIA_DEVICE, PVOID);
    PVOID Audio;
    ULONG Command;
    ULONGLONG SettleAt;
    BOOLEAN ResetReleased;
};
typedef struct { USHORT Reg; UCHAR Space, Value; } MIC_REG;
static const MIC_REG MicStart[] = {
    {0x0104, 0, 0x01},
    {0x0100, 0, 0x0a},
    {0x0000, 1, 0x01},
    {0x0004, 1, 0x03},
    {0x0004, 1, 0x01},
    {0x0080, 1, 0x02},
    {0x0008, 1, 0x01},
    {0x3128, 2, 0x20},
    {0x010c, 0, 0x01},
    {0x0108, 0, 0x05},
    {0x3127, 2, 0x01},
    {0x0114, 0, 0x01},
    {0x0110, 0, 0x06},
    {0x3127, 2, 0x19},
    {0x300e, 2, 0x26},
    {0x300f, 2, 0x06},
    {0x3011, 2, 0x06},
    {0x0000, 3, 0x10},
    {0x0000, 0, 0x01},
    {0x0004, 0, 0x01},
    {0x3409, 2, 0xc0},
    {0x3409, 2, 0xe0},
    {0x3409, 2, 0xf0},
    {0x3408, 2, 0x10},
    {0x3026, 2, 0x62},
    {0x3022, 2, 0x62},
    {0x0000, 3, 0x02},
    {0x0400, 0, 0x14},
    {0x0480, 0, 0x14},
    {0x0500, 0, 0x14},
    {0x0400, 0, 0x34},
    {0x0404, 0, 0x50},
    {0x0418, 0, 0x02},
    {0x0418, 0, 0x01},
    {0x0000, 3, 0x06},
    {0x040c, 0, 0x00},
    {0x0480, 0, 0x34},
    {0x0484, 0, 0x50},
    {0x0498, 0, 0x02},
    {0x0498, 0, 0x01},
    {0x0000, 3, 0x06},
    {0x048c, 0, 0x00},
    {0x0500, 0, 0x34},
    {0x0504, 0, 0x50},
    {0x0518, 0, 0x02},
    {0x0518, 0, 0x01},
    {0x0000, 3, 0x06},
    {0x050c, 0, 0x00},
    {0x3408, 2, 0x18},
    {0x300f, 2, 0x46},
    {0x3410, 2, 0x05},
    {0x300f, 2, 0x06},
    {0x300f, 2, 0x26},
    {0x3410, 2, 0x55},
    {0x300f, 2, 0x06},
    {0x3011, 2, 0x26},
    {0x3411, 2, 0x50},
    {0x3011, 2, 0x06},
    {0x0000, 3, 0x02},
    {0x3405, 2, 0x10},
    {0x0000, 3, 0x06},
    {0x3405, 2, 0x11},
    {0x3405, 2, 0x10},
    {0x3405, 2, 0x00},
    {0x0000, 3, 0x0b},
    {0x0400, 0, 0x24},
    {0x0480, 0, 0x24},
    {0x0500, 0, 0x24},
};
static const MIC_REG MicSettled[] = {
    {0x0504, 0, 0x10},
    {0x0518, 0, 0x02},
    {0x0484, 0, 0x10},
    {0x0498, 0, 0x02},
    {0x0404, 0, 0x10},
    {0x0418, 0, 0x02},
    {0x0518, 0, 0x01},
    {0x0498, 0, 0x01},
    {0x0418, 0, 0x01},
};
static const MIC_REG MicBias[] = {
    {0x3029,2,0xa5}, {0x306b,2,0xfa}, {0x306c,2,1}, {0x306e,2,0xfa}, {0x306f,2,1},
    {0x3071,2,0xfa}, {0x3072,2,0x81}, {0x3074,2,0xfa}, {0x3075,2,0x81},
    {0x31df,2,8}, {0x31e1,2,0x14}, {0x3417,2,0x1f}, {0x3457,2,2}, {0x345b,2,6},
    {0x3022,2,0x22}, {0x3023,2,0x22}, {0x3025,2,0x22}, {0x3026,2,0x22},
    {0x346b,2,0xff}, {0x346c,2,0xff}, {0x346d,2,0xff}
};
static VOID MicWait(PSIA_DEVICE Dev, PSIA_MIC Mic, ULONG Milliseconds)
{
    for(ULONG I=0;I<Milliseconds;I++){Mic->Service(Dev,Mic->Audio);SiaSleep(1);}
}
static PUCHAR MicMap(ULONGLONG Address, ULONG Size)
{
    PHYSICAL_ADDRESS Pa;Pa.QuadPart=Address;
    return MmMapIoSpaceEx(Pa,Size,PAGE_READWRITE|PAGE_NOCACHE);
}
static NTSTATUS MicSdw(PSIA_DEVICE Dev, PSIA_MIC Mic, BOOLEAN Read, ULONG Reg, ULONG *Value, BOOLEAN Broadcast)
{
    BOOLEAN Switch=Broadcast&&(Reg==0x60||Reg==0x70);
    ULONG Id=Switch?15:(Mic->Command=Mic->Command%14+1);
    for(ULONG I=0;((SiaRead32(Mic->Bus,0x5050)>>8)&31)>=8;I++){
        if(I==100)return STATUS_IO_TIMEOUT;MicWait(Dev,Mic,1);
    }
    if(Switch)SiaWrite32(Mic->Bus,0x5008,1u<<10);
    ULONG Cmd=Reg|(Id<<16)|((Broadcast?15u:1u)<<20)|((Read?1u:(*Value&255))<<24);
    if(Read)KeStallExecutionProcessor(100);
    SiaWrite32(Mic->Bus,Read?0x5024:0x5020,Cmd);
    if(!Read)KeStallExecutionProcessor(150);
    if(Read||Broadcast)MicWait(Dev,Mic,1);
    if(!Read&&!Switch)return STATUS_SUCCESS;
    for(ULONG I=0;I<100;I++){
        if(Switch){if((SiaRead32(Mic->Bus,0x104c)&1u)==(Reg==0x70?1u:0u))return STATUS_SUCCESS;}
        else if((SiaRead32(Mic->Bus,0x5050)>>16)&31){
            ULONG Data=SiaRead32(Mic->Bus,0x5040);
            if(((Data>>8)&15)==Id){*Value=Data&255;return STATUS_SUCCESS;}
        }
        MicWait(Dev,Mic,1);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS MicWrite(PSIA_DEVICE Dev, PSIA_MIC Mic, ULONG Reg, ULONG Value)
{
    return MicSdw(Dev,Mic,FALSE,Reg,&Value,FALSE);
}
static NTSTATUS MicTable(PSIA_DEVICE Dev, PSIA_MIC Mic, const MIC_REG *Table, ULONG Count)
{
    for(ULONG I=0;I<Count;I++){
        const MIC_REG *R=&Table[I];
        if(R->Space==3)MicWait(Dev,Mic,R->Value);
        else if(R->Space==2){NTSTATUS Status=MicWrite(Dev,Mic,R->Reg,R->Value);if(!NT_SUCCESS(Status))return Status;}
        else SiaWrite32(R->Space?Mic->Va:Mic->Tx,R->Reg,R->Value);
    }
    return STATUS_SUCCESS;
}
NTSTATUS SiaMicStart(PSIA_DEVICE Dev, PSIA_MIC *Context, VOID (*Service)(PSIA_DEVICE,PVOID), PVOID Audio)
{
    const ULONG Pins[]={0,1,2,14};
    PSIA_MIC Mic=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*Mic),'ciMS');
    if(!Mic)return STATUS_INSUFFICIENT_RESOURCES;
    *Context=NULL;Mic->Service=Service;Mic->Audio=Audio;
    Mic->Bus=MicMap(0x07630000,0x6000);Mic->Va=MicMap(0x07660000,0x4000);
    Mic->Tx=MicMap(0x06ae0000,0x2000);Mic->Pins=MicMap(0x07760000,0x10000);
    NTSTATUS Status=STATUS_INSUFFICIENT_RESOURCES;
    if(!Mic->Bus||!Mic->Va||!Mic->Tx||!Mic->Pins)goto failed;
    Status=STATUS_DEVICE_CONFIGURATION_ERROR;
    if(SiaRead32(Mic->Bus,0)!=0x02010000)goto failed;
    Mic->BusConfig=SiaRead32(Mic->Bus,4);
    Mic->Reset[0]=SiaRead32(Dev->Tlmm,101*0x1000);Mic->Reset[1]=SiaRead32(Dev->Tlmm,101*0x1000+4);
    for(ULONG I=0;I<4;I++){Mic->Pin[I]=SiaRead32(Mic->Pins,Pins[I]*0x1000);SiaWrite32(Mic->Pins,Pins[I]*0x1000,(Mic->Pin[I]&~0x3ffu)|(I==1?6:4));}
    for(ULONG I=0;I<2;I++)Mic->TxClock[I]=SiaRead32(Mic->Tx,I*4);
    const ULONG VaReg[]={0,4,8,0x80};
    for(ULONG I=0;I<4;I++)Mic->VaClock[I]=SiaRead32(Mic->Va,VaReg[I]);
    Mic->ResetReleased=TRUE;
    SiaGpioOutput(Dev,101,FALSE);MicWait(Dev,Mic,20);
    SiaWrite32(Mic->Tx,0,1);SiaWrite32(Mic->Tx,4,1);
    SiaWrite32(Mic->Va,0,1);SiaWrite32(Mic->Va,4,1);SiaWrite32(Mic->Va,0x80,2);
    SiaWrite32(Mic->Tx,0x88,0x60);SiaWrite32(Mic->Tx,0xa4,0xc);
    for(ULONG I=0;I<6;I++)SiaWrite32(Mic->Tx,0xc0+I*4,0xe);
    SiaWrite32(Mic->Va,0xa4,0xc);SiaWrite32(Mic->Va,0xdc,6);
    for(ULONG I=0;I<3;I++)SiaWrite32(Mic->Va,0xd0+I*4,0xcc);
    SiaWrite32(Mic->Va,8,3);MicWait(Dev,Mic,1);SiaWrite32(Mic->Va,8,1);
    SiaGpioOutput(Dev,101,TRUE);MicWait(Dev,Mic,20);
    SiaWrite32(Mic->Bus,8,1);MicWait(Dev,Mic,1);SiaWrite32(Mic->Bus,8,0);
    SiaWrite32(Mic->Bus,0x101c,0x5000f);SiaWrite32(Mic->Bus,0x105c,0x5000f);
    SiaWrite32(Mic->Bus,0x500,1);
    SiaWrite32(Mic->Bus,0x1048,SiaRead32(Mic->Bus,0x1048)|(31u<<17));
    SiaWrite32(Mic->Bus,0x18,1);SiaWrite32(Mic->Bus,0x5060,1);SiaWrite32(Mic->Bus,0x314,0x80000003);
    SiaWrite32(Mic->Bus,0x5004,0);SiaWrite32(Mic->Bus,0x5008,MAXULONG);
    SiaWrite32(Mic->Bus,4,2);SiaWrite32(Mic->Bus,4,3);
    for(ULONG I=0;!(SiaRead32(Mic->Bus,0x1090)&4);I++){if(I==200)goto failed;MicWait(Dev,Mic,1);}
    ULONG Id=0;
    for(ULONG I=0;I<20;I++) {
        Status=MicSdw(Dev,Mic,TRUE,0x50,&Id,FALSE);
        if(NT_SUCCESS(Status)&&Id==0x23)break;
        MicWait(Dev,Mic,1);
    }
    if(!NT_SUCCESS(Status)||Id!=0x23){Status=STATUS_DEVICE_CONFIGURATION_ERROR;goto failed;}
    Status=MicWrite(Dev,Mic,0x3001,0xc0);if(!NT_SUCCESS(Status))goto failed;
    MicWait(Dev,Mic,10);
    Status=MicWrite(Dev,Mic,0x3001,0x80);if(!NT_SUCCESS(Status))goto failed;
    Status=MicTable(Dev,Mic,MicBias,RTL_NUMBER_OF(MicBias));if(!NT_SUCCESS(Status))goto failed;
    // PDM transport: ADC1/2 use slave port 1, ADC4 port 2; master lanes 1/2.
    for(ULONG Bank=0;Bank<2;Bank++)for(ULONG Port=0;Port<2;Port++){
        ULONG Host=0x1324+Port*0x100+Bank*0x40, Slave=0x120+Port*0x100+Bank*0x10;
        SiaWrite32(Mic->Bus,Host,((Port?2u:3u)<<24)|3);
        SiaWrite32(Mic->Bus,Host+4,Port+1);
        Status=MicWrite(Dev,Mic,Slave+2,3);if(!NT_SUCCESS(Status))goto failed;
        Status=MicWrite(Dev,Mic,Slave+8,Port+1);if(!NT_SUCCESS(Status))goto failed;
        Status=MicWrite(Dev,Mic,Slave,Port?2:3);if(!NT_SUCCESS(Status))goto failed;
    }
    ULONG Divider=1;
    Status=MicSdw(Dev,Mic,FALSE,0xf0,&Divider,TRUE);if(!NT_SUCCESS(Status))goto failed;
    ULONG Shape=15;
    Status=MicSdw(Dev,Mic,FALSE,0x70,&Shape,TRUE);if(!NT_SUCCESS(Status))goto failed;
    for(ULONG I=0;I<3;I++){
        const ULONG Reg[]={0x400,0x404,0x408,0x40c,0x418,0x41c,0x420};
        const ULONG Value[]={4,0x10,0xb,0,1,0x3c,0x20};
        for(ULONG J=0;J<RTL_NUMBER_OF(Reg);J++)SiaWrite32(Mic->Tx,Reg[J]+I*0x80,Value[J]);
    }
    SiaWrite32(Mic->Tx,0x42c,0xa);
    Status=MicTable(Dev,Mic,MicStart,RTL_NUMBER_OF(MicStart));if(!NT_SUCCESS(Status))goto failed;
    Mic->SettleAt=KeQueryInterruptTime()+2770000ULL;
    *Context=Mic;return STATUS_SUCCESS;
failed:
    SiaMicStop(Dev,Mic);return Status;
}
VOID SiaMicStop(PSIA_DEVICE Dev, PSIA_MIC Mic)
{
    if(!Mic)return;
    if(Mic->ResetReleased){
        SiaWrite32(Mic->Bus,4,0);
        SiaWrite32(Dev->Tlmm,101*0x1000+4,Mic->Reset[1]);SiaWrite32(Dev->Tlmm,101*0x1000,Mic->Reset[0]);
        for(ULONG I=0;I<3;I++)SiaWrite32(Mic->Tx,0x400+I*0x80,4);
        for(ULONG I=0;I<2;I++)SiaWrite32(Mic->Tx,I*4,Mic->TxClock[I]);
        const ULONG VaReg[]={0,4,8,0x80}, Pins[]={0,1,2,14};
        for(ULONG I=0;I<4;I++){SiaWrite32(Mic->Va,VaReg[I],Mic->VaClock[I]);SiaWrite32(Mic->Pins,Pins[I]*0x1000,Mic->Pin[I]);}
        SiaWrite32(Mic->Bus,4,Mic->BusConfig);
    }
    if(Mic->Bus)MmUnmapIoSpace(Mic->Bus,0x6000);
    if(Mic->Va)MmUnmapIoSpace(Mic->Va,0x4000);
    if(Mic->Tx)MmUnmapIoSpace(Mic->Tx,0x2000);
    if(Mic->Pins)MmUnmapIoSpace(Mic->Pins,0x10000);
    ExFreePoolWithTag(Mic,'ciMS');
}

VOID SiaMicPoll(PSIA_MIC Mic)
{
    if(!Mic->SettleAt || KeQueryInterruptTime()<Mic->SettleAt)return;
    for(ULONG I=0;I<RTL_NUMBER_OF(MicSettled);I++)SiaWrite32(Mic->Tx,MicSettled[I].Reg,MicSettled[I].Value);
    Mic->SettleAt=0;
}
