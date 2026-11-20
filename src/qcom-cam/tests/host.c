#include "host.h"
#include "dma-table.h"
#include "dma-route.h"
#include "cdm-command.h"
#include "cdm-route.h"
#include "cdm-abort.h"
#include "dma-ready.h"
#include "nv12.h"
#include "rpmh-message.h"
#include "wait.h"
#include "rcg-restore.h"
#include "csid-commit.h"
#include "cphy-lanes.h"
#include "crm-vote.h"
#include "clock-measure.h"
#include "gdsc-cycle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static ULONG regs[39936],words[16],wordCount,fifo[16],fifoCount,fifoIndex;
static ULONGLONG clockValue;
static BOOLEAN nack,timeout,preempt,rcgStuck;
static ULONG rcgActive,csidActive;
static ULONG crmRegs[256];
static BOOLEAN crmStuck;
static ULONGLONG crmComplete;
static ULONG crmDoneIrq;
static UCHAR gdscBase;
static ULONG gdscControl[2],gdscStatus[2],gdscStuck,gdscRestoreStuck,gdscCount;
static LONG gdscSequence[8];
static ULONG smmuRegs[0x100000/4],smmuWrites;
static UCHAR cdmBase;
static ULONG cdmRegs[128];
static BOOLEAN cdmResetStuck,cdmPendingStuck;
static UCHAR measureBase;
static ULONG measureControl,measureStatus,measureFrequency;
static ULONGLONG measureDone;
static BOOLEAN measureStuck;

ULONG CamRead(PUCHAR base,ULONG offset) {
    if(base==&measureBase) {
        if(offset==0x62048)return measureControl;
        assert(offset==0x6204c);
        if(measureDone&&!measureStuck&&clockValue>=measureDone) {
            measureStatus=0x2000000|(ULONG)((ULONGLONG)(measureControl&0xfffff)*measureFrequency/4800000);
            measureDone=0;
        }
        return measureStatus;
    }
    if(base==&cdmBase)return cdmRegs[offset/4];
    if(base==(PUCHAR)smmuRegs){assert(offset<sizeof(smmuRegs));return smmuRegs[offset/4];}
    if(base==&gdscBase) {
        assert(offset==0x11004||offset==0x11008||offset==0x1134c||offset==0x11350);
        ULONG slot=offset>=0x1134c?1:0;
        return (offset==0x11004||offset==0x1134c)?gdscControl[slot]:gdscStatus[slot];
    }
    if(base==(PUCHAR)crmRegs)return crmRegs[offset/4];
    (void)base;
    if(offset==0x218){assert(fifoIndex<fifoCount);return fifo[fifoIndex++];}
    if(offset==0x21c)return fifoCount-fifoIndex;
    return regs[offset/4];
}

