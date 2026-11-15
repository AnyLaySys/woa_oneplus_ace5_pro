#include <stdio.h>
#include <stdlib.h>
#include "../hardware.c"

static ULONG memory[GPU_WINDOW/4],reads,writes;
static ULONG highReads;
static int rollover;
static PUCHAR base=(PUCHAR)memory;
static int valid(PULONG address) {
    uintptr_t offset=(uintptr_t)address-(uintptr_t)base;
    return offset<GPU_WINDOW && (offset<0xA0000 || offset==0xA0048 || offset==0xA0000 ||
        (offset>=0xC0000 && offset<0xC2000));
}
ULONG TestRead(PULONG address) {
    if(!valid(address)){fprintf(stderr,"Forbidden MMIO read: %zx\n",(uintptr_t)address-(uintptr_t)base);exit(1);}
    reads++;
    if(rollover && address==memory+REG_A8XX_CP_ALWAYS_ON_COUNTER+1)return highReads++?2:1;
    return *address;
}
VOID TestWrite(PULONG address,ULONG value) {
    if(!valid(address)){fprintf(stderr,"Forbidden MMIO write\n");exit(1);}
    writes++;*address=value;
}
#define REQUIRE(x) do { if(!(x)){fprintf(stderr,"Failed line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void) {
    GPU g;GpuInitialize(&g,NULL,NULL);g.Registers=base;g.CbBase=0x20000;
    for(ULONG i=0;i<32;i++)REQUIRE(!GpuInterrupt(&g,i));
    REQUIRE(reads==0 && writes==0);
    g.Powered=TRUE;g.Stage=7;
    for(ULONG i=6;i<1024;i++)REQUIRE(!GpuInterrupt(&g,i));
    REQUIRE(!GpuInterrupt(&g,UINT32_MAX));REQUIRE(reads==0);
    REQUIRE(!GpuInterrupt(&g,4));REQUIRE(!GpuInterrupt(&g,5));REQUIRE(reads==0);
    g.Vm[0].Tables.Mdl=&g;g.Vm[1].Tables.Mdl=&g;
    for(ULONG i=0;i<29;i++)REQUIRE(!GpuInterrupt(&g,i));
    REQUIRE(reads==6);
    memory[0xC1058/4]=0x400;ULONG oldWrites=writes;REQUIRE(!GpuInterrupt(&g,5));
    REQUIRE(writes==oldWrites && !g.Fault && memory[0xC1058/4]==0x400);
    memory[(0xC0058)/4]=2;REQUIRE(GpuInterrupt(&g,4));
    REQUIRE(g.Fault==(ULONG)STATUS_DEVICE_HARDWARE_ERROR && g.Snapshot[16]==4);
    memory[0xC0058/4]=0;memory[0xC1058/4]=0x402;REQUIRE(GpuInterrupt(&g,5));
    REQUIRE(g.Snapshot[16]==5);
    REQUIRE(g.Event.Value==0);GpuDpc(&g);REQUIRE(g.Event.Value==1 && g.Snapshot[1]==1);
    g.Powered=FALSE;reads=0;for(ULONG i=0;i<29;i++)REQUIRE(!GpuInterrupt(&g,i));REQUIRE(reads==0);
    ULONGLONG counter=0;
    REQUIRE(GpuTimestamp(&g,&counter)==STATUS_DEVICE_NOT_READY && reads==0);
    g.Powered=TRUE;g.Online=TRUE;
    memory[REG_A8XX_CP_ALWAYS_ON_COUNTER]=0x89abcdef;memory[REG_A8XX_CP_ALWAYS_ON_COUNTER+1]=1;
    REQUIRE(GpuTimestamp(&g,&counter)==STATUS_SUCCESS && counter==0x189abcdefULL);
    rollover=1;REQUIRE(GpuTimestamp(&g,&counter)==STATUS_SUCCESS && counter==0x289abcdefULL && highReads==4);
    puts("{\"isr_bounds\":\"PASS\",\"inactive_bank_access\":\"PASS\",\"unconfigured_sources\":\"PASS\",\"dpc_notification\":\"PASS\"}");return 0;
}
