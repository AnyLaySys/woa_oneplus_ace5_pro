#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t LONG,NTSTATUS;
typedef unsigned char *PUCHAR;
typedef int BOOLEAN;
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
typedef struct {
    ULONG Size,Abi,Width,Height,Status,Count;
    ULONG Offsets[227],Expected[227],Before[227],Configured[227],Restored[227];
} QCAM_ISP_PIXEL_STATE;
static ULONG csid[0x2000/4],tfe[0x10000/4],before[0x10000/4],writes,inject;
static ULONGLONG ticks;
static ULONG CamRead(PUCHAR base,ULONG offset) { return ((ULONG *)base)[offset/4]; }
static ULONGLONG CamRead64(PUCHAR base,ULONG offset) { return CamRead(base,offset)|((ULONGLONG)CamRead(base,offset+4)<<32); }
static void CamWrite(PUCHAR base,ULONG offset,ULONG value) {
    assert(base==(PUCHAR)tfe);
    assert(offset<0xd00||offset>0x2900);
    assert(offset!=0x6b08&&offset!=0x6b10&&offset!=0x6b14);
    ((ULONG *)base)[offset/4]=value;writes++;
    if(inject&&offset==0xe088){((ULONG *)base)[offset/4]^=1;inject=0;}
}
static ULONGLONG KeQueryInterruptTime(void) { return ticks++; }
static void CamDelay(ULONG us) { ticks+=us*10; }
#include "../isp-pixel-check.h"
int main(void) {
    QCAM_ISP_PIXEL_STATE state;
    CAM_ISP_PIXEL_PLAN plan;
    csid[0]=0x30000006;tfe[0]=0x10000000;
    assert(CamIspPixelReference(&plan,4096,3072));
    for(ULONG i=0;i<plan.Count;i++) {
        ULONG offset=plan.Registers[i].Offset;
        for(ULONG j=0;j<i;j++)assert(plan.Registers[j].Offset!=offset);
        tfe[offset/4]=i*13+0x20;
    }
    tfe[0xdbf4/4]=tfe[0xe1f4/4]=tfe[0x78f4/4]=0;
    memcpy(before,tfe,sizeof(tfe));
    const ULONG widths[]={4096,3264,1600,2304},heights[]={3072,2448,1200,1728};
    for(ULONG i=0;i<4;i++) {
        writes=0;
        assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,widths[i],heights[i],&state)==STATUS_SUCCESS);
        assert(state.Count==227&&writes==454&&!memcmp(before,tfe,sizeof(tfe)));
        assert(!memcmp(state.Before,state.Restored,sizeof(state.Before)));
    }
    writes=0;inject=1;
    assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_IO_DEVICE_ERROR);
    assert(writes==454&&!memcmp(before,tfe,sizeof(tfe)));
    assert(state.Configured[129]!=state.Expected[129]); /* last WB offset */
    writes=0;
    assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,0,3072,&state)==STATUS_INVALID_PARAMETER&&!writes);
    for(ULONG i=0;i<28;i++) {
        tfe[(0xd00+i*0x100)/4]=1;
        assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
        tfe[(0xd00+i*0x100)/4]=0;
    }
    for(ULONG i=0;i<9;i++) {
        csid[(0x600+i*0x200)/4]=0x80000000u;
        assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
        csid[(0x600+i*0x200)/4]=0;
    }
    tfe[0xe1f4/4]=0x400;
    assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_DEVICE_BUSY&&!writes);
    tfe[0xe1f4/4]=0;tfe[0]=0;
    assert(CamIspPixelStandbyCheck((PUCHAR)csid,(PUCHAR)tfe,4096,3072,&state)==STATUS_REVISION_MISMATCH&&!writes);
    plan.Count=227;ULONG word=1;
    assert(!CamIspPixelRange(&plan,0x6b58,&word,1));
    ULONG empty[0x1000/4]={0};
    assert(CamDmaDrain((PUCHAR)empty,(PUCHAR)csid,(PUCHAR)tfe,(PUCHAR)empty,(PUCHAR)empty,10)==STATUS_SUCCESS);
    puts("ISP pixel MMIO: four geometries, 227 unique registers, no DMA/DMI writes, final WB offset fault, ownership guards and complete restoration passed");
    return 0;
}