VOID CamWrite(PUCHAR base,ULONG offset,ULONG value) {
    if(base==&measureBase) {
        assert(offset==0x62048);measureControl=value;
        if(value&0x200000){measureStatus=0;measureDone=0;}
        if(value&0x100000)measureDone=clockValue+(ULONGLONG)(value&0xfffff)*10000000/4800000;
        return;
    }
    if(base==&cdmBase) {
        if(offset==0x34){cdmRegs[0x44/4]&=~value;return;}
        cdmRegs[offset/4]=value;
        if(offset==0x1c&&value==3)cdmRegs[0x28/4]|=2;
        if(offset==0x10) {
            assert(value==1&&cdmRegs[0x1c/4]==3&&!cdmRegs[0x30/4]);
            if(!cdmResetStuck)cdmRegs[0x44/4]|=1;
            if(!cdmPendingStuck)cdmRegs[0x6c/4]=0;
        }
        return;
    }
    if(base==(PUCHAR)smmuRegs){assert(offset<sizeof(smmuRegs));smmuRegs[offset/4]=value;smmuWrites++;return;}
    if(base==&gdscBase) {
        assert(offset==0x11004||offset==0x1134c);
        ULONG slot=offset==0x1134c?1:0;
        assert(gdscCount<RTL_NUMBER_OF(gdscSequence));
        if(value&1) {
            assert(!(value&0x806));
            if(slot)assert(gdscControl[0]&1);
            gdscSequence[gdscCount++]=(LONG)slot+1;
            if(!(gdscStuck&(1u<<slot)))gdscStatus[slot]=0x04068000;
        } else {
            if(!slot)assert(!(gdscControl[1]&1));
            gdscSequence[gdscCount++]=-((LONG)slot+1);
            if(!(gdscRestoreStuck&(1u<<slot)))gdscStatus[slot]=0x04070000;
        }
        gdscControl[slot]=value;return;
    }
    if(base==(PUCHAR)crmRegs) {
        if(offset==0x250||offset==0x130||offset==0x1f0||offset==0x210){crmRegs[(offset-4)/4]&=~value;return;}
        crmRegs[offset/4]=value;
        if(offset==0x21c||offset==0x11c||offset==0x1dc||offset==0x1fc) {
            crmDoneIrq=offset==0x21c?0x24c:offset+0x10;
            assert(!(crmRegs[crmDoneIrq/4]&1));
            assert(!crmRegs[(crmDoneIrq+8)/4]);
            if(offset==0x21c)assert((value&0xe0000000u)==0xe0000000u);
            else assert(value<8);
            crmComplete=clockValue+500;
        }
        return;
    }
    (void)base;
    if(offset==0x20&&(value&1)) {
        if((regs[0x18/4]&0x100)&&(regs[0x1c/4]&0x100))csidActive=regs[0xe00/4];
    }
    if(offset==0x300) {
        regs[offset/4]=value;
        if((value&1)&&!rcgStuck){rcgActive=regs[0x304/4];regs[offset/4]&=~1u;}
        return;
    }
    if(offset==0x510){assert(wordCount<16);words[wordCount++]=value;regs[0x504/4]=wordCount;return;}
    if(offset==0xc08){regs[0xc0c/4]&=~value;return;}
    if(offset==4){assert(value==0x3f001);regs[0xc0c/4]|=0x1000000;wordCount=0;regs[0x504/4]=0;return;}
    if(offset==0x34&&value==2){regs[0xc0c/4]|=0x4000000;return;}
    if(offset==8) {
        assert(value==4);
        if(nack)regs[0xc0c/4]|=0x20000000;
        else if(!timeout) {
            BOOLEAN write=(words[wordCount-1]&15)==8;
            regs[0xc0c/4]|=write?0x10000:0x1000;
            if(!write) {
                ULONG bytes=words[3]>>4;fifoCount=bytes/4+1;fifoIndex=0;
                UCHAR *packet=(UCHAR *)fifo;packet[0]=0x1a;
                for(ULONG i=0;i<bytes;i++)packet[i+1]=(UCHAR)(i^0xa5);
            }
        }
        regs[0x504/4]=0;return;
    }
    regs[offset/4]=value;
}

VOID CamDelay(ULONG us) {
    clockValue+=(ULONGLONG)us*10;
    if(crmComplete&&!crmStuck&&clockValue>=crmComplete){crmRegs[crmDoneIrq/4]|=1;crmComplete=0;}
    if(preempt){clockValue+=20000;regs[0x100/4]=1;preempt=FALSE;}
}
ULONGLONG KeQueryInterruptTime(void) {return clockValue;}
NTSTATUS CamWait(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us) {
    return CamPoll(base,offset,mask,value,us);
}

NTSTATUS CamSave(CAM *c,PUCHAR base,ULONG offset,ULONG value) {
    (void)c;assert(base==&gdscBase||base==(PUCHAR)smmuRegs);CamWrite(base,offset,value);return STATUS_SUCCESS;
}

