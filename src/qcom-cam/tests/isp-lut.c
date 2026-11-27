#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef int NTSTATUS,BOOLEAN;
typedef unsigned char *PUCHAR;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_REVISION_MISMATCH -2
#define STATUS_DEVICE_BUSY -3
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_DATA_ERROR -5
#define NT_SUCCESS(s) ((s)>=0)
static ULONG regs[0x10000/4],before[0x10000/4],words[3][512];
static ULONG cursor,reads,writes,wordFault,setupFault,restoreFault;
static ULONG CamRead(PUCHAR base,ULONG offset) {
    assert(base==(PUCHAR)regs);
    if(offset==0x6610||offset==0x6b10) {
        ULONG cfg=offset-8,selector=regs[(cfg+4)/4];
        assert(regs[cfg/4]==0x100000&&selector>=1&&selector<=3);
        assert(cursor<(offset==0x6610?512u:64u));
        ULONG value=words[selector-1][cursor++];reads++;
        return wordFault&&cursor==17?value^1:value;
    }
    return regs[offset/4];
}
static void CamWrite(PUCHAR base,ULONG offset,ULONG value) {
    assert(base==(PUCHAR)regs);
    /* No data-port write, bank switch, DMA control or unrelated MMIO. */
    assert(offset==0x6608||offset==0x660c||offset==0x6b08||offset==0x6b0c||offset==0x6cf4);
    if(offset==0x660c||offset==0x6b0c)cursor=0;
    regs[offset/4]=value;writes++;
    if((offset==0x6608||offset==0x6b08)&&
       ((setupFault&&value==0x100000)||(restoreFault&&value!=0x100000)))regs[offset/4]^=1;
}
#include "../isp-lut-read.h"
static void reset(void) {
    memset(regs,0,sizeof(regs));regs[0]=0x10000000;
    regs[0x6658/4]=regs[0x6b58/4]=1;
    regs[0x6608/4]=0x20;regs[0x660c/4]=7;
    regs[0x6b08/4]=0x40;regs[0x6b0c/4]=9;
    memcpy(before,regs,sizeof(regs));reads=writes=cursor=wordFault=setupFault=restoreFault=0;
}
int main(void) {
    ULONG observed[512],count,mismatch;NTSTATUS restore;
    for(ULONG channel=0;channel<3;channel++)for(ULONG i=0;i<512;i++)words[channel][i]=i|(channel<<24);
    for(BOOLEAN gtm=FALSE;gtm<=TRUE;gtm++)for(ULONG channel=0;channel<(gtm?1u:3u);channel++) {
        reset();memset(observed,0xa5,sizeof(observed));
        assert(CamIspLutRead((PUCHAR)regs,gtm,channel+1,1,words[channel],observed,&count,&mismatch,&restore)==STATUS_SUCCESS);
        assert(count==(gtm?512u:64u)&&reads==count&&!mismatch&&restore==STATUS_SUCCESS);
        assert(!memcmp(observed,words[channel],count*4)&&!memcmp(before,regs,sizeof(regs)));
        reset();wordFault=1;
        assert(CamIspLutRead((PUCHAR)regs,gtm,channel+1,1,words[channel],observed,&count,&mismatch,&restore)==STATUS_DATA_ERROR);
        assert(mismatch==1&&restore==STATUS_SUCCESS&&!memcmp(before,regs,sizeof(regs)));
        reset();setupFault=1;
        assert(CamIspLutRead((PUCHAR)regs,gtm,channel+1,1,words[channel],observed,&count,&mismatch,&restore)==STATUS_IO_DEVICE_ERROR);
        assert(!reads&&!count&&restore==STATUS_SUCCESS&&!memcmp(before,regs,sizeof(regs)));
        reset();restoreFault=1;
        assert(CamIspLutRead((PUCHAR)regs,gtm,channel+1,1,words[channel],observed,&count,&mismatch,&restore)==STATUS_IO_DEVICE_ERROR);
        assert(restore==STATUS_IO_DEVICE_ERROR);
    }
    reset();
    assert(CamIspLutRead((PUCHAR)regs,TRUE,1,0,words[0],observed,&count,&mismatch,&restore)==STATUS_DEVICE_BUSY);
    assert(!writes&&!reads);
    regs[0x6cf4/4]=0x8000;
    assert(CamIspLutRead((PUCHAR)regs,FALSE,1,1,words[0],observed,&count,&mismatch,&restore)==STATUS_DEVICE_BUSY);
    assert(!writes&&!reads);regs[0x6cf4/4]=0;
    assert(CamIspLutRead((PUCHAR)regs,TRUE,2,1,words[0],observed,&count,&mismatch,&restore)==STATUS_INVALID_PARAMETER);
    assert(CamIspLutRead((PUCHAR)regs,FALSE,4,1,words[0],observed,&count,&mismatch,&restore)==STATUS_INVALID_PARAMETER);
    assert(!writes&&!reads);
    regs[0]=0;
    assert(CamIspLutRead((PUCHAR)regs,TRUE,1,1,words[0],observed,&count,&mismatch,&restore)==STATUS_REVISION_MISMATCH);
    assert(!writes&&!reads);
    puts("ISP LUT readback: bounded native ports/selectors, exact restoration, mismatch and access failures passed");
}
