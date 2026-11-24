#include "host.h"
#include "dma-table.h"
#include "frame-map-table.h"
#include "dma-route.h"
#include "cdm-command.h"
#include "cdm-route.h"
#include "cdm-abort.h"
#include "smmu-fault.h"
#include "smmu-restore.h"
#include "frame-route.h"
#include "tbu-translate.h"
#include "mnoc-qos.h"
#include "scm-qos-payload.h"
#include "dma-ready.h"
#include "dma-idle.h"
#include "rdi-frame.h"
#include "frame-clock.h"
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
static BOOLEAN checkFrameStores;
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
static ULONGLONG tbuRegs[512],tbuDone;
static ULONG tbuMode,tbuWrites;

ULONGLONG CamRead64(PUCHAR base,ULONG offset) {
    assert(base==(PUCHAR)tbuRegs&&offset<sizeof(tbuRegs)&&!(offset&7));
    if(tbuDone&&clockValue>=tbuDone&&tbuMode!=2) {
        tbuRegs[0x450/8]=tbuMode==3?0:1;
        tbuRegs[0x458/8]=tbuMode==3?0x11:(tbuRegs[0x440/8]+(tbuMode==4?4096:0))<<12;
    }
    return tbuRegs[offset/8];
}

VOID CamWrite64(PUCHAR base,ULONG offset,ULONGLONG value) {
    assert(base==(PUCHAR)tbuRegs&&offset<sizeof(tbuRegs)&&!(offset&7));tbuWrites++;
    if(offset==0x448&&!value&&tbuMode==6)return;
    tbuRegs[offset/8]=value;
    if(offset==0x410) {
        if(value&1){if(tbuMode!=1)tbuRegs[0x418/8]=1;}
        else if(tbuMode!=5)tbuRegs[0x418/8]=0;
    }
    if(offset==0x448) {
        if(value) {
            assert(tbuRegs[0x418/8]==1&&
                ((tbuRegs[0x430/8]==0x60&&tbuRegs[0x438/8]==0x408)||
                 (tbuRegs[0x430/8]==0&&tbuRegs[0x438/8]==0x409)));
            tbuDone=clockValue+50;
        } else {tbuDone=0;tbuRegs[0x450/8]=tbuRegs[0x458/8]=0;}
    }
}

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
        assert(offset==0x110ec||offset==0x110f0||offset==0x1134c||offset==0x11350);
        ULONG slot=offset>=0x1134c?1:0;
        return (offset==0x110ec||offset==0x1134c)?gdscControl[slot]:gdscStatus[slot];
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
    if(base==(PUCHAR)smmuRegs){
        assert(offset<sizeof(smmuRegs));
        if(checkFrameStores&&offset>=0xc00&&offset<0xe00) {
            assert(!(smmuRegs[(0x800+offset-0xc00)/4]&0x80000000u));
            assert(!(value&0x30000)&&(value&255)<83);
        }
        smmuRegs[offset/4]=value;smmuWrites++;return;
    }
    if(base==&gdscBase) {
        assert(offset==0x110ec||offset==0x1134c);
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
    assert(!CamSmmuFault(0x400));
    assert(CamSmmuFault(0x40000002)&&CamSmmuFault(0x80000010));
    ULONG cmd[8]={0};CamCdmEncode(cmd,0x5a);
    assert(CAM_CDM_COMMAND_BYTES==24&&cmd[0]==0x08127000&&cmd[1]==0x04000001);
    assert(cmd[2]==0xe10&&cmd[3]==0x8011&&cmd[4]==0x06000001&&cmd[5]==0x5a&&cmd[6]==0);
    ULONG fetch[4]={0};CamCdmEncodeFetch(fetch,0x5a);
    assert(CAM_CDM_FETCH_BYTES==8&&fetch[0]==0x06000001&&fetch[1]==0x5a&&!fetch[2]);
    memset(regs,0,sizeof(regs));
    regs[0x80/4]=0x59;regs[0xd0/4]=0xad27e10;regs[0xd4/4]=0x8011;regs[0x44/4]=2;
    assert(!CamCdmCompleted(NULL,0x5a));
    regs[0x80/4]=0x5a;assert(CamCdmCompleted(NULL,0x5a));
    regs[0x6c/4]=1;assert(!CamCdmCompleted(NULL,0x5a));regs[0x6c/4]=0;
    regs[0x44/4]=0x40002;assert(!CamCdmCompleted(NULL,0x5a));regs[0x44/4]=0;
    assert(!CamCdmFetched(NULL,0x5a));regs[0x44/4]=2;
    regs[0xd0/4]=0;assert(CamCdmFetched(NULL,0x5a)&&!CamCdmCompleted(NULL,0x5a));
    regs[0xd0/4]=0xd27e10;assert(!CamCdmCompleted(NULL,0x5a));
    CAM c={0};c.Base[CamSmmu]=(PUCHAR)smmuRegs;
    memset(smmuRegs,0,sizeof(smmuRegs));smmuWrites=0;
    assert(CamCdmRoute(&c)==STATUS_NOT_SUPPORTED&&!smmuWrites);
    smmuRegs[0x20/4]=0x4c017e84;smmuRegs[0x24/4]=0x60000053;
    smmuRegs[0x800/4]=0x80001c00;smmuRegs[0x1000/4]=0x1f000;smmuRegs[0x80000/4]=0xa9f00e0;
    assert(CamCdmRoute(&c)==STATUS_NOT_SUPPORTED&&smmuWrites==0);
    smmuRegs[0x804/4]=0x80000040;smmuRegs[0xc04/4]=1;
    smmuRegs[0x1004/4]=0x1f000;smmuRegs[0x81000/4]=0xe0;
    memset(c.Trace.CommandRoute,0,sizeof(c.Trace.CommandRoute));
    assert(CamCdmRoute(&c)==STATUS_SUCCESS&&smmuWrites==2);
    assert(c.Trace.CommandRoute[0]==2&&c.Trace.CommandRoute[1]==1&&c.Trace.CommandRoute[3]==0xe0);
    assert(smmuRegs[0x808/4]==0x80001860&&smmuRegs[0xc08/4]==1);
    assert(c.Trace.CommandRoute[1]==1&&c.Trace.CommandRoute[3]==0xe0);
    memset(c.Trace.CommandRoute,0,sizeof(c.Trace.CommandRoute));
    assert(CamCdmRoute(&c)==STATUS_SUCCESS&&smmuWrites==2);
    assert(c.Trace.CommandRoute[0]==2&&c.Trace.CommandRoute[1]==1&&c.Trace.CommandRoute[3]==0xe0);
    smmuRegs[0x80c/4]=0x80001860;assert(CamCdmRoute(&c)==STATUS_DEVICE_BUSY&&smmuWrites==2);
    smmuRegs[0x80c/4]=0;smmuRegs[0x808/4]=0x80011860;
    assert(CamCdmRoute(&c)==STATUS_DEVICE_BUSY&&smmuWrites==2);
    smmuRegs[0x808/4]=0x80001860;smmuRegs[0x81000/4]|=1;
    assert(CamCdmRoute(&c)==STATUS_NOT_SUPPORTED&&smmuWrites==2);
    smmuRegs[0xc08/4]=0x10000;
    assert(CamCdmRoute(&c)==STATUS_SUCCESS&&smmuWrites==2);
    smmuWrites=0;
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0x0300006e,83)==STATUS_DEVICE_BUSY&&!smmuWrites);
    smmuRegs[0x808/4]=0;smmuRegs[0xc08/4]=1;
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0x0300006e,83)==STATUS_SUCCESS&&!smmuWrites);
    assert(smmuRegs[0xc08/4]==1);
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0,83)==STATUS_SUCCESS&&smmuWrites==1);
    assert(smmuRegs[0xc08/4]==0);
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0x30000,83)==STATUS_SUCCESS&&smmuWrites==1);
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0x03000052,83)==STATUS_SUCCESS&&smmuWrites==2);
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0x03000053,83)==STATUS_SUCCESS&&smmuWrites==2);
    assert(CamSmmuRestoreControl((PUCHAR)smmuRegs,0xc08,0,0)==STATUS_INVALID_PARAMETER&&smmuWrites==2);
}