static VOID CdmTests(void) {
    memset(cdmRegs,0,sizeof(cdmRegs));cdmRegs[0x44/4]=1;cdmRegs[0x30/4]=1;cdmRegs[0x6c/4]=1;
    assert(CamCdmAbortCore(&cdmBase)==STATUS_SUCCESS&&cdmRegs[0x1c/4]==0&&!cdmRegs[0x6c/4]);
    cdmResetStuck=TRUE;clockValue=0;
    assert(CamCdmAbortCore(&cdmBase)==STATUS_IO_TIMEOUT&&clockValue>=500000);
    cdmResetStuck=FALSE;cdmPendingStuck=TRUE;cdmRegs[0x6c/4]=1;
    assert(CamCdmAbortCore(&cdmBase)==STATUS_DEVICE_BUSY);
    cdmPendingStuck=FALSE;
    ULONG cmd[8]={0};CamCdmEncode(cmd,0x5a);
    assert(CAM_CDM_COMMAND_BYTES==24&&cmd[0]==0x08127000&&cmd[1]==0x04000001);
    assert(cmd[2]==0xe10&&cmd[3]==0x8011&&cmd[4]==0x06000000&&cmd[5]==0x5a&&cmd[6]==0);
    memset(regs,0,sizeof(regs));
    regs[0x80/4]=0x59;regs[0xd0/4]=0xad27e10;regs[0xd4/4]=0x8011;
    assert(!CamCdmCompleted(NULL,0x5a));
    regs[0x80/4]=0x5a;assert(CamCdmCompleted(NULL,0x5a));
    regs[0x6c/4]=1;assert(!CamCdmCompleted(NULL,0x5a));regs[0x6c/4]=0;
    regs[0x44/4]=0x40000;assert(!CamCdmCompleted(NULL,0x5a));regs[0x44/4]=0;
    regs[0xd0/4]=0xd27e10;assert(!CamCdmCompleted(NULL,0x5a));
    CAM c={0};c.Base[CamSmmu]=(PUCHAR)smmuRegs;
    memset(smmuRegs,0,sizeof(smmuRegs));smmuWrites=0;
    assert(CamCdmRoute(&c)==STATUS_NOT_SUPPORTED&&!smmuWrites);
    smmuRegs[0x20/4]=0x4c017e84;smmuRegs[0x24/4]=0x60000053;
    smmuRegs[0x800/4]=0x80001c00;smmuRegs[0x1000/4]=0x1f000;smmuRegs[0x80000/4]=0xa9f00e0;
    assert(CamCdmRoute(&c)==STATUS_SUCCESS&&smmuWrites==2);
    assert(smmuRegs[0x804/4]==0x80001860&&smmuRegs[0xc04/4]==0x10000);
    assert(CamCdmRoute(&c)==STATUS_SUCCESS&&smmuWrites==2);
    smmuRegs[0x808/4]=0x80001860;assert(CamCdmRoute(&c)==STATUS_DEVICE_BUSY&&smmuWrites==2);
    smmuRegs[0x808/4]=0;smmuRegs[0x804/4]=0x80011860;
    assert(CamCdmRoute(&c)==STATUS_DEVICE_BUSY&&smmuWrites==2);
    smmuRegs[0x804/4]=0x80001860;smmuRegs[0x80000/4]|=1;
    assert(CamCdmRoute(&c)==STATUS_SUCCESS&&smmuWrites==2);
    smmuRegs[0xc04/4]=0;
    assert(CamCdmRoute(&c)==STATUS_NOT_SUPPORTED&&smmuWrites==2);
}

static VOID Fresh(CAM *c) {
    memset(c,0,sizeof(*c));memset(regs,0,sizeof(regs));memset(words,0,sizeof(words));
    wordCount=fifoCount=fifoIndex=0;clockValue=0;nack=timeout=preempt=rcgStuck=FALSE;rcgActive=csidActive=0;regs[0]=0x10000000;
}

static VOID WaitTests(void) {
    CAM c;Fresh(&c);regs[0x100/4]=0;
    assert(CamWait(NULL,0x100,1,1,1000)==STATUS_IO_TIMEOUT);
    Fresh(&c);preempt=TRUE;
    assert(CamWait(NULL,0x100,1,1,1000)==STATUS_SUCCESS);
    assert(clockValue>=20000);
    Fresh(&c);regs[0x100/4]=3;
    assert(CamWait(NULL,0x100,1,1,1000)==STATUS_SUCCESS);
    assert(clockValue==0);
}

static VOID CrmTests(void) {
    memset(crmRegs,0,sizeof(crmRegs));clockValue=0;crmComplete=0;crmStuck=FALSE;
    const ULONG states[]={1,2};
    assert(CamCrmStates((PUCHAR)crmRegs,0x11c,states)==STATUS_SUCCESS);
    assert(crmRegs[0x120/4]==1&&crmRegs[0x124/4]==2&&crmRegs[0x11c/4]==0&&!crmComplete);
    crmRegs[0x24c/4]=1;
    assert(CamCrmCommand((PUCHAR)crmRegs,400)==STATUS_SUCCESS);
    assert(clockValue>=500&&crmRegs[0x21c/4]==0xe0000190u);
    assert(!crmRegs[0x24c/4]&&!crmRegs[0x254/4]);
    assert(CamCrmCommand((PUCHAR)crmRegs,0)==STATUS_SUCCESS);
    assert(crmRegs[0x21c/4]==0xe0000000u);
    const ULONG perf[]={0x11c,0x1dc,0x1fc};
    for(ULONG i=0;i<RTL_NUMBER_OF(perf);i++) {
        crmRegs[(perf[i]+0x10)/4]=1;
        assert(CamCrmTransfer((PUCHAR)crmRegs,perf[i],1)==STATUS_SUCCESS);
        assert(crmRegs[perf[i]/4]==1&&!crmRegs[(perf[i]+0x10)/4]);
        assert(CamCrmTransfer((PUCHAR)crmRegs,perf[i],0)==STATUS_SUCCESS);
        assert(!crmRegs[perf[i]/4]);
    }
    crmStuck=TRUE;
    assert(CamCrmCommand((PUCHAR)crmRegs,400)==STATUS_IO_TIMEOUT);
    assert(!crmRegs[0x24c/4]&&crmComplete);
    crmStuck=FALSE;crmComplete=0;
}

