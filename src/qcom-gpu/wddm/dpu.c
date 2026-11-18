#include "adapter.h"
#include "dpu.h"
#include "dpu-completion.h"

/* SM8750 DPU12: Linux dpu_12_0_sm8750.h, dpu_hw_sspp.c,
 * dpu_hw_ctl.c and dpu_hw_intf.c. No DSI/DSC/clock programming is performed.
 */
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
        /* Linux clk_branch2_check_halt: CLK_OFF clear OR NoC FSM ON. */
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
        /* Release only our added SW enable, preserving all firmware fields. */
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
typedef struct { ULONG Base,OriginalAddress,OriginalStride,Offset; } DPU_PIPE;
typedef struct { ULONG Base,Mask; } DPU_CTL;
typedef struct {
    A830_ADAPTER *Adapter;
    PUCHAR Registers,Smmu,Gcc,Dispcc;
    ULONG ClockAdded;
    PEX_TIMER Timer;
    DPU_PIPE Pipes[10];ULONG PipeCount;
    DPU_CTL Ctls[6];ULONG CtlCount;
    ULONG Intf,Frame,AutoRefresh;
    ULONG SmmuEntry,SmmuOriginal;BOOLEAN SmmuChanged;
    BOOLEAN Restored;
    volatile LONG Stopping,Enabled,Vsync;
    volatile LONG64 Queued,Applied;
    ULONGLONG PendingAddress;
    ULONG PendingPitch;
    volatile LONG64 OwnershipSequence;
    volatile LONG64 PendingPhysical,ActivePhysical;
    volatile LONG64 TransactionPhysical;
    LONG64 TransactionSequence;
    ULONGLONG TransactionAddress,TransactionDeadline;
    ULONG TransactionPitch,TransactionFrame,TransactionState,TransactionSync,TransactionAuto,PpMask;
    BOOLEAN TransactionGated;
    volatile LONG Kick;
    volatile LONG WakeNeeded;
    ULONG TeCounter,TxFrame;
    ULONG PendingFrame,PendingFence;
    ULONGLONG LastEdge,Period,PollBegin;
} A830_DPU;
VOID A830DpuWork(A830_ADAPTER *a) {
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d && d->Enabled && !d->Stopping && d->Timer &&
        (InterlockedExchange(&d->Kick,0) || ReadAcquire64(&d->Queued)>ReadAcquire64(&d->Applied)))
        ExSetTimer(d->Timer,-2000,0,NULL);
    KeReleaseSpinLock(&a->QueueLock,old);
}
/* Preserve the inherited rates, PLLs, power domain and interconnect votes.
 * Linux msm_mdss_enable requires clocks before the first MDSS read. Only
 * retain branch enables on a domain that firmware already powered; bringing
 * up an off domain would also require RPMh/ICC and is deliberately rejected.
 */
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
    /* Command mode without inherited autorefresh requires a CTL kickoff. */
    if(!d->AutoRefresh)for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x1c,1);
}
static BOOLEAN FlushComplete(A830_DPU *d) {
    for(ULONG i=0;i<d->CtlCount;i++)if(Read(d,d->Ctls[i].Base+0x18)&d->Ctls[i].Mask)return FALSE;
    return TRUE;
}
static VOID RestoreTransaction(A830_DPU *d) {
    if(!d->TransactionGated)return;
    /* Restore only the exact configuration held by this transaction. */
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
/* Short display-interrupt-lock sections only. The timer provides all waits;
 * an Address callback never waits or writes a live SSPP source register. */
static BOOLEAN AdvanceTransaction(PVOID context) {
    A830_DPU *d=context;
    if(d->Stopping || !d->Enabled || d->TransactionState==4)return TRUE;
    ULONGLONG now=PreciseTime();
    if(d->TransactionState==1 && now>=d->TransactionDeadline) {
        FailTransaction(d,STATUS_IO_TIMEOUT);
        return TRUE;
    }
    if(!d->TransactionState) {
        LONG64 sequence=ReadAcquire64(&d->Queued);
        if((sequence&1) || sequence<=ReadAcquire64(&d->Applied) ||
           A830FenceAfter(d->PendingFence,d->Adapter->Completed))return TRUE;
        InterlockedIncrement64(&d->OwnershipSequence);
        d->TransactionSequence=sequence;d->TransactionAddress=d->PendingAddress;
        d->TransactionPitch=d->PendingPitch;d->TransactionFrame=d->PendingFrame;
        InterlockedExchange64(&d->TransactionPhysical,ReadAcquire64(&d->PendingPhysical));
        InterlockedIncrement64(&d->OwnershipSequence);
        d->TransactionSync=Read(d,d->Intf+0x288);d->TransactionAuto=Read(d,d->Intf+0x2b4);
        d->TransactionDeadline=now+500000;d->TransactionGated=TRUE;
        /* Qualcomm prepare_commit: prevent a subsequent automatic kickoff,
           then allow an already running pixel transfer to finish. */
        Write(d,d->Intf+0x288,d->TransactionSync&~(1u<<20));
        Write(d,d->Intf+0x2b4,d->TransactionAuto&~(1u<<31));KeMemoryBarrier();
        if((Read(d,d->Intf+0x288)&(1u<<20)) || (Read(d,d->Intf+0x2b4)&(1u<<31))) {
            FailTransaction(d,STATUS_IO_DEVICE_ERROR);return TRUE;
        }
        d->TransactionState=1;

    }
    ULONG line=Read(d,d->Intf+0x2b0)&0xffffu;
    BOOLEAN active=line>0 && line<d->Adapter->DisplayInfo.Height;
    if(d->TransactionState==1) {
        if(active)return TRUE;
        /* Both events are cleared only after old PPTX has drained. No old
           latched 4f/100 may be mistaken for completion of this commit. */
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
        }
        /* Do not issue CTL_START: the exact inherited autorefresh and TE
           configuration below supplies the kickoff after this atomic flush. */
        KeMemoryBarrier();
        for(ULONG i=0;i<d->CtlCount;i++)Write(d,d->Ctls[i].Base+0x18,d->Ctls[i].Mask);
        d->TransactionFrame=TxFrame(d);RestoreTransaction(d);d->TransactionState=2;

        return TRUE;
    }
    ULONG event=Read(d,d->Intf+0x804),done=Read(d,0x14)&d->PpMask;
    if(d->TransactionState==2 && A830DpuCommitComplete(d->TransactionFrame,
        TxFrame(d),active,event,done,FlushComplete(d))) {
        CompleteTransaction(d);
    } else if(d->TransactionState==2 && now>=d->TransactionDeadline) {
        /* A late timer must inspect the latched evidence before declaring
           timeout. Do not discard PPdone while waiting for another sample. */
        FailTransaction(d,STATUS_IO_TIMEOUT);
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
    if(d->Stopping || !d->Enabled)return;
    BOOLEAN advanced=FALSE;
    NTSTATUS transaction=d->Adapter->Dxgk.DxgkCbSynchronizeExecution(d->Adapter->Dxgk.DeviceHandle,AdvanceTransaction,d,0,&advanced);
    if(!NT_SUCCESS(transaction))InterlockedExchange(&d->Adapter->Failed,transaction);
    if(InterlockedExchange(&d->WakeNeeded,0))KeSetEvent(&d->Adapter->Wake,IO_NO_INCREMENT,FALSE);
    ULONGLONG now=PreciseTime();d->TxFrame=TxFrame(d);ULONG frame=d->TeCounter;
    {
        if(Read(d,d->Intf+0x804)&8) {
            Write(d,d->Intf+0x808,8);d->TeCounter++;
        }
        frame=d->TeCounter;
    }
    if(frame!=d->Frame) {
        ULONG delta=(frame-d->Frame)&0xffffu;
        ULONG edgeDelta=delta<0x8000?delta:0;

        if(d->LastEdge && delta && delta<8) {
            ULONGLONG period=(now-d->LastEdge)/delta;
            if(period>=60000 && period<=400000)d->Period=(d->Period*3+period)/4;
        }
        d->LastEdge=now;d->Frame=frame;
        if(d->Vsync && edgeDelta) {
            BOOLEAN result=FALSE;
            NTSTATUS status=d->Adapter->Dxgk.DxgkCbSynchronizeExecution(
                d->Adapter->Dxgk.DeviceHandle,Notify,d,0,&result);
            if(!NT_SUCCESS(status))InterlockedExchange(&d->Adapter->Failed,status);
        }
    }
    /* Sleep until shortly before the next hardware edge. Only the narrow
       edge window is polled at 200 us; no fabricated VSync is ever emitted. */
    now=PreciseTime();ULONGLONG wait=2000;
    if(d->LastEdge && now<d->LastEdge+d->Period && d->LastEdge+d->Period-now>10000)
        wait=d->LastEdge+d->Period-now-10000;
    if((!d->Vsync && d->Applied==InterlockedCompareExchange64(&d->Queued,0,0)) ||
       (d->LastEdge && now-d->LastEdge>d->Period*2) ||
       (!d->LastEdge && now-d->PollBegin>d->Period*2))wait=100000;
    if(d->TransactionState || ReadAcquire64(&d->Queued)>ReadAcquire64(&d->Applied))wait=2000;
    if(d->TransactionState==4 || (!d->TransactionState && A830FenceAfter(d->PendingFence,d->Adapter->Completed)))wait=100000;
    if(!d->Vsync && !d->TransactionState && ReadAcquire64(&d->Queued)<=ReadAcquire64(&d->Applied))return;
    if(!d->Stopping)ExSetTimer(d->Timer,-(LONGLONG)wait,0,NULL);
}
static NTSTATUS Translation(A830_DPU *d) {
    if(!TranslationProved(d->Adapter))return STATUS_NOT_SUPPORTED;
    /* Revalidate only the explicitly probed MDSS entries. A mask of 2 is
       the Linux DT's two MDSS stream IDs (0x800/0x802), not another device.
       No S2CR or context-bank register is ever modified here. */
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
    d->SmmuEntry=Platform.Slots[0];d->SmmuOriginal=Platform.S2cr[0];return STATUS_SUCCESS;
}
NTSTATUS A830DpuInitialize(PVOID context) {
    A830_ADAPTER *a=context;
    NTSTATUS status=PreparePlatform(a);
    if(NT_SUCCESS(status))status=QcomGpuScanoutReserveStaging(a);
    if(NT_SUCCESS(status))status=A830DpuStart(a);
    if(!NT_SUCCESS(status)) {
        NTSTATUS restored=RestoreAhb(a);
        if(!NT_SUCCESS(restored))status=restored;
    }
    return status;
}
NTSTATUS A830DpuStart(PVOID context) {
    A830_ADAPTER *a=context;

    if(a->Dpu)return STATUS_NOT_SUPPORTED;
    if(!TranslationProved(a))return STATUS_DEVICE_NOT_READY;
    if(!QcomGpuScanoutStagingReady(a))return STATUS_NOT_SUPPORTED;
    A830_DPU *d=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*d),GPU_TAG);
    if(!d)return STATUS_INSUFFICIENT_RESOURCES;
    d->Adapter=a;d->Period=166667;a->Dpu=d;
    NTSTATUS status=Clocks(d);if(!NT_SUCCESS(status))goto failed;
    /* Access to global apps_smmu requires its own platform verification.
       MDSS clocks do not establish TCU register accessibility. */
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
    /* This inherited split/merged DSI0 topology uses master PP0. PP1 does
       not generate done (confirmed 0x100 status), even with two VIG pipes. */
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
    /* Verify that the selected hardware counter is alive before replacing
       the old software-vblank producer. A silent rdptr counter must never
       disable the desktop's only progress source. */
    ULONG initial=TxFrame(d);
    ULONGLONG deadline=PreciseTime()+500000;
    LARGE_INTEGER pause;pause.QuadPart=-10000;
    do {
        KeDelayExecutionThread(KernelMode,FALSE,&pause);d->Frame=TxFrame(d);
    } while(d->Frame==initial && PreciseTime()<deadline);
    if(d->Frame==initial){status=STATUS_IO_TIMEOUT;goto failed;}
    {
        /* Prove a freshly acknowledged input TE event can recur. Do not use
           the inherited permanently-latched bit as a clock sample. */
        Write(d,d->Intf+0x808,8);ULONG edges=0;
        deadline=PreciseTime()+500000;
        do {
            KeDelayExecutionThread(KernelMode,FALSE,&pause);
            if(Read(d,d->Intf+0x804)&8){Write(d,d->Intf+0x808,8);edges++;}
        }while(edges<2 && PreciseTime()<deadline);
        if(edges<2){status=STATUS_IO_TIMEOUT;goto failed;}
        d->Frame=0;d->TeCounter=0;
    }
    d->TxFrame=TxFrame(d);
    d->Timer=ExAllocateTimer(Poll,d,EX_TIMER_HIGH_RESOLUTION);
    if(!d->Timer){status=STATUS_INSUFFICIENT_RESOURCES;goto failed;}
    d->Vsync=InterlockedCompareExchange(&a->VsyncEnabled,0,0);d->PollBegin=PreciseTime();
    /* Transfer the explicit probe's added branch vote to this backend.
       Backend failure/stop then releases the same enable exactly once. */
    d->ClockAdded|=Platform.AddedAhb<<1;Platform.AddedAhb=0;
    InterlockedExchange(&d->Enabled,1);ExSetTimer(d->Timer,-10000,0,NULL);
    return STATUS_SUCCESS;