static VOID FrameRouteTests(void) {
    CAM c={0};c.Base[CamSmmu]=(PUCHAR)smmuRegs;
    memset(smmuRegs,0,sizeof(smmuRegs));smmuWrites=0;checkFrameStores=TRUE;
    smmuRegs[0x20/4]=0x4c017e84;smmuRegs[0x24/4]=0x60000053;
    smmuRegs[0x814/4]=0x80001c00;smmuRegs[0xc14/4]=0;
    smmuRegs[0x1000/4]=0x1f000;smmuRegs[0x80000/4]=0xa9f00e0;smmuRegs[0x80058/4]=0x400;
    assert(CamFrameRouteBind(&c)==STATUS_NOT_SUPPORTED&&!smmuWrites&&!c.FrameRouteOwned);
    smmuRegs[0x804/4]=0x80000040;smmuRegs[0xc04/4]=1;
    smmuRegs[0x1004/4]=0x1f000;smmuRegs[0x81000/4]=0xe0;smmuRegs[0x81058/4]=0x400;
    c.DmaActive=TRUE;assert(CamFrameRouteBind(&c)==STATUS_DEVICE_BUSY&&!smmuWrites);c.DmaActive=FALSE;
    smmuRegs[0x81058/4]=2;assert(CamFrameRouteBind(&c)==STATUS_NOT_SUPPORTED&&!smmuWrites);smmuRegs[0x81058/4]=0x400;
    assert(CamFrameRouteBind(&c)==STATUS_SUCCESS&&c.FrameRouteOwned&&smmuWrites==3);
    assert(smmuRegs[0x814/4]==0x80001c00&&smmuRegs[0xc14/4]==1&&c.Trace.SmmuState[7]==1);
    c.DmaActive=TRUE;assert(CamFrameRouteRestore(&c)==STATUS_DEVICE_BUSY&&smmuWrites==3);c.DmaActive=FALSE;
    smmuRegs[0xc14/4]=2;assert(CamFrameRouteRestore(&c)==STATUS_DEVICE_BUSY&&smmuWrites==3);smmuRegs[0xc14/4]=1;
    assert(CamFrameRouteRestore(&c)==STATUS_SUCCESS&&!c.FrameRouteOwned&&smmuWrites==6);
    assert(smmuRegs[0x814/4]==0x80001c00&&smmuRegs[0xc14/4]==0);
    smmuRegs[0xc14/4]=0x0300006e;assert(CamFrameRouteBind(&c)==STATUS_NOT_SUPPORTED&&smmuWrites==6);
    checkFrameStores=FALSE;
}