static VOID MeasureTests(void) {
    measureControl=0;measureStatus=0x2000000;measureFrequency=10000000;measureStuck=FALSE;clockValue=0;
    ULONG shortCount=0,fullCount=0;
    assert(CamMeasureCount(&measureBase,0x1000,&shortCount)==STATUS_SUCCESS);
    assert(CamMeasureCount(&measureBase,0x20000,&fullCount)==STATUS_SUCCESS);
    ULONGLONG hz=CamMeasuredHz(shortCount,fullCount,8);
    assert(hz>79900000&&hz<80100000&&!(measureControl&0x100000));
    assert(CamMeasuredHz(37,37,8)==0);
    measureStuck=TRUE;clockValue=0;
    assert(CamMeasureCount(&measureBase,0x1000,&shortCount)==STATUS_IO_TIMEOUT);
    assert(!(measureControl&0x100000)&&clockValue>=500000);
    measureControl=0x100000;
    assert(CamMeasureCount(&measureBase,0x1000,&shortCount)==STATUS_DEVICE_BUSY&&measureControl==0x100000);
}

static VOID GdscTests(void) {
    CAM c={0};c.Base[CamCc]=&gdscBase;gdscCount=0;
    assert(CamGdscCycle(&c)==STATUS_DEVICE_NOT_READY&&!gdscCount);
    c.Powered=TRUE;c.DmaActive=TRUE;
    assert(CamGdscCycle(&c)==STATUS_DEVICE_BUSY&&!gdscCount);
    c.DmaActive=FALSE;c.ScmPending=TRUE;
    assert(CamGdscCycle(&c)==STATUS_DEVICE_BUSY&&!gdscCount);c.ScmPending=FALSE;
    for(ULONG fail=0;fail<3;fail++) {
        gdscCount=0;gdscStuck=fail;gdscRestoreStuck=0;clockValue=0;
        gdscControl[0]=gdscControl[1]=0xfa22f800;
        gdscStatus[0]=gdscStatus[1]=0x04070000;
        NTSTATUS status=CamGdscCycle(&c);
        assert(status==(fail?STATUS_IO_TIMEOUT:STATUS_SUCCESS));
        assert(gdscControl[0]==0xfa22f800&&gdscControl[1]==0xfa22f800&&!c.Faulted);
        if(!fail)assert(gdscCount==4&&gdscSequence[0]==1&&gdscSequence[1]==2&&gdscSequence[2]==-2&&gdscSequence[3]==-1);
        else assert(clockValue>=200000);
    }
    gdscCount=0;gdscStuck=0;gdscRestoreStuck=2;
    assert(CamGdscCycle(&c)==STATUS_IO_TIMEOUT&&c.Faulted&&c.Diag.Step==157);
    gdscRestoreStuck=0;
}

static VOID CciTests(void) {
    CAM c;UCHAR data[40];Fresh(&c);assert(CamCciInit(&c)==STATUS_SUCCESS);
    assert(regs[0x200/4]==0x00260038);assert(regs[0x204/4]==0x00280028);
    assert(regs[0x208/4]==0x00160023);assert(regs[0x20c/4]==0x3e);assert(regs[0x210/4]==0x63);
    for(ULONG count=1;count<=8;count++) {
        Fresh(&c);for(ULONG i=0;i<count;i++)data[i]=(UCHAR)(0x90+i);
        assert(CamCciWrite(&c,0x1a,0x1234,data,count)==STATUS_SUCCESS);
        assert(words[0]==6&&words[1]==0x301a1&&words[wordCount-2]==7&&words[wordCount-1]==0x308);
        UCHAR *packet=(UCHAR *)(words+2);
        assert((packet[0]&15)==9&&(packet[0]>>4)==count+2&&packet[1]==0x12&&packet[2]==0x34);
        assert(memcmp(packet+3,data,count)==0);
    }
    for(ULONG count=1;count<=32;count++) {
        Fresh(&c);memset(data,0,sizeof(data));
        assert(CamCciRead(&c,0x1a,0x1234,data,count)==STATUS_SUCCESS);
        assert(words[2]==0x0034122b);
        for(ULONG i=0;i<count;i++)assert(data[i]==(UCHAR)(i^0xa5));
        assert(data[count]==0);
    }
    Fresh(&c);assert(CamCciWrite(&c,0x1a,0,data,9)==STATUS_INVALID_PARAMETER);
    assert(CamCciRead(&c,0x1a,0,data,33)==STATUS_INVALID_PARAMETER);
    nack=TRUE;assert(CamCciWrite(&c,0x1a,0,data,1)==STATUS_IO_DEVICE_ERROR);assert(!c.Faulted);
    Fresh(&c);timeout=TRUE;assert(CamCciWrite(&c,0x1a,0,data,1)==STATUS_IO_TIMEOUT);assert(clockValue>=200000&&!c.Faulted);
}

