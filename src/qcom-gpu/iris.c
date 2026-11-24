#include "iris.h"
#include "video-config.h"

#define IRIS_QUEUE_WORDS 262144u
#define IRIS_QUEUE_BYTES (IRIS_QUEUE_WORDS*4u)
#define IRIS_FIRMWARE_BASE 0x9c300000ULL
#define IRIS_FIRMWARE_BYTES 0x800000u
VOID IrisSmc(ULONGLONG *registers);

typedef struct {
    ULONG Status,Address,Type,Size,PacketSize,Dropped,RxWatermark,TxWatermark;
    volatile ULONG RxRequest,TxRequest,RxInterrupt,TxInterrupt,Read,Write;
} IRIS_QUEUE;
typedef struct { ULONG Version,Size,Offset,HeaderSize,Count,Active;ULONGLONG Device;UCHAR Name[256];IRIS_QUEUE Queue[3]; } IRIS_QUEUES;
typedef struct { ULONG Size,Session,Id,Reserved[4],Count; } IRIS_HEADER;
typedef struct { ULONG Size,Type,Flags,Payload,Port,Id,Reserved[2]; } IRIS_PACKET;
typedef struct {
    ULONG Type,Index;ULONGLONG Address;ULONG Offset,Size,DataOffset,DataSize;ULONGLONG Time;ULONG Flags,Reserved[5];
} IRIS_BUFFER;
typedef struct { GPU_BO Buffer;ULONG Offset,Size,Picture;ULONGLONG Time;BOOLEAN Ready; } IRIS_OUTPUT;
struct IRIS {
    GPU *Gpu;
    PDRIVER_OBJECT Driver;
    PUCHAR Registers,Clocks,Smmu;
    GPU_BO Queues,Arp,Metadata;
    GPU_BO Internal[6];
    IRIS_OUTPUT Output[4];
    ULONGLONG InputAddress;
    ULONG Width,Height,Bitrate,MaxBitrate,Session,Started,Picture;
    NTSTATUS Error;
    BOOLEAN InputDone;
    ULONG SavedClocks[9],SavedExtra[4],SavedPll[9],SavedRcg[2],SavedGcc[2],ClockMask,VoteMask,HeaderId,PacketId;
    BOOLEAN PasStarted,FirmwareReady,Prepared,PllChanged,RcgChanged,ScmPending;
    PEX_TIMER Timer;
};
static const ULONG ClockRegisters[]={0x80a4,0x80f8,0x80d4,0x8034,0x8068,0x805c,0x804c,0x808c,0x807c};
static const ULONG ExtraRegisters[]={0x8040,0x8074,0x9f24,0x8090},ExtraMasks[]={0x3e00000,0x3e00000,1,8};
static const ULONG PllRegisters[]={0,4,0x10,0x14,0x18,0x1c,0x20,0x24,0x28};
static NTSTATUS Smc(IRIS *v,ULONG function,ULONG count,ULONGLONG a,ULONGLONG b,ULONGLONG c,ULONGLONG e,PULONGLONG result) {
    ULONGLONG registers[8]={function,count,a,b,c,e,0,0};
    IrisSmc(registers);
    if(registers[0]==2){v->ScmPending=TRUE;return STATUS_DEVICE_BUSY;}
    if(result)*result=registers[1];
    if(registers[0])return (LONG)registers[0]==-1?STATUS_NOT_SUPPORTED:(LONG)registers[0]==-2?STATUS_INVALID_PARAMETER:STATUS_DEVICE_HARDWARE_ERROR;
    return STATUS_SUCCESS;
}
static NTSTATUS Pas(IRIS *v,ULONG function,ULONGLONG a,ULONGLONG b,ULONG count) {
    ULONGLONG result=0;NTSTATUS status=Smc(v,function,count,9,a,b,0,&result);
    return NT_SUCCESS(status) && result?STATUS_DEVICE_HARDWARE_ERROR:status;
}
static NTSTATUS Arc(IRIS *v,PCSTR name,ULONG level) {
    ULONG address,aux,length;NTSTATUS status=GpuDb(v->Gpu,name,&address,&aux,&length);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<length/2;i++) {
        ULONG offset=aux+i*2;
        USHORT value=(USHORT)(GpuRead(v->Gpu->Db,offset&~3u)>>((offset&3)*8));
        if(value>=level)return GpuRscWrite(v->Gpu,address,i);
    }
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS Branch(IRIS *v,ULONG index) {
    ULONG offset=ClockRegisters[index],value=GpuRead(v->Clocks,offset);
    if(!(value&1)){v->ClockMask|=1u<<index;GpuWrite(v->Clocks,offset,value|1);}
    return GpuPoll(v->Clocks,offset,0x80000000,0,2000);
}
static NTSTATUS Power(IRIS *v) {
    NTSTATUS status=Arc(v,"mxc.lvl",256);
    if(NT_SUCCESS(status)){v->VoteMask|=1;status=Arc(v,"mmcx.lvl",256);}
    if(NT_SUCCESS(status))v->VoteMask|=2;
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<RTL_NUMBER_OF(ClockRegisters);i++)v->SavedClocks[i]=GpuRead(v->Clocks,ClockRegisters[i]);
    for(ULONG i=0;i<RTL_NUMBER_OF(ExtraRegisters);i++)v->SavedExtra[i]=GpuRead(v->Clocks,ExtraRegisters[i]);
    for(ULONG i=0;i<RTL_NUMBER_OF(PllRegisters);i++)v->SavedPll[i]=GpuRead(v->Clocks,PllRegisters[i]);
    v->SavedRcg[0]=GpuRead(v->Clocks,0x8000);v->SavedRcg[1]=GpuRead(v->Clocks,0x8004);
    v->SavedGcc[0]=GpuRead(v->Gpu->Gcc,0x32018);v->SavedGcc[1]=GpuRead(v->Gpu->Gcc,0x32028);
    v->Prepared=TRUE;
    GpuWrite(v->Gpu->Gcc,0x32028,v->SavedGcc[1]|1);
    GpuWrite(v->Gpu->Gcc,0x32018,v->SavedGcc[0]|1);
    status=Branch(v,0);
    if(NT_SUCCESS(status))status=Branch(v,1);
    if(NT_SUCCESS(status))status=Branch(v,2);
    if(!NT_SUCCESS(status))return status;
    if(!(GpuRead(v->Clocks,0)&0x80000000u)) {
        v->PllChanged=TRUE;
        GpuWrite(v->Clocks,0x10,0x25);GpuWrite(v->Clocks,0x14,0x8000);
        GpuWrite(v->Clocks,0x20,0x19660387);GpuWrite(v->Clocks,0x24,0x098060a0);GpuWrite(v->Clocks,0x28,0xb416cb20);
        GpuWrite(v->Clocks,0x18,0x400);GpuWrite(v->Clocks,0x1c,2);
        GpuWrite(v->Clocks,4,0);GpuWrite(v->Clocks,0,GpuRead(v->Clocks,0)|4);GpuWrite(v->Clocks,4,1);
        status=GpuPoll(v->Clocks,0,0x80000000u,0x80000000u,10000);
        if(!NT_SUCCESS(status))return status;
    }
    v->PllChanged=TRUE;
    GpuWrite(v->Clocks,0x18,GpuRead(v->Clocks,0x18)|3);GpuWrite(v->Clocks,0,GpuRead(v->Clocks,0)|1);
    v->RcgChanged=TRUE;
    GpuWrite(v->Clocks,0x8004,0x100);GpuWrite(v->Clocks,0x8000,GpuRead(v->Clocks,0x8000)|1);
    status=GpuPoll(v->Clocks,0x8000,1,0,2000);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=3;i<=4;i++) {
        ULONG offset=ClockRegisters[i];v->ClockMask|=1u<<i;
        GpuWrite(v->Clocks,offset,(v->SavedClocks[i]&~0xfffff003u)|0x226800u);
        status=GpuPoll(v->Clocks,offset+4,1u<<16,1u<<16,2000);
        if(!NT_SUCCESS(status))return status;
    }
    GpuWrite(v->Clocks,0x8040,GpuRead(v->Clocks,0x8040)|0x3e00000);
    GpuWrite(v->Clocks,0x8074,GpuRead(v->Clocks,0x8074)|0x3e00000);
    GpuWrite(v->Clocks,0x9f24,GpuRead(v->Clocks,0x9f24)|1);
    status=Branch(v,5);if(NT_SUCCESS(status))status=Branch(v,6);
    if(!NT_SUCCESS(status))return status;
    GpuWrite(v->Clocks,0x8090,GpuRead(v->Clocks,0x8090)&~8u);
    status=GpuPoll(v->Clocks,0x8090,0xc00,0xc00,2000);
    if(NT_SUCCESS(status))status=Branch(v,7);
    if(NT_SUCCESS(status))status=Branch(v,8);
    return status;
}
static NTSTATUS Translation(IRIS *v) {
    ULONG id=GpuRead(v->Smmu,0x20),count=min(id&255u,128u),found=0;
    if(!(id&(1u<<27)) || !count)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG i=0;i<count;i++) {
        ULONG smr=GpuRead(v->Smmu,0x800+i*4);if(!(smr&0x80000000u))continue;
        ULONG mask=(smr>>16)&0x7fff;
        if(mask>7)continue;
        for(ULONG j=0;j<2;j++)if(!((smr^(j?0x1947u:0x1940u))&0x7fffu&~mask))found|=1u<<j;
    }
    return found==3?STATUS_SUCCESS:STATUS_DEVICE_NOT_READY;
}
static NTSTATUS Firmware(IRIS *v) {
    HANDLE directory=NULL;NTSTATUS status=IoGetDriverDirectory(v->Driver,DriverDirectoryImage,0,&directory);
    if(!NT_SUCCESS(status))return status;
    PUCHAR data=NULL;ULONG size=0;status=GpuFirmware(directory,L"vpu35_4v.mbn",IRIS_FIRMWARE_BYTES,&data,&size);
    ZwClose(directory);
    if(!NT_SUCCESS(status))return status;
    GPU_BO *metadata=&v->Metadata;PUCHAR memory=NULL;
    if(size<52 || RtlCompareMemory(data,"\177ELF\1\1",6)!=6){status=STATUS_INVALID_IMAGE_FORMAT;goto done;}
    ULONG offset=*(UNALIGNED ULONG *)(data+28);USHORT count=*(UNALIGNED USHORT *)(data+44);
    if(*(UNALIGNED USHORT *)(data+42)!=32 || !count || offset>size || count>(size-offset)/32){status=STATUS_INVALID_IMAGE_FORMAT;goto done;}
    ULONG header=offset+count*32,hashOffset=0,hashSize=0,minimum=MAXULONG,maximum=0;
    for(ULONG i=0;i<count;i++) {
        UNALIGNED ULONG *p=(UNALIGNED ULONG *)(data+offset+i*32);
        if((p[6]&0x07000000u)==0x02000000u){hashOffset=p[1];hashSize=p[4];continue;}
        if(p[0]!=1 || !p[5])continue;
        if(!(p[6]&0x08000000u) || p[4]>p[5] || p[1]>size || p[4]>size-p[1] || p[5]>MAXULONG-p[3]){status=STATUS_INVALID_IMAGE_FORMAT;goto done;}
        minimum=min(minimum,p[3]);maximum=max(maximum,p[3]+p[5]);
    }
    if(!hashSize || hashOffset>size || hashSize>size-hashOffset || minimum==MAXULONG || maximum-minimum>IRIS_FIRMWARE_BYTES){status=STATUS_INVALID_IMAGE_FORMAT;goto done;}
    status=GpuAllocateContiguous(metadata,header+hashSize,MmNonCached,MAXULONG,0);if(!NT_SUCCESS(status))goto done;
    RtlCopyMemory(metadata->Cpu,data,header);RtlCopyMemory(metadata->Cpu+header,data+hashOffset,hashSize);KeMemoryBarrier();
    ULONGLONG result=0;status=Smc(v,0xc2000201,0x82,9,metadata->Physical,0,0,&result);
    if(!NT_SUCCESS(status) || result){if(NT_SUCCESS(status))status=STATUS_INVALID_IMAGE_HASH;goto done;}
    v->PasStarted=TRUE;
    status=Pas(v,0xc2000202,IRIS_FIRMWARE_BASE,IRIS_FIRMWARE_BYTES,3);if(!NT_SUCCESS(status))goto done;
    PHYSICAL_ADDRESS physical;physical.QuadPart=IRIS_FIRMWARE_BASE;
    memory=MmMapIoSpaceEx(physical,IRIS_FIRMWARE_BYTES,PAGE_READWRITE|PAGE_WRITECOMBINE);
    if(!memory){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    RtlZeroMemory(memory,IRIS_FIRMWARE_BYTES);
    for(ULONG i=0;i<count;i++) {
        UNALIGNED ULONG *p=(UNALIGNED ULONG *)(data+offset+i*32);
        if(p[0]==1 && p[5] && (p[6]&0x07000000u)!=0x02000000u && p[4])RtlCopyMemory(memory+p[3]-minimum,data+p[1],p[4]);
    }
    KeMemoryBarrier();status=Pas(v,0xc2000205,0,0,1);
    if(NT_SUCCESS(status)) {
        v->FirmwareReady=TRUE;
        status=Smc(v,0xc2000c08,4,0,0x25800000,0x01000000,0x24800000,&result);
        if(NT_SUCCESS(status) && result)status=STATUS_DEVICE_HARDWARE_ERROR;
    }
done:if(memory)MmUnmapIoSpace(memory,IRIS_FIRMWARE_BYTES);if(!v->ScmPending)GpuFree(metadata);if(data)ExFreePoolWithTag(data,GPU_TAG);return status;
}
static ULONG Packet(IRIS *v,IRIS_HEADER *header,ULONG type,ULONG flags,ULONG payload,ULONG port,const VOID *data,ULONG bytes) {
    IRIS_PACKET *p=(IRIS_PACKET *)((PUCHAR)header+header->Size);
    p->Size=sizeof(*p)+bytes;p->Type=type;p->Flags=flags;p->Payload=payload;p->Port=port;p->Id=++v->PacketId;
    if(bytes)RtlCopyMemory(p+1,data,bytes);
    header->Size+=p->Size;header->Count++;return p->Id;
}
static NTSTATUS WriteCommand(IRIS *v,IRIS_HEADER *header) {
    IRIS_QUEUE *q=&((IRIS_QUEUES *)v->Queues.Cpu)->Queue[0];ULONG write=q->Write,read=q->Read,words=header->Size/4;
    if(write>=IRIS_QUEUE_WORDS || read>=IRIS_QUEUE_WORDS)return STATUS_DEVICE_DATA_ERROR;
    ULONG available=(read+IRIS_QUEUE_WORDS-write-1)%IRIS_QUEUE_WORDS;
    if(words>available)return STATUS_DEVICE_BUSY;
    PULONG buffer=(PULONG)(v->Queues.Cpu+sizeof(IRIS_QUEUES));q->TxRequest=0;
    for(ULONG i=0;i<words;i++)buffer[(write+i)%IRIS_QUEUE_WORDS]=((PULONG)header)[i];
    KeMemoryBarrier();q->Write=(write+words)%IRIS_QUEUE_WORDS;KeMemoryBarrier();GpuWrite(v->Registers,0xa0150,1);
    return STATUS_SUCCESS;
}
static NTSTATUS Send(IRIS *v,ULONG type,ULONG flags,ULONG payload,ULONG session,ULONG port,const VOID *data,ULONG bytes,PULONG id) {
    ULONG packet[256]={0};
    if((bytes&3) || bytes>sizeof(packet)-sizeof(IRIS_HEADER)-sizeof(IRIS_PACKET))return STATUS_INVALID_BUFFER_SIZE;
    IRIS_HEADER *header=(IRIS_HEADER *)packet;header->Size=sizeof(*header);header->Session=session;header->Id=++v->HeaderId;
    ULONG next=Packet(v,header,type,flags,payload,port,data,bytes);if(id)*id=next;
    return WriteCommand(v,header);
}
static NTSTATUS Response(IRIS *v,ULONG id) {
    IRIS_QUEUE *q=&((IRIS_QUEUES *)v->Queues.Cpu)->Queue[1];PULONG buffer=(PULONG)(v->Queues.Cpu+sizeof(IRIS_QUEUES)+IRIS_QUEUE_BYTES);
    ULONGLONG deadline=KeQueryInterruptTime()+10000000;
    while(KeQueryInterruptTime()<deadline) {
        if(!id && v->InputDone)for(ULONG i=0;i<RTL_NUMBER_OF(v->Output);i++)if(v->Output[i].Ready)return STATUS_SUCCESS;
        if(GpuRead(v->Registers,0xb000c)&8)return STATUS_DEVICE_HARDWARE_ERROR;
        ULONG read=q->Read,write=q->Write;KeMemoryBarrier();
        if(read>=IRIS_QUEUE_WORDS || write>=IRIS_QUEUE_WORDS)return STATUS_DEVICE_DATA_ERROR;
        if(read!=write) {
            ULONG packet[256],bytes=buffer[read],available=(write+IRIS_QUEUE_WORDS-read)%IRIS_QUEUE_WORDS;
            if((bytes&3) || bytes<sizeof(IRIS_HEADER) || bytes>sizeof(packet) || bytes/4>available)return STATUS_DEVICE_DATA_ERROR;
            for(ULONG i=0;i<bytes/4;i++)packet[i]=buffer[(read+i)%IRIS_QUEUE_WORDS];
            KeMemoryBarrier();q->Read=(read+bytes/4)%IRIS_QUEUE_WORDS;q->RxRequest=1;KeMemoryBarrier();
            GpuWrite(v->Registers,0xa001c,1);
            IRIS_HEADER *header=(IRIS_HEADER *)packet;ULONG offset=sizeof(*header),outputs=0;BOOLEAN matched=FALSE;
            for(ULONG i=0;i<header->Count;i++) {
                if(offset>bytes || sizeof(IRIS_PACKET)>bytes-offset)return STATUS_DEVICE_DATA_ERROR;
                IRIS_PACKET *p=(IRIS_PACKET *)((PUCHAR)packet+offset);
                if(p->Size<sizeof(*p) || p->Size>bytes-offset || (p->Size&3))return STATUS_DEVICE_DATA_ERROR;
                if(p->Flags&12)return STATUS_DEVICE_HARDWARE_ERROR;
                if(id && p->Id==id){if(!(p->Flags&1))return STATUS_NOT_SUPPORTED;matched=TRUE;}
                if(header->Session==v->Session && v->Session && p->Type==0x03000162 && p->Size>=sizeof(*p)+4)
                    v->Picture=*(PULONG)(p+1);
                if(header->Session==v->Session && v->Session && p->Type==0x01000009) {
                    if(p->Size<sizeof(*p)+sizeof(IRIS_BUFFER))return STATUS_DEVICE_DATA_ERROR;
                    IRIS_BUFFER *b=(IRIS_BUFFER *)(p+1);
                    if(b->Type==2 && b->Address==v->InputAddress)v->InputDone=TRUE;
                    if(b->Type==1) {
                        if(b->Index>=RTL_NUMBER_OF(v->Output))return STATUS_DEVICE_DATA_ERROR;
                        IRIS_OUTPUT *o=v->Output+b->Index;
                        if(b->Address!=o->Buffer.Physical || b->Offset || b->DataOffset>o->Buffer.Size ||
                           b->DataSize>o->Buffer.Size-b->DataOffset || o->Ready)return STATUS_DEVICE_DATA_ERROR;
                        o->Offset=b->DataOffset;o->Size=b->DataSize;o->Time=b->Time;o->Ready=TRUE;outputs|=1u<<b->Index;
                    }
                }
                offset+=p->Size;
            }
            if(offset!=bytes)return STATUS_DEVICE_DATA_ERROR;
            for(ULONG i=0;i<RTL_NUMBER_OF(v->Output);i++)if(outputs&(1u<<i))v->Output[i].Picture=v->Picture;
            if(matched)return STATUS_SUCCESS;
            continue;
        }
        ExSetTimer(v->Timer,-5000,0,NULL);KeWaitForSingleObject(v->Timer,Executive,KernelMode,FALSE,NULL);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS SystemInit(IRIS *v) {
    ULONG data[80]={0},values[]={1,8,32,16,0,1,1,1};
    IRIS_HEADER *header=(IRIS_HEADER *)data;header->Size=sizeof(*header);header->Id=++v->HeaderId;
    ULONG id=Packet(v,header,0x01000001,7,1,0,values,4);
    for(ULONG i=1;i<RTL_NUMBER_OF(values);i++)Packet(v,header,0x03000002+i,0,1,0,values+i,4);
    NTSTATUS status=WriteCommand(v,header);return NT_SUCCESS(status)?Response(v,id):status;
}
static ULONG Align(ULONG value,ULONG alignment) {return (value+alignment-1)&~(alignment-1);}
static ULONG OutputSize(ULONG width,ULONG height) {
    ULONG mbs=Align(width,32)*Align(height,32)/256,bytes=width*height*3;
    bytes=mbs<3680?bytes*2:bytes/4;
    return Align(bytes*5/4,4096);
}
static VOID InternalSizes(ULONG width,ULONG height,ULONG sizes[6]) {
    ULONG w=Align(width,16),h=Align(height,16),mw=w/16,mh=h/16,n=mw*mh;
    ULONG bitstream=w*h*3;if(w*h>480*320)bitstream/=4;
    ULONG bin=Align(Align(Align(bitstream,256)*17/10,256)/2,256);
    sizes[0]=Align(bin+64*((w+32)*(h+32)/1024)+384,256)*12+512;
    sizes[1]=Align(48*(n+32),256)*2+Align(((mw+7)/8)*32*mh,256)*6;
    ULONG rc=Align(Align((256+16*(14+(mh+7)/8))*11,256)*4,512)*6;
    sizes[2]=Align(256+n*16,256)+20480+6144+rc+Align(256+4*n,256)+Align(256+n/8,256)+
        98304+2816+Align(16*((mh*(mw+8)+7)/8),256)*2+Align(Align(n*2,8)*3,256);
    sizes[3]=Align(w,256)*2+Align(8*w+1024,256)+Align((8*w+1024)/2,256)*2+
        Align(mh*80+512,512)*4+Align(2*h+256,2048)+Align(256+16*mw,256)+
        Align(256+64*(h/64),256)*4+Align(256+64*(h/32),256)*4+Align(2*w,256)+
        Align(256+64*mh,2048)*4+Align(256+16*mw,256)+Align(16*(w/32),256)+
        Align((65600+((max(w,h)*2+3)/4)*32+64)*8+
            (((max(w,h)*2+3)/4)*16+256+((max(w,h)+3)/4)*16+256)*2+(w+1)*8+h*32,256);
    ULONG metadata=Align(Align((width+31)/32,64)*Align((height+7)/8,16),4096);
    sizes[4]=sizes[5]=(Align(height,32)+Align(height/2,32))*Align(width,128)+metadata*2;
}
static NTSTATUS Property(IRIS *v,ULONG type,ULONG payload,ULONG port,ULONG value) {
    return Send(v,type,0,payload,v->Session,port,&value,4,NULL);
}
static NTSTATUS Buffer(IRIS *v,ULONG type,ULONG index,GPU_BO *bo,ULONG port,ULONG bytes,ULONGLONG time) {
    IRIS_BUFFER b={0};b.Type=type;b.Index=index;b.Address=bo->Physical;b.Size=bo->Size;
    b.DataSize=bytes;b.Time=time;b.Flags=type==2 || type==6?0x2000:0x200;
    return Send(v,0x01000009,1,5,v->Session,port,&b,sizeof(b),NULL);
}
static NTSTATUS Command(IRIS *v,ULONG type,ULONG port) {
    ULONG id=0;NTSTATUS status=Send(v,type,type==0x01000005?3:7,0,v->Session,port,NULL,0,&id);
    return NT_SUCCESS(status)?Response(v,id):status;
}
NTSTATUS IrisStart(IRIS *v,const GPU_VIDEO_CONFIG *config) {
    if(!v || !v->FirmwareReady || v->Session || !NT_SUCCESS(v->Error))return STATUS_INVALID_DEVICE_STATE;
    if(!config || !GpuVideoConfigValid(config))return STATUS_INVALID_PARAMETER;
    ULONG width=config->Width,height=config->Height,rate=config->Rate,bitrate=config->Bitrate;
    v->MaxBitrate=GpuVideoMaxBitrate(config);
    ULONG session=1,id=0;
    NTSTATUS status=Send(v,0x01000003,3,1,0,0,&session,4,&id);
    if(!NT_SUCCESS(status))return status;
    v->Session=session;v->Width=width;v->Height=height;v->Bitrate=bitrate;
    status=Property(v,0x03000100,9,0,2);
    if(NT_SUCCESS(status))status=Response(v,id);
    if(NT_SUCCESS(status))status=Property(v,0x03000102,1,0,0);
    if(NT_SUCCESS(status))status=GpuAllocateContiguous(&v->Arp,204800,MmNonCached,0xdfffffff,0);
    if(NT_SUCCESS(status))status=Buffer(v,9,0,&v->Arp,0,0,0);
    if(!NT_SUCCESS(status)){v->Error=status;return status;}
    ULONG data[256]={0},stride=Align(width,128),scanline=Align(height,64);
    IRIS_HEADER *header=(IRIS_HEADER *)data;header->Size=sizeof(*header);header->Session=session;header->Id=++v->HeaderId;
    ULONG inputValues[][3]={{0x03000101,9,1},{0x03000178,10,(width<<16)|height},{0x03000155,10,0x35010101}};
    for(ULONG i=0;i<RTL_NUMBER_OF(inputValues);i++)Packet(v,header,inputValues[i][0],0,inputValues[i][1],2,inputValues[i]+2,4);
    ULONG lines[]={stride<<16|scanline,stride<<16|(scanline/2)},crop[]={0,0};
    Packet(v,header,0x03000104,0,13,2,lines,sizeof(lines));Packet(v,header,0x03000105,0,13,2,crop,sizeof(crop));
    status=WriteCommand(v,header);
    if(NT_SUCCESS(status)){status=Command(v,0x01000005,2);if(NT_SUCCESS(status))v->Started=2;}
    if(!NT_SUCCESS(status)){v->Error=status;return status;}
    RtlZeroMemory(data,sizeof(data));header->Size=sizeof(*header);header->Session=session;header->Id=++v->HeaderId;
    ULONG values[][3]={
        {0x03000103,10,(Align(width,16)<<16)|Align(height,16)},
        {0x03000107,9,config->Profile},{0x03000108,9,config->Level},
        {0x0300010c,8,rate<<16},{0x03000121,1,config->Profile>=2},{0x03000123,1,4},
        {0x0300012a,9,1},{0x03000134,1,0},{0x03000139,1,1},{0x0300013b,1,bitrate},
        {0x03000146,1,config->Gop?config->Gop:rate*2},{0x03000147,1,0},{0x03000148,9,2},{0x03000149,9,4}
    };
    for(ULONG i=0;i<RTL_NUMBER_OF(values);i++)Packet(v,header,values[i][0],0,values[i][1],1,values[i]+2,4);
    ULONG stage=2,pipes=4;
    Packet(v,header,0x0300010a,0,1,0,&stage,4);Packet(v,header,0x0300010b,0,1,0,&pipes,4);
    crop[1]=(Align(width,16)-width)<<16|(Align(height,16)-height);
    Packet(v,header,0x03000105,0,13,1,crop,sizeof(crop));
    ULONG subscribe[]={2,0x03000162};Packet(v,header,0x0100000b,0,11,1,subscribe,sizeof(subscribe));
    status=WriteCommand(v,header);
    ULONG sizes[6],types[]={7,10,11,8,6,6};InternalSizes(width,height,sizes);
    for(ULONG i=0;NT_SUCCESS(status) && i<RTL_NUMBER_OF(v->Internal);i++) {
        status=GpuAllocateContiguous(v->Internal+i,sizes[i],MmNonCached,0xdfffffff,0);
        if(NT_SUCCESS(status))status=Buffer(v,types[i],i>3?i-4:0,v->Internal+i,1,0,0);
    }
    for(ULONG i=0;NT_SUCCESS(status) && i<RTL_NUMBER_OF(v->Output);i++)
        status=GpuAllocateContiguous(&v->Output[i].Buffer,OutputSize(width,height),MmNonCached,0xdfffffff,0);
    if(NT_SUCCESS(status)){status=Command(v,0x01000005,1);if(NT_SUCCESS(status))v->Started=3;}
    for(ULONG i=0;NT_SUCCESS(status) && i<RTL_NUMBER_OF(v->Output);i++)
        status=Buffer(v,1,i,&v->Output[i].Buffer,1,0,0);
    v->Error=status;return status;
}
NTSTATUS IrisEncode(IRIS *v,GPU_BO *input,ULONGLONG time,BOOLEAN key,ULONG bitrate,PUCHAR output,ULONG capacity,PULONG bytes,PBOOLEAN keyframe) {
    if(!v || v->Started!=3 || !NT_SUCCESS(v->Error) || !input || !input->Contiguous || !output || !bytes || !keyframe)
        return STATUS_INVALID_DEVICE_STATE;
    ULONG size=Align(v->Width,128)*Align(v->Height,64)*3/2;
    if(input->Physical>0xdfffffff || size>input->Size || size-1>0xdfffffff-input->Physical)return STATUS_INVALID_PARAMETER;
    if(capacity<OutputSize(v->Width,v->Height))return STATUS_BUFFER_TOO_SMALL;
    if(bitrate && (bitrate<64000 || bitrate>v->MaxBitrate))return STATUS_INVALID_PARAMETER;
    v->InputAddress=input->Physical;v->InputDone=FALSE;v->Picture=0;*bytes=0;*keyframe=FALSE;
    NTSTATUS status=key?Property(v,0x03000145,9,1,2):STATUS_SUCCESS;
    if(NT_SUCCESS(status) && bitrate && bitrate!=v->Bitrate) {
        status=Property(v,0x0300013b,1,1,bitrate);if(NT_SUCCESS(status))v->Bitrate=bitrate;
    }
    if(input->Cached)KeFlushIoBuffers(input->Mdl,FALSE,TRUE);
    if(NT_SUCCESS(status))status=Buffer(v,2,0,input,2,size,time);
    if(NT_SUCCESS(status))status=Response(v,0);
    if(!NT_SUCCESS(status)){v->Error=status;return status;}
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Output);i++) {
        IRIS_OUTPUT *o=v->Output+i;if(!o->Ready)continue;
        if(o->Time!=time || !o->Size){v->Error=STATUS_DEVICE_DATA_ERROR;return v->Error;}
        RtlCopyMemory(output,o->Buffer.Cpu+o->Offset,o->Size);*bytes=o->Size;*keyframe=(o->Picture&9)!=0;
        o->Ready=FALSE;v->InputAddress=0;
        status=Buffer(v,1,i,&o->Buffer,1,0,0);v->Error=status;return status;
    }
    return STATUS_DEVICE_DATA_ERROR;
}
static NTSTATUS LowPower(IRIS *v,ULONG offset) {
    for(ULONG i=0;i<1000;i++) {
        GpuWrite(v->Registers,offset,1);KeStallExecutionProcessor(15);
        ULONG value=GpuRead(v->Registers,offset+4);
        if((value&1) || !(value&6)) {
            NTSTATUS status=GpuPoll(v->Registers,offset+4,1,1,2000);
            if(NT_SUCCESS(status))GpuWrite(v->Registers,offset,0);
            return status;
        }
        GpuWrite(v->Registers,offset,0);KeStallExecutionProcessor(15);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS Drain(IRIS *v) {
    GpuWrite(v->Registers,0xa0148,0);
    GpuWrite(v->Registers,0xb0010,GpuRead(v->Registers,0xb0010)|12);
    GpuWrite(v->Registers,0xa001c,1);GpuWrite(v->Registers,0xa0168,3);
    if(GpuRead(v->Registers,0xb0080)&2) {
        GpuWrite(v->Registers,0xb0088,0);
        for(ULONG i=0;i<4;i++) {
            NTSTATUS status=GpuPoll(v->Registers,0x70+i*4,0x400000,0x400000,20000);
            if(!NT_SUCCESS(status))return status;
        }
        NTSTATUS status=LowPower(v,0xe0000);if(!NT_SUCCESS(status))return status;
        GpuWrite(v->Registers,0xa0160,3);GpuWrite(v->Registers,0xa0160,2);GpuWrite(v->Registers,0xa0160,0);
    }
    GpuWrite(v->Registers,0xb005c,1);
    NTSTATUS status=GpuPoll(v->Registers,0xb0060,1,1,2000);if(!NT_SUCCESS(status))return status;
    GpuWrite(v->Registers,0xb005c,0);
    status=LowPower(v,0xe002c);if(!NT_SUCCESS(status))return status;
    GpuWrite(v->Registers,0xb0054,0);
    return GpuPoll(v->Registers,0xb0058,MAXULONG,0,2000);
}
NTSTATUS IrisDelete(IRIS *v) {
    if(!v)return STATUS_SUCCESS;
    if(v->ScmPending)return STATUS_DEVICE_BUSY;
    if(v->Session && NT_SUCCESS(v->Error)) {
        for(ULONG port=2;port;port--)if(v->Started&port) {
            v->Error=Command(v,0x01000006,port);if(!NT_SUCCESS(v->Error))break;
        }
        if(NT_SUCCESS(v->Error))v->Error=Command(v,0x01000004,0);
    }
    if(v->PasStarted) {
        NTSTATUS status=Pas(v,0xc2000206,0,0,1);if(!NT_SUCCESS(status))return status;
        v->PasStarted=FALSE;
    }
    if(v->FirmwareReady) {
        NTSTATUS status=Drain(v);if(!NT_SUCCESS(status))return status;
        v->FirmwareReady=FALSE;
    }
    if(v->Prepared) {
        for(ULONG i=9;i-->5;)if(v->ClockMask&(1u<<i))GpuWrite(v->Clocks,ClockRegisters[i],v->SavedClocks[i]);
        for(ULONG i=RTL_NUMBER_OF(ExtraRegisters);i--;)GpuWrite(v->Clocks,ExtraRegisters[i],
            (GpuRead(v->Clocks,ExtraRegisters[i])&~ExtraMasks[i])|(v->SavedExtra[i]&ExtraMasks[i]));
        for(ULONG i=5;i-->3;)if(v->ClockMask&(1u<<i))GpuWrite(v->Clocks,ClockRegisters[i],v->SavedClocks[i]);
        if(v->RcgChanged) {
            GpuWrite(v->Clocks,0x8004,v->SavedRcg[1]);GpuWrite(v->Clocks,0x8000,v->SavedRcg[0]|1);
            NTSTATUS status=GpuPoll(v->Clocks,0x8000,1,0,2000);if(!NT_SUCCESS(status))return status;
            v->RcgChanged=FALSE;
        }
        if(v->PllChanged) {
            GpuWrite(v->Clocks,0,v->SavedPll[0]&~1u);
            for(ULONG i=RTL_NUMBER_OF(PllRegisters);i-->1;)GpuWrite(v->Clocks,PllRegisters[i],v->SavedPll[i]);
            GpuWrite(v->Clocks,0,v->SavedPll[0]);v->PllChanged=FALSE;
        }
        for(ULONG i=3;i--;)if(v->ClockMask&(1u<<i))GpuWrite(v->Clocks,ClockRegisters[i],v->SavedClocks[i]);
        GpuWrite(v->Gpu->Gcc,0x32018,v->SavedGcc[0]);GpuWrite(v->Gpu->Gcc,0x32028,v->SavedGcc[1]);
    }
    if(v->VoteMask&2){NTSTATUS status=Arc(v,"mmcx.lvl",0);if(!NT_SUCCESS(status))return status;v->VoteMask&=~2u;}
    if(v->VoteMask&1){NTSTATUS status=Arc(v,"mxc.lvl",0);if(!NT_SUCCESS(status))return status;v->VoteMask&=~1u;}
    if(v->Timer)ExDeleteTimer(v->Timer,TRUE,TRUE,NULL);
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Internal);i++)GpuFree(v->Internal+i);
    for(ULONG i=0;i<RTL_NUMBER_OF(v->Output);i++)GpuFree(&v->Output[i].Buffer);
    GpuFree(&v->Metadata);GpuFree(&v->Arp);GpuFree(&v->Queues);
    if(v->Registers)MmUnmapIoSpace(v->Registers,0xf0000);
    if(v->Clocks)MmUnmapIoSpace(v->Clocks,0xa000);
    if(v->Smmu)MmUnmapIoSpace(v->Smmu,4096);
    ExFreePoolWithTag(v,GPU_TAG);return STATUS_SUCCESS;
}
NTSTATUS IrisCreate(GPU *gpu,PDRIVER_OBJECT driver,IRIS **out) {
    if(!gpu || !gpu->Online || !out || *out)return STATUS_INVALID_DEVICE_STATE;
    IRIS *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),GPU_TAG);if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    v->Gpu=gpu;v->Driver=driver;
    PHYSICAL_ADDRESS p;p.QuadPart=0xaa00000;v->Registers=MmMapIoSpaceEx(p,0xf0000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0xaaf0000;v->Clocks=MmMapIoSpaceEx(p,0xa000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x15000000;v->Smmu=MmMapIoSpaceEx(p,4096,PAGE_READONLY|PAGE_NOCACHE);
    NTSTATUS status=v->Registers && v->Clocks && v->Smmu?STATUS_SUCCESS:STATUS_INSUFFICIENT_RESOURCES;
    ULONGLONG supported=0;
    if(NT_SUCCESS(status))status=Smc(v,0xc2000207,1,9,0,0,0,&supported);
    if(NT_SUCCESS(status) && !supported)status=STATUS_NOT_SUPPORTED;
    if(NT_SUCCESS(status))status=Translation(v);
    if(NT_SUCCESS(status))status=Power(v);
    if(NT_SUCCESS(status))GpuWrite(v->Registers,0xb0088,0);
    if(NT_SUCCESS(status))status=GpuAllocateContiguous(&v->Queues,0x400000,MmNonCached,0xdfffffff,0x400000);
    v->Queues.Address=v->Queues.Physical;
    if(NT_SUCCESS(status)) {
        IRIS_QUEUES *table=(IRIS_QUEUES *)v->Queues.Cpu;
        table->Size=sizeof(*table);table->Offset=FIELD_OFFSET(IRIS_QUEUES,Queue);table->HeaderSize=sizeof(IRIS_QUEUE);table->Count=table->Active=3;
        RtlCopyMemory(table->Name,"iris-hfi-queues",15);
        for(ULONG i=0;i<3;i++) {
            IRIS_QUEUE *q=table->Queue+i;q->Status=1;q->Address=(ULONG)v->Queues.Address+sizeof(*table)+i*IRIS_QUEUE_BYTES;
            q->Type=0x01010000u|i;q->Size=IRIS_QUEUE_WORDS;q->RxWatermark=q->TxWatermark=1;q->RxRequest=i!=2;
        }
        v->Timer=ExAllocateTimer(NULL,NULL,EX_TIMER_HIGH_RESOLUTION);
        if(!v->Timer)status=STATUS_INSUFFICIENT_RESOURCES;
    }
    if(NT_SUCCESS(status))status=Firmware(v);
    if(NT_SUCCESS(status)) {
        ULONG address=(ULONG)v->Queues.Address,sfr=address+v->Queues.Size-4096;
        *(PULONG)(v->Queues.Cpu+v->Queues.Size-4096)=4096;KeMemoryBarrier();
        GpuWrite(v->Registers,0xb0010,GpuRead(v->Registers,0xb0010)|12);
        GpuWrite(v->Registers,0xb0078,1);GpuWrite(v->Registers,0xa0064,address);
        GpuWrite(v->Registers,0xa0068,(v->Queues.Size+0xfffff)&~0xfffffu);
        GpuWrite(v->Registers,0xa0054,address);GpuWrite(v->Registers,0xa0050,1);GpuWrite(v->Registers,0xa005c,sfr+1);
        GpuWrite(v->Registers,0xa0048,1);GpuWrite(v->Registers,0xa0058,1);
        ULONGLONG end=KeQueryInterruptTime()+1000000;ULONG value=0;
        while(!value && KeQueryInterruptTime()<end) {
            value=GpuRead(v->Registers,0xa004c);
            if(!value){ExSetTimer(v->Timer,-5000,0,NULL);KeWaitForSingleObject(v->Timer,Executive,KernelMode,FALSE,NULL);}
        }
        if(!value || (value&0xfe))status=STATUS_DEVICE_HARDWARE_ERROR;
        else {GpuWrite(v->Registers,0xa0148,1);GpuWrite(v->Registers,0xa0168,0);}
    }
    if(NT_SUCCESS(status))status=SystemInit(v);
    if(!NT_SUCCESS(status)) {
        NTSTATUS released=IrisDelete(v);
        if(!NT_SUCCESS(released)){*out=v;return released;}
        return status;
    }
    *out=v;return STATUS_SUCCESS;
}
