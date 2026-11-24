#include "gpu.h"
#include "mmu-table.h"

static NTSTATUS Allocate(GPU_BO *bo,ULONG size,MEMORY_CACHING_TYPE cache) {
    if(!size || size>GPU_BUFFER_LIMIT)return STATUS_INVALID_BUFFER_SIZE;
    size=(size+4095)&~4095u;
    PHYSICAL_ADDRESS low={0},high,skip={0};high.QuadPart=0xFFFFFFFFFFF;
    bo->Mdl=MmAllocatePagesForMdlEx(low,high,skip,size,cache,MM_ALLOCATE_FULLY_REQUIRED);
    if(!bo->Mdl)return STATUS_INSUFFICIENT_RESOURCES;
    bo->Cpu=MmMapLockedPagesSpecifyCache(bo->Mdl,KernelMode,cache,NULL,FALSE,NormalPagePriority|MdlMappingNoExecute);
    if(!bo->Cpu){MmFreePagesFromMdl(bo->Mdl);ExFreePool(bo->Mdl);bo->Mdl=NULL;return STATUS_INSUFFICIENT_RESOURCES;}
    bo->Size=size;bo->Physical=(ULONGLONG)MmGetMdlPfnArray(bo->Mdl)[0]<<12;
    RtlZeroMemory(bo->Cpu,size);KeMemoryBarrier();
    bo->Cached=cache==MmCached;
    if(bo->Cached)KeFlushIoBuffers(bo->Mdl,TRUE,TRUE);
    return STATUS_SUCCESS;
}
NTSTATUS GpuAllocate(GPU_BO *bo,ULONG size){return Allocate(bo,size,MmWriteCombined);}
NTSTATUS GpuAllocateCached(GPU_BO *bo,ULONG size){return Allocate(bo,size,MmCached);}
VOID GpuFree(GPU_BO *bo) {
    if(!bo->Mdl)return;
    if(bo->Contiguous)MmFreeContiguousMemory(bo->Cpu);
    else { MmUnmapLockedPages(bo->Cpu,bo->Mdl);MmFreePagesFromMdl(bo->Mdl); }
    ExFreePool(bo->Mdl);RtlZeroMemory(bo,sizeof(*bo));
}
static NTSTATUS Tables(GPU_VM *vm) {
    GPU_BO *b=&vm->Tables;
    if(b->Mdl)return STATUS_SUCCESS;
    PHYSICAL_ADDRESS low={0},high,skip={0};high.QuadPart=0xFFFFFFFF;
    /* The 3-level 4K tables cover the supported VA below 16 GB.
     * Sixteen L2 pages plus 8192 L3 pages fit even after mapping churn.
     */
    b->Size=GPU_TABLE_BYTES;
    b->Cpu=MmAllocateContiguousMemorySpecifyCache(b->Size,low,high,skip,MmWriteCombined);
    if(!b->Cpu)return STATUS_INSUFFICIENT_RESOURCES;
    b->Mdl=IoAllocateMdl(b->Cpu,b->Size,FALSE,FALSE,NULL);
    if(!b->Mdl){MmFreeContiguousMemory(b->Cpu);b->Cpu=NULL;return STATUS_INSUFFICIENT_RESOURCES;}
    MmBuildMdlForNonPagedPool(b->Mdl);b->Contiguous=TRUE;b->Physical=MmGetPhysicalAddress(b->Cpu).QuadPart;
    RtlZeroMemory(b->Cpu,b->Size);vm->Used=1;
    return STATUS_SUCCESS;
}
static NTSTATUS Flush(GPU *g,ULONG vm) {
    PUCHAR cb=g->Registers+0xA0000+g->CbBase+g->Vm[vm].Bank*4096;
    KeMemoryBarrier();
    GpuWrite(cb,0x610,0);GpuWrite(cb,0x7F0,0);
    return GpuPoll(cb,0x7F4,1,0,10000);
}
static NTSTATUS MapRangeLocked(GPU *g,ULONG vm,ULONGLONG address,ULONGLONG size,GPU_BO *bo,ULONGLONG offset,BOOLEAN zero,BOOLEAN flush) {
    if(vm>1 || !size || ((address|size|offset)&4095) || !address || address>=GPU_VA_END || size>GPU_VA_END-address ||
       (bo && (!bo->Mdl || offset>bo->Size || size>bo->Size-offset)))return STATUS_INVALID_PARAMETER;
    if(!g->Vm[vm].Tables.Mdl || !g->Vm[vm].Tables.Cpu || !g->Registers)return STATUS_DEVICE_NOT_READY;
    if(!bo && zero) {
        PUCHAR m=g->Registers+0xA0000;
        if(!g->Prr.Mdl) {
            NTSTATUS status=GpuAllocate(&g->Prr,4096);if(!NT_SUCCESS(status))return status;
            GpuWrite(m,0x6008,(ULONG)g->Prr.Physical);GpuWrite(m,0x600C,(ULONG)(g->Prr.Physical>>32));
        }
        PUCHAR cb=m+g->CbBase+g->Vm[vm].Bank*4096;
        ULONG actlr=GpuRead(cb,4);if(!(actlr&32))GpuWrite(cb,4,actlr|32);
    }
    BOOLEAN present=bo || zero;
    for(ULONGLONG at=0;present && at<size;) {
        ULONGLONG current=address+at;
        if(!Entry(g->Vm+vm,current,TRUE))return STATUS_INSUFFICIENT_RESOURCES;
        at+=min(size-at,0x200000-(current&0x1fffff));
    }
    PPFN_NUMBER pfn=bo?MmGetMdlPfnArray(bo->Mdl):NULL;
    ULONG attributes=0xE43;
    if(bo==&g->Sqe || bo==&g->Aqe)attributes|=0x80;
    else if(address<(vm?0x60000000:0x44000000))attributes&=~0x40u;
    for(ULONGLONG at=0;at<size;) {
        ULONGLONG current=address+at;
        ULONG count=(ULONG)(min(size-at,0x200000-(current&0x1fffff))>>12);
        PULONGLONG p=Entry(g->Vm+vm,current,FALSE);
        if(p)for(ULONG i=0;i<count;i++)p[i]=bo?((ULONGLONG)pfn[((offset+at)>>12)+i]<<12)|attributes:zero?g->Prr.Physical|attributes:0;
        at+=(ULONGLONG)count<<12;
    }
    return flush?Flush(g,vm):STATUS_SUCCESS;
}
static NTSTATUS MapLocked(GPU *g,ULONG vm,GPU_BO *bo,ULONGLONG address) {
    if(!bo || !bo->Mdl)return STATUS_INVALID_PARAMETER;
    NTSTATUS status=MapRangeLocked(g,vm,address,bo->Size,bo,0,FALSE,TRUE);
    if(NT_SUCCESS(status))bo->Address=address;
    return status;
}
static VOID UnmapLocked(GPU *g,ULONG vm,GPU_BO *bo) {
    if(!bo->Address)return;
    if(vm>1 || !g->Vm[vm].Tables.Mdl || !g->Vm[vm].Tables.Cpu || !g->Registers){bo->Address=0;return;}
    for(ULONG i=0;i<bo->Size/4096;) {
        ULONGLONG current=bo->Address+((ULONGLONG)i<<12);
        ULONG count=min(bo->Size/4096-i,512u-(ULONG)((current>>12)&511));
        PULONGLONG p=Entry(g->Vm+vm,current,FALSE);
        if(p)for(ULONG j=0;j<count;j++)p[j]=0;
        i+=count;
    }
    NTSTATUS status=Flush(g,vm);bo->Address=0;
    if(!NT_SUCCESS(status)){g->Fault=(ULONG)status;GpuStop(g);}
}
static NTSTATUS MmuStartLocked(GPU *g) {
    PUCHAR m=g->Registers+0xA0000;
    ULONG id=GpuRead(m,0x20),id1=GpuRead(m,0x24),id2=GpuRead(m,0x28);
    g->CbBase=(1u<<(((id1>>28)&7)+1))*4096;
    if(g->CbBase+(id1&255)*4096>0x40000)return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(!(id&(1u<<30)) || !(id2&0x1000) || (id1&255)<2 || (id1&0x80000000))return STATUS_NOT_SUPPORTED;
    for(ULONG i=0;i<2;i++) {
        GPU_VM *vm=g->Vm+i;vm->Bank=i;
        NTSTATUS status=Tables(vm);if(!NT_SUCCESS(status))return status;
        PUCHAR cb=m+g->CbBase+i*4096;
        GpuWrite(cb,0,0);
        GpuWrite(m,0x1000+i*4,0x1F300);
        GpuWrite(m,0x1800+i*4,1);
        GpuWrite(cb,0x10,0x38000|((id2>>4)&15)|0x10);
        /* AArch64 stage-1, 4K granule, 39-bit input (T0SZ=25). */
        GpuWrite(cb,0x30,0x803019);
        GpuWrite(cb,0x20,(ULONG)vm->Tables.Physical);GpuWrite(cb,0x24,(ULONG)(vm->Tables.Physical>>32));
        GpuWrite(cb,0x28,0);GpuWrite(cb,0x2C,0);
        GpuWrite(cb,0x38,0xFF44);GpuWrite(cb,0x3C,0);
        GpuWrite(cb,0x58,0xFFFFFFFF);
        GpuWrite(cb,0,0x1167);
    }
    g->LastRoot=0;
    static const ULONG streams[]={0,1,5};
    for(ULONG i=0;i<RTL_NUMBER_OF(streams);i++) {
        ULONG sid=streams[i];
        GpuWrite(m,0xC00+sid*4,sid==5?1:0);
        GpuWrite(m,0x800+sid*4,0x80000000|sid);
    }
    GpuWrite(m,0x60,0);GpuWrite(m,0x70,0);
    GpuWrite(m,0x0,GpuRead(m,0x0)&~0x403u);
    return GpuPoll(m,0x74,1,0,10000);
}
static VOID MmuStopLocked(GPU *g) {
    if(!g->Vm[0].Tables.Mdl)return;
    PUCHAR m=g->Registers+0xA0000;
    GpuWrite(m,0x800,0);GpuWrite(m,0x804,0);GpuWrite(m,0x814,0);
    GpuWrite(m,g->CbBase,0);GpuWrite(m,g->CbBase+4096,0);
    GpuFree(&g->Prr);
}