static VOID RcgTests(void) {
    CAM c;Fresh(&c);regs[0x300/4]=2;regs[0x304/4]=0x203;rcgActive=0x203;
    assert(CamRcgRestore(NULL,0x300,1,0)==STATUS_SUCCESS);
    assert(rcgActive==1&&regs[0x300/4]==0&&regs[0x304/4]==1);
    Fresh(&c);regs[0x300/4]=2;regs[0x304/4]=0x203;rcgActive=0x203;rcgStuck=TRUE;
    assert(CamRcgRestore(NULL,0x300,1,0)==STATUS_IO_TIMEOUT);
    assert(rcgActive==0x203&&(regs[0x300/4]&3)==3);
}

static ULONGLONG Walk(ULONGLONG *tables,ULONGLONG root,ULONGLONG address) {
    ULONG shifts[]={30,21,12};ULONGLONG at=root;
    for(ULONG i=0;i<3;i++) {
        assert(at>=root&&at<root+40960);
        ULONGLONG value=tables[(at-root)/8+((address>>shifts[i])&511)];
        if((value&3)!=3)return UINT64_MAX;
        at=value&0x0000fffffffff000ULL;
    }
    return at|(address&4095);
}

static VOID DmaTests(void) {
    CAM ready={0};memset(regs,0,sizeof(regs));
    assert(!CamDiagnosticDmaReady(&ready));ready.ReceiverVerified=TRUE;
    assert(!CamDiagnosticDmaReady(&ready));
    regs[0x11300/4]=1;regs[0x112e8/4]=0;regs[0x26014/4]=0x08200001;
    regs[0x228/4]=0xe0000190;regs[0x11350/4]=regs[0x11008/4]=0x10000;
    regs[0x266c/4]=regs[0x32cc/4]=regs[0x34dc/4]=1;
    assert(CamDiagnosticDmaReady(&ready));
    const ULONG disabled[]={0x11300,0x26014,0x228,0x11350,0x11008,0x266c,0x32cc,0x34dc};
    for(ULONG i=0;i<RTL_NUMBER_OF(disabled);i++) {
        ULONG old=regs[disabled[i]/4];regs[disabled[i]/4]=0;
        assert(!CamDiagnosticDmaReady(&ready));regs[disabled[i]/4]=old;
    }
    regs[0x112e8/4]=0x80000000;assert(!CamDiagnosticDmaReady(&ready));regs[0x112e8/4]=0;
    regs[0x11300/4]|=0x80000000;assert(!CamDiagnosticDmaReady(&ready));regs[0x11300/4]=1;
    regs[0x228/4]=400;assert(!CamDiagnosticDmaReady(&ready));
    const ULONG assignments[]={0x210,0x201,0x120,0x102,0x021,0x012};
    for(ULONG i=0;i<RTL_NUMBER_OF(assignments);i++)assert(CamCphyLaneMask(assignments[i],3)==0x2a);
    assert(CamCphyLaneMask(0,1)==2&&CamCphyLaneMask(1,1)==8&&CamCphyLaneMask(2,1)==32);
    assert(CamCphyLaneMask(0x20,2)==34);
    assert(!CamCphyLaneMask(0,2)&&!CamCphyLaneMask(0x310,3));
    assert(!CamCphyLaneMask(0x210,0)&&!CamCphyLaneMask(0x210,4));
    CAM c;Fresh(&c);regs[0xe00/4]=0x802b2260;
    CamWrite(NULL,0x18,0x100);CamWrite(NULL,0x1c,0x100);assert(!csidActive);
    CamCsidCommit(NULL,0x100);assert(csidActive==0x802b2260);
    assert(CamDmaIdentity(0x10000,0,0));
    assert(CamDmaIdentity(0,0x1f000,0x0a9f00e0));
    assert(CamDmaIdentity(3,0x1f000,0xe0));
    assert(!CamDmaIdentity(0,0x1f000,0x0a9f00e1));
    assert(!CamDmaIdentity(0,0xf000,0xe0));
    assert(!CamDmaIdentity(0,0x3f000,0xe0));
    assert(!CamDmaIdentity(0x20000,0x1f000,0xe0));
    assert(!CamDmaIdentity(0x30000,0x1f000,0xe0));
    ULONGLONG *table=calloc(5120,sizeof(*table));PFN_NUMBER *pfns=calloc(4096,sizeof(*pfns));
    assert(table&&pfns);
    for(ULONG i=0;i<4096;i++)pfns[i]=0x880000+3*i;
    assert(CamDmaBuild(table,0x100000,pfns,3840));
    for(ULONG i=0;i<3840;i++) {
        ULONGLONG address=CAM_DMA_IOVA+4096ULL*i;
        ULONGLONG encoded=((ULONGLONG)CamDmaImageAddress(address)<<8)|(address&255);
        assert(Walk(table,0x100000,encoded)==pfns[i]*4096);
        assert(Walk(table,0x100000,0x10000000ULL+4096ULL*i)==pfns[i]*4096);
        assert(Walk(table,0x100000,0x10000000ULL+4096ULL*i+4095)==pfns[i]*4096+4095);
    }
    assert(Walk(table,0x100000,0x0fffffff)==UINT64_MAX);
    assert(Walk(table,0x100000,0x10f00000)==UINT64_MAX);
    assert(Walk(table,0x100000,0x30000000)==UINT64_MAX);
    assert(!CamDmaBuild(table,0x100001,pfns,3840));assert(!CamDmaBuild(table,0x100000,pfns,4097));
    pfns[0]=0x100000000ULL;assert(!CamDmaBuild(table,0x100000,pfns,3840));
    free(pfns);free(table);
}