failed:A830DpuStop(a);return status;
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
NTSTATUS A830DpuQueue(PVOID context,ULONGLONG physical,ULONG pitch,ULONGLONG address) {
    A830_DPU *d=((A830_ADAPTER *)context)->Dpu;
    if(!d || !d->Enabled || d->Stopping || d->TransactionState==4)return STATUS_DEVICE_NOT_READY;
    if(!physical || physical>MAXULONG || pitch<d->Adapter->DisplayInfo.Width*4 || pitch>0xffffu ||
       (ULONGLONG)pitch*d->Adapter->DisplayInfo.Height>MAXULONG-physical)return STATUS_INVALID_PARAMETER;
    /* The display callback is serialized by dxgkrnl. Publish odd before any
       register or metadata write, so the timer never acknowledges a mixture
       of the preceding sequence and the new frame's address. */
    InterlockedIncrement64(&d->OwnershipSequence);InterlockedIncrement64(&d->Queued);
    d->PendingAddress=address;d->PendingPitch=pitch;InterlockedExchange64(&d->PendingPhysical,(LONG64)physical);
    d->PendingFrame=d->TxFrame;d->PendingFence=d->Adapter->LastSubmitted;KeMemoryBarrier();
    InterlockedIncrement64(&d->Queued);InterlockedIncrement64(&d->OwnershipSequence);
    InterlockedExchange(&d->Kick,1);A830WakeDisplay(d->Adapter);return STATUS_SUCCESS;
}
VOID A830DpuEnableVsync(PVOID context,BOOLEAN enabled) {
    A830_ADAPTER *a=context;
    if(KeGetCurrentIrql()>DISPATCH_LEVEL) {
        /* Display DIRQL callers already hold dxgkrnl's interrupt lock. */
        A830_DPU *d=a->Dpu;if(d && !d->Stopping)InterlockedExchange(&d->Vsync,enabled);
        return;
    }
    KIRQL old;KeAcquireSpinLock(&a->QueueLock,&old);A830_DPU *d=a->Dpu;
    if(d && !d->Stopping) {
        InterlockedExchange(&d->Vsync,enabled);
        if(enabled && d->Timer)ExSetTimer(d->Timer,-10000,0,NULL);
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
    /* Same order as scheduler IssueJob: QueueLock -> display interrupt lock.
       The DIRQL callback performs no wait, event signal or lock acquisition. */
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
        /* Returning POST ownership must not free the active BO before the
           firmware address has actually latched. Stop is passive and bounded. */
        ULONG frame=TxFrame(d);ULONGLONG end=KeQueryInterruptTime()+1000000;
        LARGE_INTEGER delay;delay.QuadPart=-10000;
        while(KeQueryInterruptTime()<end && (TxFrame(d)==frame || !FlushComplete(d)))
            KeDelayExecutionThread(KernelMode,FALSE,&delay);
        /* Keep mappings and both references alive if hardware never latches.
           The caller must preserve the pool/segment and fail device stop. */
        if(TxFrame(d)==frame || !FlushComplete(d))return STATUS_IO_TIMEOUT;
        d->Restored=TRUE;
    }
    NTSTATUS released=ReleaseClocks(d);if(!NT_SUCCESS(released))return released;
    stopped=SynchronizeStop(a,d,TRUE);if(!NT_SUCCESS(stopped))return stopped;
    if(d->Registers)MmUnmapIoSpace(d->Registers,DPU_REGISTER_BYTES);
    if(d->Smmu)MmUnmapIoSpace(d->Smmu,0x1000);
    ExFreePoolWithTag(d,GPU_TAG);return RestoreAhb(a);
}
