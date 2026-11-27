#pragma once

/* Original Apollo980 dump emitter0x17b950, frontend SHA256d779e1d2...,
   emits read mode0x100000, selector at DMI base+4, and repeated reads
   at base+8: GTM6608/660c/6610 (512 words), Gamma6b08/6b0c/6b10
   (64 words per channel, context6cf4). Read only the already selected
   loaded bank; never write a DMI data port or select the original bank.
   Caller must own powered, idle ISP and exclude command/image DMA. */
static inline NTSTATUS CamIspLutRead(PUCHAR base,BOOLEAN gtm,ULONG selector,ULONG bank,
    const ULONG *expected,ULONG *observed,ULONG *count,ULONG *mismatch,NTSTATUS *restore) {
    if(!base||!expected||!observed||!count||!mismatch||!restore||bank>1||
       (gtm!=TRUE&&gtm!=FALSE)||!selector||selector>(gtm?1u:3u))return STATUS_INVALID_PARAMETER;
    *count=*mismatch=0;*restore=STATUS_SUCCESS;
    if(CamRead(base,0)!=0x10000000)return STATUS_REVISION_MISMATCH;
    ULONG cfg=gtm?0x6608:0x6b08,addr=cfg+4,data=cfg+8,words=gtm?512:64;
    if(CamRead(base,gtm?0x6658:0x6b58)!=bank)return STATUS_DEVICE_BUSY;
    ULONG beforeCfg=CamRead(base,cfg),beforeAddr=CamRead(base,addr);
    ULONG beforeContext=gtm?0:CamRead(base,0x6cf4);
    /* Native FD readback uses context0. A different context could expose
       another bank behind the same selector; do not switch into it. */
    if(!gtm&&beforeContext!=0)return STATUS_DEVICE_BUSY;
    if(!gtm)CamWrite(base,0x6cf4,0);
    CamWrite(base,cfg,0x100000);CamWrite(base,addr,selector);
    NTSTATUS status=STATUS_SUCCESS;
    if(CamRead(base,cfg)!=0x100000||CamRead(base,addr)!=selector||
       (!gtm&&CamRead(base,0x6cf4)!=0))status=STATUS_IO_DEVICE_ERROR;
    if(NT_SUCCESS(status))for(ULONG i=0;i<words;i++) {
        observed[i]=CamRead(base,data);(*count)++;
        if(observed[i]!=expected[i])(*mismatch)++;
    }
    /* Restore access state even on a failed setup or mismatched table. */
    CamWrite(base,addr,beforeAddr);CamWrite(base,cfg,beforeCfg);
    if(!gtm)CamWrite(base,0x6cf4,beforeContext);
    if(CamRead(base,addr)!=beforeAddr||CamRead(base,cfg)!=beforeCfg||
       (!gtm&&CamRead(base,0x6cf4)!=beforeContext))*restore=STATUS_IO_DEVICE_ERROR;
    if(!NT_SUCCESS(*restore))return *restore;
    if(NT_SUCCESS(status)&&*mismatch)status=STATUS_DATA_ERROR;
    return status;
}
