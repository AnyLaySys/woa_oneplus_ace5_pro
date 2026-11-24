#include "adapter.h"
#include "dpu.h"
#include "dpu-completion.h"

#define DPU_REGISTER_BYTES 0x90000u
static ULONGLONG PreciseTime(VOID) {
    ULONGLONG qpc;
    return KeQueryInterruptTimePrecise(&qpc);
}
typedef struct {
    ULONG ProofMask,Cn1Address,Cn1Vote,DispPower,DispBranches[3],HwVersion;
    ULONG SmmuId0,SmmuId1,Slots[2],Smr[2],S2cr[2],Sctlr[2],Cbar[2];
    ULONG AddedAhb;
} DPU_PLATFORM;
static DPU_PLATFORM Platform;
static PVOID PlatformAdapter;
static NTSTATUS WaitBranch(PUCHAR mapping,ULONG offset,PULONG observed) {
    ULONGLONG end=KeQueryInterruptTime()+20000;
    do {
        ULONG value=READ_REGISTER_ULONG((PULONG)(mapping+offset));*observed=value;
        if(value==MAXULONG)return STATUS_IO_DEVICE_ERROR;

        if((value&1) && (!(value&0x80000000u) || ((value>>28)&7u)==2))return STATUS_SUCCESS;
        KeStallExecutionProcessor(10);
    } while(KeQueryInterruptTime()<end);
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS RestoreAhb(A830_ADAPTER *a) {
    if(PlatformAdapter!=a || !Platform.AddedAhb)return STATUS_SUCCESS;
    PHYSICAL_ADDRESS p;p.QuadPart=0xaf08000;
    PUCHAR mapping=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
    if(!mapping)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG value=READ_REGISTER_ULONG((PULONG)(mapping+0xb0));
    if(value==MAXULONG){MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_IO_DEVICE_ERROR;}
    if(Platform.AddedAhb&1) {

        WRITE_REGISTER_ULONG((PULONG)(mapping+0xb0),value&~1u);KeMemoryBarrier();
        value=READ_REGISTER_ULONG((PULONG)(mapping+0xb0));
        if(value==MAXULONG || (value&1)){MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_IO_DEVICE_ERROR;}
        Platform.DispBranches[0]=value;Platform.ProofMask&=~4u;Platform.AddedAhb&=~1u;
    }
    MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_SUCCESS;
}
static NTSTATUS EnableAhb(A830_ADAPTER *a) {
    if(PlatformAdapter!=a || (Platform.ProofMask&3)!=3 || A830DpuEnabled(a))return STATUS_DEVICE_NOT_READY;
    KeMemoryBarrier();
    PHYSICAL_ADDRESS p;p.QuadPart=0xaf08000;
    PUCHAR mapping=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READWRITE|PAGE_NOCACHE);
    if(!mapping)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG value=READ_REGISTER_ULONG((PULONG)(mapping+0xb0));
    if(value==MAXULONG){MmUnmapIoSpace(mapping,PAGE_SIZE);return STATUS_IO_DEVICE_ERROR;}
    if(!(value&1)) {

        Platform.AddedAhb|=1;KeMemoryBarrier();
        WRITE_REGISTER_ULONG((PULONG)(mapping+0xb0),value|1);KeMemoryBarrier();
    }
    NTSTATUS status=WaitBranch(mapping,0xb0,&Platform.DispBranches[0]);
    MmUnmapIoSpace(mapping,PAGE_SIZE);
    if(NT_SUCCESS(status))Platform.ProofMask|=4;
    else {
        Platform.ProofMask&=~4u;
        NTSTATUS restored=RestoreAhb(a);if(!NT_SUCCESS(restored))return restored;
    }
    return status;
}
static BOOLEAN TranslationProved(A830_ADAPTER *a) {
    if(PlatformAdapter!=a || (Platform.ProofMask&0x1ff)!=0x1ff)return FALSE;
    if(!(Platform.DispPower&(1u<<16)) || !Platform.HwVersion)return FALSE;
    for(ULONG i=0;i<3;i++)if(!(Platform.DispBranches[i]&1))return FALSE;
    for(ULONG i=0;i<2;i++) {
        ULONG smr=Platform.Smr[i],mask=(smr>>16)&0x7fffu;
        if(!(smr&0x80000000u) || (mask!=0 && mask!=2) ||
           (((smr&0xffffu)&~mask)!=0x800u && ((smr&0xffffu)&~mask)!=0x802u))return FALSE;
        ULONG type=(Platform.S2cr[i]>>16)&3u;
        if(type==1)continue;
        if(type || !(Platform.ProofMask&(0x200u<<i)) || !(Platform.ProofMask&(0x800u<<i)) ||
           (Platform.Sctlr[i]&1) || ((Platform.Cbar[i]>>16)&3u)!=1)return FALSE;
    }
    return TRUE;
}
static NTSTATUS ReadPlatform(ULONGLONG physical,PULONG value) {
    KeMemoryBarrier();
    PHYSICAL_ADDRESS p;p.QuadPart=(LONGLONG)(physical&~(ULONGLONG)(PAGE_SIZE-1));
    PUCHAR mapping=MmMapIoSpaceEx(p,PAGE_SIZE,PAGE_READONLY|PAGE_NOCACHE);
    if(!mapping)return STATUS_INSUFFICIENT_RESOURCES;
    *value=READ_REGISTER_ULONG((PULONG)(mapping+(ULONG)(physical&(PAGE_SIZE-1))));
    MmUnmapIoSpace(mapping,PAGE_SIZE);
    return *value==MAXULONG?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
}
static NTSTATUS PreparePlatform(A830_ADAPTER *a) {
    RtlZeroMemory(&Platform,sizeof(Platform));PlatformAdapter=a;
    GPU *g=&a->Hardware;ULONG aux=0,length=0;
    NTSTATUS status=GpuDb(g,"CN1",&Platform.Cn1Address,&aux,&length);
    if(!NT_SUCCESS(status))return status;
    if(length<8 || (aux&3) || aux>0x20000-8)return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG unit=GpuRead(g->Db,aux),width=GpuRead(g->Db,aux+4)&0xffffu;
    if(!unit || !width)return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONGLONG scaled=((ULONGLONG)57000*width/4)*1000;
    ULONGLONG vote=scaled/unit;if(scaled && !vote)vote=1;
    if(vote>0x3fff)return STATUS_INTEGER_OVERFLOW;
    Platform.Cn1Vote=0x60000000u|(ULONG)vote;
    status=GpuRscWrite(g,Platform.Cn1Address,Platform.Cn1Vote);
    if(!NT_SUCCESS(status))return status;
    Platform.ProofMask=1;
    ULONG ahb=GpuRead(g->Gcc,0x27004),axi=GpuRead(g->Gcc,0x27008);
    if(ahb==MAXULONG || axi==MAXULONG || !(ahb&1) || !(axi&1))return STATUS_DEVICE_NOT_READY;
    status=ReadPlatform(0xaf09004,&Platform.DispPower);
    if(!NT_SUCCESS(status))return status;
    if(!(Platform.DispPower&(1u<<16)))return STATUS_DEVICE_NOT_READY;
    Platform.ProofMask|=2;
    static const ULONG branches[]={0x80b0,0x8010,0x8030};
    for(ULONG i=0;i<RTL_NUMBER_OF(branches);i++) {
        status=ReadPlatform(0xaf00000ULL+branches[i],Platform.DispBranches+i);
        if(!NT_SUCCESS(status))return status;
        if(Platform.DispBranches[i]&1)Platform.ProofMask|=4u<<i;
    }
    if(!(Platform.DispBranches[0]&1)){status=EnableAhb(a);if(!NT_SUCCESS(status))return status;}
    if((Platform.ProofMask&0x1f)!=0x1f)return STATUS_DEVICE_NOT_READY;
    status=ReadPlatform(0xae00000,&Platform.HwVersion);
    if(!NT_SUCCESS(status))return status;
    if(!Platform.HwVersion)return STATUS_DEVICE_NOT_READY;
    Platform.ProofMask|=0x20;
    status=ReadPlatform(0x15000020,&Platform.SmmuId0);
    if(NT_SUCCESS(status))status=ReadPlatform(0x15000024,&Platform.SmmuId1);
    if(!NT_SUCCESS(status))return status;
    if(!(Platform.SmmuId0&(1u<<27)) || !(Platform.SmmuId0&255))return STATUS_DEVICE_CONFIGURATION_ERROR;
    Platform.ProofMask|=0xc0;Platform.Slots[0]=Platform.Slots[1]=MAXULONG;
    for(ULONG slot=0;slot<min(Platform.SmmuId0&255u,128u);slot++) {
        ULONG smr=0;status=ReadPlatform(0x15000800ULL+slot*4,&smr);
        if(!NT_SUCCESS(status))return status;
        if(!(smr&0x80000000u))continue;
        ULONG mask=(smr>>16)&0x7fffu;
        for(ULONG i=0;i<2;i++) {
            if(((smr^(0x800u+i*2))&0xffffu&~mask)!=0)continue;
            if(Platform.Slots[i]!=MAXULONG)return STATUS_DEVICE_CONFIGURATION_ERROR;
            Platform.Slots[i]=slot;Platform.Smr[i]=smr;
            status=ReadPlatform(0x15000c00ULL+slot*4,Platform.S2cr+i);
            if(!NT_SUCCESS(status))return status;
        }
    }
    if(Platform.Slots[0]==MAXULONG || Platform.Slots[1]==MAXULONG)return STATUS_NOT_FOUND;
    Platform.ProofMask|=0x100;
    for(ULONG i=0;i<2;i++) {
        ULONG bank=Platform.S2cr[i]&255u,shift=(Platform.SmmuId1&0x80000000u)?16u:12u;
        ULONG pages=1u<<(((Platform.SmmuId1>>28)&7u)+1);
        ULONGLONG offset=(ULONGLONG)(pages+bank)<<shift;
        if(bank>=(Platform.SmmuId1&255u) || offset>=0x100000)return STATUS_DEVICE_CONFIGURATION_ERROR;
        status=ReadPlatform(0x15000000+offset,Platform.Sctlr+i);
        if(NT_SUCCESS(status))status=ReadPlatform(0x15000000+(1ULL<<shift)+bank*4,Platform.Cbar+i);
        if(!NT_SUCCESS(status))return status;
        Platform.ProofMask|=(0x200u<<i)|(0x800u<<i);
    }
    return TranslationProved(a)?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;
}
typedef struct { ULONG Base,Flush,Active; } DPU_PIPE_DESCRIPTION;
static const DPU_PIPE_DESCRIPTION PipeDescriptions[]={
    {0x4000,1u<<0,1u<<16},{0x6000,1u<<1,1u<<17},
    {0x8000,1u<<2,1u<<18},{0xa000,1u<<18,1u<<19},
    {0x24000,1u<<11,1u<<0},{0x26000,1u<<12,1u<<1},
    {0x28000,1u<<24,1u<<2},{0x2a000,1u<<25,1u<<3},
    {0x2c000,1u<<13,1u<<4},{0x2e000,1u<<14,1u<<5}
};
typedef struct { ULONG Base,OriginalAddress,OriginalStride,Offset,OriginalFormat,OriginalUnpack,OriginalColor[11],Format;BOOLEAN Hdr; } DPU_PIPE;
typedef struct { ULONG Base,Mask; } DPU_CTL;
typedef struct {
    A830_ADAPTER *Adapter;
    PUCHAR Registers,Smmu,Gcc,Dispcc;
    ULONG ClockAdded;
    PEX_TIMER Timer;
    DPU_PIPE Pipes[10];ULONG PipeCount;
    DPU_CTL Ctls[6];ULONG CtlCount;
    ULONG Intf,AutoRefresh;
    BOOLEAN Restored;
    volatile LONG Stopping,Enabled,Vsync;
    volatile LONG64 Queued,Applied;
    ULONGLONG PendingAddress;
    ULONG PendingPitch,PendingFormat;BOOLEAN PendingHdr;
    volatile LONG64 OwnershipSequence;
    volatile LONG64 PendingPhysical,ActivePhysical;
    volatile LONG64 TransactionPhysical;
    LONG64 TransactionSequence;
    ULONGLONG TransactionAddress;
    ULONG TransactionPitch,TransactionFrame,TransactionState,TransactionSync,TransactionAuto,PpMask,TransactionFormat;
    BOOLEAN TransactionHdr;
    BOOLEAN TransactionGated;
    volatile LONG Kick;
    volatile LONG WakeNeeded;
    ULONG Line;
    ULONGLONG LastEdge,LastSample,EdgeError,Period,TimerLate,PollBegin,PollDue,LineTime,TransactionWait;
} A830_DPU;
static BOOLEAN AdvanceTransaction(PVOID context);
static VOID SampleEdge(A830_DPU *d,ULONGLONG now) {
    if(d->LastEdge) {
        ULONGLONG elapsed=now-d->LastEdge;
        if(elapsed>=d->Period*16 && elapsed<=d->Period*128) {
            ULONGLONG frames=(elapsed+d->Period/2)/d->Period;
            d->Period=(d->Period*3+elapsed/frames)/4;
        }
    }
    d->LastEdge=now;
}
static VOID SetPoll(A830_DPU *d,ULONGLONG due) {
    if(d->Stopping || !d->Enabled || d->TransactionState==4 || !d->Timer)return;
    if(d->PollDue && d->PollDue<=due)return;
    ULONGLONG now=PreciseTime();d->PollDue=due;
    ExSetTimer(d->Timer,due>now?-(LONGLONG)(due-now):-1,0,NULL);
}
VOID A830DpuWork(A830_ADAPTER *a) {
    BOOLEAN wake=FALSE;KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d && d->Enabled && !d->Stopping && d->TransactionState!=4 && InterlockedExchange(&d->Kick,0)) {
        BOOLEAN result=FALSE;
        NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,AdvanceTransaction,d,0,&result);
        if(!NT_SUCCESS(status))InterlockedExchange(&a->Failed,status);
        if(d->Vsync || d->TransactionState || ReadAcquire64(&d->Queued)>ReadAcquire64(&d->Applied))
            SetPoll(d,PreciseTime()+2000);
    }
    if(d)wake=InterlockedExchange(&d->WakeNeeded,0)!=0;
    KeReleaseSpinLock(&a->QueueLock,old);
    if(wake)KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);
}

