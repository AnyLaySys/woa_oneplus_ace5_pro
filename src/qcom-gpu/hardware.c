#include "gpu.h"
#include "regs.h"

VOID GpuInitialize(GPU *g,GPU_SYNCHRONIZE synchronize,PVOID host) {
    RtlZeroMemory(g,sizeof(*g));g->Synchronize=synchronize;g->Host=host;
    KeInitializeEvent(&g->Event,SynchronizationEvent,FALSE);KeInitializeSpinLock(&g->RingLock);
    KeInitializeMutex(&g->MappingLock,0);
    KeInitializeMutex(&g->PowerLock,0);
}
ULONG GpuRead(PUCHAR base,ULONG offset) {
    return READ_REGISTER_ULONG((PULONG)(base+offset));
}
VOID GpuWrite(PUCHAR base,ULONG offset,ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(base+offset),value);
}
NTSTATUS GpuPoll(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG timeout) {
    ULONGLONG end=KeQueryInterruptTime()+(ULONGLONG)timeout*10;
    do {
        if((GpuRead(base,offset)&mask)==value)return STATUS_SUCCESS;
        KeStallExecutionProcessor(10);
    } while(KeQueryInterruptTime()<end);
    return STATUS_IO_TIMEOUT;
}
VOID GpuUnbind(GPU *g) {
    if(g->Registers)MmUnmapIoSpace(g->Registers,GPU_WINDOW);
    if(g->Gcc)MmUnmapIoSpace(g->Gcc,0x1F5000);
    if(g->Rsc)MmUnmapIoSpace(g->Rsc,0x10000);
    if(g->Db)MmUnmapIoSpace(g->Db,0x20000);
    if(g->Thermal)MmUnmapIoSpace(g->Thermal,4096);
    g->Registers=g->Gcc=g->Rsc=g->Db=g->Thermal=NULL;
}
NTSTATUS GpuBind(GPU *g,PHYSICAL_ADDRESS registers,ULONG length) {
    if(!g->Synchronize || g->Registers || g->Gcc || g->Rsc || g->Db || g->Thermal)return STATUS_INVALID_DEVICE_STATE;
    if(registers.QuadPart!=0x03D00000 || length!=GPU_WINDOW)return STATUS_DEVICE_CONFIGURATION_ERROR;
    PHYSICAL_ADDRESS p;
    g->Registers=MmMapIoSpaceEx(registers,length,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x100000;g->Gcc=MmMapIoSpaceEx(p,0x1F5000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x16520000;g->Rsc=MmMapIoSpaceEx(p,0x10000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x81C60000;g->Db=MmMapIoSpaceEx(p,0x20000,PAGE_READWRITE|PAGE_NOCACHE);
    p.QuadPart=0x0C22A000;g->Thermal=MmMapIoSpaceEx(p,4096,PAGE_READONLY|PAGE_NOCACHE);
    if(!g->Registers || !g->Gcc || !g->Rsc || !g->Db || !g->Thermal){GpuUnbind(g);return STATUS_INSUFFICIENT_RESOURCES;}
    g->Stage=1;return STATUS_SUCCESS;
}
BOOLEAN GpuInterrupt(GPU *g,ULONG index) {
    if(!g->Powered)return FALSE;
    ULONG value=0;
    if(index>=4+RTL_NUMBER_OF(g->Vm))return FALSE;
    if(index==0) {
        if(g->Stage<6)return FALSE;
        value=Gr(g,REG_A8XX_RBBM_INT_0_STATUS);
        if(value){Gw(g,REG_A8XX_RBBM_INT_CLEAR_CMD,value);}
    } else if(index==1) {
        if(g->Stage<4)return FALSE;
        value=Gr(g,REG_A6XX_GMU_GMU2HOST_INTR_INFO)&3;
        if(value)Gw(g,REG_A6XX_GMU_GMU2HOST_INTR_CLR,value);
    } else if(index==2) {
        if(g->Stage<4)return FALSE;
        value=Gr(g,REG_A6XX_GMU_AO_HOST_INTERRUPT_STATUS)&GMU_AO_INTERRUPT_STATUS_BITS;
        if(value) {
            Gw(g,REG_A6XX_GMU_AO_HOST_INTERRUPT_CLR,value);
            if(value&GMU_AO_FAULT_BITS)g->Fault=(ULONG)STATUS_DEVICE_HARDWARE_ERROR;
        }
    } else {
        if(g->Stage<3)return FALSE;
        if(index>3 && !g->Vm[index-4].Tables.Mdl)return FALSE;
        PUCHAR m=g->Registers+0xA0000;
        ULONG offset=index==3?0x48:g->CbBase+0x58+(index-4)*4096;
        value=GpuRead(m,offset);
        if(index>3 && !(value&0xC00001FEu))return FALSE;
        if(value){
            if(index>3) {
                GpuWrite(m,offset-0x58,GpuRead(m,offset-0x58)&~0x40u);

            } else GpuWrite(m,0,GpuRead(m,0)&~0x24u);
            GpuWrite(m,offset,value);g->Fault=(ULONG)STATUS_DEVICE_HARDWARE_ERROR;
            if(index>3 && (value&0x40000000))GpuWrite(m,offset-0x50,1);
        }
    }
    if(!value)return FALSE;
    return TRUE;
}
VOID GpuDpc(GPU *g) {

    KeSetEvent(&g->Event,IO_NO_INCREMENT,FALSE);
}
NTSTATUS GpuTimestamp(GPU *g,PULONGLONG counter) {
    if(!g->Online || !g->Powered)return STATUS_DEVICE_NOT_READY;
    for(ULONG i=0;i<8;i++) {
        ULONG high=Gr(g,REG_A8XX_CP_ALWAYS_ON_COUNTER+1);
        ULONG low=Gr(g,REG_A8XX_CP_ALWAYS_ON_COUNTER);
        if(high==Gr(g,REG_A8XX_CP_ALWAYS_ON_COUNTER+1)) {
            *counter=((ULONGLONG)high<<32)|low;return STATUS_SUCCESS;
        }
    }
    return STATUS_RETRY;
}
