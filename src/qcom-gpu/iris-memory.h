#pragma once
#include "../platform/lock.h"

/* Keep physical firmware, queues and encoder inputs identity mapped. Decoder
   pages use the nonsecure firmware IOVA window below system RAM. */
#define IRIS_IOVA_START 0x25800000u
#define IRIS_IOVA_END 0x80000000u
#define IRIS_MEMORY_BYTES ((5+(IRIS_IOVA_END-IRIS_IOVA_START)/0x200000)*4096u)
typedef struct {
    PUCHAR Registers;
    GPU_BO Tables;
    ULONG Bank,Stream,Slot,Control,Before[12],Sctlr;
    NTSTATUS Error;
    BOOLEAN Owned,Route;
} IRIS_MEMORY;
static PULONGLONG IrisMemoryLeaf(IRIS_MEMORY *m,ULONG address) {
    return (PULONGLONG)m->Tables.Cpu+(5+(address-IRIS_IOVA_START)/0x200000)*512+((address>>12)&511);
}
static NTSTATUS IrisMemorySync(IRIS_MEMORY *m) {
    ULONG context=0x80000+m->Bank*4096;
    KeMemoryBarrier();GpuWrite(m->Registers,context+0x610,0);GpuWrite(m->Registers,context+0x7f0,0);
    NTSTATUS status=GpuPoll(m->Registers,context+0x7f4,1,0,10000);
    if(!NT_SUCCESS(status))m->Error=status;
    return status;
}
static NTSTATUS IrisMemoryStop(IRIS_MEMORY *m) {
    if(!m->Owned){GpuFree(&m->Tables);return STATUS_SUCCESS;}
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    PUCHAR base=m->Registers;ULONG context=0x80000+m->Bank*4096,count=min(GpuRead(base,0x20)&255u,128u);
    /* PAS shutdown must have completed before restoring these routes. */
    if(m->Route) {
        ULONG control=GpuRead(base,0xc00+m->Slot*4);
        if(GpuRead(base,0x800+m->Slot*4)!=(0x80000000u|m->Stream) ||
           (control!=m->Bank && control!=m->Control)){status=STATUS_DEVICE_BUSY;goto done;}
        GpuWrite(base,0xc00+m->Slot*4,m->Control);
        if(GpuRead(base,0xc00+m->Slot*4)!=m->Control){status=STATUS_DEVICE_HARDWARE_ERROR;goto done;}
        m->Route=FALSE;
    }
    for(ULONG i=0;NT_SUCCESS(status) && i<count;i++)
        if((GpuRead(base,0x800+i*4)&0x80000000u) && (GpuRead(base,0xc00+i*4)&0x300ffu)==m->Bank)status=STATUS_DEVICE_BUSY;
    if(NT_SUCCESS(status)) {
        GpuWrite(base,context,0);
        status=(GpuRead(base,context)&1)?STATUS_DEVICE_HARDWARE_ERROR:IrisMemorySync(m);
    }
    if(NT_SUCCESS(status)) {
        const ULONG offsets[]={0x1000+m->Bank*4,0x1800+m->Bank*4,context+4,context+0x10,context+0x30,
            context+0x20,context+0x24,context+0x28,context+0x2c,context+0x34,context+0x38,context+0x3c};
        if(m->Before[0]) {
            for(ULONG i=12;i--;)GpuWrite(base,offsets[i],m->Before[i]);
            GpuWrite(base,context,m->Sctlr);
            for(ULONG i=0;i<12;i++)if(GpuRead(base,offsets[i])!=m->Before[i])status=STATUS_DEVICE_HARDWARE_ERROR;
        } else {
            /* CBAR=0 means unassigned, not a stage-1 format to restore. */
            for(ULONG i=5;i<=8;i++)GpuWrite(base,offsets[i],0);
        }
        if(NT_SUCCESS(status))m->Owned=FALSE;
    }
done:;
    NTSTATUS released=PlatformLockRelease(&guard);
    if(!NT_SUCCESS(status))return status;
    if(!NT_SUCCESS(released))return released;
    GpuFree(&m->Tables);return STATUS_SUCCESS;
}
static NTSTATUS IrisMemoryStart(IRIS_MEMORY *m,PUCHAR base,ULONG stream) {
    ULONG id=GpuRead(base,0x20),id1=GpuRead(base,0x24),id2=GpuRead(base,0x28);
    if(!(id&(1u<<27)) || !(id2&0x1000) || (id1&0x80000000u) ||
       (1u<<(((id1>>28)&7)+1))!=128)return STATUS_NOT_SUPPORTED;
    PPHYSICAL_MEMORY_RANGE ranges=MmGetPhysicalMemoryRanges();
    if(!ranges)return STATUS_INSUFFICIENT_RESOURCES;
    BOOLEAN conflict=FALSE;
    for(ULONG i=0;ranges[i].NumberOfBytes.QuadPart;i++)
        if(ranges[i].BaseAddress.QuadPart<IRIS_IOVA_END &&
           ranges[i].BaseAddress.QuadPart+ranges[i].NumberOfBytes.QuadPart>IRIS_IOVA_START)conflict=TRUE;
    ExFreePool(ranges);if(conflict)return STATUS_DEVICE_CONFIGURATION_ERROR;
    NTSTATUS status=GpuAllocateContiguous(&m->Tables,IRIS_MEMORY_BYTES,MmNonCached,0xdfffffff,0);
    if(!NT_SUCCESS(status))return status;
    m->Registers=base;m->Stream=stream;
    PULONGLONG table=(PULONGLONG)m->Tables.Cpu;ULONGLONG physical=m->Tables.Physical;
    for(ULONG i=0;i<4;i++)table[i]=(physical+(i+1)*4096)|3;
    for(ULONG i=0;i<2048;i++) {
        ULONG address=i*0x200000u;
        table[512+i]=address>=IRIS_IOVA_START && address<IRIS_IOVA_END?
            (physical+(5+(address-IRIS_IOVA_START)/0x200000)*4096)|3:(ULONGLONG)address|0xe41;
    }
    PLATFORM_LOCK_GUARD guard={0};status=PlatformLockAcquire(&guard,TRUE);
    if(status!=STATUS_SUCCESS){GpuFree(&m->Tables);return status;}
    BOOLEAN used[128]={0};ULONG found=0,bank=MAXULONG,count=min(id&255u,128u);
    for(ULONG i=0;i<count;i++) {
        ULONG smr=GpuRead(base,0x800+i*4),control=GpuRead(base,0xc00+i*4);
        if(!(smr&0x80000000u))continue;
        if(!(control&0x30000))used[control&127]=TRUE;
        if(!((smr^stream)&0x7fff&~(smr>>16))) {
            if(smr!=(0x80000000u|stream) || found){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
            m->Slot=i;m->Control=control;found=1;
        }
    }
    if(!found){status=STATUS_DEVICE_NOT_READY;goto done;}
    for(ULONG i=0;i<min(id1&255u,16u);i++)if(!used[i]) {
        ULONG cbar=GpuRead(base,0x1000+i*4),context=0x80000+i*4096;
        if((!cbar || (cbar&0x300ff)==0x10000 || (cbar&0x300ff)==0x100ff) &&
           !(GpuRead(base,context)&1) && !(GpuRead(base,context+0x58)&0xc00001ffu)){bank=i;break;}
    }
    if(bank==MAXULONG){status=STATUS_NOT_SUPPORTED;goto done;}
    {
    m->Bank=bank;
    ULONG context=0x80000+bank*4096;
    const ULONG offsets[]={0x1000+bank*4,0x1800+bank*4,context+4,context+0x10,context+0x30,
        context+0x20,context+0x24,context+0x28,context+0x2c,context+0x34,context+0x38,context+0x3c};
    const ULONG values[]={0x1f300,1,1,0x38010|((id2>>4)&15),0x803020,
        (ULONG)physical,(ULONG)(physical>>32),0,0,bank,0xff44,0};
    for(ULONG i=0;i<12;i++)m->Before[i]=GpuRead(base,offsets[i]);
    m->Sctlr=GpuRead(base,context);m->Owned=TRUE;
    for(ULONG i=0;i<12;i++) {
        GpuWrite(base,offsets[i],values[i]);
        ULONG mask=i==3?0x3801f:i==4?0x00c0ff3f:MAXULONG;
        if((GpuRead(base,offsets[i])^values[i])&mask){status=STATUS_DEVICE_HARDWARE_ERROR;goto done;}
    }
    KeMemoryBarrier();GpuWrite(base,context,0x27);
    if((GpuRead(base,context)&0x27)!=0x27){status=STATUS_DEVICE_HARDWARE_ERROR;goto done;}
    status=IrisMemorySync(m);if(!NT_SUCCESS(status))goto done;
    m->Route=TRUE;GpuWrite(base,0xc00+m->Slot*4,bank);
    if(GpuRead(base,0xc00+m->Slot*4)!=bank)status=STATUS_DEVICE_HARDWARE_ERROR;

    }
done:;
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}
static NTSTATUS IrisMemoryAllocate(IRIS_MEMORY *m,GPU_BO *bo,ULONG bytes,BOOLEAN cached) {
    if(!m->Owned || !m->Route || !NT_SUCCESS(m->Error) || !bytes || bytes>GPU_BUFFER_LIMIT || bo->Mdl)return STATUS_INVALID_DEVICE_STATE;
    bytes=(bytes+4095)&~4095u;
    ULONG pages=bytes/4096,run=0,address=0;
    for(ULONG at=IRIS_IOVA_START;at<IRIS_IOVA_END;at+=4096) {
        run=*IrisMemoryLeaf(m,at)?0:run+1;
        if(run==pages){address=at+4096-bytes;break;}
    }
    if(!address)return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=cached?GpuAllocateCached(bo,bytes):GpuAllocate(bo,bytes);if(!NT_SUCCESS(status))return status;
    PPFN_NUMBER pfns=MmGetMdlPfnArray(bo->Mdl);
    for(ULONG i=0;i<pages;i++)*IrisMemoryLeaf(m,address+i*4096)=((ULONGLONG)pfns[i]<<12)|(cached?0xe47:0xe43);
    bo->Address=address;
    return IrisMemorySync(m);
}
static NTSTATUS IrisMemoryBorrow(IRIS_MEMORY *m,GPU_BO *target,GPU_BO *source,ULONG offset,ULONG chroma,ULONG luma,ULONG bytes) {
    if(!m->Owned || !m->Route || !NT_SUCCESS(m->Error) || !source->Mdl || target->Mdl ||
       !bytes || ((offset|chroma|luma|bytes)&4095) || luma>bytes || offset>source->Size || luma>source->Size-offset ||
       chroma>source->Size || bytes-luma>source->Size-chroma)return STATUS_INVALID_PARAMETER;
    ULONG pages=bytes/4096,run=0,address=0;
    for(ULONG at=IRIS_IOVA_START;at<IRIS_IOVA_END;at+=4096) {
        run=*IrisMemoryLeaf(m,at)?0:run+1;
        if(run==pages){address=at+4096-bytes;break;}
    }
    if(!address)return STATUS_INSUFFICIENT_RESOURCES;
    PVOID virtualAddress=(PVOID)((ULONG_PTR)MmGetMdlVirtualAddress(source->Mdl)+offset);
    PMDL mdl=IoAllocateMdl(virtualAddress,bytes,FALSE,FALSE,NULL);
    if(!mdl)return STATUS_INSUFFICIENT_RESOURCES;
    /* GPU arrays may group all luma layers before all chroma layers. Present
       their two page runs as one contiguous Iris IOVA without copying pixels. */
    mdl->MdlFlags|=MDL_PARTIAL;
    PPFN_NUMBER original=MmGetMdlPfnArray(source->Mdl),pfns=MmGetMdlPfnArray(mdl);
    for(ULONG i=0;i<pages;i++)pfns[i]=original[i<luma/4096?offset/4096+i:chroma/4096+i-luma/4096];
    target->Mdl=mdl;target->Size=bytes;target->Address=address;target->Cached=source->Cached;
    for(ULONG i=0;i<pages;i++)*IrisMemoryLeaf(m,address+i*4096)=((ULONGLONG)pfns[i]<<12)|(source->Cached?0xe47:0xe43);
    return IrisMemorySync(m);
}
static NTSTATUS IrisMemoryReturn(IRIS_MEMORY *m,GPU_BO *bo) {
    if(bo->Address && m->Owned) {
        for(ULONG i=0;i<bo->Size;i+=4096)*IrisMemoryLeaf(m,(ULONG)bo->Address+i)=0;
        NTSTATUS status=IrisMemorySync(m);if(!NT_SUCCESS(status))return status;
    }
    if(bo->Mdl)IoFreeMdl(bo->Mdl);
    RtlZeroMemory(bo,sizeof(*bo));return STATUS_SUCCESS;
}
static NTSTATUS IrisMemoryFree(IRIS_MEMORY *m,GPU_BO *bo) {
    if(bo->Address && m->Owned) {
        for(ULONG i=0;i<bo->Size;i+=4096)*IrisMemoryLeaf(m,(ULONG)bo->Address+i)=0;
        NTSTATUS status=IrisMemorySync(m);if(!NT_SUCCESS(status))return status;
    }
    GpuFree(bo);return STATUS_SUCCESS;
}