static NTSTATUS Clocks(A830_DPU *d) {
    PHYSICAL_ADDRESS p;p.QuadPart=0x100000;
    d->Gcc=MmMapIoSpaceEx(p,0x30000,PAGE_READWRITE|PAGE_NOCACHE);
    if(!d->Gcc)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG axi=READ_REGISTER_ULONG((PULONG)(d->Gcc+0x27008));
    if(axi==MAXULONG || !(axi&1))return STATUS_NOT_SUPPORTED;
    ULONG ahb=READ_REGISTER_ULONG((PULONG)(d->Gcc+0x27004));
    if(ahb==MAXULONG)return STATUS_NOT_SUPPORTED;
    if(!(ahb&1)){WRITE_REGISTER_ULONG((PULONG)(d->Gcc+0x27004),ahb|1);d->ClockAdded|=1;}
    NTSTATUS status=(ahb&2)?STATUS_SUCCESS:GpuPoll(d->Gcc,0x27004,0x80000000u,0,2000);
    if(!NT_SUCCESS(status))return status;
    p.QuadPart=0xaf00000;
    d->Dispcc=MmMapIoSpaceEx(p,0x20000,PAGE_READWRITE|PAGE_NOCACHE);
    if(!d->Dispcc)return STATUS_INSUFFICIENT_RESOURCES;
    ULONG domain=READ_REGISTER_ULONG((PULONG)(d->Dispcc+0x9004));
    if(domain==MAXULONG || !(domain&(1u<<16)))return STATUS_NOT_SUPPORTED;
    static const ULONG branches[]={0x80b0,0x8010,0x8030};
    for(ULONG i=0;i<RTL_NUMBER_OF(branches);i++) {
        ULONG value=READ_REGISTER_ULONG((PULONG)(d->Dispcc+branches[i]));
        if(value==MAXULONG)return STATUS_NOT_SUPPORTED;
        if(!(value&1)) {
            WRITE_REGISTER_ULONG((PULONG)(d->Dispcc+branches[i]),value|1);
            d->ClockAdded|=2u<<i;
        }
        status=GpuPoll(d->Dispcc,branches[i],0x80000000u,0,2000);
        if(!NT_SUCCESS(status))return status;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS ReleaseClocks(A830_DPU *d) {
    static const ULONG branches[]={0x80b0,0x8010,0x8030};
    if(d->Dispcc) {
        for(LONG i=RTL_NUMBER_OF(branches)-1;i>=0;i--)if(d->ClockAdded&(2u<<i)) {
            PULONG reg=(PULONG)(d->Dispcc+branches[i]);
            ULONG value=READ_REGISTER_ULONG(reg);if(value==MAXULONG)return STATUS_IO_DEVICE_ERROR;
            WRITE_REGISTER_ULONG(reg,value&~1u);KeMemoryBarrier();value=READ_REGISTER_ULONG(reg);
            if(value==MAXULONG || (value&1))return STATUS_IO_DEVICE_ERROR;
            d->ClockAdded&=~(2u<<i);
            if(PlatformAdapter==d->Adapter) {
                Platform.DispBranches[i]=value;Platform.ProofMask&=~(4u<<i);
            }
        }
        MmUnmapIoSpace(d->Dispcc,0x20000);d->Dispcc=NULL;
    }
    if(d->Gcc) {
        if(d->ClockAdded&1) {
            PULONG reg=(PULONG)(d->Gcc+0x27004);
            ULONG value=READ_REGISTER_ULONG(reg);if(value==MAXULONG)return STATUS_IO_DEVICE_ERROR;
            WRITE_REGISTER_ULONG(reg,value&~1u);KeMemoryBarrier();value=READ_REGISTER_ULONG(reg);
            if(value==MAXULONG || (value&1))return STATUS_IO_DEVICE_ERROR;
            d->ClockAdded&=~1u;
        }
        MmUnmapIoSpace(d->Gcc,0x30000);d->Gcc=NULL;
    }
    return STATUS_SUCCESS;
}
static ULONG Read(A830_DPU *d,ULONG offset) {
    return READ_REGISTER_ULONG((PULONG)(d->Registers+offset));
}
static VOID Write(A830_DPU *d,ULONG offset,ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(d->Registers+offset),value);
}
static ULONG TxFrame(A830_DPU *d) {return Read(d,d->Intf+0xac);}
static VOID Flush(A830_DPU *d) {
    KeMemoryBarrier();
    for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x18,d->Ctls[i].Mask);

    if(!d->AutoRefresh)for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x1c,1);
}
static BOOLEAN FlushComplete(A830_DPU *d) {
    for(ULONG i=0;i<d->CtlCount;i++)if(Read(d,d->Ctls[i].Base+0x18)&d->Ctls[i].Mask)return FALSE;
    return TRUE;
}
static VOID RestoreTransaction(A830_DPU *d) {
    if(!d->TransactionGated)return;

    Write(d,d->Intf+0x2b4,d->TransactionAuto);
    Write(d,d->Intf+0x288,d->TransactionSync);KeMemoryBarrier();
    d->TransactionGated=FALSE;
}
static VOID CompleteTransaction(A830_DPU *d) {
    InterlockedIncrement64(&d->OwnershipSequence);
    InterlockedExchange64((volatile LONG64 *)&d->Adapter->ScanoutAddress,(LONG64)d->TransactionAddress);
    InterlockedExchange64(&d->ActivePhysical,ReadAcquire64(&d->TransactionPhysical));
    InterlockedExchange64(&d->Applied,d->TransactionSequence);

    InterlockedExchange64(&d->TransactionPhysical,0);d->TransactionState=0;
    InterlockedIncrement64(&d->OwnershipSequence);

    InterlockedExchange(&d->WakeNeeded,1);
}
static VOID FailTransaction(A830_DPU *d,NTSTATUS status) {
    RestoreTransaction(d);d->TransactionState=4;
    InterlockedExchange(&d->Adapter->Failed,status);InterlockedExchange(&d->WakeNeeded,1);
}