/* MappingLock is a recursive dispatcher mutex. GPU start/stop hold it for
   their entire hardware transition, and every page-table access joins it.
   Core initialization can map while Online=false once Tables exist. All
   callers are PASSIVE/APC paths; ISR/DPC and Submit never acquire this mutex.
   No function entered under MappingLock acquires the outer Bridge.Lock. */
NTSTATUS GpuMapRange(GPU *g,ULONG vm,ULONGLONG address,ULONGLONG size,GPU_BO *bo,ULONGLONG offset,BOOLEAN zero,BOOLEAN flush) {
    if(KeGetCurrentIrql()>APC_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=MapRangeLocked(g,vm,address,size,bo,offset,zero,flush);
    KeReleaseMutex(&g->MappingLock,FALSE);return status;
}
NTSTATUS GpuFlushMappings(GPU *g,ULONG vm) {
    if(vm>1 || KeGetCurrentIrql()>APC_LEVEL)return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=!g->Online?STATUS_SUCCESS:
        g->Registers && g->Vm[vm].Tables.Mdl?Flush(g,vm):STATUS_DEVICE_NOT_READY;
    KeReleaseMutex(&g->MappingLock,FALSE);return status;
}
NTSTATUS GpuMap(GPU *g,ULONG vm,GPU_BO *bo,ULONGLONG address) {
    if(KeGetCurrentIrql()>APC_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=MapLocked(g,vm,bo,address);
    KeReleaseMutex(&g->MappingLock,FALSE);return status;
}
VOID GpuUnmap(GPU *g,ULONG vm,GPU_BO *bo) {
    if(KeGetCurrentIrql()>APC_LEVEL)return;
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    UnmapLocked(g,vm,bo);KeReleaseMutex(&g->MappingLock,FALSE);
}
NTSTATUS GpuMmuStart(GPU *g) {
    if(KeGetCurrentIrql()>APC_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    NTSTATUS status=MmuStartLocked(g);KeReleaseMutex(&g->MappingLock,FALSE);return status;
}
VOID GpuMmuStop(GPU *g) {
    KeWaitForSingleObject(&g->MappingLock,Executive,KernelMode,FALSE,NULL);
    MmuStopLocked(g);KeReleaseMutex(&g->MappingLock,FALSE);
}
