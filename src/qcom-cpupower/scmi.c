#include "cpupower.h"

static ULONG Read(ULONG offset) {
    return READ_REGISTER_ULONG((PULONG)(Sm.Mailbox+offset));
}
static VOID Write(ULONG offset,ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(Sm.Mailbox+offset),value);
}

/* Only the worker and initialization path enter this passive-level transport.
 * The mailbox is shared with firmware: never overwrite an occupied channel,
 * including after a timeout, and validate every response's token and length.
 */
NTSTATUS SmScmiRequest(ULONG command,ULONG domain,ULONG index,SM_REPLY *reply) {
    ULONG bytes,header,status,i;
    LARGE_INTEGER begin,frequency,now,delay;
    NTSTATUS result=STATUS_SUCCESS;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || domain>=2) return STATUS_INVALID_PARAMETER;
    switch(command) {
    case 0:case 1:bytes=0;break;
    case 3:bytes=4;break;
    case 4:bytes=8;if(index>=SM_LEVEL_COUNT)return STATUS_INVALID_PARAMETER;break;
    case 7:bytes=8;
        if(index>=SM_LEVEL_COUNT)
            return STATUS_ACCESS_DENIED;
        break;
    default:return STATUS_ACCESS_DENIED;
    }
    RtlZeroMemory(reply,sizeof(*reply));
    /* Initialization precedes the worker; all runtime requests use that one
       worker, so no second host-side transport lock is needed. */
    status=Read(4);
    if(!(status&1) || (status&2)){result=STATUS_DEVICE_BUSY;goto done;}
    header=command | (0x13u<<10) | ((++Sm.Token&1023u)<<18);
    Write(4,0);Write(16,0);Write(20,4+bytes);Write(24,header);
    if(bytes>=4)Write(28,domain);
    if(bytes==8)Write(32,index);
    KeMemoryBarrier();WRITE_REGISTER_ULONG((PULONG)(Sm.Doorbell+0x104),header);
    begin=KeQueryPerformanceCounter(&frequency);
    delay.QuadPart=-1000;
    for(;;) {
        status=Read(4);if(status&3)break;
        now=KeQueryPerformanceCounter(NULL);
        if(now.QuadPart-begin.QuadPart>frequency.QuadPart/10){result=STATUS_IO_TIMEOUT;goto done;}
        KeDelayExecutionThread(KernelMode,FALSE,&delay);
    }
    KeMemoryBarrier();bytes=Read(20);
    if((status&2) || Read(24)!=header || bytes<8 || bytes>488 || (bytes&3)) {
        result=STATUS_DEVICE_PROTOCOL_ERROR;goto done;
    }
    reply->Count=(bytes-8)/4;
    for(i=0;i<reply->Count;i++)reply->Words[i]=Read(32+i*4);
    if((LONG)Read(28)!=0)result=STATUS_UNSUCCESSFUL;
done:
    return result;
}

NTSTATUS SmScmiSet(ULONG domain,ULONG index) {
    SM_REPLY reply;NTSTATUS status;
    if(domain>=2)return STATUS_INVALID_PARAMETER;
    status=SmScmiRequest(7,domain,index,&reply);
    InterlockedExchange(&Sm.Applied[domain],NT_SUCCESS(status)?(LONG)index:-1);
    return status;
}

NTSTATUS SmScmiInitialize(VOID) {
    PHYSICAL_ADDRESS address;SM_REPLY reply;ULONG domain,index;NTSTATUS status;
    address.QuadPart=0x17b4e000;
    Sm.Mailbox=MmMapIoSpaceEx(address,4096,PAGE_READWRITE|PAGE_NOCACHE);
    address.QuadPart=0x17830000;
    Sm.Doorbell=MmMapIoSpaceEx(address,4096,PAGE_READWRITE|PAGE_NOCACHE);
    if(!Sm.Mailbox || !Sm.Doorbell)return STATUS_INSUFFICIENT_RESOURCES;
    status=SmScmiRequest(0,0,0,&reply);
    if(!NT_SUCCESS(status) || !reply.Count || reply.Words[0]!=0x40000)return STATUS_NOT_SUPPORTED;
    status=SmScmiRequest(1,0,0,&reply);
    if(!NT_SUCCESS(status) || !reply.Count || (reply.Words[0]&65535)!=2)return STATUS_NOT_SUPPORTED;
    for(domain=0;domain<2;domain++) {
        status=SmScmiRequest(3,domain,0,&reply);
        if(!NT_SUCCESS(status) || reply.Count<4 || (reply.Words[0]&0x42000000)!=0x42000000)
            return STATUS_NOT_SUPPORTED;
        Sm.NominalKhz[domain]=reply.Words[2];
        if(Sm.NominalKhz[domain]<SmFrequencyKhz[domain][0] ||
           Sm.NominalKhz[domain]>SmFrequencyKhz[domain][15])return STATUS_NOT_SUPPORTED;
        for(index=0;index<SM_LEVEL_COUNT;index++) {
            status=SmScmiRequest(4,domain,index,&reply);
            if(!NT_SUCCESS(status) || reply.Count<6 || ! (reply.Words[0]&0xfff) ||
               reply.Words[4]!=SmFrequencyKhz[domain][index] || reply.Words[5]!=index)
                return STATUS_NOT_SUPPORTED;
        }
        Sm.Ceiling[domain]=SM_LEVEL_COUNT-1;Sm.Applied[domain]=-1;
        status=SmScmiSet(domain,0);if(!NT_SUCCESS(status))return status;
    }
    ULONG bytes=0;
    status=ZwQuerySystemInformation(8,Sm.Previous,sizeof(Sm.Previous),&bytes);
    if(!NT_SUCCESS(status))return status;
    if(bytes!=sizeof(Sm.Previous))return STATUS_INFO_LENGTH_MISMATCH;
    Sm.LastSample=KeQueryInterruptTime();
    return STATUS_SUCCESS;
}
VOID SmScmiClose(VOID) {
    if(Sm.Mailbox)MmUnmapIoSpace(Sm.Mailbox,4096);
    if(Sm.Doorbell)MmUnmapIoSpace(Sm.Doorbell,4096);
    Sm.Mailbox=Sm.Doorbell=NULL;
}