static BOOLEAN AdvanceTransaction(PVOID context) {
    A830_DPU *d=context;
    if(d->Stopping || !d->Enabled || d->TransactionState==4)return TRUE;
    if(!d->TransactionState) {
        LONG64 sequence=ReadAcquire64(&d->Queued);
        if((sequence&1) || sequence<=ReadAcquire64(&d->Applied))return TRUE;
        InterlockedIncrement64(&d->OwnershipSequence);
        d->TransactionSequence=sequence;d->TransactionAddress=d->PendingAddress;
        d->TransactionPitch=d->PendingPitch;
        d->TransactionFormat=d->PendingFormat;d->TransactionHdr=d->PendingHdr;
        InterlockedExchange64(&d->TransactionPhysical,ReadAcquire64(&d->PendingPhysical));
        InterlockedIncrement64(&d->OwnershipSequence);
        d->TransactionSync=Read(d,d->Intf+0x288);d->TransactionAuto=Read(d,d->Intf+0x2b4);
        d->TransactionGated=TRUE;

        Write(d,d->Intf+0x288,d->TransactionSync&~(1u<<20));
        Write(d,d->Intf+0x2b4,d->TransactionAuto&~(1u<<31));KeMemoryBarrier();
        if((Read(d,d->Intf+0x288)&(1u<<20)) || (Read(d,d->Intf+0x2b4)&(1u<<31))) {
            FailTransaction(d,STATUS_IO_DEVICE_ERROR);return TRUE;
        }
        d->TransactionState=1;d->LineTime=0;

    }
    ULONG line=Read(d,d->Intf+0x2b0)&0xffffu;
    BOOLEAN active=line>0 && line<d->Adapter->DisplayInfo.Height;
    ULONGLONG now=PreciseTime();d->TransactionWait=2000;
    if(active && d->LineTime && line>d->Line && now-d->LineTime<=d->Period) {
        ULONGLONG remaining=(d->Adapter->DisplayInfo.Height-line)*(now-d->LineTime)/(line-d->Line);
        d->TransactionWait=remaining>4000?min(remaining/2,d->Period/8):1000;
    }
    d->Line=line;d->LineTime=active?now:0;
    if(d->TransactionState==1) {
        if(active)return TRUE;

        Write(d,d->Intf+0x808,1);Write(d,0x18,d->PpMask);KeMemoryBarrier();
        if((Read(d,d->Intf+0x804)&1) || (Read(d,0x14)&d->PpMask)) {
            FailTransaction(d,STATUS_IO_DEVICE_ERROR);return TRUE;
        }
        ULONGLONG physical=(ULONGLONG)ReadAcquire64(&d->TransactionPhysical);

        for(ULONG i=0;i<d->PipeCount;i++) {
            DPU_PIPE *p=d->Pipes+i;
            ULONG row=p->Offset/d->Adapter->DisplayInfo.Pitch,column=p->Offset%d->Adapter->DisplayInfo.Pitch;
            Write(d,p->Base+0x14,(ULONG)physical+row*d->TransactionPitch+column);
            Write(d,p->Base+0x24,(p->OriginalStride&0xffff0000u)|d->TransactionPitch);
            if(p->Format!=d->TransactionFormat) {
                ULONG format=d->TransactionFormat,source=0x236ffu,unpack=0x03020001u;
                if(format==D3DDDIFMT_A2R10G10B10 || format==D3DDDIFMT_A2B10G10R10)source|=0x4000u;
                if(format==D3DDDIFMT_A8B8G8R8 || format==D3DDDIFMT_A2B10G10R10)unpack=0x03010002u;
                if(format==(ULONG)d->Adapter->DisplayInfo.ColorFormat){source=p->OriginalFormat;unpack=p->OriginalUnpack;}
                Write(d,p->Base+0x30,source);Write(d,p->Base+0x34,unpack);p->Format=format;
            }
            if(p->Hdr!=d->TransactionHdr) {
                static const ULONG matrix[]={0xb4843d60u,0x0000abdcu,0x3c4dac2eu,0x0000a15fu,0xa50419c8u,
                    0x00003c11u,0x3c000000u,0x3c000000u,0x3c000000u,0x3c000000u};
                for(ULONG j=0;j<RTL_NUMBER_OF(matrix);j++)Write(d,p->Base+0x704+j*4,
                    d->TransactionHdr?matrix[j]:p->OriginalColor[j+1]);
                Write(d,p->Base+0x700,d->TransactionHdr?0x300456u:p->OriginalColor[0]);p->Hdr=d->TransactionHdr;
            }
        }

        KeMemoryBarrier();
        for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x18,d->Ctls[i].Mask);
        d->TransactionFrame=TxFrame(d);RestoreTransaction(d);d->TransactionState=2;d->LineTime=0;

        return TRUE;
    }
    if(active)return TRUE;
    ULONG event=Read(d,d->Intf+0x804);if(!(event&1u))return TRUE;
    ULONG done=Read(d,0x14)&d->PpMask;if(!done)return TRUE;
    if(d->TransactionState==2 && A830DpuCommitComplete(d->TransactionFrame,
        TxFrame(d),active,event,done,FlushComplete(d))) {
        CompleteTransaction(d);
    }
    return TRUE;
}
static BOOLEAN Notify(PVOID context) {
    A830_DPU *d=context;A830_ADAPTER *a=d->Adapter;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n={0};
    n.InterruptType=DXGK_INTERRUPT_CRTC_VSYNC;n.Flags.ValidPhysicalAdapterMask=1;
    n.CrtcVsync.PhysicalAdapterMask=1;n.CrtcVsync.PhysicalAddress.QuadPart=(LONGLONG)a->ScanoutAddress;
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle,&n);

    InterlockedExchange(&a->SchedulerDpcPending,1);
    return a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}