static VOID ColorFrame(UCHAR *raw,ULONG r,ULONG g,ULONG b) {
    for(ULONG y=0;y<QCAM_HEIGHT;y++)for(ULONG x=0;x<QCAM_WIDTH;x+=4) {
        ULONG pixels[4]={y&1?g:r,y&1?b:g,y&1?g:r,y&1?b:g};
        UCHAR *p=raw+y*QCAM_STRIDE+x/4*5;p[4]=0;
        for(ULONG i=0;i<4;i++){p[i]=(UCHAR)(pixels[i]>>2);p[4]|=(UCHAR)((pixels[i]&3)<<(2*i));}
    }
}

static VOID ColorTests(void) {
    UCHAR *raw=malloc(QCAM_FRAME_BYTES),*image=malloc(QCAM_VIDEO_BYTES+1);assert(raw&&image);
    const ULONG colors[][6]={{64,64,64,16,128,128},{1023,1023,1023,235,128,128},
        {1023,64,64,82,90,240},{64,1023,64,144,54,34},{64,64,1023,41,240,110}};
    for(ULONG i=0;i<RTL_NUMBER_OF(colors);i++) {
        ColorFrame(raw,colors[i][0],colors[i][1],colors[i][2]);memset(image,0xa5,QCAM_VIDEO_BYTES+1);
        assert(CamNv12(raw,QCAM_FRAME_BYTES,image,QCAM_VIDEO_BYTES));
        for(ULONG p=0;p<QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;p++)assert(image[p]==colors[i][3]);
        for(ULONG p=QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;p<QCAM_VIDEO_BYTES;p+=2) {
            assert(image[p]==colors[i][4]&&image[p+1]==colors[i][5]);
        }
        assert(image[QCAM_VIDEO_BYTES]==0xa5);
    }
    assert(!CamNv12(raw,QCAM_FRAME_BYTES-1,image,QCAM_VIDEO_BYTES));
    assert(!CamNv12(raw,QCAM_FRAME_BYTES,image,QCAM_VIDEO_BYTES-1));
    free(image);free(raw);
}

static ULONG VrmTransaction(ULONG message,ULONG data,ULONG *rail) {
    assert((message&255)==8);
    assert(message&0x100);
    if(message&0x10000)*rail=data;
    return *rail;
}

static VOID PowerTests(void) {
    assert(CamRpmhSameResource(0x30010,0x30010));
    assert(!CamRpmhSameResource(0x30010,0x30014));
    assert(CamRpmhSameResource(0x40020,0x40028));
    assert(CamRpmhSameResource(0x4002c,0x40024));
    assert(!CamRpmhSameResource(0x40020,0x40030));
    assert(!CamRpmhSameResource(0x40020,0x50020));
    ULONG rail=1100;
    assert(VrmTransaction(CamRpmhMessage(FALSE),0,&rail)==1100);
    assert(rail==1100);
    assert(VrmTransaction(CamRpmhMessage(TRUE),1200,&rail)==1200);
    assert(VrmTransaction(CamRpmhMessage(FALSE),0,&rail)==1200);
    assert(rail==1200);
}

