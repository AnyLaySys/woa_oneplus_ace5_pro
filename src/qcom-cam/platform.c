#include "cam.h"
#include "wait.h"
#include "rcg-restore.h"
#include "smmu-restore.h"
#include "mnoc-qos.h"
#include "../platform/lock.h"

NTSTATUS CamRailsOn(CAM *c);
NTSTATUS CamRailsOff(CAM *c);

VOID CamDelay(ULONG us) {
    if(us<=50){KeStallExecutionProcessor(us);return;}
    LARGE_INTEGER delay;delay.QuadPart=-(LONGLONG)us*10;
    KeDelayExecutionThread(KernelMode,FALSE,&delay);
}

NTSTATUS CamWait(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us) {
    return CamPoll(base,offset,mask,value,us);
}

NTSTATUS CamSave(CAM *c,PUCHAR base,ULONG offset,ULONG value) {
    ULONG i;
    for(i=0;i<c->SavedCount;i++)if(c->Saved[i].Base==base&&c->Saved[i].Offset==offset)break;
    if(i==c->SavedCount) {
        if(i>=RTL_NUMBER_OF(c->Saved))return STATUS_INSUFFICIENT_RESOURCES;
        c->Saved[i].Base=base;c->Saved[i].Offset=offset;c->Saved[i].Original=CamRead(base,offset);c->SavedCount++;
        c->Saved[i].Rcg=FALSE;
    }
    CamWrite(base,offset,value);return STATUS_SUCCESS;
}

static NTSTATUS Spmi(CAM *c,PUCHAR base,UCHAR reg,UCHAR *value,BOOLEAN write) {
    if(write)CamWrite(base,0x10,*value);
    CamWrite(base,0,(write?0:0x08000000u)|((ULONG)reg<<4));
    NTSTATUS status=CamWait(base,8,1,1,10000);
    ULONG observed=CamRead(base,8);
    if(NT_SUCCESS(status)&&(observed&14))status=STATUS_IO_DEVICE_ERROR;
    if(!NT_SUCCESS(status)) {
        c->Diag.Step=write?102:101;c->Diag.Region=write?CamChannels:CamObserve;
        c->Diag.Offset=(ULONG)(base-c->Base[c->Diag.Region])+8;c->Diag.Observed=observed;
        c->Diag.Mask=1;c->Diag.Expected=1;c->Diag.Address=0x88c00u|reg;
        c->Diag.Phase=write?1:0;c->Diag.Count=1;c->Diag.Slot=c->ResetRead?(ULONG)((c->ResetRead-c->Base[CamObserve])/0x20):0;
        RtlCopyMemory(c->Diag.Resource,"pmicgpio",8);return status;
    }
    if(!write)*value=(UCHAR)CamRead(base,0x18);
    return STATUS_SUCCESS;
}

NTSTATUS CamReset(CAM *c,BOOLEAN high) {
    if(c->Sensor==1||c->Sensor==3){PUCHAR b=c->Base[CamTlmm]+(c->Sensor==1?178:7)*0x1000;CamWrite(b,4,high?2:0);return STATUS_SUCCESS;}
    if(!c->ResetWrite)return STATUS_DEVICE_NOT_READY;
    UCHAR value=high?0x80:0;
    return Spmi(c,c->ResetWrite,0x44,&value,TRUE);
}

NTSTATUS CamPmic(CAM *c,ULONG ppid,UCHAR reg,UCHAR *value,BOOLEAN write) {
    ULONG count=CamRead(c->Base[CamSpmi],4)&0x7ff;
    for(ULONG i=0;i<count&&i<1024;i++) {
        ULONG entry=CamRead(c->Base[CamSpmi],0x2000+4*i);
        if(((entry>>8)&0xfff)!=ppid)continue;
        if(write&&(CamRead(c->Base[CamOwners],4*i)&7))continue;
        return Spmi(c,c->Base[write?CamChannels:CamObserve]+i*(write?0x1000:0x20),reg,value,write);
    }
    return STATUS_ACCESS_DENIED;
}

