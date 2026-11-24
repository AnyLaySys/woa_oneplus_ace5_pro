#include "gpu.h"
#include "regs.h"
#include "tables.h"
#include "context-protocol.h"

static ULONG Packet(ULONG op,ULONG count) {
    ULONG a=op,b=count;
    a^=a>>4;a^=a>>2;a^=a>>1;
    b^=b>>8;b^=b>>4;b^=b>>2;b^=b>>1;
    return 0x70000000|(op<<16)|count|((~a&1)<<23)|((~b&1)<<15);
}
static VOID Emit(GPU *g,ULONG value) {
    ((PULONG)g->Ring.Cpu)[g->WriteIndex++&8191]=value;
}
static VOID Flush(GPU *g) {
    KeMemoryBarrier();Gw(g,REG_A6XX_CP_RB_WPTR,g->WriteIndex&8191);
}
static NTSTATUS Idle(GPU *g) {
    Flush(g);
    return GpuPoll(g->Registers,REG_A6XX_CP_RB_RPTR*4,8191,g->WriteIndex&8191,1000000);
}
NTSTATUS GpuWait(GPU *g,ULONGLONG fence,ULONGLONG timeout) {
    if(fence>g->Submitted || timeout>100000000)return STATUS_INVALID_PARAMETER;
    if(!fence)return STATUS_SUCCESS;
    if(!g->Memory.Cpu)return STATUS_DEVICE_NOT_READY;
    ULONGLONG end=KeQueryInterruptTime()+timeout;
    for(;;) {
        KeMemoryBarrier();
        if(*(volatile ULONGLONG *)g->Memory.Cpu>=fence)return STATUS_SUCCESS;
        if(g->Fault)return (NTSTATUS)g->Fault;
        ULONGLONG now=KeQueryInterruptTime();if(now>=end)return STATUS_IO_TIMEOUT;
        LARGE_INTEGER delay;delay.QuadPart=-(LONGLONG)(end-now);
        KeWaitForSingleObject(&g->Event,Executive,KernelMode,FALSE,&delay);
    }
}
static VOID Fence(GPU *g) {
    /* Linux a7xx_submit (also used by A8xx): retire BV before BR's
       CACHE_CLEAN timestamp. Neither pipe requires CP_WAIT_FOR_IDLE. */
    ULONG sequence=(ULONG)++g->Submitted;
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,1);
    Emit(g,Packet(CP_EVENT_WRITE,1));Emit(g,0x18);
    Emit(g,Packet(CP_EVENT_WRITE,1));Emit(g,0x19);
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,2);
    Emit(g,Packet(CP_EVENT_WRITE,4));Emit(g,0x31|(1u<<27));
    Emit(g,(ULONG)(g->Memory.Address+8));Emit(g,(ULONG)((g->Memory.Address+8)>>32));Emit(g,sequence);
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,1);
    Emit(g,Packet(0x14,4));Emit(g,0);
    Emit(g,(ULONG)(g->Memory.Address+8));Emit(g,(ULONG)((g->Memory.Address+8)>>32));Emit(g,sequence);
    Emit(g,Packet(CP_EVENT_WRITE,4));Emit(g,0x31|(1u<<27)|(1u<<31));
    Emit(g,(ULONG)g->Memory.Address);Emit(g,(ULONG)(g->Memory.Address>>32));Emit(g,sequence);
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,3);
    Flush(g);
}
NTSTATUS GpuSubmit(GPU *g,GPU_BO *bo,ULONG offset,ULONG words) {
    GPU_COMMAND entry={bo,offset,words};
    return GpuSubmitBatch(g,&entry,1);
}
NTSTATUS GpuSubmitBatch(GPU *g,const GPU_COMMAND *entries,ULONG count) {
    return GpuSubmitContextBatch(g,entries,count,0);
}
NTSTATUS GpuSubmitContextBatch(GPU *g,const GPU_COMMAND *entries,ULONG count,ULONGLONG cookie) {
    if(g->Fault)return (NTSTATUS)g->Fault;
    if(!g->Online || !entries || !count || count>GPU_MAX_COMMANDS)return STATUS_INVALID_PARAMETER;
    GPU_IB ibs[GPU_MAX_COMMANDS];
    for(ULONG i=0;i<count;i++) {
        const GPU_COMMAND *s=entries+i;
        if(!s->Buffer || !s->Words || s->Words>0xFFFFF || (s->Offset&3))return STATUS_INVALID_PARAMETER;
        GPU_BO *bo=s->Buffer;
        if(!bo->Mdl || !bo->Address || s->Offset>bo->Size || s->Words>(bo->Size-s->Offset)/4)return STATUS_INVALID_PARAMETER;
        ibs[i].Address=bo->Address+s->Offset;ibs[i].Words=s->Words;
    }
    return GpuSubmitRootBatch(g,ibs,count,0,cookie);
}
NTSTATUS GpuSubmitRootBatch(GPU *g,const GPU_IB *entries,ULONG count,ULONGLONG root,ULONGLONG cookie) {
    if(g->Fault)return (NTSTATUS)g->Fault;
    if(!g->Online || !entries || !count || count>GPU_MAX_COMMANDS)return STATUS_INVALID_PARAMETER;
    for(ULONG i=0;i<count;i++)if(!entries[i].Words || entries[i].Words>0xfffff ||
        (entries[i].Address&3) || !entries[i].Address || entries[i].Address>=0x8000000000ULL ||
        (ULONGLONG)entries[i].Words*4>0x8000000000ULL-entries[i].Address)return STATUS_INVALID_PARAMETER;
    KIRQL old;KeAcquireSpinLock(&g->RingLock,&old);
    if(!g->Online || !g->Ring.Mdl || !g->Ring.Cpu || !g->Memory.Mdl || !g->Memory.Cpu){KeReleaseSpinLock(&g->RingLock,old);return STATUS_DEVICE_NOT_READY;}
    ULONG read=Gr(g,REG_A6XX_CP_RB_RPTR)&8191;
    ULONG space=(read-g->WriteIndex-1)&8191;
    ULONGLONG defaultRoot=g->Vm[0].Tables.Physical;if(!root)root=defaultRoot;
    if(!root || (root&4095) || (root>>44)){KeReleaseSpinLock(&g->RingLock,old);return STATUS_INVALID_PARAMETER;}
    BOOLEAN changed=root!=(g->LastRoot?g->LastRoot:defaultRoot);
    BOOLEAN switched=(!g->LastContextValid || cookie!=g->LastContextCookie);
    GPU_U32 contextWords[GPU_PAGETABLE_PREAMBLE_MAX_WORDS],restoreWords[GPU_PAGETABLE_PREAMBLE_MAX_WORDS];
    ULONG n=changed?GpuBuildPageTablePreamble(contextWords,RTL_NUMBER_OF(contextWords),g->Memory.Address,(ULONG)g->Submitted,root):
        switched?GpuBuildContextPreamble(contextWords,RTL_NUMBER_OF(contextWords),g->Memory.Address,(ULONG)g->Submitted):0;
    if((changed || switched) && !n){KeReleaseSpinLock(&g->RingLock,old);return STATUS_INVALID_PARAMETER;}
    ULONG restore=root!=defaultRoot?GpuBuildPageTablePreamble(restoreWords,RTL_NUMBER_OF(restoreWords),g->Memory.Address,(ULONG)g->Submitted+1,defaultRoot):0;
    if(root!=defaultRoot && !restore){KeReleaseSpinLock(&g->RingLock,old);return STATUS_INVALID_PARAMETER;}
    if(space<4*count+48*(restore?2:1)+n+restore || g->Submitted>=MAXULONG-(restore?2:1)){KeReleaseSpinLock(&g->RingLock,old);return STATUS_DEVICE_BUSY;}
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,0x80000001);
    for(ULONG i=0;i<n;i++)Emit(g,contextWords[i]);
    if(switched || changed){g->LastContextCookie=cookie;g->LastContextValid=TRUE;}g->LastRoot=root;
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,3);
    Emit(g,Packet(CP_SET_MARKER,1));Emit(g,0x101);
    for(ULONG i=0;i<count;i++) {
        const GPU_IB *s=entries+i;
        Emit(g,Packet(CP_INDIRECT_BUFFER,3));Emit(g,(ULONG)s->Address);Emit(g,(ULONG)(s->Address>>32));Emit(g,s->Words);
    }
    /* Retire private-root work before restoring the default page table;
       the published fence must also cover the restore. */
    if(restore) {
        Fence(g);
        for(ULONG i=0;i<restore;i++)Emit(g,restoreWords[i]);
        g->LastRoot=defaultRoot;g->LastContextValid=FALSE;
    }
    g->LastSubmitTime=KeQueryInterruptTime();
    Fence(g);KeReleaseSpinLock(&g->RingLock,old);return STATUS_SUCCESS;
}
static NTSTATUS Microcode(GPU *g,GPU_BO *bo,PCWSTR name,ULONGLONG address) {
    PUCHAR data=NULL;ULONG size=0;
    NTSTATUS status=GpuFirmware(name,&data,&size);
    if(!NT_SUCCESS(status))return status;
    if(size<8 || (size&3))status=STATUS_INVALID_IMAGE_FORMAT;
    else {
        status=GpuAllocate(bo,size-4);
        if(NT_SUCCESS(status)) {
            RtlCopyMemory(bo->Cpu,data+4,size-4);status=GpuMap(g,0,bo,address);
        }
    }
    ExFreePoolWithTag(data,GPU_TAG);return status;
}
static VOID Pipe(GPU *g,ULONG pipe) {
    Gw(g,REG_A8XX_CP_APERTURE_CNTL_HOST,pipe<<12);
    Gr(g,REG_A8XX_CP_APERTURE_CNTL_HOST);
}
static NTSTATUS Buffers(GPU *g) {

    NTSTATUS status=Microcode(g,&g->Sqe,L"\\SystemRoot\\System32\\drivers\\qcom-gpu\\gen80000_sqe.fw",0x40010000);
    if(NT_SUCCESS(status))status=Microcode(g,&g->Aqe,L"\\SystemRoot\\System32\\drivers\\qcom-gpu\\gen80000_aqe.fw",0x40030000);
    if(NT_SUCCESS(status))status=GpuAllocate(&g->Memory,4096);
    if(NT_SUCCESS(status))status=GpuMap(g,0,&g->Memory,0x40000000);
    if(NT_SUCCESS(status))status=GpuAllocate(&g->Ring,32768);
    if(NT_SUCCESS(status))status=GpuMap(g,0,&g->Ring,0x40040000);
    return status;
}
static NTSTATUS Core(GPU *g) {

    Gw(g,REG_A6XX_GMU_HOST2GMU_INTR_SET,1u<<30);
    NTSTATUS status=GpuPoll(g->Registers,REG_A6XX_GMU_GMU2HOST_INTR_INFO*4,0x80000000,0x80000000,100000);
    Gw(g,REG_A6XX_GMU_GMU2HOST_INTR_CLR,0x80000000);
    if(!NT_SUCCESS(status))return status;
    g->Stage=6;
    Pipe(g,0);Gw(g,REG_A6XX_GBIF_HALT,0);Gw(g,REG_A8XX_RBBM_GBIF_HALT,0);
    Gw(g,REG_A6XX_RBBM_SECVID_TSB_CNTL,0);Gw64(g,REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE,0);Gw(g,REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE,0);
    Gw(g,REG_A8XX_RBBM_PERFCTR_GPU_BUSY_MASKED,MAXULONG);
    for(ULONG i=0;i<RTL_NUMBER_OF(GpuRegisters);i++)for(ULONG p=0;p<3;p++)if(GpuRegisters[i].Pipes&(1u<<p)) {
        if(GpuRegisters[i].Register==REG_A8XX_RB_GC_GMEM_PROTECT)continue;
        Pipe(g,p);Gw(g,GpuRegisters[i].Register,GpuRegisters[i].Value);
    }
    Pipe(g,0);
    Gw64(g,REG_A8XX_UCHE_CCHE_GC_GMEM_RANGE_MIN,0x4000000);Gw64(g,REG_A8XX_SP_HLSQ_GC_GMEM_RANGE_MIN,0x4000000);
    static const ULONG traps[]={REG_A8XX_UCHE_WRITE_THRU_BASE,REG_A8XX_UCHE_TRAP_BASE,REG_A8XX_UCHE_CCHE_WRITE_THRU_BASE,REG_A8XX_UCHE_CCHE_TRAP_BASE};
    for(ULONG i=0;i<RTL_NUMBER_OF(traps);i++)Gw64(g,traps[i],0x1FFFFFFFFF000ULL);
    Gw(g,REG_A6XX_SP_NC_MODE_CNTL,0x26);Gw(g,REG_A6XX_TPL1_NC_MODE_CNTL,6);
    for(ULONG p=1;p<=2;p++){Pipe(g,p);Gw(g,REG_A8XX_GRAS_NC_MODE_CNTL,0x60);}
    Pipe(g,1);Gw(g,REG_A8XX_RB_CCU_NC_MODE_CNTL,6);Gw(g,REG_A8XX_RB_CMP_NC_MODE_CNTL,0x20006);
    for(ULONG p=1;p<=5;p++) {
        if(p==3)continue;
        Pipe(g,p);Gw(g,REG_A8XX_CP_APRIV_CNTL_PIPE,p==1?0x6F:0xF);
        Gw(g,REG_A8XX_CP_HW_FAULT_STATUS_MASK_PIPE,0xFFFFFFFF);
        Gw(g,REG_A8XX_CP_INTERRUPT_STATUS_MASK_PIPE,0xFFFFFFFF);
    }
    Pipe(g,0);Gw(g,REG_A8XX_CP_INTERRUPT_STATUS_MASK_GLOBAL,0xFFFFFFFF);
    Gw(g,REG_A8XX_RBBM_INT_0_MASK,0xFFFFFFFF);
    Gw64(g,REG_A8XX_CP_SQE_INSTR_BASE,g->Sqe.Address);
    Gw64(g,REG_A8XX_CP_AQE_INSTR_BASE_0,g->Aqe.Address);
    Gw64(g,REG_A6XX_CP_RB_BASE,g->Ring.Address);Gw(g,REG_A6XX_CP_RB_CNTL,0x20C);
    Gw64(g,REG_A6XX_CP_RB_RPTR_ADDR,g->Memory.Address+64);Gw64(g,REG_A8XX_CP_RB_RPTR_ADDR_BV,g->Memory.Address+68);
    g->WriteIndex=0;Gw(g,REG_A8XX_CP_SQE_CNTL,1);
    Emit(g,Packet(CP_THREAD_CONTROL,1));Emit(g,1u<<27);
    Emit(g,Packet(CP_ME_INIT,7));Emit(g,0x14B);Emit(g,3);Emit(g,0x20000000);Emit(g,2);
    Emit(g,(ULONG)g->Memory.Address+256);Emit(g,(ULONG)(g->Memory.Address>>32));Emit(g,0x80000000);

    status=Idle(g);if(!NT_SUCCESS(status))return status;
    Emit(g,Packet(CP_SET_SECURE_MODE,1));Emit(g,0);

    status=Idle(g);if(!NT_SUCCESS(status))return status;
    Pipe(g,1);Gw(g,REG_A8XX_RB_GC_GMEM_PROTECT,0x0C000000);Pipe(g,0);
    Gw(g,REG_A8XX_RBBM_CGC_0_PC,0x702);
    Gw(g,0x23B09,0);Gw(g,0x23B0A,0x110111);Gw(g,0x23B0B,0x55555);
    Gw(g,REG_A8XX_RBBM_CLOCK_CNTL_GLOBAL,1);Gw(g,REG_A8XX_RBBM_CGC_GLOBAL_LOAD_CMD,1);Gw(g,REG_A8XX_RBBM_CGC_P2S_TRIG_CMD,1);
    status=GpuPoll(g->Registers,REG_A8XX_RBBM_CGC_P2S_STATUS*4,1,1,100);
    if(!NT_SUCCESS(status))return status;
    Gw(g,REG_A8XX_RBBM_CLOCK_CNTL_GLOBAL,0);
    Gw(g,0x23b0e,0xff000000);
    g->Stage=6;
    Emit(g,Packet(CP_MEM_WRITE,3));Emit(g,(ULONG)g->Memory.Address+128);Emit(g,0);Emit(g,0x08308750);
    Fence(g);status=GpuWait(g,g->Submitted,10000000);
    if(NT_SUCCESS(status) && *(volatile ULONG *)(g->Memory.Cpu+128)!=0x08308750)status=STATUS_DEVICE_DATA_ERROR;
    return status;
}
static NTSTATUS StartLocked(GPU *g) {
    KIRQL contextIrql;KeAcquireSpinLock(&g->RingLock,&contextIrql);g->LastContextValid=FALSE;g->LastContextCookie=g->LastRoot=0;KeReleaseSpinLock(&g->RingLock,contextIrql);
    g->Fault=0;g->Submitted=0;g->Sequence=0;g->GmuResetReleased=FALSE;
    g->LastSubmitTime=g->PendingSince=g->RaisedAt=0;
    g->Stage=1;
    NTSTATUS status=GpuPower(g,TRUE);
    if(NT_SUCCESS(status)){g->Powered=TRUE;g->Stage=2;status=GpuMmuStart(g);}
    if(NT_SUCCESS(status))status=Buffers(g);
    if(NT_SUCCESS(status)){g->Stage=3;status=GpuGmuStart(g);}
    if(NT_SUCCESS(status))status=Core(g);
    if(NT_SUCCESS(status)){KIRQL old;KeAcquireSpinLock(&g->RingLock,&old);g->Stage=7;g->Online=TRUE;KeReleaseSpinLock(&g->RingLock,old);}
    else {g->Fault=(ULONG)status;GpuStop(g);}
    return status;
}
static VOID StopLocked(GPU *g) {
    /* Serialize the Online gate with the ring producer before any ring or
       completion storage can be freed. Submit rechecks it inside RingLock. */
    KIRQL old;KeAcquireSpinLock(&g->RingLock,&old);g->Online=FALSE;KeReleaseSpinLock(&g->RingLock,old);
    if(g->Powered && g->Stage>=6) {
        Gw(g,REG_A8XX_CP_SQE_CNTL,3);Gw(g,REG_A6XX_GMU_HOST2GMU_INTR_SET,0x80000000);
        for(ULONG bit=1;bit<=2;bit++) {
            Gw(g,REG_A6XX_GBIF_HALT,bit);GpuPoll(g->Registers,0x3C46*4,bit,bit,10000);
        }
        Gw(g,REG_A6XX_GBIF_HALT,0);
    }
    if(g->Powered && g->Stage>=3)GpuGmuStop(g);
    BOOLEAN powered=g->Powered;g->Powered=FALSE;KeMemoryBarrier();
    if(g->Synchronize)g->Synchronize(g->Host);
    if(powered)GpuMmuStop(g);
    GPU_BO *internal[]={&g->Cache,&g->Dummy,&g->Debug,&g->Log,&g->Hfi,&g->Sqe,&g->Aqe,&g->Ring,&g->Memory};
    for(ULONG i=0;i<RTL_NUMBER_OF(internal);i++)GpuFree(internal[i]);
    if(g->ClockMask)GpuPower(g,FALSE);
}

NTSTATUS GpuStart(GPU *g) {
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    KeWaitForSingleObject(&g->PowerLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=StartLocked(g);KeReleaseMutex(&g->PowerLock,FALSE);KeReleaseMutex(&g->MappingLock,FALSE);return status;
}
VOID GpuStop(GPU *g) {
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    KeWaitForSingleObject(&g->PowerLock,Executive,KernelMode,FALSE,NULL);
    StopLocked(g);KeReleaseMutex(&g->PowerLock,FALSE);KeReleaseMutex(&g->MappingLock,FALSE);
}