static struct { ULONG Calls,WakePolls,Resumes,Acks; LONG QueryStatus,QosStatus; BOOLEAN Supported,WaitStuck,WaitError,WaitForeign,WaitSlow; } ScmMock;

VOID CamScmSlow(ULONGLONG *registers) {
    ScmMock.Calls++;
    if(registers[0]==0x42000601) {
        assert(registers[1]==1&&registers[2]==0x0200180a);
        for(ULONG i=3;i<8;i++)assert(registers[i]==0);
        registers[0]=(ULONG)ScmMock.QueryStatus;registers[1]=ScmMock.Supported;
    } else if(registers[0]==0x4200180a) {
        assert(registers[0]==0x4200180a&&registers[1]==0x83);
        for(ULONG i=2;i<8;i++)assert(registers[i]==0);
        registers[0]=(ULONG)ScmMock.QosStatus;
        if(ScmMock.QosStatus==2||ScmMock.QosStatus==3){registers[1]=7;registers[2]=0x1234;registers[3]=1;}
    } else if(registers[0]==0xc2002404) {
        for(ULONG i=1;i<8;i++)assert(registers[i]==0);
        registers[0]=ScmMock.WaitSlow?MAXULONG:0;
    } else if(registers[0]==0xc2002403||registers[0]==0x42002403) {
        assert(registers[0]==(ScmMock.WaitSlow?0x42002403:0xc2002403));
        for(ULONG i=1;i<8;i++)assert(registers[i]==0);
        ScmMock.WakePolls++;
        registers[0]=ScmMock.WaitError?MAXULONG:0;
        registers[1]=ScmMock.WaitForeign?9:7;
        registers[2]=ScmMock.WaitStuck?0:ScmMock.WakePolls>=3?1:0;
    } else if(registers[0]==0x42002402||registers[0]==0x42002401) {
        assert(registers[1]==1&&registers[2]==0x1234);
        for(ULONG i=3;i<8;i++)assert(registers[i]==0);
        if(registers[0]==0x42002402)ScmMock.Resumes++;else ScmMock.Acks++;
        memset(registers,0,8*sizeof(*registers));
    } else {
        assert(registers[0]==0xc2000501&&registers[1]==1&&registers[2]==0x0adcfd00);
        for(ULONG i=3;i<8;i++)assert(registers[i]==0);
        registers[0]=0;registers[1]=0x10000;
    }
}