static VOID Poll(PEX_TIMER timer,PVOID context) {
    A830_DPU *d=context;UNREFERENCED_PARAMETER(timer);
    KIRQL old;KeAcquireSpinLock(&d->Adapter->QueueLock,&old);
    ULONGLONG due=d->PollDue;d->PollDue=0;
    KeReleaseSpinLock(&d->Adapter->QueueLock,old);
    if(d->Stopping || !d->Enabled || d->TransactionState==4)return;
    ULONGLONG now=PreciseTime(),late=due && now>due?now-due:0;
    d->TimerLate=max(min(late,d->Period/4),d->TimerLate-d->TimerLate/32);
    if(d->TransactionState || ReadAcquire64(&d->Queued)>ReadAcquire64(&d->Applied)) {
        BOOLEAN advanced=FALSE;
        NTSTATUS transaction=d->Adapter->Dxgk.DxgkCbSynchronizeExecution(d->Adapter->Dxgk.DeviceHandle,AdvanceTransaction,d,0,&advanced);
        if(!NT_SUCCESS(transaction))InterlockedExchange(&d->Adapter->Failed,transaction);
    }
    if(InterlockedExchange(&d->WakeNeeded,0))KeSetEvent(&d->Adapter->Wake,IO_NO_INCREMENT,FALSE);
    if(d->TransactionState==4)return;
    now=PreciseTime();ULONGLONG gap=d->LastSample?now-d->LastSample:0;d->LastSample=now;
    BOOLEAN edge=(Read(d,d->Intf+0x804)&8)!=0;
    if(edge) {
        Write(d,d->Intf+0x808,8);
        if(gap>d->Period*3/2)d->LastEdge=0;
        else if(gap && gap<=d->Period/4) {
            SampleEdge(d,now-gap/2);d->EdgeError=gap/2;
        }
        else if(!d->LastEdge)d->LastEdge=now;
        d->PollBegin=now;
        if(d->Vsync) {
            BOOLEAN result=FALSE;
            NTSTATUS status=d->Adapter->Dxgk.DxgkCbSynchronizeExecution(
                d->Adapter->Dxgk.DeviceHandle,Notify,d,0,&result);
            if(!NT_SUCCESS(status))InterlockedExchange(&d->Adapter->Failed,status);
        }
    }

    now=PreciseTime();ULONGLONG wait=d->LastEdge?1000:2000;
    if(d->LastEdge) {
        ULONGLONG elapsed=edge?now-d->LastEdge:d->PollBegin-d->LastEdge+d->Period/2;
        ULONGLONG next=d->LastEdge+(elapsed/d->Period+1)*d->Period;
        ULONGLONG lead=d->EdgeError+(d->TimerLate<2000?2000-d->TimerLate:0);
        if(next>now)wait=next-now>lead?next-now-lead:1000;
    }
    if((!d->Vsync && d->Applied==InterlockedCompareExchange64(&d->Queued,0,0)) ||
       now-d->PollBegin>d->Period*2)wait=100000;
    else if(d->LastEdge && now-d->LastEdge>d->Period*32)wait=min(wait,2000);
    if(d->TransactionState)wait=min(wait,d->TransactionWait);
    else if(ReadAcquire64(&d->Queued)>ReadAcquire64(&d->Applied))wait=min(wait,2000);
    if(!d->Vsync && !d->TransactionState && ReadAcquire64(&d->Queued)<=ReadAcquire64(&d->Applied))return;
    KeAcquireSpinLock(&d->Adapter->QueueLock,&old);SetPoll(d,now+wait);
    KeReleaseSpinLock(&d->Adapter->QueueLock,old);
}
static NTSTATUS Translation(A830_DPU *d) {
    if(!TranslationProved(d->Adapter))return STATUS_NOT_SUPPORTED;

    for(ULONG i=0;i<2;i++) {
        ULONG slot=Platform.Slots[i];if(slot>=128)return STATUS_NOT_SUPPORTED;
        ULONG smr=READ_REGISTER_ULONG((PULONG)(d->Smmu+0x800+slot*4));
        ULONG value=READ_REGISTER_ULONG((PULONG)(d->Smmu+0xc00+slot*4));
        if(smr!=Platform.Smr[i] || value!=Platform.S2cr[i])return STATUS_DEVICE_CONFIGURATION_ERROR;
        if(((value>>16)&3u)==0) {
            ULONG bank=value&255u,shift=(Platform.SmmuId1&0x80000000u)?16u:12u;
            ULONG pages=1u<<(((Platform.SmmuId1>>28)&7u)+1),sctlr=0,cbar=0;
            NTSTATUS status=ReadPlatform(0x15000000ULL+((ULONGLONG)(pages+bank)<<shift),&sctlr);
            if(NT_SUCCESS(status))status=ReadPlatform(0x15000000ULL+(1ULL<<shift)+bank*4,&cbar);
            if(!NT_SUCCESS(status))return status;
            if(sctlr!=Platform.Sctlr[i] || cbar!=Platform.Cbar[i])return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Start(A830_ADAPTER *a) {

    if(a->Dpu)return STATUS_NOT_SUPPORTED;
    if(!TranslationProved(a))return STATUS_DEVICE_NOT_READY;
    if(!QcomGpuScanoutStagingReady(a))return STATUS_NOT_SUPPORTED;
    A830_DPU *d=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*d),GPU_TAG);
    if(!d)return STATUS_INSUFFICIENT_RESOURCES;
    d->Adapter=a;d->Period=83333;d->EdgeError=10000;a->Dpu=d;
    NTSTATUS status=Clocks(d);if(!NT_SUCCESS(status))goto failed;

    if(!TranslationProved(a)){status=STATUS_NOT_SUPPORTED;goto failed;}
    PHYSICAL_ADDRESS p;p.QuadPart=0xae01000;
    d->Registers=MmMapIoSpaceEx(p,DPU_REGISTER_BYTES,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x15000000;d->Smmu=MmMapIoSpaceEx(p,0x1000,PAGE_READWRITE|PAGE_NOCACHE);
    if(!d->Registers || !d->Smmu){status=STATUS_INSUFFICIENT_RESOURCES;goto failed;}
    status=Translation(d);if(!NT_SUCCESS(status))goto failed;
    ULONGLONG firmware=(ULONGLONG)a->DisplayInfo.PhysicAddress.QuadPart;
    if(firmware>MAXULONG){status=STATUS_NOT_SUPPORTED;goto failed;}
    ULONG active=0,flush=0;
    for(ULONG i=0;i<RTL_NUMBER_OF(PipeDescriptions);i++) {
        ULONG base=PipeDescriptions[i].Base,address=Read(d,base+0x14),stride=Read(d,base+0x24);
        if((ULONGLONG)address<firmware || (ULONGLONG)address>=firmware+a->FramebufferSize ||
           (stride&0xffffu)!=a->DisplayInfo.Pitch)continue;
        if((Read(d,base+0x30)&0xc0000000u) || Read(d,base+0x170)){status=STATUS_NOT_SUPPORTED;goto failed;}
        DPU_PIPE *pipe=d->Pipes+d->PipeCount++;
        pipe->Base=base;pipe->OriginalAddress=address;pipe->OriginalStride=stride;
        pipe->OriginalFormat=Read(d,base+0x30);pipe->OriginalUnpack=Read(d,base+0x34);
        pipe->Format=a->DisplayInfo.ColorFormat;
        for(ULONG j=0;j<RTL_NUMBER_OF(pipe->OriginalColor);j++)pipe->OriginalColor[j]=Read(d,base+0x700+j*4);
        pipe->Offset=(ULONG)((ULONGLONG)address-firmware);
        active|=PipeDescriptions[i].Active;flush|=PipeDescriptions[i].Flush;
    }
    if(!d->PipeCount){status=STATUS_NOT_SUPPORTED;goto failed;}
    ULONG intfs=0;
    for(ULONG i=0;i<6;i++) {
        ULONG base=0x15000+i*0x1000,pipes=Read(d,base+0x12c);
        if(!(pipes&active))continue;
        if(pipes&~active){status=STATUS_NOT_SUPPORTED;goto failed;}
        d->Ctls[d->CtlCount].Base=base;d->Ctls[d->CtlCount++].Mask=flush;
        intfs|=Read(d,base+0xf4);
    }
    if(!d->CtlCount || !intfs){status=STATUS_NOT_SUPPORTED;goto failed;}
    ULONG mixers=0;for(ULONG i=0;i<d->CtlCount;i++)mixers|=Read(d,d->Ctls[i].Base+0x130);

    if(!(mixers&1) || (mixers&~3u)){status=STATUS_NOT_SUPPORTED;goto failed;}
    d->PpMask=1u<<8;
    for(ULONG i=0;i<4;i++)if(intfs&(1u<<i)) {
        ULONG base=0x34000+i*0x1000;
        if(!(Read(d,base+0x284)&1) || (Read(d,base+0x288)&0x180000u)!=0x180000u){status=STATUS_NOT_SUPPORTED;goto failed;}
        if(!d->Intf)d->Intf=base;
        d->AutoRefresh|=Read(d,base+0x2b4)&0x80000000u;
    }
    if(Read(d,d->Intf+0x2b4)!=0x80000001u || !(Read(d,d->Intf+0x284)&1) ||
        (Read(d,d->Intf+0x288)&0x180000u)!=0x180000u) {
        status=STATUS_DEVICE_NOT_READY;goto failed;
    }

    Write(d,0x10,Read(d,0x10)&~d->PpMask);Write(d,d->Intf+0x800,Read(d,d->Intf+0x800)&~9u);
    ULONG initial=TxFrame(d),frame=initial;
    ULONGLONG deadline=PreciseTime()+500000;
    LARGE_INTEGER pause;pause.QuadPart=-10000;
    do {
        KeDelayExecutionThread(KernelMode,FALSE,&pause);frame=TxFrame(d);
    } while(frame==initial && PreciseTime()<deadline);
    if(frame==initial){status=STATUS_IO_TIMEOUT;goto failed;}
    {

        Write(d,d->Intf+0x808,8);ULONG edges=0;
        deadline=PreciseTime()+500000;
        do {
            KeDelayExecutionThread(KernelMode,FALSE,&pause);
            if(Read(d,d->Intf+0x804)&8) {
                Write(d,d->Intf+0x808,8);d->LastEdge=PreciseTime();edges++;
            }
        }while(edges<2 && PreciseTime()<deadline);
        if(edges<2){status=STATUS_IO_TIMEOUT;goto failed;}
    }
    d->Timer=ExAllocateTimer(Poll,d,EX_TIMER_HIGH_RESOLUTION);
    if(!d->Timer){status=STATUS_INSUFFICIENT_RESOURCES;goto failed;}
    d->Vsync=InterlockedCompareExchange(&a->VsyncEnabled,0,0);d->PollBegin=d->LastSample=PreciseTime();

    d->ClockAdded|=Platform.AddedAhb<<1;Platform.AddedAhb=0;
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    InterlockedExchange(&d->Enabled,1);SetPoll(d,PreciseTime()+10000);
    KeReleaseSpinLock(&a->QueueLock,old);
    return STATUS_SUCCESS;
failed:A830DpuStop(a);return status;
}
NTSTATUS A830DpuInitialize(PVOID context) {
    A830_ADAPTER *a=context;
    NTSTATUS status=PreparePlatform(a);
    if(NT_SUCCESS(status))status=QcomGpuScanoutReserveStaging(a);
    if(NT_SUCCESS(status))status=Start(a);
    if(!NT_SUCCESS(status)) {
        NTSTATUS restored=RestoreAhb(a);
        if(!NT_SUCCESS(restored))status=restored;
    }
    return status;
}
BOOLEAN A830DpuEnabled(PVOID context) {
    A830_ADAPTER *a=context;
    if(KeGetCurrentIrql()>DISPATCH_LEVEL) {
        A830_DPU *d=a->Dpu;return d && d->Enabled && !d->Stopping;
    }
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    BOOLEAN enabled=d && d->Enabled && !d->Stopping;
    KeReleaseSpinLock(&a->QueueLock,old);return enabled;
}
BOOLEAN A830DpuReferences(PVOID context,ULONGLONG physical) {
    A830_ADAPTER *a=context;KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    A830_DPU *d=a->Dpu;
    BOOLEAN referenced=FALSE;
    if(d) {
        LONG64 before=ReadAcquire64(&d->OwnershipSequence);
        referenced=(before&1) || (ULONGLONG)ReadAcquire64(&d->PendingPhysical)==physical ||
            (ULONGLONG)ReadAcquire64(&d->ActivePhysical)==physical || (ULONGLONG)ReadAcquire64(&d->TransactionPhysical)==physical;
        KeMemoryBarrier();if(before!=ReadAcquire64(&d->OwnershipSequence))referenced=TRUE;
    }
    KeReleaseSpinLock(&a->QueueLock,old);return referenced;
}
NTSTATUS A830DpuQueue(PVOID context,ULONGLONG physical,ULONG pitch,ULONGLONG address,ULONG format,BOOLEAN hdr) {
    A830_DPU *d=((A830_ADAPTER *)context)->Dpu;
    if(!d || !d->Enabled || d->Stopping || d->TransactionState==4)return STATUS_DEVICE_NOT_READY;
    if(!A830ScanoutFormat(format) || !physical || physical>MAXULONG || pitch<d->Adapter->DisplayInfo.Width*4 || pitch>0xffffu ||
       (ULONGLONG)pitch*d->Adapter->DisplayInfo.Height>MAXULONG-physical)return STATUS_INVALID_PARAMETER;

    InterlockedIncrement64(&d->OwnershipSequence);InterlockedIncrement64(&d->Queued);
    d->PendingAddress=address;d->PendingPitch=pitch;InterlockedExchange64(&d->PendingPhysical,(LONG64)physical);
    d->PendingFormat=format;d->PendingHdr=hdr;KeMemoryBarrier();
    InterlockedIncrement64(&d->Queued);InterlockedIncrement64(&d->OwnershipSequence);
    InterlockedExchange(&d->Kick,1);A830WakeDisplay(d->Adapter);return STATUS_SUCCESS;
}
VOID A830DpuEnableVsync(PVOID context,BOOLEAN enabled) {
    A830_ADAPTER *a=context;
    if(KeGetCurrentIrql()>DISPATCH_LEVEL) {

        A830_DPU *d=a->Dpu;
        if(d && !d->Stopping && InterlockedExchange(&d->Vsync,enabled)!=enabled && enabled)
            InterlockedExchange(&d->Kick,1);
        return;
    }
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d && !d->Stopping) {
        if(InterlockedExchange(&d->Vsync,enabled)!=enabled && enabled)SetPoll(d,PreciseTime()+10000);
    }
    KeReleaseSpinLock(&a->QueueLock,old);
}
VOID A830DpuRestoreRegisters(PVOID context) {
    A830_DPU *d=((A830_ADAPTER *)context)->Dpu;
    if(!d || !d->Enabled || d->Restored)return;
    RestoreTransaction(d);
    for(ULONG i=0;i<d->PipeCount;i++) {
        Write(d,d->Pipes[i].Base+0x14,d->Pipes[i].OriginalAddress);
        Write(d,d->Pipes[i].Base+0x24,d->Pipes[i].OriginalStride);
        Write(d,d->Pipes[i].Base+0x30,d->Pipes[i].OriginalFormat);Write(d,d->Pipes[i].Base+0x34,d->Pipes[i].OriginalUnpack);
        for(ULONG j=1;j<RTL_NUMBER_OF(d->Pipes[i].OriginalColor);j++)Write(d,d->Pipes[i].Base+0x700+j*4,d->Pipes[i].OriginalColor[j]);
        Write(d,d->Pipes[i].Base+0x700,d->Pipes[i].OriginalColor[0]);
    }
    Flush(d);
}
typedef struct { A830_ADAPTER *Adapter;A830_DPU *Dpu;BOOLEAN Retire; } DPU_STOP_ACCESS;
static BOOLEAN StopAccess(PVOID context) {
    DPU_STOP_ACCESS *access=context;
    if(access->Adapter->Dpu!=access->Dpu)return FALSE;
    if(access->Retire)access->Adapter->Dpu=NULL;
    else InterlockedExchange(&access->Dpu->Stopping,1);
    return TRUE;
}
static NTSTATUS SynchronizeStop(A830_ADAPTER *a,A830_DPU *d,BOOLEAN retire) {
    DPU_STOP_ACCESS access={a,d,retire};BOOLEAN result=FALSE;

    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);
    NTSTATUS status=a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle,StopAccess,&access,0,&result);
    KeReleaseSpinLock(&a->QueueLock,old);
    return NT_SUCCESS(status)?(result?STATUS_SUCCESS:STATUS_DEVICE_BUSY):status;
}
NTSTATUS A830DpuStop(PVOID context) {
    A830_ADAPTER *a=context;A830_DPU *d=a->Dpu;if(!d)return RestoreAhb(a);
    NTSTATUS stopped=SynchronizeStop(a,d,FALSE);if(!NT_SUCCESS(stopped))return stopped;
    if(d->Timer){ExDeleteTimer(d->Timer,TRUE,TRUE,NULL);d->Timer=NULL;}
    RestoreTransaction(d);
    if(d->Enabled && !d->Restored) {
        A830DpuRestoreRegisters(a);

        ULONG frame=TxFrame(d);ULONGLONG end=KeQueryInterruptTime()+1000000;
        LARGE_INTEGER delay;delay.QuadPart=-10000;
        while(KeQueryInterruptTime()<end && (TxFrame(d)==frame || !FlushComplete(d)))
            KeDelayExecutionThread(KernelMode,FALSE,&delay);

        if(TxFrame(d)==frame || !FlushComplete(d))return STATUS_IO_TIMEOUT;
        d->Restored=TRUE;
    }
    NTSTATUS released=ReleaseClocks(d);if(!NT_SUCCESS(released))return released;
    stopped=SynchronizeStop(a,d,TRUE);if(!NT_SUCCESS(stopped))return stopped;
    if(d->Registers)MmUnmapIoSpace(d->Registers,DPU_REGISTER_BYTES);
    if(d->Smmu)MmUnmapIoSpace(d->Smmu,0x1000);
    ExFreePoolWithTag(d,GPU_TAG);return RestoreAhb(a);
}
