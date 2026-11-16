#include "gpu.h"
#include "regs.h"
#include "perf-table.h"

typedef struct { ULONG Status,Address,Type,Size,MessageSize,Dropped,RxWatermark,TxWatermark,RxRequest,TxRequest,Read,Write; } HFI_QUEUE;
typedef struct { ULONG Version,Size,Offset,Stride,Count,Active; HFI_QUEUE Queue[3]; } HFI_TABLE;
NTSTATUS GpuFirmware(PCWSTR name,PUCHAR *data,PULONG size) {
    UNICODE_STRING path;RtlInitUnicodeString(&path,name);
    OBJECT_ATTRIBUTES attr;InitializeObjectAttributes(&attr,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);
    IO_STATUS_BLOCK io;HANDLE file;
    NTSTATUS status=ZwCreateFile(&file,FILE_GENERIC_READ,&attr,&io,NULL,0,FILE_SHARE_READ,FILE_OPEN,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,NULL,0);
    if(!NT_SUCCESS(status))return status;
    FILE_STANDARD_INFORMATION info;
    status=ZwQueryInformationFile(file,&io,&info,sizeof(info),FileStandardInformation);
    if(NT_SUCCESS(status) && (info.EndOfFile.QuadPart<=0 || info.EndOfFile.QuadPart>0x40000))status=STATUS_INVALID_IMAGE_FORMAT;
    if(NT_SUCCESS(status)) {
        *size=(ULONG)info.EndOfFile.QuadPart;*data=ExAllocatePool2(POOL_FLAG_NON_PAGED,*size,GPU_TAG);
        if(!*data)status=STATUS_INSUFFICIENT_RESOURCES;
        else {
            status=ZwReadFile(file,NULL,NULL,NULL,&io,*data,*size,NULL,NULL);
            if(NT_SUCCESS(status) && io.Information!=*size)status=STATUS_END_OF_FILE;
            if(!NT_SUCCESS(status)){ExFreePoolWithTag(*data,GPU_TAG);*data=NULL;}
        }
    }
    ZwClose(file);return status;
}
static NTSTATUS Allocate(GPU *g,GPU_BO *bo,ULONG vm,ULONG size,ULONGLONG address) {
    NTSTATUS status=GpuAllocate(bo,size);
    return NT_SUCCESS(status)?GpuMap(g,vm,bo,address):status;
}
static NTSTATUS Load(GPU *g) {
    PUCHAR data=NULL;ULONG size=0;
    NTSTATUS status=GpuFirmware(L"\\SystemRoot\\System32\\drivers\\qcom-gpu\\gen80000_gmu.bin",&data,&size);
    if(!NT_SUCCESS(status))return status;
    for(ULONG at=0;at<size;) {
        if(size-at<16){status=STATUS_INVALID_IMAGE_FORMAT;break;}
        ULONG *block=(PULONG)(data+at),address=block[0],bytes=block[1];
        at+=16;
        if(bytes>size-at || (bytes|address)&3){status=STATUS_INVALID_IMAGE_FORMAT;break;}
        if(bytes) {
            if(address<0x4000 && bytes<=0x4000-address) {
                for(ULONG i=0;i<bytes/4;i++)Gw(g,REG_A6XX_GMU_CM3_ITCM_START+address/4+i,((PULONG)(data+at))[i]);
            } else if(address>=0x10004000 && address<0x10008000 && bytes<=0x10008000-address) {
                for(ULONG i=0;i<bytes/4;i++)Gw(g,REG_A6XX_GMU_CM3_DTCM_START+(address-0x10004000)/4+i,((PULONG)(data+at))[i]);
            } else if(address>=0x4000 && address<0x1000000 && bytes<=0x1000000-address)
                RtlCopyMemory(g->Cache.Cpu+address-0x4000,data+at,bytes);
            else {status=STATUS_INVALID_IMAGE_FORMAT;break;}
        }
        at+=bytes;
    }
    ExFreePoolWithTag(data,GPU_TAG);KeMemoryBarrier();return status;
}
static VOID Queues(GPU *g) {
    HFI_TABLE *t=(PVOID)g->Hfi.Cpu;
    t->Size=sizeof(*t);t->Offset=6;t->Stride=12;t->Count=t->Active=3;
    for(ULONG i=0;i<3;i++) {
        HFI_QUEUE *q=t->Queue+i;
        q->Status=1;q->Address=(ULONG)g->Hfi.Address+(i+1)*4096;q->Type=0xA00|i;q->Size=1024;
        q->RxWatermark=q->TxWatermark=q->RxRequest=1;
    }
}
static NTSTATUS Send(GPU *g,ULONG id,PULONG data,ULONG words) {

    if(!words || words>255)return STATUS_INVALID_PARAMETER;
    volatile HFI_TABLE *t=(PVOID)g->Hfi.Cpu;volatile HFI_QUEUE *tx=t->Queue,*rx=t->Queue+1;
    volatile ULONG *out=(PULONG)(g->Hfi.Cpu+4096),*in=(PULONG)(g->Hfi.Cpu+8192);
    ULONG at=tx->Write,space=(tx->Read-at-1)&1023,padded=(words+3)&~3u;
    if(at>=1024 || tx->Read>=1024 || space<padded)return STATUS_DEVICE_BUSY;
    g->Sequence=(g->Sequence+1)%4095;data[0]=(g->Sequence<<20)|(words<<8)|id;
    for(ULONG i=0;i<padded;i++)out[(at+i)&1023]=i<words?data[i]:0xFAFAFAFA;
    KeMemoryBarrier();tx->Write=(at+padded)&1023;Gw(g,REG_A6XX_GMU_HOST2GMU_INTR_SET,1);
    ULONGLONG end=KeQueryInterruptTime()+10000000;
    do {
        ULONG r=rx->Read,w=rx->Write;
        if(r>=1024 || w>=1024)return STATUS_DEVICE_PROTOCOL_ERROR;
        if(r!=w) {
            KeMemoryBarrier();
            ULONG header=in[r],n=(header>>8)&255,available=(w-r)&1023;
            if(n<3 || n>20 || ((n+3)&~3u)>available)return STATUS_DEVICE_PROTOCOL_ERROR;
            ULONG original=in[(r+1)&1023],error=in[(r+2)&1023];

            KeMemoryBarrier();rx->Read=(r+n+3)&~3u;rx->Read&=1023;
            Gw(g,REG_A6XX_GMU_GMU2HOST_INTR_CLR,1);
            if((header&255)==100)return STATUS_DEVICE_HARDWARE_ERROR;
            if(((header>>16)&15)!=1 || original!=data[0])continue;
            return error?STATUS_DEVICE_PROTOCOL_ERROR:STATUS_SUCCESS;
        }
        KeStallExecutionProcessor(10);
    } while(KeQueryInterruptTime()<end && (!g->Fault || id==33));
    return g->Fault?(NTSTATUS)g->Fault:STATUS_IO_TIMEOUT;
}
static NTSTATUS Bandwidth(GPU *g,PULONG bw) {
    ULONG addr=0,aux=0,n=0;NTSTATUS status;
    RtlZeroMemory(bw,160*sizeof(ULONG));bw[1]=g->PerfCount?g->PerfCount:2;bw[2]=1;bw[3]=3;bw[4]=1;
    status=GpuDb(g,"CN0",&addr,&aux,&n);
    if(!NT_SUCCESS(status))return status;
    bw[6]=addr;bw[12]=0x40000000;bw[18]=0x60000001;
    static const PCSTR names[]={"SH0","MC0","ACV"};
    ULONG units[3],widths[3],vcd[3];
    for(ULONG i=0;i<3;i++) {
        status=GpuDb(g,names[i],&addr,&aux,&n);if(!NT_SUCCESS(status) || n<8)return STATUS_DEVICE_CONFIGURATION_ERROR;
        bw[24+i]=addr;units[i]=GpuRead(g->Db,aux);
        ULONG tail=GpuRead(g->Db,aux+4);widths[i]=tail&65535;vcd[i]=(tail>>16)&255;
        if(!units[i] && i<2)return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for(ULONG i=0;i<3;i++) {
        ULONG commit=i==2 || vcd[i]!=vcd[i+1]?0x40000000:0;
        bw[32+i]=commit;if(commit)bw[5]|=1u<<i;
        for(ULONG j=1;j<bw[1];j++) {
            ULONG bandwidth=g->PerfCount?g->PerfBw[j]:2136718;
            ULONGLONG peak=i==2?0:((ULONGLONG)bandwidth*widths[i]*1000/((i?4:16)*units[i]));
            ULONG vote=i==2?0:(ULONG)min(max(peak,1),0x3FFF);
            bw[32+j*8+i]=commit|0x20000000|(vote<<14)|vote;
            if(i==2 && bandwidth>=10687500)bw[32+j*8+i]|=4;
        }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Configure(GPU *g) {
    PHYSICAL_ADDRESS fuseAddress;fuseAddress.QuadPart=0x221c8000;
    PUCHAR fuse=MmMapIoSpaceEx(fuseAddress,4096,PAGE_READONLY|PAGE_NOCACHE);
    if(!fuse)return STATUS_INSUFFICIENT_RESOURCES;
    g->SpeedBin=GpuRead(fuse,0x138)&511;MmUnmapIoSpace(fuse,4096);
    ULONG mask=GpuSpeedMask(g->SpeedBin);if(!mask)return STATUS_NOT_SUPPORTED;
    ULONG cx=0,addr=0,aux=0,n=0;
    NTSTATUS status=GpuArc(g,"cx.lvl","mx.lvl",64,&cx);
    if(NT_SUCCESS(status))status=GpuDb(g,"mx.lvl",&addr,&aux,&n);
    if(!NT_SUCCESS(status))return status;
    ULONG perf[64]={0,0,1,0,3,0,0xFFFFFFFF,0};g->PerfCount=1;
    for(ULONG i=0;i<RTL_NUMBER_OF(GpuOpps);i++)if(GpuOpps[i].Mask&mask) {
        const GPU_OPP *opp=GpuOpps+i;ULONG gx=0,mxindex=MAXULONG;
        status=GpuArc(g,"gfx.lvl","gmxc.lvl",opp->Level,&gx);if(!NT_SUCCESS(status))return status;
        for(ULONG j=0;j<n/2;j++) {
            USHORT v=(USHORT)(GpuRead(g->Db,(aux+j*2)&~3u)>>(((aux+j*2)&3)*8));
            if(v>=opp->Level){mxindex=j;break;}
        }
        if(mxindex==MAXULONG || g->PerfCount>=GPU_PERF_LEVELS)return STATUS_NOT_SUPPORTED;
        ULONG index=g->PerfCount++,at=5+index*3;
        perf[at]=gx;perf[at+1]=0xFFFC000|(mxindex<<8)|255;perf[at+2]=opp->Khz;
        g->PerfKhz[index]=opp->Khz;g->PerfBw[index]=opp->Bandwidth;
    }
    g->MaxKhz=0;g->FixedKhz=0;
    perf[3]=g->PerfCount;ULONG at=5+g->PerfCount*3;
    perf[at++]=2;perf[at++]=2;perf[at++]=0;perf[at++]=0;perf[at++]=cx;perf[at++]=500000;
    status=Send(g,15,perf,at);if(!NT_SUCCESS(status))return status;
    ULONG bw[160];status=Bandwidth(g,bw);if(!NT_SUCCESS(status))return status;
    status=Send(g,3,bw,RTL_NUMBER_OF(bw));if(!NT_SUCCESS(status))return status;
    ULONG start[]={0,0};status=Send(g,14,start,2);
    if(NT_SUCCESS(status))status=Send(g,10,start,1);
    if(NT_SUCCESS(status))status=GpuSetFrequency(g,1);
    if(NT_SUCCESS(status)) {
        Gw(g,0x1fc40,(Gr(g,0x1fc40)&~255u)|32);Gw(g,0x1fc10,1);
        g->PerfLast=KeQueryInterruptTime();g->BusyLast=((ULONGLONG)Gr(g,0x1fca1)<<32)|Gr(g,0x1fca0);
    }
    return status;
}
NTSTATUS GpuSetFrequency(GPU *g,ULONG index) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL || !index || index>=g->PerfCount)return STATUS_INVALID_PARAMETER;
    ULONG message[]={0,1,index,index};NTSTATUS status=Send(g,30,message,4);

    if(NT_SUCCESS(status)){g->PerfIndex=index;}
    return status;
}
VOID GpuGovern(GPU *g) {
    if(!g->Online || !g->PerfCount || g->Fault)return;
    ULONGLONG now=KeQueryInterruptTime(),elapsed=now-g->PerfLast;
    if(elapsed<500000)return;
    ULONG high,low;do{high=Gr(g,0x1fca1);low=Gr(g,0x1fca0);}while(high!=Gr(g,0x1fca1));
    ULONGLONG busy=((ULONGLONG)high<<32)|low,delta=busy-g->BusyLast;
    g->PerfLast=now;g->BusyLast=busy;
    g->BusyPercent=(ULONG)min(delta*100/(elapsed*192/100),100);
    ULONG maximum=g->PerfCount-1;
    if(g->MaxKhz)while(maximum>1 && g->PerfKhz[maximum]>g->MaxKhz)maximum--;
    ULONG target=min(g->PerfIndex,maximum);
    if(g->FixedKhz){target=1;while(target<maximum && g->PerfKhz[target]<g->FixedKhz)target++;}
    else if(g->BusyPercent>=40)target=maximum;
    else if(g->BusyPercent<15 && target>1)target--;
    if(target!=g->PerfIndex)GpuSetFrequency(g,target);
}
NTSTATUS GpuGmuStart(GPU *g) {

    if(GpuRead(g->Registers,0x50010)&(1u<<16)) {
        Gw(g,REG_A6XX_GMU_RSCC_CONTROL_REQ,2);
        NTSTATUS status=GpuPoll(g->Registers,REG_A6XX_GMU_RSCC_CONTROL_ACK*4,2,2,10000);
        if(NT_SUCCESS(status))status=GpuPoll(g->Registers,0x50404,MAXULONG,0,10000);
        Gw(g,REG_A6XX_GMU_RSCC_CONTROL_REQ,0);
        if(!NT_SUCCESS(status))return status;
    }
    Gw(g,REG_A6XX_GMU_AHB_FENCE_STATUS_CLR,7);Gw(g,REG_A6XX_GMU_AO_AHB_FENCE_CTRL,0);
    NTSTATUS status=Allocate(g,&g->Cache,1,0xFFC000,0x4000);

    if(NT_SUCCESS(status))status=Allocate(g,&g->Dummy,1,0x4000,0x60000000);
    if(NT_SUCCESS(status))status=Allocate(g,&g->Debug,1,0x7000,0x60400000);
    if(NT_SUCCESS(status))status=Allocate(g,&g->Log,1,0x4000,0x60004000);
    if(NT_SUCCESS(status))status=Allocate(g,&g->Hfi,1,0x4000,0x60008000);
    if(!NT_SUCCESS(status))return status;
    Queues(g);status=Load(g);if(!NT_SUCCESS(status))return status;
    ULONG bw[160];status=Bandwidth(g,bw);if(!NT_SUCCESS(status))return status;
    status=GpuRscWrite(g,bw[6],bw[18]);
    for(ULONG i=0;i<3 && NT_SUCCESS(status);i++)status=GpuRscWrite(g,bw[24+i],bw[40+i]);
    if(!NT_SUCCESS(status))return status;
    Gw(g,REG_A8XX_GPU_GMU_CX_GMU_CX_FALNEXT_INTF,1);
    Gw(g,REG_A8XX_GPU_GMU_CX_GMU_CX_FAL_INTF,1);
    Gw(g,REG_A6XX_GMU_CM3_SYSRESET,1);Gw(g,REG_A6XX_GMU_CM3_FW_INIT_RESULT,0);
    Gw(g,REG_A6XX_GMU_CM3_BOOT_CONFIG,2);
    Gw(g,REG_A6XX_GMU_HFI_QTBL_ADDR,(ULONG)g->Hfi.Address);Gw(g,REG_A6XX_GMU_HFI_QTBL_INFO,1);
    Gw(g,REG_A6XX_GMU_AHB_FENCE_RANGE_0,0x80C808C0);Gw(g,REG_A6XX_GMU_CM3_CFG,0x4052);
    Gw(g,REG_A8XX_GMU_GENERAL_10,0x8000100);
    Gw(g,REG_A8XX_GMU_GENERAL_8,(ULONG)g->Log.Address|3);Gw(g,REG_A8XX_GMU_GENERAL_9,0);
    for(ULONG i=0;i<4;i++)Gw(g,REG_A6XX_GBIF_QSB_SIDE0+i,0x71E20);
    Gw(g,REG_A8XX_GBIF_CX_CONFIG,0x20023000);Gw(g,REG_A6XX_GMU_MRC_GBIF_QOS_CTRL,0x33);
    Gw(g,REG_A6XX_GMU_SYS_BUS_CONFIG,1);Gw(g,REG_A6XX_GMU_ICACHE_CONFIG,1);Gw(g,REG_A6XX_GMU_DCACHE_CONFIG,1);
    Gw(g,REG_A6XX_GMU_CM3_SYSRESET,0);
    g->GmuResetReleased=TRUE;
    ULONGLONG end=KeQueryInterruptTime()+1000000;
    do {
        if(g->Fault)return (NTSTATUS)g->Fault;
        if((Gr(g,REG_A6XX_GMU_CM3_FW_INIT_RESULT)&0x1FF)==0x100)break;
        LARGE_INTEGER delay;delay.QuadPart=-1000;KeDelayExecutionThread(KernelMode,FALSE,&delay);
    } while(KeQueryInterruptTime()<end);
    status=(Gr(g,REG_A6XX_GMU_CM3_FW_INIT_RESULT)&0x1FF)==0x100?STATUS_SUCCESS:STATUS_IO_TIMEOUT;
    if(!NT_SUCCESS(status))return status;
    g->Stage=4;
    Gw(g,REG_A6XX_GMU_GMU2HOST_INTR_CLR,0xFFFFFFFF);Gw(g,REG_A6XX_GMU_GMU2HOST_INTR_MASK,0xFFFFFFFC);
    Gw(g,REG_A6XX_GMU_AO_HOST_INTERRUPT_CLR,0xFFFFFFFF);Gw(g,REG_A6XX_GMU_AO_HOST_INTERRUPT_MASK,0xFFFFFFFC);
    Gw(g,REG_A6XX_GMU_HFI_CTRL_INIT,1);
    status=GpuPoll(g->Registers,REG_A6XX_GMU_HFI_CTRL_STATUS*4,1,1,100000);
    if(NT_SUCCESS(status)){status=Configure(g);}
    if(NT_SUCCESS(status))g->Stage=5;
    return status;
}
VOID GpuGmuStop(GPU *g) {
    if(g->Stage>=4) {
        ULONG slumber[]={0,0,0};Send(g,33,slumber,3);
        Gw(g,REG_A6XX_GMU_AHB_FENCE_STATUS_CLR,7);Gw(g,REG_A6XX_GMU_AO_AHB_FENCE_CTRL,0);
    }
    if(g->GmuResetReleased)Gw(g,REG_A6XX_GMU_CM3_SYSRESET,1);
    if(g->Stage>=4) {
        Gw(g,REG_A6XX_GMU_GMU2HOST_INTR_MASK,0xFFFFFFFF);
        Gw(g,REG_A6XX_GMU_AO_HOST_INTERRUPT_MASK,0xFFFFFFFF);
        Gw(g,REG_A6XX_GMU_CM3_SYSRESET,1);
        Gw(g,REG_A6XX_GMU_RSCC_CONTROL_REQ,1);
        GpuPoll(g->Registers,0x50010,1u<<16,1u<<16,10000);
        Gw(g,REG_A6XX_GMU_RSCC_CONTROL_REQ,0);
    }
    g->GmuResetReleased=FALSE;
}