static VOID ScmTests(void) {
    CAM c={0};RtlZeroMemory(&ScmMock,sizeof(ScmMock));
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==0);
    c.Powered=TRUE;c.DmaActive=TRUE;
    assert(CamScmQos(&c)==STATUS_DEVICE_BUSY&&ScmMock.Calls==0);
    c.DmaActive=FALSE;c.RxActive=TRUE;
    assert(CamScmQos(&c)==STATUS_DEVICE_BUSY&&ScmMock.Calls==0);c.RxActive=FALSE;
    regs[0xf0/4]=0;
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==0);
    regs[0xf0/4]=1;regs[0xf8/4]=1;
    assert(CamScmQos(&c)==STATUS_NOT_SUPPORTED&&ScmMock.Calls==1&&!c.ScmAvailable);
    ScmMock.QueryStatus=-1;ScmMock.Supported=TRUE;
    assert(CamScmProbe(&c)==STATUS_NOT_SUPPORTED&&!c.ScmAvailable);
    ScmMock.QueryStatus=1;
    assert(CamScmProbe(&c)==STATUS_DEVICE_BUSY&&!c.ScmAvailable);
    ScmMock.QueryStatus=0;ScmMock.Calls=0;
    assert(CamScmQos(&c)==STATUS_SUCCESS&&ScmMock.Calls==3&&c.ScmAvailable);
    assert(c.Diag.Step==151&&c.Diag.Address==0x4200180a&&c.Diag.Count==3);
    assert(CamScmQos(&c)==STATUS_SUCCESS&&ScmMock.Calls==4);
    ScmMock.QosStatus=-1;
    assert(CamScmQos(&c)==STATUS_IO_DEVICE_ERROR&&c.Trace.ScmReturn[0]==MAXULONG);
    c.Powered=FALSE;
    assert(CamScmIoProbe(&c)==STATUS_DEVICE_NOT_READY);
    c.Powered=TRUE;
    assert(CamScmIoProbe(&c)==STATUS_SUCCESS&&c.Trace.ScmReturn[1]==0x10000);
    assert(c.Diag.Step==152&&c.Diag.Address==0x0adcfd00);
    ScmMock.QosStatus=2;ScmMock.WakePolls=0;
    assert(CamScmQos(&c)==STATUS_SUCCESS&&!c.ScmPending);
    assert(ScmMock.Resumes==1&&ScmMock.WakePolls==3&&c.Trace.ScmReturn[0]==0);
    ScmMock.QosStatus=3;
    assert(CamScmQos(&c)==STATUS_SUCCESS&&!c.ScmPending&&ScmMock.Acks==1);
    ScmMock.QosStatus=2;ScmMock.WaitSlow=TRUE;ScmMock.WakePolls=0;c.ScmWaitqKnown=FALSE;
    assert(CamScmQos(&c)==STATUS_SUCCESS&&!c.ScmPending&&ScmMock.Resumes==2);
    ScmMock.WaitStuck=TRUE;clockValue=0;
    assert(CamScmQos(&c)==STATUS_IO_TIMEOUT&&c.ScmPending&&clockValue>=20000000);
    ULONG calls=ScmMock.Calls;
    assert(CamScmQos(&c)==STATUS_DEVICE_BUSY&&ScmMock.Calls==calls);
    c.ScmPending=FALSE;ScmMock.WaitStuck=FALSE;ScmMock.WaitError=TRUE;
    assert(CamScmQos(&c)==STATUS_DEVICE_BUSY&&c.ScmPending);
    c.ScmPending=FALSE;ScmMock.WaitError=FALSE;ScmMock.WaitForeign=TRUE;ScmMock.WakePolls=0;
    assert(CamScmQos(&c)==STATUS_DEVICE_BUSY&&c.ScmPending);
    c.ScmPending=FALSE;ScmMock.WaitForeign=FALSE;ScmMock.QosStatus=0;
    regs[0xf0/4]=0;regs[0x228/4]=400;regs[0x266c/4]=1;regs[0x32cc/4]=1;regs[0x34dc/4]=1;
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY);
    regs[0x32cc/4]=0;
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY);
    c.ReceiverVerified=TRUE;regs[0xf0/4]=0;regs[0xf8/4]=0;
    regs[0x11300/4]=1;regs[0x11310/4]=1;regs[0x112e8/4]=0;regs[0x26014/4]=1;
    regs[0x11350/4]=0x10000;regs[0x11008/4]=0x10000;
    regs[0x228/4]=0xe0000190;regs[0x220/4]=0xe0000190;regs[0x224/4]=0xe0000190;
    const ULONG domains[]={0,6,7};
    for(ULONG i=0;i<3;i++) {
        regs[(0x266c+domains[i]*0x210)/4]=1;
        for(ULONG state=0;state<3;state++)regs[(0x11c+domains[i]*0x20+state*4)/4]=1;
    }
    calls=ScmMock.Calls;
    regs[0x224/4]=0;assert(CamScmQosDiagnostic(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    regs[0x224/4]=0xe0000190;regs[0x1e4/4]=0;
    assert(CamScmQosDiagnostic(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    regs[0x1e4/4]=1;c.CdmPending=TRUE;
    assert(CamScmQosDiagnostic(&c)==STATUS_DEVICE_BUSY&&ScmMock.Calls==calls);
    c.CdmPending=FALSE;
    assert(CamScmQosDiagnostic(&c)==STATUS_SUCCESS&&!c.ScmPending);
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY);
    ScmMock.WaitSlow=FALSE;ScmMock.QosStatus=2;ScmMock.WakePolls=0;
    assert(CamScmWaitqProbe(&c)==STATUS_SUCCESS&&c.ScmFastWaitq);
    calls=ScmMock.Calls;ULONG resumes=ScmMock.Resumes;
    assert(CamScmQosRaw(&c)==STATUS_DEVICE_BUSY&&c.ScmPending&&c.Trace.ScmReturn[0]==2);
    assert(ScmMock.Calls==calls+1&&ScmMock.Resumes==resumes&&ScmMock.WakePolls==0);
    assert(CamScmWaitqContext(&c)==STATUS_SUCCESS&&c.ScmPending&&c.Trace.ScmReturn[0]==2);
    assert(ScmMock.Resumes==resumes&&ScmMock.WakePolls==1);
}

int main(void) {MeasureTests();CdmTests();WaitTests();CrmTests();GdscTests();RcgTests();CciTests();DmaTests();ColorTests();PowerTests();ScmTests();puts("Clock measurement, CDM routing and completion, wait preemption, CRM acknowledgment, GDSC sequencing, RCG restore, CCI, C-PHY, CSID commit, DMA, NV12, RPMh and powered secure QoS passed");return 0;}