static NTSTATUS ResetPrepare(CAM *c) {
    c->ResetRead=c->ResetWrite=NULL;
    if(c->Sensor==1||c->Sensor==3) {
        ULONG off=(c->Sensor==1?178:7)*0x1000;NTSTATUS status=CamSave(c,c->Base[CamTlmm],off+4,0);
        if(NT_SUCCESS(status))status=CamSave(c,c->Base[CamTlmm],off,(CamRead(c->Base[CamTlmm],off)&~0x3ffu)|0x1200);
        if(NT_SUCCESS(status))c->ResetOwned=TRUE;return status;
    }
    PUCHAR b=c->Base[CamSpmi];ULONG count=CamRead(b,4)&0x7ff,version=CamRead(b,0);
    if(version<0x70000000||version>=0x80000000||!count||count>1024)return STATUS_REVISION_MISMATCH;
    for(ULONG i=0;i<count;i++) {
        ULONG entry=CamRead(b,0x2000+4*i);
        if(((entry>>8)&0xfff)!=(c->Sensor==2?0x68cu:0x88cu)||(CamRead(c->Base[CamOwners],4*i)&7))continue;
        c->ResetRead=c->Base[CamObserve]+i*0x20;c->ResetWrite=c->Base[CamChannels]+i*0x1000;
    }
    if(!c->ResetWrite)return STATUS_ACCESS_DENIED;
    for(UCHAR i=0;i<7;i++) {
        NTSTATUS status=Spmi(c,c->ResetRead,(UCHAR)(0x40+i),&c->ResetSaved[i],FALSE);
        if(!NT_SUCCESS(status))return status;
    }
    c->ResetOwned=TRUE;
    static const UCHAR values[]={1,1,5,0,0,1,0x80};
    for(UCHAR i=0;i<7;i++) {
        UCHAR value=values[i];NTSTATUS status=Spmi(c,c->ResetWrite,(UCHAR)(0x40+i),&value,TRUE);
        if(!NT_SUCCESS(status))return status;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Branch(CAM *c,PUCHAR b,ULONG offset) {
    BOOLEAN shared=b==c->Base[CamGcc]||(b==c->Base[CamCc]&&
        (offset==0x113c4||offset==0x113c0||offset==0x1137c||offset==0x11398||offset==0x11360||
         (offset==0x11310&&(CamRead(b,offset)&2))));
    c->Diag.Step=130;c->Diag.Offset=offset;c->Diag.Mask=shared?1:0x80000000;c->Diag.Expected=shared?1:0;
    c->Diag.Region=b==c->Base[CamGcc]?CamGcc:b==c->Base[CamMclk]?CamMclk:CamCc;
    ULONG requested=CamRead(b,offset)|1;
    BOOLEAN forced=b==c->Base[CamGcc]&&(offset==0x26004||offset==0x26008||offset==0x2600c||offset==0x26010);
    if(forced)requested&=~2u;
    NTSTATUS status=CamSave(c,b,offset,requested);
    if(forced) {
        if(NT_SUCCESS(status))status=CamWait(b,offset,0x80000000,0,10000);
        c->Diag.Mask=0x80000000;c->Diag.Expected=0;c->Diag.Observed=CamRead(b,offset);return status;
    }
    if(b==c->Base[CamCc]&&offset==0x11360) {
        if(NT_SUCCESS(status))CamDelay(10);
        c->Diag.Mask=0;c->Diag.Expected=0;c->Diag.Observed=CamRead(b,offset);return status;
    }
    if(NT_SUCCESS(status)) {
        ULONGLONG deadline=KeQueryInterruptTime()+100000;
        do {
            ULONG value=CamRead(b,offset);
            if(shared?(value&1)!=0:!(value&0x80000000)||((value>>28)&7)==2){status=STATUS_SUCCESS;break;}
            status=STATUS_IO_TIMEOUT;CamDelay(10);
        }while(KeQueryInterruptTime()<deadline);
        ULONG value=CamRead(b,offset);
        if(shared?(value&1)!=0:!(value&0x80000000)||((value>>28)&7)==2)status=STATUS_SUCCESS;
    }
    c->Diag.Observed=CamRead(b,offset);return status;
}

NTSTATUS CamIspPixelClocksOn(CAM *c) {
    if(!c->IspFd||!c->Powered||!c->BusOn||c->DmaActive||c->RxActive||c->IspFdOwned||
       c->CdmPending||c->ScmPending||c->TbuPending)return STATUS_DEVICE_BUSY;
    /* OEM sun-camera.dtsi TFE2 requires these Bayer processing/NoC clocks
       separately from Bayer fast AHB. RAW RDI transport did not need them. */
    const ULONG offsets[]={0x1112c,0x1113c};
    PUCHAR base=c->Base[CamCc];
    for(ULONG i=0;i<RTL_NUMBER_OF(offsets);i++) {
        c->IspFd->PixelClocks[i][0]=CamRead(base,offsets[i]);
        NTSTATUS status=Branch(c,base,offsets[i]);
        ULONG observed=CamRead(base,offsets[i]);c->IspFd->PixelClocks[i][1]=observed;
        if(!NT_SUCCESS(status))return status;
        if((observed&0x80000001u)!=1)return STATUS_DEVICE_NOT_READY;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Rcg(CAM *c,PUCHAR b,ULONG offset,ULONG cfg) {
    c->Diag.Step=132;c->Diag.Offset=offset;c->Diag.Mask=1;c->Diag.Expected=0;
    c->Diag.Region=b==c->Base[CamMclk]?CamMclk:CamCc;
    NTSTATUS status=CamSave(c,b,offset+4,cfg);
    if(!NT_SUCCESS(status))return status;
    ULONG original=0;
    for(ULONG i=0;i<c->SavedCount;i++)if(c->Saved[i].Base==b&&c->Saved[i].Offset==offset+4){original=c->Saved[i].Original;break;}
    status=CamSave(c,b,offset,CamRead(b,offset)|2);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<c->SavedCount;i++)if(c->Saved[i].Base==b&&c->Saved[i].Offset==offset){c->Saved[i].Rcg=TRUE;c->Saved[i].Config=original;break;}
    CamWrite(b,offset,CamRead(b,offset)|1);
    status=CamWait(b,offset,1,0,10000);
    if(NT_SUCCESS(status)&&b==c->Base[CamCc])CamWrite(b,offset,CamRead(b,offset)&~2u);
    c->Diag.Observed=CamRead(b,offset);return status;
}

static NTSTATUS Pll(CAM *c,ULONG offset,ULONG l,ULONG alpha,ULONG user) {
    c->Diag.Step=131;c->Diag.Offset=offset;c->Diag.Mask=0x80000000;c->Diag.Expected=0x80000000;
    c->Diag.Region=CamCc;
    PUCHAR b=c->Base[CamCc];ULONG mode=CamRead(b,offset);
    if(mode&0x80000000) {
        if((CamRead(b,offset+0x10)&0xffff)!=l||CamRead(b,offset+0x14)!=alpha)return STATUS_DEVICE_BUSY;
        return STATUS_SUCCESS;
    }
    const CAM_REG regs[]={{offset+0x10,0x00440000u|l,0},{offset+0x14,alpha,0},{offset+0x18,user,0},{offset+0x1c,2,0},
        {offset+0x20,0x19660387,0},{offset+0x24,0x098060a0,0},{offset+0x28,0xb416cb20,0},
        {offset+4,0,0},{offset,mode|4,0},{offset+4,1,0}};
    for(ULONG i=0;i<RTL_NUMBER_OF(regs);i++) {
        NTSTATUS status=CamSave(c,b,regs[i].Address,regs[i].Value);if(!NT_SUCCESS(status))return status;
    }
    NTSTATUS status=CamWait(b,offset,0x80000000,0x80000000,10000);
    c->Diag.Observed=CamRead(b,offset);
    if(NT_SUCCESS(status))status=CamSave(c,b,offset+0x18,CamRead(b,offset+0x18)|7);
    if(NT_SUCCESS(status))status=CamSave(c,b,offset,CamRead(b,offset)|1);
    return status;
}

static NTSTATUS Domain(CAM *c,ULONG offset) {
    PUCHAR b=c->Base[CamCc];c->Diag.Step=133;c->Diag.Region=CamCc;
    c->Diag.Offset=offset+4;c->Diag.Mask=0x10000;c->Diag.Expected=0x10000;
    NTSTATUS status=CamSave(c,b,offset,(CamRead(b,offset)&~7u)|0x800);
    CamDelay(1);
    if(NT_SUCCESS(status))status=CamWait(b,offset+4,0x10000,0x10000,20000);
    c->Diag.Observed=CamRead(b,offset+4);return status;
}

static const ULONG MclkPllRegisters[]={0,4,0x10,0x14,0x18,0x1c,0x20,0x24,0x28,0x2c,0x30};

static NTSTATUS Mclk24(CAM *c) {
    PUCHAR b=c->Base[CamMclk];ULONG mode=CamRead(b,0),opmode=CamRead(b,4);
    if((mode&1)&&(opmode&1)&&CamRead(b,0x10)!=50)return STATUS_DEVICE_BUSY;
    for(ULONG i=0;i<RTL_NUMBER_OF(MclkPllRegisters);i++)c->MclkPllSaved[i]=CamRead(b,MclkPllRegisters[i]);
    c->MclkPllOwned=TRUE;
    if(!((mode&1)&&(opmode&1))) {
        const CAM_REG regs[]={{0,mode&~7u,0},{4,0,0},{0x10,50,0},{0x14,0,0},{0x18,0,0},
            {0x1c,0x12000000,0},{0x20,0x00890263,0},{0x24,0x1af04237,0},{0x28,0,0},{0x2c,0,0},{0x30,0,0}};
        for(ULONG i=0;i<RTL_NUMBER_OF(regs);i++)CamWrite(b,regs[i].Address,regs[i].Value);
        CamWrite(b,0,(mode&~7u)|6);CamWrite(b,4,1);
        NTSTATUS status=CamWait(b,0,0x80000000,0x80000000,10000);if(!NT_SUCCESS(status))return status;
    }
    CamWrite(b,0x14,CamRead(b,0x14)|7);CamWrite(b,0,CamRead(b,0)|1);
    ULONG rcg=0x4054;NTSTATUS status=CamSave(c,b,rcg+8,(CamRead(b,rcg+8)&~255u)|1);
    if(NT_SUCCESS(status))status=CamSave(c,b,rcg+12,(CamRead(b,rcg+12)&~255u)|0xfc);
    if(NT_SUCCESS(status))status=CamSave(c,b,rcg+16,(CamRead(b,rcg+16)&~255u)|0xfb);
    if(NT_SUCCESS(status))status=Rcg(c,b,rcg,0x2313);
    for(ULONG i=0;i<RTL_NUMBER_OF(MclkPllRegisters);i++)c->Trace.Mclk[i]=CamRead(b,MclkPllRegisters[i]);c->Trace.Mclk[11]=CamRead(b,rcg+4);
    return status;
}

static VOID MclkRestore(CAM *c) {
    if(!c->MclkPllOwned)return;PUCHAR b=c->Base[CamMclk];
    if(!((c->MclkPllSaved[0]&1)&&(c->MclkPllSaved[1]&1))) {
        CamWrite(b,0,CamRead(b,0)&~1u);CamWrite(b,4,0);
        for(ULONG i=RTL_NUMBER_OF(MclkPllRegisters);i>2;i--)CamWrite(b,MclkPllRegisters[i-1],c->MclkPllSaved[i-1]);
        CamWrite(b,4,c->MclkPllSaved[1]);
    } else CamWrite(b,0x14,c->MclkPllSaved[3]);
    CamWrite(b,0,c->MclkPllSaved[0]);c->MclkPllOwned=FALSE;
}

NTSTATUS CamPowerOn(CAM *c) {
    if(c->ScmPending||c->CdmPending||c->TbuPending||c->FrameRouteOwned||c->FrameContextOwned||c->IspFdOwned)return STATUS_DEVICE_BUSY;
    if(c->Powered)return STATUS_SUCCESS;
    if(c->PowerState||c->Faulted)return STATUS_DEVICE_HARDWARE_ERROR;
    RtlZeroMemory(&c->Diag,sizeof(c->Diag));c->Diag.Step=100;
    NTSTATUS status=ResetPrepare(c);
    if(!NT_SUCCESS(status))return status;
    if(c->ProbeLimit==1)return STATUS_SUCCESS;
    c->Info.Stage=1;status=CamRailsOn(c);
    if(!NT_SUCCESS(status))return status;
    if(c->ProbeLimit==2)return STATUS_SUCCESS;
    PUCHAR gcc=c->Base[CamGcc],cc=c->Base[CamCc],mclk=c->Base[CamMclk],tlmm=c->Base[CamTlmm];
    const ULONG gates[]={0xa0004,0x26004,0x26034,0x26014,0x26024,0x2600c,0x26010,0x26008};
    for(ULONG i=0;i<RTL_NUMBER_OF(gates)&&NT_SUCCESS(status);i++)status=Branch(c,gcc,gates[i]);
    if(NT_SUCCESS(status)&&c->ProbeLimit==3)return STATUS_SUCCESS;
    const ULONG critical[]={0x113c4,0x113c0,0x1137c,0x11398};
    for(ULONG i=0;i<RTL_NUMBER_OF(critical)&&NT_SUCCESS(status);i++)status=Branch(c,cc,critical[i]);
    if(NT_SUCCESS(status))status=Domain(c,0x1134c);
    if(NT_SUCCESS(status)&&c->ProbeLimit==4)return STATUS_SUCCESS;
    if(NT_SUCCESS(status))status=Pll(c,0,0x3e,0x8000,0x8400);
    if(NT_SUCCESS(status)&&c->ProbeLimit==5)return STATUS_SUCCESS;
    const ULONG rcgs[]={c->Sensor==2?0x11288:0x1126c,c->Sensor==1?0x10024:c->Sensor==2?0x10064:c->Sensor==3?0x10084:0x100a4,0x100dc,0x10100,0x11364};
    const ULONG cfgs[]={0x10021f,0x100105,0x100203,0x10020e,0x100001};
    for(ULONG i=0;i<RTL_NUMBER_OF(rcgs)&&NT_SUCCESS(status);i++)status=Rcg(c,cc,rcgs[i],cfgs[i]);
    if(NT_SUCCESS(status)&&c->ProbeLimit==6)return STATUS_SUCCESS;
    if(NT_SUCCESS(status))status=Domain(c,0x110ec);
    if(NT_SUCCESS(status)&&c->ProbeLimit==7)return STATUS_SUCCESS;
    if(NT_SUCCESS(status))status=CamCrmOn(c);
    if(NT_SUCCESS(status)) {
        c->Trace.DebugXo[0]=CamRead(cc,0x11348);c->Trace.DebugXo[1]=c->Trace.DebugXo[0]|1;
        status=Branch(c,cc,0x11348);c->Trace.DebugXo[2]=CamRead(cc,0x11348);
    }
    const ULONG clocks[]={0x113ac,0x1139c,0x11360,0x11320,0x11324,c->Sensor==2?0x112a0:0x11284,c->Sensor==1?0x1003c:c->Sensor==2?0x1007c:c->Sensor==3?0x1009c:0x100bc,c->Sensor==1?0x10040:c->Sensor==2?0x10080:c->Sensor==3?0x100a0:0x100c0,0x10020,0x11148,0x1114c};
    for(ULONG i=0;i<RTL_NUMBER_OF(clocks)&&NT_SUCCESS(status);i++)status=Branch(c,cc,clocks[i]);
    if(NT_SUCCESS(status)&&c->ProbeLimit==8)return STATUS_SUCCESS;
    if(NT_SUCCESS(status)&&c->Sensor==2)status=Mclk24(c);
    if(NT_SUCCESS(status)&&c->Sensor!=2)status=Rcg(c,mclk,0x4000+(c->Sensor==1?0:c->Sensor==2?3:c->Sensor==3?4:1)*0x1c,1);
    if(NT_SUCCESS(status))status=Branch(c,mclk,0x4018+(c->Sensor==1?0:c->Sensor==2?3:c->Sensor==3?4:1)*0x1c);
    const ULONG pins[]={c->Sensor==1?89:c->Sensor==2?92:c->Sensor==3?93:90,c->Sensor==1?113:c->Sensor==2?117:115,c->Sensor==1?114:c->Sensor==2?118:116};
    for(ULONG i=0;i<RTL_NUMBER_OF(pins)&&NT_SUCCESS(status);i++) {
        ULONG off=pins[i]*0x1000;status=CamSave(c,tlmm,off,(CamRead(tlmm,off)&~0x3ffu)|4u|(i?0:0x200u));
    }
    if(!NT_SUCCESS(status))return status;
    CamDelay(1000);status=CamReset(c,TRUE);CamDelay(8000);
    if(!NT_SUCCESS(status))return status;
    if(c->ProbeLimit==9)return STATUS_SUCCESS;
    c->StaticQosReady=FALSE;c->Powered=TRUE;c->Info.Stage=2;
    status=CamCciInit(c);
    if(NT_SUCCESS(status)&&c->ProbeLimit==10)return STATUS_SUCCESS;
    if(NT_SUCCESS(status)){c->Info.Stage=3;status=CamSensorInit(c);}
    return status;
}

NTSTATUS CamPowerOff(CAM *c) {
    if(c->ScmPending||c->CdmPending||c->TbuPending||c->FrameRouteOwned||c->FrameContextOwned||c->IspFdOwned)return STATUS_DEVICE_BUSY;
    if(c->DmaActive||c->RxActive||c->BusOn)return STATUS_DEVICE_BUSY;
    NTSTATUS status=CamFlashStop(c);
    if(NT_SUCCESS(status))status=CamCrmOff(c);
    if(!NT_SUCCESS(status))return status;
    if(c->Powered&&c->Configured)status=CamSensorStream(c,FALSE);
    if(c->LensReady){NTSTATUS lens=CamFocusStop(c);if(NT_SUCCESS(status))status=lens;}
    if(c->ResetOwned) {
        NTSTATUS reset=CamReset(c,FALSE);
        if(!NT_SUCCESS(reset))return reset;
        CamDelay(1000);
    }
    c->Configured=FALSE;c->Powered=FALSE;c->StaticQosReady=FALSE;
    while(c->SavedCount) {
        CAM_SAVE *s=&c->Saved[c->SavedCount-1];
        if(s->Base==c->Base[CamMnoc]) {
            PLATFORM_LOCK_GUARD guard={0};NTSTATUS locked=PlatformLockAcquire(&guard,FALSE);
            if(locked!=STATUS_SUCCESS)return locked;
            do {
                s=&c->Saved[c->SavedCount-1];
                ULONG restored=CamMnocQosRestore(CamRead(s->Base,s->Offset),s->Original);
                CamWrite(s->Base,s->Offset,restored);
                if((CamRead(s->Base,s->Offset)&CAM_MNOC_QOS_MASK)!=(s->Original&CAM_MNOC_QOS_MASK)) {
                    c->Faulted=TRUE;PlatformLockRelease(&guard);return STATUS_IO_DEVICE_ERROR;
                }
                c->SavedCount--;
            } while(c->SavedCount&&c->Saved[c->SavedCount-1].Base==c->Base[CamMnoc]);
            NTSTATUS released=PlatformLockRelease(&guard);if(!NT_SUCCESS(released))return released;
            continue;
        }
        if(s->Base==c->Base[CamSmmu]) {
            PLATFORM_LOCK_GUARD guard={0};NTSTATUS locked=PlatformLockAcquire(&guard,TRUE);
            if(locked!=STATUS_SUCCESS)return locked;
            do {
                s=&c->Saved[c->SavedCount-1];
                NTSTATUS restored=STATUS_SUCCESS;
                if(s->Offset>=0xc00&&s->Offset<0xe00)
                    restored=CamSmmuRestoreControl(s->Base,s->Offset,s->Original,c->Trace.Smmu[1]&255);
                else CamWrite(s->Base,s->Offset,s->Original);
                if(!NT_SUCCESS(restored)) {
                    c->Diag.Step=173;c->Diag.Region=CamSmmu;c->Diag.Offset=s->Offset;
                    c->Diag.Expected=s->Original;c->Diag.Observed=CamRead(s->Base,s->Offset);
                    PlatformLockRelease(&guard);return restored;
                }
                c->SavedCount--;
            } while(c->SavedCount&&c->Saved[c->SavedCount-1].Base==c->Base[CamSmmu]);
            NTSTATUS released=PlatformLockRelease(&guard);if(!NT_SUCCESS(released))return released;
            continue;
        }
        if(s->Rcg) {
            NTSTATUS restored=CamRcgRestore(s->Base,s->Offset,s->Config,s->Original);
            if(!NT_SUCCESS(restored)) {
                c->Diag.Step=134;c->Diag.Region=s->Base==c->Base[CamMclk]?CamMclk:CamCc;
                c->Diag.Offset=s->Offset;c->Diag.Mask=1;c->Diag.Expected=0;c->Diag.Observed=CamRead(s->Base,s->Offset);
            }
            if(!NT_SUCCESS(restored)){c->Faulted=TRUE;return restored;}
        }
        CamWrite(s->Base,s->Offset,s->Original);
        if(s->Base==c->Base[CamCc]&&(s->Offset==0x1134c||s->Offset==0x110ec)&&(s->Original&3)==1) {
            NTSTATUS restored=CamWait(s->Base,s->Offset+4,0x8000,0x8000,20000);
            if(!NT_SUCCESS(restored)) {
                c->Diag.Step=135;c->Diag.Region=CamCc;c->Diag.Offset=s->Offset+4;
                c->Diag.Mask=0x8000;c->Diag.Expected=0x8000;c->Diag.Observed=CamRead(s->Base,s->Offset+4);
                c->Faulted=TRUE;return restored;
            }
        }
        c->SavedCount--;
    }
    MclkRestore(c);NTSTATUS rails=CamRailsOff(c);
    if(!NT_SUCCESS(rails))return rails;
    if(c->ResetOwned&&c->Sensor!=1&&c->Sensor!=3) {
        for(UCHAR i=0;i<7;i++) {
            UCHAR value=c->ResetSaved[i];NTSTATUS restore=Spmi(c,c->ResetWrite,(UCHAR)(0x40+i),&value,TRUE);
            if(!NT_SUCCESS(restore))return restore;
        }
        c->ResetOwned=FALSE;
    }
    c->ResetOwned=FALSE;return status;
}
