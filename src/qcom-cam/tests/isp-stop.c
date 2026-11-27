#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t ULONG;
typedef int32_t NTSTATUS;
typedef unsigned char *PUCHAR;
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_BUSY -1
#define STATUS_IO_TIMEOUT -2
#define NT_SUCCESS(s) ((s)>=0)
static ULONG csid[0x3000/4],tfe[0x10000/4],writes,fault;
static ULONG CamRead(PUCHAR p,ULONG offset) { return ((ULONG *)p)[offset/4]; }
static void CamWrite(PUCHAR p,ULONG offset,ULONG value) {
    assert(p==(PUCHAR)csid);
    assert(offset==0x8c||offset==0x14||offset==0xc||offset==0x10);
    writes++;csid[offset/4]=value;
    if(offset==0x10) {
        assert(value==2&&csid[0xc/4]==0x11);
        if(fault!=1)csid[0x84/4]|=1;
        if(!fault)csid[0x654/4]|=1;
    }
}
static NTSTATUS CamWait(PUCHAR p,ULONG offset,ULONG mask,ULONG value,ULONG us) {
    assert(us==50000);
    return (CamRead(p,offset)&mask)==value?STATUS_SUCCESS:STATUS_IO_TIMEOUT;
}
#include "../isp-stop.h"
int main(void) {
    ULONG reset=0x103;
    csid[0x654/4]=1;
    assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==0&&reset==0&&!writes);
    csid[0x654/4]=0;
    for(ULONG i=0;i<28;i++)if(i!=7&&i!=8) {
        tfe[(0xd00+i*0x100)/4]=1;
        assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==STATUS_DEVICE_BUSY&&!writes);
        tfe[(0xd00+i*0x100)/4]=0;
    }
    for(ULONG i=1;i<9;i++) {
        csid[(0x604+i*0x200)/4]=1;
        assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==STATUS_DEVICE_BUSY&&!writes);
        csid[(0x604+i*0x200)/4]=0;csid[(0x600+i*0x200)/4]=0x80000000u;
        assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==STATUS_DEVICE_BUSY&&!writes);
        csid[(0x600+i*0x200)/4]=0;
    }
    assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==0&&reset==0&&writes==4);
    memset(csid,0,sizeof(csid));fault=1;
    assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==STATUS_IO_TIMEOUT&&reset==(ULONG)STATUS_IO_TIMEOUT);
    memset(csid,0,sizeof(csid));fault=2;
    assert(CamIspFdHalt((PUCHAR)csid,(PUCHAR)tfe,&reset)==STATUS_IO_TIMEOUT&&reset==0);
    puts("FD halt: incomplete frame immediate reset, shared path/writer reset rejection, reset completion and post-reset halt failures passed");
    return 0;
}
