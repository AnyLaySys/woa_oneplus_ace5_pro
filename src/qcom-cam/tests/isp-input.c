#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef unsigned char *PUCHAR;
typedef int BOOLEAN;
typedef int32_t NTSTATUS;
typedef void VOID;
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_REVISION_MISMATCH -2
#define STATUS_DEVICE_BUSY -3
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_IO_TIMEOUT -5
#define QCAM_ABI 1
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef struct { ULONG Size,Abi,Width,Height,Status,Before[25],Configured[25],Restored[25]; } QCAM_ISP_INPUT_STATE;

static ULONG csid[0x2000/4],tfe[0x10000/4],writes,inject,commits,commit_mask;
static ULONGLONG ticks;
static ULONG CamRead(PUCHAR base,ULONG offset) { return ((ULONG *)base)[offset/4]; }
static ULONGLONG CamRead64(PUCHAR base,ULONG offset) { return CamRead(base,offset)|((ULONGLONG)CamRead(base,offset+4)<<32); }
static void CamWrite(PUCHAR base,ULONG offset,ULONG value) {
    assert(base==(PUCHAR)csid);
    assert(!(offset>=0x600&&offset<=0x1600&&offset%0x200==4));
    if(offset==0x600)assert(!(value&0x80000000u));
    writes++;
    if(offset==0x18||offset==0x1c){assert(value==1);commit_mask|=offset==0x18?1:2;return;}
    if(offset==0x20){assert(value==1&&commit_mask==3);commits++;commit_mask=0;return;}
    ((ULONG *)base)[offset/4]=value;
    if(inject&&offset==0x1810){((ULONG *)base)[offset/4]^=1;inject=0;}
}
static ULONGLONG KeQueryInterruptTime(void) { return ticks++; }
static void CamDelay(ULONG us) { ticks+=us*10; }
#include "../isp-input-check.h"

int main(void) {
    QCAM_ISP_INPUT_STATE state;
    CAM_ISP_RAW10_INPUT plan;
    csid[0]=0x30000006;tfe[0]=0x10000000;
    for(ULONG i=0;i<28;i++)tfe[(0xd00+i*0x100)/4]=0x50;
    assert(CamIspRaw10Input(&plan,0,0,0,0,4096,3072));
    for(ULONG i=1;i<23;i++)csid[plan.Registers[i].Offset/4]=i+0x1230;
    ULONG before[sizeof(csid)/sizeof(*csid)];memcpy(before,csid,sizeof(csid));
    assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_SUCCESS);
    assert(writes==56&&commits==2&&!memcmp(before,csid,sizeof(csid)));
    assert(state.Configured[24]==0x2b2260);
    assert(state.Configured[0]==0&&state.Configured[22]==0);
    assert(!memcmp(state.Before,state.Restored,sizeof(state.Before)));
    writes=0;inject=1;
    assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,3264,2448,&state)==STATUS_IO_DEVICE_ERROR);
    assert(!memcmp(before,csid,sizeof(csid))&&writes==56&&commits==4);
    writes=0;
    assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4095,3072,&state)==STATUS_INVALID_PARAMETER&&!writes);
    csid[0x3f0/4]=1;
    assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
    csid[0x3f0/4]=0;
    tfe[0]=0;
    assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_REVISION_MISMATCH&&!writes);
    tfe[0]=0x10000000;
    for(ULONG i=0;i<28;i++) {
        ULONG offset=(0xd00+i*0x100)/4;tfe[offset]|=1;
        assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
        tfe[offset]&=~1u;
    }
    for(ULONG i=0;i<9;i++) {
        ULONG offset=(0x600+i*0x200)/4,original=csid[offset];csid[offset]|=0x80000000u;
        assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
        csid[offset]=original;csid[offset+1]=1;
        assert(CamIspRaw10Check((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
        csid[offset+1]=0;
    }
    ULONG empty[0x1000/4]={0};
    assert(CamDmaDrain((PUCHAR)empty,(PUCHAR)csid,(PUCHAR)tfe,(PUCHAR)empty,(PUCHAR)empty,10)==STATUS_SUCCESS);
    puts("RAW10 input: bounded registers, idle ownership, readback failure and complete restoration passed");
    return 0;
}
