#pragma once

static __inline VOID A830FlushPageTable(PVOID table) {
    struct { MDL Header; PFN_NUMBER Page; } mapping={0};
    MmInitializeMdl(&mapping.Header,table,PAGE_SIZE);
    mapping.Header.MdlFlags=MDL_PAGES_LOCKED|MDL_MAPPED_TO_SYSTEM_VA;mapping.Header.MappedSystemVa=table;
    mapping.Page=(PFN_NUMBER)(MmGetPhysicalAddress(table).QuadPart>>12);
    KeFlushIoBuffers(&mapping.Header,FALSE,TRUE);
}

static __inline NTSTATUS A830ResolveGpuAddress(A830_ADAPTER *a,ULONGLONG root,ULONGLONG address,PULONGLONG physical) {
    if(address>>39)return STATUS_INVALID_ADDRESS;
    for(ULONG level=2;;level--) {
        if(!root || (root&4095) || root>>44)return STATUS_INVALID_ADDRESS;
        ULONG index=(ULONG)((address>>(12+9*level))&511);ULONGLONG entry;
        if(a->Memory.Segment.Cpu && root>=a->Memory.Segment.Physical && root-a->Memory.Segment.Physical<=A830_SEGMENT_BYTES-PAGE_SIZE) {
            volatile ULONGLONG *table=(PVOID)(a->Memory.Segment.Cpu+(SIZE_T)(root-a->Memory.Segment.Physical));entry=table[index];
        } else {
            MM_COPY_ADDRESS source;source.PhysicalAddress.QuadPart=root+index*8;SIZE_T copied=0;
            NTSTATUS status=MmCopyMemory(&entry,source,8,MM_COPY_MEMORY_PHYSICAL,&copied);
            if(!NT_SUCCESS(status) || copied!=8)return STATUS_INVALID_ADDRESS;
        }
        KeMemoryBarrier();
        if((entry&3)!=3)return STATUS_INVALID_ADDRESS;
        root=entry&0xFFFFFFFF000ULL;
        if(!level){*physical=root+(address&4095);return STATUS_SUCCESS;}
    }
}

/* WDDM level 0 is the leaf; SMMU stage-1 uses 4K, 512-entry tables. */
static __inline BOOLEAN A830GpuPte(const DXGK_PTE *p,ULONG level,ULONGLONG physical,PULONGLONG out) {
    if(level>2 || p->Zero || p->LargePage || p->PhysicalAdapterIndex ||
       p->PageTablePageSize!=DXGK_PTE_PAGE_TABLE_PAGE_4KB)return FALSE;
    if(!p->Valid){*out=0;return TRUE;}
    if((physical&4095) || (physical>>44))return FALSE;
    if(level) {
        *out=physical|3|(p->ReadOnly?0x4000000000000000ULL:0)|(p->NoExecute?0x1800000000000000ULL:0);
    } else {
        *out=physical|0xE43ULL|(p->CacheCoherent?4ULL:0)|(p->ReadOnly?0x80ULL:0)|
            (p->NoExecute?0x60000000000000ULL:0);
    }
    return TRUE;
}
