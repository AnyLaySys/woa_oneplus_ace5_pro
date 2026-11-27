#pragma once

/* An unused context with CBAR=0 has no assigned translation format. The
   native arm_smmu_write_context_bank()/destroy_domain_context() release it
   by disabling SCTLR, never by reconstructing CBAR=0 (stage-2 translation).
   Keep stage-1 CBAR legal and invalidate translations before freeing tables. */
static inline BOOLEAN CamFrameContextFormat(ULONG cbar) {
    return !cbar||((cbar>>16)&3)==1;
}

static inline BOOLEAN CamFrameContextUnreferenced(PUCHAR base,ULONG bank) {
    if(!base||bank>=16)return FALSE;
    for(ULONG i=0;i<128;i++)
        if((CamRead(base,0x800+i*4)&0x80000000u)&&
           (CamRead(base,0xc00+i*4)&0x300ffu)==bank)return FALSE;
    return TRUE;
}

/* Called only after context disable, TLB synchronization and original route
   restoration. Reject a late route claimant or pending invalidation. */
static inline NTSTATUS CamFrameContextRetire(PUCHAR base,ULONG bank,const ULONG before[12],ULONG sctlr) {
    if(!before||bank>=16||!CamFrameContextFormat(before[0])||(sctlr&1))return STATUS_INVALID_PARAMETER;
    ULONG context=0x80000+bank*4096;
    if(!CamFrameContextUnreferenced(base,bank)||(CamRead(base,context)&1)||CamRead(base,context+0x7f4))return STATUS_DEVICE_BUSY;
    const ULONG offsets[]={0x1000+bank*4,0x1800+bank*4,context+4,context+0x10,context+0x30,
        context+0x20,context+0x24,context+0x28,context+0x2c,context+0x34,context+0x38,context+0x3c};
    if(before[0]) {
        for(ULONG i=12;i;i--)CamWrite(base,offsets[i-1],before[i-1]);
        for(ULONG i=0;i<12;i++)if(CamRead(base,offsets[i])!=before[i])return STATUS_IO_DEVICE_ERROR;
        CamWrite(base,context,sctlr);
        if(CamRead(base,context)!=sctlr)return STATUS_IO_DEVICE_ERROR;
    } else {
        for(ULONG i=5;i<=8;i++) {
            CamWrite(base,offsets[i],0);
            if(CamRead(base,offsets[i]))return STATUS_IO_DEVICE_ERROR;
        }
    }
    return CamRead(base,context)&1?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
}
