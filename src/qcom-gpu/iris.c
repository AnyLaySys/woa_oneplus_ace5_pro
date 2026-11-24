#include "iris.h"
#include "../platform/lock.h"

#define IRIS_QUEUE_WORDS 262144u
#define IRIS_QUEUE_BYTES (IRIS_QUEUE_WORDS*4u)
#define IRIS_FIRMWARE_BASE 0x9c300000ULL
#define IRIS_FIRMWARE_BYTES 0x800000u

typedef struct {
    ULONG Status,Address,Type,Size,PacketSize,Dropped,RxWatermark,TxWatermark;
    volatile ULONG RxRequest,TxRequest,RxInterrupt,TxInterrupt,Read,Write;
} IRIS_QUEUE;
typedef struct { ULONG Version,Size,Offset,HeaderSize,Count,Active;ULONGLONG Device;UCHAR Name[256];IRIS_QUEUE Queue[3]; } IRIS_QUEUES;
typedef struct { ULONG Size,Session,Id,Reserved[4],Count; } IRIS_HEADER;
typedef struct { ULONG Size,Type,Flags,Payload,Port,Id,Reserved[2]; } IRIS_PACKET;
struct IRIS {
    GPU *Gpu;
    PDRIVER_OBJECT Driver;
    PUCHAR Registers,Clocks,Smmu;
    GPU_BO Tables,Queues;
    ULONG TableUsed,NextAddress,Bank,CbBase,PageSize,Slots[2],Smr[2],S2cr[2],Cbar,Cba2r;
    ULONG SavedCb[10],SavedClocks[9],SavedGcc[2],ClockMask,VoteMask,HeaderId,PacketId;
    BOOLEAN Claimed,PasStarted,Prepared;
    PEX_TIMER Timer;
};
static const ULONG CbRegisters[]={0,4,0x20,0x24,0x28,0x2c,0x30,0x10,0x38,0x3c};
static const ULONG ClockRegisters[]={0x80a4,0x80f8,0x80d4,0x8034,0x8068,0x805c,0x804c,0x808c,0x807c};
static NTSTATUS Smc(ULONG function,ULONG count,ULONGLONG a,ULONGLONG b,ULONGLONG c,ULONGLONG e,PULONGLONG result) {
    ULONGLONG registers[8]={function,count,a,b,c,e,0,0};GpuSmc(registers);
    if(result)*result=registers[1];
    if(registers[0])return registers[0]==MAXULONGLONG?STATUS_NOT_SUPPORTED:STATUS_DEVICE_HARDWARE_ERROR;
    return STATUS_SUCCESS;
}
static NTSTATUS Pas(ULONG function,ULONGLONG a,ULONGLONG b,ULONG count) {
    ULONGLONG result=0;NTSTATUS status=Smc(function,count,9,a,b,0,&result);
    return NT_SUCCESS(status) && result?STATUS_DEVICE_HARDWARE_ERROR:status;
}
static NTSTATUS Contiguous(GPU_BO *bo,ULONG bytes) {
    PHYSICAL_ADDRESS low={0},high,skip={0};high.QuadPart=MAXULONG;
    bo->Size=(bytes+4095)&~4095u;
    bo->Cpu=MmAllocateContiguousMemorySpecifyCache(bo->Size,low,high,skip,MmNonCached);
    if(!bo->Cpu)return STATUS_INSUFFICIENT_RESOURCES;
    bo->Mdl=IoAllocateMdl(bo->Cpu,bo->Size,FALSE,FALSE,NULL);
    if(!bo->Mdl){MmFreeContiguousMemory(bo->Cpu);bo->Cpu=NULL;return STATUS_INSUFFICIENT_RESOURCES;}
    MmBuildMdlForNonPagedPool(bo->Mdl);bo->Contiguous=TRUE;
    bo->Physical=MmGetPhysicalAddress(bo->Cpu).QuadPart;RtlZeroMemory(bo->Cpu,bo->Size);
    return STATUS_SUCCESS;
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
    v->SavedGcc[0]=GpuRead(v->Gpu->Gcc,0x32018);v->SavedGcc[1]=GpuRead(v->Gpu->Gcc,0x32028);
    v->Prepared=TRUE;
    GpuWrite(v->Gpu->Gcc,0x32028,v->SavedGcc[1]|1);
    GpuWrite(v->Gpu->Gcc,0x32018,v->SavedGcc[0]|1);
    status=Branch(v,0);
    if(NT_SUCCESS(status))status=Branch(v,1);
    if(NT_SUCCESS(status))status=Branch(v,2);
    if(!NT_SUCCESS(status))return status;
    if(!(GpuRead(v->Clocks,0)&0x80000000u)) {
        GpuWrite(v->Clocks,0x10,0x25);GpuWrite(v->Clocks,0x14,0x8000);
        GpuWrite(v->Clocks,0x20,0x19660387);GpuWrite(v->Clocks,0x24,0x098060a0);GpuWrite(v->Clocks,0x28,0xb416cb20);
        GpuWrite(v->Clocks,0x18,0x400);GpuWrite(v->Clocks,0x1c,2);
        GpuWrite(v->Clocks,4,0);GpuWrite(v->Clocks,0,GpuRead(v->Clocks,0)|4);GpuWrite(v->Clocks,4,1);
        status=GpuPoll(v->Clocks,0,0x80000000u,0x80000000u,2000);
        if(!NT_SUCCESS(status))return status;
        GpuWrite(v->Clocks,0x18,GpuRead(v->Clocks,0x18)|3);GpuWrite(v->Clocks,0,GpuRead(v->Clocks,0)|1);
    }
    GpuWrite(v->Clocks,0x8004,0x100);GpuWrite(v->Clocks,0x8000,GpuRead(v->Clocks,0x8000)|1);
    status=GpuPoll(v->Clocks,0x8000,1,0,2000);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=3;i<=4;i++) {
        ULONG offset=ClockRegisters[i];v->ClockMask|=1u<<i;
        GpuWrite(v->Clocks,offset,(v->SavedClocks[i]&~0xfffff001u)|0x226800u);
        status=GpuPoll(v->Clocks,offset+4,1u<<16,1u<<16,2000);
        if(!NT_SUCCESS(status))return status;
    }
    GpuWrite(v->Clocks,0x8040,GpuRead(v->Clocks,0x8040)|0x3e00000);
    GpuWrite(v->Clocks,0x8074,GpuRead(v->Clocks,0x8074)|0x3e00000);
    GpuWrite(v->Clocks,0x9f24,GpuRead(v->Clocks,0x9f24)|1);
    status=Branch(v,5);if(NT_SUCCESS(status))status=Branch(v,6);
    if(!NT_SUCCESS(status))return status;
    GpuWrite(v->Clocks,0x8090,GpuRead(v->Clocks,0x8090)&~8u);
    status=GpuPoll(v->Clocks,0x8090,0xc00,0,2000);
    if(NT_SUCCESS(status))status=Branch(v,7);
    if(NT_SUCCESS(status))status=Branch(v,8);
    return status;
}
static NTSTATUS Flush(IRIS *v) {
    PUCHAR cb=v->Smmu+v->CbBase+v->Bank*v->PageSize;
    KeMemoryBarrier();GpuWrite(cb,0x618,0);GpuWrite(cb,0x7f0,0);
    return GpuPoll(cb,0x7f4,1,0,2000);
}
static NTSTATUS Map(IRIS *v,GPU_BO *bo) {
    if(bo->Size>0xe0000000u-v->NextAddress)return STATUS_INSUFFICIENT_RESOURCES;
    bo->Address=v->NextAddress;v->NextAddress+=bo->Size;
    PPFN_NUMBER pages=MmGetMdlPfnArray(bo->Mdl);
    for(ULONG i=0;i<bo->Size/4096;i++) {
        ULONGLONG address=bo->Address+i*4096ULL;PULONGLONG table=(PULONGLONG)v->Tables.Cpu;
        for(ULONG shift=30;shift>12;shift-=9) {
            PULONGLONG entry=table+((address>>shift)&511);
            if(!*entry) {
                if(v->TableUsed>=v->Tables.Size/4096)return STATUS_INSUFFICIENT_RESOURCES;
                *entry=(v->Tables.Physical+(v->TableUsed++*4096ULL))|3;
            }
            ULONGLONG offset=(*entry&0xfffffffff000ULL)-v->Tables.Physical;
            if(offset>=v->Tables.Size)return STATUS_DEVICE_CONFIGURATION_ERROR;
            table=(PULONGLONG)(v->Tables.Cpu+offset);
        }
        table[(address>>12)&511]=((ULONGLONG)pages[i]<<12)|0x60000000000f43ULL;
    }
    return Flush(v);
}
static NTSTATUS Translation(IRIS *v) {
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(!NT_SUCCESS(status))return status;
    ULONG id0=GpuRead(v->Smmu,0x20),id1=GpuRead(v->Smmu,0x24),count=min(id0&255u,128u),banks=id1&255u;
    v->PageSize=id1&0x80000000u?65536:4096;
    v->CbBase=v->PageSize<<(1+((id1>>28)&7));
    if(!(id0&(1u<<30)) || !(id0&(1u<<27)) || !count || !banks || v->CbBase>0x200000 || banks>(0x200000-v->CbBase)/v->PageSize) {
        status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;
    }
    BOOLEAN used[256]={0};
    for(ULONG i=0;i<count;i++) {
        ULONG smr=GpuRead(v->Smmu,0x800+i*4),s2cr=GpuRead(v->Smmu,0xc00+i*4);
        if(!(smr&0x80000000u))continue;
        if(((s2cr>>16)&3)==0)used[s2cr&255]=TRUE;
        ULONG mask=(smr>>16)&0x7fff;
        for(ULONG j=0;j<2;j++)if(!((smr^(j?0x1947u:0x1940u))&0x7fffu&~mask)) {
            if(mask>7 || v->Slots[j]!=MAXULONG){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
            v->Slots[j]=i;
        }
    }
    for(ULONG j=0;j<2;j++)if(v->Slots[j]==MAXULONG) {
        for(ULONG i=0;i<count;i++)if(!(GpuRead(v->Smmu,0x800+i*4)&0x80000000u) && (!j || i!=v->Slots[0])){v->Slots[j]=i;break;}
        if(v->Slots[j]==MAXULONG){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    }
    for(ULONG i=banks;i--;)if(!used[i] && !(GpuRead(v->Smmu,v->CbBase+i*v->PageSize)&1)){v->Bank=i;break;}
    if(v->Bank==MAXULONG){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    PUCHAR cb=v->Smmu+v->CbBase+v->Bank*v->PageSize;
    v->Cbar=GpuRead(v->Smmu,v->PageSize+v->Bank*4);v->Cba2r=GpuRead(v->Smmu,v->PageSize+0x800+v->Bank*4);
    for(ULONG i=0;i<RTL_NUMBER_OF(CbRegisters);i++)v->SavedCb[i]=GpuRead(cb,CbRegisters[i]);
    for(ULONG j=0;j<2;j++){v->Smr[j]=GpuRead(v->Smmu,0x800+v->Slots[j]*4);v->S2cr[j]=GpuRead(v->Smmu,0xc00+v->Slots[j]*4);}
    v->Claimed=TRUE;
    GpuWrite(v->Smmu,v->PageSize+0x800+v->Bank*4,1);GpuWrite(v->Smmu,v->PageSize+v->Bank*4,0x1f300);
    GpuWrite(cb,0x10,0x38014);GpuWrite(cb,0x30,0x803520);
    GpuWrite(cb,0x20,(ULONG)v->Tables.Physical);GpuWrite(cb,0x24,(ULONG)(v->Tables.Physical>>32)|(v->Bank<<16));
    GpuWrite(cb,0x28,0);GpuWrite(cb,0x2c,0);GpuWrite(cb,0x38,0x04ff44);GpuWrite(cb,0x3c,0);
    GpuWrite(cb,0,0x1027);KeMemoryBarrier();
    if(GpuRead(cb,0)!=0x1027){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
    for(ULONG j=0;j<2;j++) {
        GpuWrite(v->Smmu,0xc00+v->Slots[j]*4,v->Bank);
        if(!(v->Smr[j]&0x80000000u))GpuWrite(v->Smmu,0x800+v->Slots[j]*4,0x80000000u|(j?0x1947u:0x1940u));
    }
    status=Flush(v);
done:{NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;}
}
static NTSTATUS Firmware(IRIS *v) {
    HANDLE directory=NULL;NTSTATUS status=IoGetDriverDirectory(v->Driver,DriverDirectoryImage,0,&directory);
    if(!NT_SUCCESS(status))return status;
    PUCHAR data=NULL;ULONG size=0;status=GpuFirmware(directory,L"vpu35_4v.mbn",IRIS_FIRMWARE_BYTES,&data,&size);
    ZwClose(directory);
    if(!NT_SUCCESS(status))return status;
    GPU_BO metadata={0};PUCHAR memory=NULL;
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
    status=Contiguous(&metadata,header+hashSize);if(!NT_SUCCESS(status))goto done;
    RtlCopyMemory(metadata.Cpu,data,header);RtlCopyMemory(metadata.Cpu+header,data+hashOffset,hashSize);KeMemoryBarrier();
    ULONGLONG result=0;status=Smc(0xc2000201,0x22,9,metadata.Physical,0,0,&result);
    if(!NT_SUCCESS(status) || result){if(NT_SUCCESS(status))status=STATUS_INVALID_IMAGE_HASH;goto done;}
    v->PasStarted=TRUE;
    status=Pas(0xc2000202,IRIS_FIRMWARE_BASE,IRIS_FIRMWARE_BYTES,3);if(!NT_SUCCESS(status))goto done;
    PHYSICAL_ADDRESS physical;physical.QuadPart=IRIS_FIRMWARE_BASE;
    memory=MmMapIoSpaceEx(physical,IRIS_FIRMWARE_BYTES,PAGE_READWRITE|PAGE_NOCACHE);
    if(!memory){status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    RtlZeroMemory(memory,IRIS_FIRMWARE_BYTES);
    for(ULONG i=0;i<count;i++) {
        UNALIGNED ULONG *p=(UNALIGNED ULONG *)(data+offset+i*32);
        if(p[0]==1 && p[5] && (p[6]&0x07000000u)!=0x02000000u && p[4])RtlCopyMemory(memory+p[3]-minimum,data+p[1],p[4]);
    }
    KeMemoryBarrier();status=Pas(0xc2000205,0,0,1);
done:if(memory)MmUnmapIoSpace(memory,IRIS_FIRMWARE_BYTES);GpuFree(&metadata);if(data)ExFreePoolWithTag(data,GPU_TAG);return status;
}
static NTSTATUS Send(IRIS *v,ULONG type,ULONG flags,ULONG payload,ULONG session,ULONG port,const VOID *data,ULONG bytes,PULONG id) {
    ULONG packet[256]={0},words=(sizeof(IRIS_HEADER)+sizeof(IRIS_PACKET)+bytes)/4;
    if((bytes&3) || words>RTL_NUMBER_OF(packet))return STATUS_INVALID_BUFFER_SIZE;
    IRIS_HEADER *header=(IRIS_HEADER *)packet;IRIS_PACKET *p=(IRIS_PACKET *)(header+1);
    header->Size=words*4;header->Session=session;header->Id=++v->HeaderId;header->Count=1;
    p->Size=sizeof(*p)+bytes;p->Type=type;p->Flags=flags;p->Payload=payload;p->Port=port;p->Id=++v->PacketId;
    if(id)*id=p->Id;if(bytes)RtlCopyMemory(p+1,data,bytes);
    IRIS_QUEUE *q=&((IRIS_QUEUES *)v->Queues.Cpu)->Queue[0];ULONG write=q->Write,read=q->Read;
    if(write>=IRIS_QUEUE_WORDS || read>=IRIS_QUEUE_WORDS)return STATUS_DEVICE_DATA_ERROR;
    ULONG available=(read+IRIS_QUEUE_WORDS-write-1)%IRIS_QUEUE_WORDS;
    if(words>available)return STATUS_DEVICE_BUSY;
    PULONG buffer=(PULONG)(v->Queues.Cpu+sizeof(IRIS_QUEUES));
    for(ULONG i=0;i<words;i++)buffer[(write+i)%IRIS_QUEUE_WORDS]=packet[i];
    KeMemoryBarrier();q->Write=(write+words)%IRIS_QUEUE_WORDS;KeMemoryBarrier();GpuWrite(v->Registers,0xa0150,1);
    return STATUS_SUCCESS;
}
static NTSTATUS Response(IRIS *v,ULONG id) {
    IRIS_QUEUE *q=&((IRIS_QUEUES *)v->Queues.Cpu)->Queue[1];PULONG buffer=(PULONG)(v->Queues.Cpu+sizeof(IRIS_QUEUES)+IRIS_QUEUE_BYTES);
    ULONGLONG deadline=KeQueryInterruptTime()+10000000;
    while(KeQueryInterruptTime()<deadline) {
        if(GpuRead(v->Registers,0xb000c)&8)return STATUS_DEVICE_HARDWARE_ERROR;
        if(GpuRead(v->Smmu,v->CbBase+v->Bank*v->PageSize+0x58)&0xc00001feu)return STATUS_DEVICE_DATA_ERROR;
        ULONG read=q->Read,write=q->Write;KeMemoryBarrier();
        if(read>=IRIS_QUEUE_WORDS || write>=IRIS_QUEUE_WORDS)return STATUS_DEVICE_DATA_ERROR;
        if(read!=write) {
            ULONG packet[256],bytes=buffer[read],available=(write+IRIS_QUEUE_WORDS-read)%IRIS_QUEUE_WORDS;
            if((bytes&3) || bytes<sizeof(IRIS_HEADER) || bytes>sizeof(packet) || bytes/4>available)return STATUS_DEVICE_DATA_ERROR;
            for(ULONG i=0;i<bytes/4;i++)packet[i]=buffer[(read+i)%IRIS_QUEUE_WORDS];
            KeMemoryBarrier();q->Read=(read+bytes/4)%IRIS_QUEUE_WORDS;q->RxRequest=1;KeMemoryBarrier();
            IRIS_HEADER *header=(IRIS_HEADER *)packet;ULONG offset=sizeof(*header);
            for(ULONG i=0;i<header->Count;i++) {
                if(offset>bytes || sizeof(IRIS_PACKET)>bytes-offset)return STATUS_DEVICE_DATA_ERROR;
                IRIS_PACKET *p=(IRIS_PACKET *)((PUCHAR)packet+offset);
                if(p->Size<sizeof(*p) || p->Size>bytes-offset || (p->Size&3))return STATUS_DEVICE_DATA_ERROR;
                if(p->Flags&12)return STATUS_DEVICE_HARDWARE_ERROR;
                if(p->Id==id)return p->Flags&1?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
                offset+=p->Size;
            }
            continue;
        }
        ExSetTimer(v->Timer,-5000,0,NULL);KeWaitForSingleObject(v->Timer,Executive,KernelMode,FALSE,NULL);
    }
    return STATUS_IO_TIMEOUT;
}
NTSTATUS IrisDelete(IRIS *v) {
    if(!v)return STATUS_SUCCESS;
    if(v->PasStarted) {
        NTSTATUS status=Pas(0xc2000206,0,0,1);if(!NT_SUCCESS(status))return status;
        v->PasStarted=FALSE;
    }
    if(v->Claimed) {
        PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
        if(!NT_SUCCESS(status))return status;
        for(ULONG i=0;i<2;i++){GpuWrite(v->Smmu,0x800+v->Slots[i]*4,v->Smr[i]);GpuWrite(v->Smmu,0xc00+v->Slots[i]*4,v->S2cr[i]);}
        PUCHAR cb=v->Smmu+v->CbBase+v->Bank*v->PageSize;
        GpuWrite(cb,0,0);status=Flush(v);
        for(ULONG i=RTL_NUMBER_OF(CbRegisters);i--;)GpuWrite(cb,CbRegisters[i],v->SavedCb[i]);
        GpuWrite(v->Smmu,v->PageSize+v->Bank*4,v->Cbar);GpuWrite(v->Smmu,v->PageSize+0x800+v->Bank*4,v->Cba2r);
        NTSTATUS released=PlatformLockRelease(&guard);if(!NT_SUCCESS(status))return status;if(!NT_SUCCESS(released))return released;
        v->Claimed=FALSE;
    }
    if(v->Prepared) {
        for(ULONG i=RTL_NUMBER_OF(ClockRegisters);i--;)if(v->ClockMask&(1u<<i))GpuWrite(v->Clocks,ClockRegisters[i],v->SavedClocks[i]);
        GpuWrite(v->Gpu->Gcc,0x32018,v->SavedGcc[0]);GpuWrite(v->Gpu->Gcc,0x32028,v->SavedGcc[1]);
    }
    if(v->VoteMask&2){NTSTATUS status=Arc(v,"mmcx.lvl",0);if(!NT_SUCCESS(status))return status;v->VoteMask&=~2u;}
    if(v->VoteMask&1){NTSTATUS status=Arc(v,"mxc.lvl",0);if(!NT_SUCCESS(status))return status;v->VoteMask&=~1u;}
    if(v->Timer)ExDeleteTimer(v->Timer,TRUE,TRUE,NULL);
    GpuFree(&v->Queues);GpuFree(&v->Tables);
    if(v->Registers)MmUnmapIoSpace(v->Registers,0xf0000);
    if(v->Clocks)MmUnmapIoSpace(v->Clocks,0xa000);
    if(v->Smmu)MmUnmapIoSpace(v->Smmu,0x200000);
    ExFreePoolWithTag(v,GPU_TAG);return STATUS_SUCCESS;
}
NTSTATUS IrisCreate(GPU *gpu,PDRIVER_OBJECT driver,IRIS **out) {
    if(!gpu || !gpu->Online || !out || *out)return STATUS_INVALID_DEVICE_STATE;
    IRIS *v=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*v),GPU_TAG);if(!v)return STATUS_INSUFFICIENT_RESOURCES;
    v->Gpu=gpu;v->Driver=driver;v->Bank=v->Slots[0]=v->Slots[1]=MAXULONG;v->NextAddress=0x80000000;v->TableUsed=1;
    PHYSICAL_ADDRESS p;p.QuadPart=0xaa00000;v->Registers=MmMapIoSpaceEx(p,0xf0000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0xaaf0000;v->Clocks=MmMapIoSpaceEx(p,0xa000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x15000000;v->Smmu=MmMapIoSpaceEx(p,0x200000,PAGE_READWRITE|PAGE_NOCACHE);
    NTSTATUS status=v->Registers && v->Clocks && v->Smmu?STATUS_SUCCESS:STATUS_INSUFFICIENT_RESOURCES;
    ULONGLONG supported=0;
    if(NT_SUCCESS(status))status=Smc(0xc2000207,1,9,0,0,0,&supported);
    if(NT_SUCCESS(status) && !supported)status=STATUS_NOT_SUPPORTED;
    if(NT_SUCCESS(status))status=Contiguous(&v->Tables,0x60000);
    if(NT_SUCCESS(status))status=Power(v);
    if(NT_SUCCESS(status))status=Translation(v);
    if(NT_SUCCESS(status))status=GpuAllocate(&v->Queues,((sizeof(IRIS_QUEUES)+3*IRIS_QUEUE_BYTES+4095)&~4095u)+4096);
    if(NT_SUCCESS(status))status=Map(v,&v->Queues);
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
    if(NT_SUCCESS(status)) {
        ULONG payload=1,id=0;status=Send(v,0x01000001,7,1,0,0,&payload,4,&id);
        if(NT_SUCCESS(status))status=Response(v,id);
    }
    if(!NT_SUCCESS(status)) {
        NTSTATUS released=IrisDelete(v);
        if(!NT_SUCCESS(released)){*out=v;return released;}
        return status;
    }
    *out=v;return STATUS_SUCCESS;
}