static VOID Fresh(CAM *c) {
    memset(c,0,sizeof(*c));memset(regs,0,sizeof(regs));memset(words,0,sizeof(words));
    wordCount=fifoCount=fifoIndex=0;clockValue=0;nack=timeout=preempt=rcgStuck=FALSE;rcgActive=csidActive=0;regs[0]=0x10000000;
}

static VOID TbuTests(void) {
    ULONG payload[6]={0,0,0,0,0xdeadbeef,0xdeadbeef};CamScmSmartEncode(payload);
    assert(CAM_SCM_SMART_BYTES==4*sizeof(ULONG)&&payload[4]==0xdeadbeef&&payload[5]==0xdeadbeef);
    for(ULONG i=0;i<2;i++)assert((payload[i*2]>>28)==1&&(payload[i*2]&0x0fff0000)==0);
    assert((payload[0]&0xffff)==0x5230&&(payload[2]&0xffff)==0x5234);
    assert(payload[1]==0x65555544&&payload[3]==0x66666666);
    ULONG rdi[12]={0};rdi[10]=rdi[11]=0xdeadbeef;CamScmRdiEncode(rdi);
    assert(CAM_SCM_RDI_BYTES==10*sizeof(ULONG)&&rdi[10]==0xdeadbeef&&rdi[11]==0xdeadbeef);
    const ULONG offsets[]={0x5230,0x5234,0x5238,0x5240,0x5248};
    for(ULONG i=0;i<5;i++)assert((rdi[i*2]&0xffff)==offsets[i]&&(rdi[i*2]>>16)==0x1000);
    assert(rdi[5]==0x1e40&&rdi[7]==0xffffff00&&rdi[9]==15);
    ULONG cdm[4]={0,0,0xdeadbeef,0xdeadbeef};CamScmCdmEncode(cdm);
    assert(CAM_SCM_CDM_BYTES==2*sizeof(ULONG)&&cdm[0]==0x10004c38&&cdm[1]==3);
    assert(cdm[2]==0xdeadbeef&&cdm[3]==0xdeadbeef);
    for(ULONG i=0;i<3;i++)for(ULONG bit=0;bit<32;bit++) {
        ULONG original=1u<<bit,value=CamMnocQosValue(i,original);
        assert((value&CAM_MNOC_QOS_MASK)==CamMnocQosValues[i]);
        assert((value&~CAM_MNOC_QOS_MASK)==(original&~CAM_MNOC_QOS_MASK));
        ULONG current=~original,restored=CamMnocQosRestore(current,original);
        assert((restored&CAM_MNOC_QOS_MASK)==(original&CAM_MNOC_QOS_MASK));
        assert((restored&~CAM_MNOC_QOS_MASK)==(current&~CAM_MNOC_QOS_MASK));
    }
    ULONGLONG sample[8];
    for(ULONG access=0;access<2;access++)for(ULONG mode=0;mode<7;mode++) {
        memset(tbuRegs,0,sizeof(tbuRegs));tbuMode=mode;tbuWrites=0;tbuDone=0;clockValue=0;
        tbuRegs[0x430/8]=0x15;tbuRegs[0x438/8]=0x27;tbuRegs[0x440/8]=0x39;
        NTSTATUS status=access?CamTbuTranslateAccess((PUCHAR)tbuRegs,0x1c00,0x90123000,0x409,sample):
            CamTbuTranslatePage((PUCHAR)tbuRegs,0x1860,0x90123000,sample);
        if(!mode)assert(status==STATUS_SUCCESS&&sample[5]==0x90123000000ULL);
        if(mode==1||mode==2)assert(status==STATUS_IO_TIMEOUT&&clockValue>=1000);
        if(mode==3||mode==4)assert(status==STATUS_DATA_ERROR);
        if(mode==5||mode==6)assert(status==STATUS_DEVICE_BUSY);
        if(mode!=6) {
            assert(!tbuRegs[0x410/8]&&!tbuRegs[0x448/8]);
            assert(tbuRegs[0x430/8]==0x15&&tbuRegs[0x438/8]==0x27&&tbuRegs[0x440/8]==0x39);
        } else assert(sample[6]==1&&sample[7]==1&&tbuRegs[0x410/8]==1);
    }
    tbuMode=0;tbuDone=0;tbuWrites=0;memset(tbuRegs,0,sizeof(tbuRegs));
    assert(CamTbuTranslatePage((PUCHAR)tbuRegs,0x1c00,0x90123000,sample)==STATUS_INVALID_PARAMETER&&!tbuWrites);
    assert(CamTbuTranslateAccess((PUCHAR)tbuRegs,0x1860,0x90123000,0x409,sample)==STATUS_INVALID_PARAMETER&&!tbuWrites);
    assert(CamTbuTranslateAccess((PUCHAR)tbuRegs,0x1c00,0x90123000,0x408,sample)==STATUS_INVALID_PARAMETER&&!tbuWrites);
    assert(CamTbuTranslatePage((PUCHAR)tbuRegs,0x1860,0x90123001,sample)==STATUS_INVALID_PARAMETER&&!tbuWrites);
    tbuRegs[0x450/8]=1;
    assert(CamTbuTranslatePage((PUCHAR)tbuRegs,0x1860,0x90123000,sample)==STATUS_DEVICE_BUSY&&!tbuWrites);
    tbuRegs[0x450/8]=0;tbuRegs[0xc10/8]=1;
    assert(CamTbuTranslatePage((PUCHAR)tbuRegs,0x1860,0x90123000,sample)==STATUS_DEVICE_BUSY&&!tbuWrites);
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
    for(ULONG count=1;count<=8;count++) {
        Fresh(&c);for(ULONG i=0;i<count;i++)data[i]=(UCHAR)(0x90+i);
        assert(CamCciWriteByte(&c,0x0c,0x84,data,count)==STATUS_SUCCESS);
        UCHAR *packet=(UCHAR *)(words+2);
        assert((packet[0]>>4)==count+1&&packet[1]==0x84&&memcmp(packet+2,data,count)==0);
        assert(words[wordCount-2]==7&&words[wordCount-1]==0x308);
    }
    Fresh(&c);memset(data,0,sizeof(data));
    assert(CamCciReadByte(&c,0x0c,0x03,data,2)==STATUS_SUCCESS&&words[2]==0x31b);
    assert(data[0]==0xa5&&data[1]==0xa4&&data[2]==0);
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

static ULONGLONG WalkBound(ULONGLONG *tables,ULONGLONG root,ULONGLONG address,ULONG bytes) {
    ULONG shifts[]={30,21,12};ULONGLONG at=root;
    for(ULONG i=0;i<3;i++) {
        assert(at>=root&&at<root+bytes);
        ULONGLONG value=tables[(at-root)/8+((address>>shifts[i])&511)];
        if((value&3)!=3)return UINT64_MAX;
        at=value&0x0000fffffffff000ULL;
    }
    return at|(address&4095);
}

static ULONGLONG Walk(ULONGLONG *tables,ULONGLONG root,ULONGLONG address) {
    return WalkBound(tables,root,address,40960);
}

static VOID FrameMapTableTests(void) {
    ULONGLONG *table=malloc(CAM_FRAME_MAP_TABLE_BYTES);assert(table);
    const ULONGLONG root=0x200000,physical=0xbc0000000ULL;
    assert(CamFrameMapTable(table,root,physical,QCAM_ARENA_BYTES));
    for(ULONG page=0;page<QCAM_ARENA_BYTES/4096;page++) {
        ULONGLONG va=CAM_FRAME_MAP_IOVA+(ULONGLONG)page*4096;
        for(ULONG offset=0;offset<4096;offset+=256)
            assert(WalkBound(table,root,va+offset,CAM_FRAME_MAP_TABLE_BYTES)==physical+(ULONGLONG)page*4096+offset);
    }
    assert(WalkBound(table,root,CAM_FRAME_MAP_IOVA-1,CAM_FRAME_MAP_TABLE_BYTES)==UINT64_MAX);
    assert(WalkBound(table,root,CAM_FRAME_MAP_IOVA+QCAM_ARENA_BYTES,CAM_FRAME_MAP_TABLE_BYTES)==UINT64_MAX);
    assert(!CamFrameMapTable(table,root+1,physical,QCAM_ARENA_BYTES));
    assert(!CamFrameMapTable(table,root,physical+1,QCAM_ARENA_BYTES));
    assert(!CamFrameMapTable(table,root,physical,16385u*4096));
    assert(!CamFrameMapTable(table,0xfffff000,physical,QCAM_ARENA_BYTES));
    assert(!CamFrameMapTable(table,root,0xffffff000ULL,QCAM_ARENA_BYTES));
    free(table);
}

static VOID DmaTests(void) {
    CAM ready={0};memset(regs,0,sizeof(regs));
    assert(!CamDiagnosticDmaReady(&ready));ready.ReceiverVerified=TRUE;
    assert(!CamDiagnosticDmaReady(&ready));
    regs[0x11300/4]=1;regs[0x112e8/4]=0;regs[0x26014/4]=0x08200001;
    regs[0x228/4]=0xe0000190;regs[0x11350/4]=regs[0x110f0/4]=0x10000;
    regs[0x2a8c/4]=regs[0x32cc/4]=regs[0x34dc/4]=1;
    assert(CamDiagnosticDmaReady(&ready));
    const ULONG disabled[]={0x11300,0x26014,0x228,0x11350,0x110f0,0x2a8c,0x32cc,0x34dc};
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
    ColorFrame(raw,200,400,240);assert(CamNv12(raw,QCAM_FRAME_BYTES,image,QCAM_VIDEO_BYTES));
    for(ULONG p=QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;p<QCAM_VIDEO_BYTES;p++)assert(image[p]>=127&&image[p]<=129);
    ColorFrame(raw,72,80,74);assert(CamNv12(raw,QCAM_FRAME_BYTES,image,QCAM_VIDEO_BYTES));
    for(ULONG p=QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;p<QCAM_VIDEO_BYTES;p++)assert(image[p]>=127&&image[p]<=129);
    ULONG gains[3]={512,256,384};ColorFrame(raw,64,64,64);CamWhiteBalance(raw,0,gains);
    assert(gains[0]==512&&gains[1]==256&&gains[2]==384);
    UCHAR palette[3][1024];USHORT workspace[6144];
    for(ULONG c=0;c<3;c++)for(ULONG v=0;v<1024;v++)palette[c][v]=(UCHAR)CamColor(v);
    ColorFrame(raw,64,64,1023);CamNv12RowsBayer(raw,0,image,palette,workspace,0,768,NULL,NULL,TRUE);
    for(ULONG p=0;p<QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;p++)assert(image[p]==82);
    for(ULONG p=QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;p<QCAM_VIDEO_BYTES;p+=2)assert(image[p]==90&&image[p+1]==240);
    ColorFrame(raw,100,200,300);ULONG bggr[3]={256,256,256},rggb[3]={256,256,256};
    CamWhiteBalanceBayer(raw,0,QCAM_WIDTH,QCAM_HEIGHT,TRUE,bggr);CamWhiteBalance(raw,0,rggb);
    assert(bggr[0]==rggb[2]&&bggr[2]==rggb[0]&&bggr[1]==256);
    for(ULONG value=64;value<1023;value++)assert(CamColor(value)<=CamColor(value+1));
    assert(!CamNv12(raw,QCAM_FRAME_BYTES-1,image,QCAM_VIDEO_BYTES));
    assert(!CamNv12(raw,QCAM_FRAME_BYTES,image,QCAM_VIDEO_BYTES-1));
    UCHAR *dual=malloc(QCAM_FRAME_BYTES*4+256),*expected=malloc(QCAM_VIDEO_BYTES);assert(dual&&expected);
    ColorFrame(raw,240,350,280);assert(CamNv12(raw,QCAM_FRAME_BYTES,expected,QCAM_VIDEO_BYTES));
    memset(dual,0xa5,QCAM_FRAME_BYTES*4+256);
    for(ULONG offset=0;offset<QCAM_FRAME_BYTES;offset++)dual[offset+((offset&256)?QCAM_FRAME_BYTES*2+256:0)]=raw[offset];
    assert(CamNv12Dual(dual,QCAM_FRAME_BYTES,QCAM_FRAME_BYTES*2+256,image,QCAM_VIDEO_BYTES));
    assert(!memcmp(image,expected,QCAM_VIDEO_BYTES));free(expected);free(dual);
    free(image);free(raw);
}

static VOID ExposureTests(void) {
    UCHAR *raw=malloc(QCAM_FRAME_BYTES),*dual=malloc(QCAM_FRAME_BYTES*4+256);assert(raw&&dual);
    ULONG gains[3]={256,256,256};
    ColorFrame(raw,320,320,320);assert(CamExposureTarget(raw,0,gains,QCAM_ZOOM_MIN,1000)==1000);
    ColorFrame(raw,64,64,64);assert(CamExposureTarget(raw,0,gains,QCAM_ZOOM_MIN,1000)==2000);
    ColorFrame(raw,1023,1023,1023);assert(CamExposureTarget(raw,0,gains,QCAM_ZOOM_MIN,1000)==500);
    ColorFrame(raw,864,192,192);
    ULONGLONG normal=CamExposureTarget(raw,0,gains,QCAM_ZOOM_MIN,1000);assert(normal>800&&normal<900);
    gains[0]=512;assert(CamExposureTarget(raw,0,gains,QCAM_ZOOM_MIN,1000)==500);gains[0]=256;
    ColorFrame(raw,80,80,80);
    for(ULONG y=QCAM_HEIGHT/4;y<QCAM_HEIGHT*3/4;y++)for(ULONG x=QCAM_WIDTH/4;x<QCAM_WIDTH*3/4;x+=4) {
        UCHAR *p=raw+y*QCAM_STRIDE+x/4*5;for(ULONG i=0;i<4;i++)p[i]=216;p[4]=0;
    }
    normal=CamExposureTarget(raw,0,gains,QCAM_ZOOM_MIN,1000);assert(normal>800&&normal<900);
    assert(CamExposureTarget(raw,0,gains,QCAM_ZOOM_MAX,1000)==500);
    memset(dual,0xa5,QCAM_FRAME_BYTES*4+256);
    for(ULONG offset=0;offset<QCAM_FRAME_BYTES;offset++)dual[offset+((offset&256)?QCAM_FRAME_BYTES*2+256:0)]=raw[offset];
    assert(CamExposureTarget(dual,QCAM_FRAME_BYTES*2+256,gains,QCAM_ZOOM_MIN,1000)==normal);
    assert(CamExposureTarget(dual,QCAM_FRAME_BYTES*2+256,gains,QCAM_ZOOM_MAX,1000)==500);
    free(dual);free(raw);
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

static struct { ULONG Calls,WakePolls,Resumes,Acks,InterruptResumes; LONG QueryStatus,QosStatus; BOOLEAN Supported,WaitStuck,WaitError,WaitForeign,WaitSlow; } ScmMock;
static NTSTATUS scmAuditError;
static ULONG scmAuditPhase;

NTSTATUS CamScmAudit(CAM *c,ULONG phase,const ULONGLONG *registers,NTSTATUS result) {
    (void)registers;(void)result;scmAuditPhase=phase;
    c->ScmAuditSequence++;c->Trace.ScmAuditSequence=c->ScmAuditSequence;c->Trace.ScmAuditStatus=scmAuditError;
    return scmAuditError;
}

VOID CamScmSlow(ULONGLONG *registers) {
    ScmMock.Calls++;
    if(registers[0]==1) {
        assert(scmAuditPhase==4);
        assert(registers[1]==0x83&&registers[2]==0&&registers[3]==0&&registers[4]==0&&registers[5]==0&&registers[7]==0);
        assert(registers[6]==0xa6000001);ScmMock.InterruptResumes++;
        memset(registers,0,8*sizeof(*registers));
    } else if(registers[0]==0x42000601) {
        assert(registers[1]==1&&registers[2]==0x0200180a);
        for(ULONG i=3;i<8;i++)assert(registers[i]==0);
        registers[0]=(ULONG)ScmMock.QueryStatus;registers[1]=ScmMock.Supported;
    } else if(registers[0]==0x4200180a) {
        assert(scmAuditPhase==1);
        assert(registers[0]==0x4200180a&&registers[1]==0x83);
        for(ULONG i=2;i<8;i++)assert(registers[i]==0);
        registers[0]=(ULONG)ScmMock.QosStatus;
        if(ScmMock.QosStatus==1) {
            for(ULONG i=1;i<6;i++)registers[i]=0xbad00000+i;
            registers[6]=0xa6000001;
        }
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

VOID CamScmOnce(ULONGLONG *registers) { CamScmSlow(registers); }

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
    assert(c.StaticQosReady&&CamEnsureStaticQos(&c)==STATUS_SUCCESS&&ScmMock.Calls==3);
    c.Powered=FALSE;
    assert(CamEnsureStaticQos(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==3);
    c.Powered=TRUE;c.CdmPending=TRUE;
    assert(CamEnsureStaticQos(&c)==STATUS_DEVICE_BUSY&&ScmMock.Calls==3);
    c.CdmPending=FALSE;
    assert(c.Diag.Step==151&&c.Diag.Address==0x4200180a&&c.Diag.Count==3);
    assert(CamScmQos(&c)==STATUS_SUCCESS&&ScmMock.Calls==4);
    ScmMock.QosStatus=-1;
    assert(CamScmQos(&c)==STATUS_IO_DEVICE_ERROR&&c.Trace.ScmReturn[0]==MAXULONG);
    assert(!c.StaticQosReady);
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
    regs[0xf0/4]=0;regs[0x228/4]=400;regs[0x2a8c/4]=1;regs[0x32cc/4]=1;regs[0x34dc/4]=1;
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY);
    regs[0x32cc/4]=0;
    assert(CamScmQos(&c)==STATUS_DEVICE_NOT_READY);
    c.ReceiverVerified=TRUE;regs[0xf0/4]=0;regs[0xf8/4]=0;
    regs[0x11300/4]=1;regs[0x11310/4]=1;regs[0x112e8/4]=0;regs[0x26014/4]=1;
    regs[0x11350/4]=0x10000;regs[0x110f0/4]=0x10000;
    regs[0x228/4]=0xe0000190;regs[0x220/4]=0xe0000190;regs[0x224/4]=0xe0000190;
    const ULONG domains[]={2,6,7};
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
    assert(CamScmQosDiagnostic(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    assert(CamScmQosRaw(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    regs[0xf0/4]=1;assert(CamScmQosRaw(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    regs[0xf0/4]=0;regs[0xf8/4]=1;assert(CamScmQosRaw(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    regs[0xf0/4]=1;
    assert(CamScmQosDiagnostic(&c)==STATUS_SUCCESS&&!c.ScmPending);
    ScmMock.WaitSlow=FALSE;ScmMock.QosStatus=2;ScmMock.WakePolls=0;
    assert(CamScmWaitqProbe(&c)==STATUS_SUCCESS&&c.ScmFastWaitq);
    calls=ScmMock.Calls;ULONG resumes=ScmMock.Resumes;
    assert(CamScmQosRaw(&c)==STATUS_DEVICE_BUSY&&c.ScmPending&&c.Trace.ScmReturn[0]==2);
    assert(ScmMock.Calls==calls+1&&ScmMock.Resumes==resumes&&ScmMock.WakePolls==0);
    assert(CamScmWaitqContext(&c)==STATUS_SUCCESS&&c.ScmPending&&c.Trace.ScmReturn[0]==2);
    assert(ScmMock.Resumes==resumes&&ScmMock.WakePolls==1);
    assert(c.ScmResumeValid&&CamScmContinue(&c)==STATUS_SUCCESS&&!c.ScmPending&&!c.ScmResumeValid);
    assert(ScmMock.Resumes==resumes+1);
    calls=ScmMock.Calls;assert(CamScmContinue(&c)==STATUS_DEVICE_NOT_READY&&ScmMock.Calls==calls);
    scmAuditError=STATUS_IO_DEVICE_ERROR;
    assert(CamScmQosRaw(&c)==STATUS_IO_DEVICE_ERROR&&ScmMock.Calls==calls&&!c.ScmPending&&!c.ScmResumeValid);
    scmAuditError=STATUS_SUCCESS;
    ScmMock.QosStatus=1;
    assert(CamScmQosRaw(&c)==STATUS_DEVICE_BUSY&&c.ScmPending&&c.ScmResumeValid);
    assert(c.Trace.ScmReturn[1]!=0x83&&c.ScmResumeArgs[1]==0x83&&c.ScmResumeArgs[0]==0x4200180a);
    c.DmaActive=TRUE;calls=ScmMock.Calls;assert(CamScmContinue(&c)==STATUS_DEVICE_BUSY&&ScmMock.Calls==calls);c.DmaActive=FALSE;
    scmAuditError=STATUS_IO_DEVICE_ERROR;
    assert(CamScmContinue(&c)==STATUS_IO_DEVICE_ERROR&&ScmMock.Calls==calls&&c.ScmPending&&c.ScmResumeValid);
    scmAuditError=STATUS_SUCCESS;
    assert(CamScmContinue(&c)==STATUS_SUCCESS&&!c.ScmPending&&!c.ScmResumeValid&&ScmMock.InterruptResumes==1);
}

static VOID DmaIdleTests(void) {
    RtlZeroMemory(regs,sizeof(regs));RtlZeroMemory(tbuRegs,sizeof(tbuRegs));tbuDone=0;
    PUCHAR tbu=(PUCHAR)tbuRegs;
    regs[0x5220/4]=0x90001;
    assert(CamDmaStopped(NULL,NULL,NULL,tbu,tbu));
    assert(CamDmaDrain(NULL,NULL,NULL,tbu,tbu,5)==STATUS_SUCCESS);
    const ULONG busyRegs[]={0xf0,0xf8,0x2400,0x2500,0x2600,0x2700,0xe04,0x1004,0x1204,0x1404};
    for(ULONG i=0;i<RTL_NUMBER_OF(busyRegs);i++) {
        regs[busyRegs[i]/4]=1;
        assert(!CamDmaStopped(NULL,NULL,NULL,tbu,tbu));
        assert(CamDmaDrain(NULL,NULL,NULL,tbu,tbu,5)==STATUS_IO_TIMEOUT);
        assert(regs[busyRegs[i]/4]==1);regs[busyRegs[i]/4]=0;
    }
    for(ULONG port=0;port<2;port++) {
        ULONG slot=(0xc10+port*0x10)/8;tbuRegs[slot]=1ULL<<40;
        assert(CamDmaDrain(NULL,NULL,NULL,tbu,tbu,5)==STATUS_IO_TIMEOUT);
        assert(tbuRegs[slot]==(1ULL<<40));tbuRegs[slot]=0;
    }
    assert(CamDmaStopped(NULL,NULL,NULL,tbu,tbu));
    assert(CamRdiPairReady(0xf0000,0xc0000,100,100,120,120,80));
    assert(!CamRdiPairReady(0x40000,0xc0000,100,100,120,120,80));
    assert(!CamRdiPairReady(0xc0000,0xc0000,100,100,120,121,80));
    assert(!CamRdiPairReady(0xc0000,0xc0000,130,130,120,120,80));
    assert(!CamRdiPairReady(0xc0000,0xc0000,100,110,120,120,80));
    assert(!CamRdiPairReady(0xc0000,0xc0000,100,100,120,120,120));
    assert(!CamRdiPairReady(0xc0000,0xc0000,0,0,0,0,0));
    assert(!CamRdiFrameComplete(0,0xe11e18,0));
    assert(!CamRdiFrameComplete(QCAM_HEIGHT-1,0xe11e18,0));
    assert(!CamRdiFrameComplete(QCAM_HEIGHT,0x800010,0));
    assert(!CamRdiFrameComplete(QCAM_HEIGHT,0xe11e18,0));
    assert(CamRdiFrameComplete(QCAM_HEIGHT,0xe11e18,0x10000));
    assert(CamRdiFrameComplete(QCAM_HEIGHT,0x10,0x10000));
    for(ULONG bit=0;bit<32;bit++)if(CAM_RDI_FATAL_IRQ&(1u<<bit))
        assert(!CamRdiFrameComplete(QCAM_HEIGHT,8|(1u<<bit),0x10000));
}

static VOID FrameClockTests(void) {
    CAM_FRAME_CLOCK clock={0};QCAM_FRAME_TIME frame={sizeof(frame),QCAM_ABI,QCAM_SENSOR_CLOCK,1,0,0,0xfffff000ULL,0xfffff000ULL+264000,10200000};
    LONGLONG time=0;assert(CamFrameClock(&clock,&frame,10000000,50000000,&time));assert(time==50062500);
    frame.Sof+=320000;frame.Eof+=320000;frame.Captured=10750000;frame.Sequence++;
    LONGLONG previous=time;assert(CamFrameClock(&clock,&frame,10000000,50000000,&time));assert(time-previous==166666);
    frame.Sof+=640000;frame.Eof+=640000;frame.Captured=10900000;frame.Sequence++;frame.Dropped++;
    previous=time;assert(CamFrameClock(&clock,&frame,10000000,50000000,&time));assert(time-previous==333334);
    assert(!CamFrameClock(&clock,&frame,10000000,50000000,&time));
    frame.ClockHz=0;assert(!CamFrameClock(&clock,&frame,10000000,50000000,&time));
    frame.ClockHz=QCAM_SENSOR_CLOCK;frame.Eof=frame.Sof-1;assert(!CamFrameClock(&clock,&frame,10000000,50000000,&time));
    assert(CamFrameTicks(19200000ULL*1000000000+320000,QCAM_SENSOR_CLOCK)==10000000000000000ULL+166666);
}

static VOID CameraGeometryTests(void) {
    assert(CamRdiFrameCompleteHeight(1200,1200,0,0x10000));
    assert(!CamRdiFrameCompleteHeight(1199,1200,0,0x10000));
    CAM_RAW_BLOCK blocks[128];ULONG rows[768];
    for(ULONG zoom=QCAM_ZOOM_MIN;zoom<=QCAM_ZOOM_MAX;zoom+=QCAM_ZOOM_MIN) {
        CamRawZoomGeometry(zoom,1600,1200,blocks,rows);
        for(ULONG y=0;y<768;y++)assert(rows[y]/QCAM_STRIDE+1<1200);
        for(ULONG x=0;x<128;x++)for(ULONG j=0;j<8;j++)assert(blocks[x].Offset+blocks[x].Low[j]<2000);
    }
}

int main(void) {CameraGeometryTests();FrameClockTests();TbuTests();MeasureTests();CdmTests();FrameRouteTests();WaitTests();CrmTests();GdscTests();RcgTests();CciTests();DmaTests();FrameMapTableTests();ColorTests();ExposureTests();PowerTests();ScmTests();DmaIdleTests();puts("DMA idle gating, RDI frame boundaries, TBU translation, clock measurement, frame and CDM routing, completion, wait preemption, CRM acknowledgment, GDSC sequencing, CCI, C-PHY, DMA, NV12, exposure highlights and crop metering, RPMh and powered secure QoS passed");return 0;}
