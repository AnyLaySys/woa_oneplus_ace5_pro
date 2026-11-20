#include "cam.h"
#include "dma-route.h"
#include "dma-ready.h"
#include "cdm-route.h"
#include "cdm-command.h"
#include "cdm-abort.h"
#include "trace-regs.h"
#include "../platform/lock.h"

static NTSTATUS AllocateCommand(CAM *c) {
    if(c->Command&&c->CommandMdl)return STATUS_SUCCESS;
    if(!c->DmaEnabler)return STATUS_DEVICE_NOT_READY;
    if(!c->Command) {
        PHYSICAL_ADDRESS lowest={0},highest,boundary={0};highest.QuadPart=MAXULONG;
        c->Command=MmAllocateContiguousMemorySpecifyCache(4096,lowest,highest,boundary,MmNonCached);
        if(!c->Command)return STATUS_INSUFFICIENT_RESOURCES;
        c->CommandContiguous=TRUE;c->CommandAddress=MmGetPhysicalAddress(c->Command);
    }
    if(!c->CommandAddress.QuadPart||(c->CommandAddress.QuadPart&255)||
       (ULONGLONG)c->CommandAddress.QuadPart+4095>MAXULONG||
       c->CommandAddress.QuadPart!=MmGetPhysicalAddress(c->Command).QuadPart)return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->CommandMdl=IoAllocateMdl(c->Command,4096,FALSE,FALSE,NULL);
    if(!c->CommandMdl)return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(c->CommandMdl);
    return STATUS_SUCCESS;
}

NTSTATUS CamCdmConfigure(CAM *c) {
    if(c->CdmPending||c->DmaActive||c->RxActive||c->ScmPending||c->CrmPending)return STATUS_DEVICE_BUSY;
    if(!c->Powered||!CamDiagnosticDmaReady(c))return STATUS_DEVICE_NOT_READY;
    PUCHAR b=c->Base[CamCdm];
    c->Diag.Step=161;c->Diag.Region=CamCdm;c->Diag.Address=0x1860;c->Diag.Slot=MAXULONG;
    if(CamRead(b,0)!=0x20020000)return STATUS_REVISION_MISMATCH;
    if(CamRead(b,0x1c)!=1||(CamRead(b,0x6c)&0x01ff01ff)||
       (CamRead(b,0x70)&0x01ff01ff)||(CamRead(b,0x44)&0x70000))return STATUS_DEVICE_BUSY;
    NTSTATUS status=CamSave(c,b,0x1c,3);
    if(NT_SUCCESS(status))status=CamSave(c,b,0x30,0);
    if(NT_SUCCESS(status))status=CamSave(c,b,0x5c,CamRead(b,0x5c));
    if(NT_SUCCESS(status))status=CamSave(c,b,0xd8,CamRead(b,0xd8));
    if(NT_SUCCESS(status))status=CamSave(c,b,0x18,CamRead(b,0x18));
    if(!NT_SUCCESS(status))return status;
    c->Diag.Step=162;c->Diag.Offset=0x44;c->Diag.Mask=1;c->Diag.Expected=1;
    c->CdmPending=TRUE;
    status=CamCdmAbortCore(b);c->Diag.Observed=CamRead(b,0x44);
    if(!NT_SUCCESS(status)) {
        for(ULONG i=0;i<RTL_NUMBER_OF(CamCdmTraceRegs);i++)c->Trace.Cdm[i]=CamRead(b,CamCdmTraceRegs[i]);
        c->Faulted=TRUE;return status;
    }
    c->CdmPending=FALSE;
    CamWrite(b,0x18,0x11f);CamWrite(b,0xd8,0x100);CamWrite(b,0x5c,0x00400000);
    CamWrite(b,0x1c,1);
    if(CamRead(b,0x18)!=0x11f||!(CamRead(b,0xd8)&0x100)||
       CamRead(b,0x5c)!=0x00400000||CamRead(b,0x1c)!=1)return STATUS_IO_DEVICE_ERROR;
    status=AllocateCommand(c);if(!NT_SUCCESS(status))return status;
    c->Diag.Step=161;
    PLATFORM_LOCK_GUARD routeGuard={0};status=PlatformLockAcquire(&routeGuard,TRUE);
    if(status!=STATUS_SUCCESS)return status;
    status=CamCdmRoute(c);NTSTATUS routeReleased=PlatformLockRelease(&routeGuard);
    if(!NT_SUCCESS(status))return status;
    if(!NT_SUCCESS(routeReleased))return routeReleased;
    ULONG irqMask=CamRead(b,0x30),marker=(CamRead(b,0x80)+1)&255;
    status=CamSave(c,b,0x30,0);if(!NT_SUCCESS(status))return status;
    CamWrite(b,0x30,0);
    if(CamRead(b,0x30))return STATUS_IO_DEVICE_ERROR;
    CamWrite(b,0x34,MAXULONG);CamWrite(b,0x38,1);
    if(CamRead(b,0x44)&0x70000){CamWrite(b,0x30,irqMask);return STATUS_IO_DEVICE_ERROR;}
    RtlZeroMemory(c->Command,4096);CamCdmEncode(c->Command,marker);
    KeFlushIoBuffers(c->CommandMdl,FALSE,TRUE);
    KeMemoryBarrier();
    RtlCopyMemory(c->Trace.CommandWords,c->Command,CAM_CDM_COMMAND_BYTES);
    c->Trace.CommandAddress=c->CommandAddress.QuadPart;c->Trace.CommandMarker=marker;
    c->CdmPending=TRUE;
    CamWrite(b,0x50,(ULONG)c->CommandAddress.QuadPart);
    CamWrite(b,0x54,CAM_CDM_COMMAND_BYTES-1);
    CamWrite(b,0x58,1);
    ULONGLONG deadline=KeQueryInterruptTime()+1000000;
    status=STATUS_IO_TIMEOUT;
    do {
        if(CamCdmCompleted(b,marker)){status=STATUS_SUCCESS;break;}
        if(CamRead(b,0x44)&0x70000){status=STATUS_IO_DEVICE_ERROR;break;}
        CamDelay(100);
    } while(KeQueryInterruptTime()<deadline);
    for(ULONG i=0;i<RTL_NUMBER_OF(CamCdmTraceRegs);i++)c->Trace.Cdm[i]=CamRead(b,CamCdmTraceRegs[i]);
    c->Diag.Offset=0x80;c->Diag.Mask=MAXULONG;c->Diag.Expected=marker;c->Diag.Observed=CamRead(b,0x80);
    if(NT_SUCCESS(status)) {
        c->CdmPending=FALSE;
        CamWrite(b,0x34,MAXULONG);CamWrite(b,0x38,1);CamWrite(b,0x30,irqMask);
        c->Trace.Csid[20]=CamRead(c->Base[CamCsid],0xe10);
    } else c->Faulted=TRUE;
    return status;
}

NTSTATUS CamCdmAbort(CAM *c) {
    if(!c->CdmPending)return STATUS_SUCCESS;
    NTSTATUS status=CamCdmAbortCore(c->Base[CamCdm]);
    c->Trace.CommandAbortStatus=status;
    if(NT_SUCCESS(status)){c->CdmPending=FALSE;c->Faulted=FALSE;}
    return status;
}
