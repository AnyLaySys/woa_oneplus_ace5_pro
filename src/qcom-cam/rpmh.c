#include "cam.h"
#include "../platform/lock.h"
#include "rpmh-message.h"

typedef struct { ULONG Address,Original[3],Count; BOOLEAN Changed; } VOTE;
typedef struct { ULONG Major,Count; BOOLEAN Fault; VOTE Votes[18]; } POWER;

static USHORT Db16(PUCHAR b,ULONG off) { return (USHORT)(CamRead(b,off&~3u)>>(8*(off&3))); }

static NTSTATUS Find(CAM *c,const CHAR *name,ULONG *address,ULONG *aux,ULONG *length) {
    PUCHAR db=c->Base[CamDb];ULONG key[2]={0};
    for(ULONG i=0;i<8&&name[i];i++)key[i/4]|=(ULONG)(UCHAR)name[i]<<(8*(i&3));
    if(CamRead(db,4)!=0x0c0330db)return STATUS_DEVICE_CONFIGURATION_ERROR;
    for(ULONG h=0;h<8;h++) {
        ULONG sh=8+h*16,slv=Db16(db,sh),count=Db16(db,sh+6);
        ULONG start=144+Db16(db,sh+2),data=144+Db16(db,sh+4);
        if(!slv||count>2048||start+count*24>0x20000)continue;
        for(ULONG i=0;i<count;i++) {
            ULONG e=start+i*24;
            if(CamRead(db,e)!=key[0]||CamRead(db,e+4)!=key[1])continue;
            *address=CamRead(db,e+16);*length=Db16(db,e+20);*aux=data+Db16(db,e+22);
            if(!*address||*aux+*length>0x20000)return STATUS_DATA_ERROR;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

static NTSTATUS Commands(CAM *c,ULONG address,ULONG *values,ULONG count,BOOLEAN write) {
    POWER *p=c->PowerState;PUCHAR r=c->Base[CamRsc];
    if(p->Fault||!count||count>3)return STATUS_INVALID_DEVICE_STATE;
    PLATFORM_LOCK_GUARD guard={0};NTSTATUS status=PlatformLockAcquire(&guard,FALSE);
    if(status!=STATUS_SUCCESS)return status;
    ULONG wait=p->Major==3?0x20:0x10,control=wait+4,state=wait+8,enable=wait+12;
    ULONG msg=p->Major==3?0x34:0x30,stride=p->Major==3?24:20,slot=MAXULONG,irq=CamRead(r,0xd00);
    BOOLEAN triggered=FALSE,done=FALSE;
    c->Diag.Address=address;c->Diag.Count=count;c->Diag.Slot=MAXULONG;c->Diag.Phase=write?1:0;
    ULONG commands=(CamRead(r,0xc)>>27)&31;
    for(ULONG i=0;i<3;i++) {
        ULONG b=0xd00+i*0x2a0,enabled=CamRead(r,b+enable);
        for(ULONG j=0;j<commands;j++)if(enabled&(1u<<j)) {
            ULONG pending=CamRead(r,b+msg+4+j*stride);
            if(CamRpmhSameResource(address,pending)){c->Diag.Slot=i;status=STATUS_DEVICE_BUSY;goto out;}
        }
    }
    for(ULONG i=0;i<3;i++) {
        ULONG b=0xd00+i*0x2a0;
        if(!CamRead(r,b+control)&&!CamRead(r,b+enable)&&!CamRead(r,b+wait)&&
           (CamRead(r,b+state)&1)&&!(CamRead(r,0xd04)&(1u<<i))){slot=i;break;}
    }
    if(slot==MAXULONG){status=STATUS_DEVICE_BUSY;goto out;}
    ULONG base=0xd00+slot*0x2a0,mask=(1u<<count)-1;
    c->Diag.Address=address;c->Diag.Count=count;c->Diag.Slot=slot;c->Diag.Phase=write?1:0;
    CamWrite(r,0xd00,irq&~(1u<<slot));
    for(ULONG i=0;i<count;i++) {
        ULONG off=base+msg+i*stride;
        CamWrite(r,off,CamRpmhMessage(write));
        CamWrite(r,off+4,address+(count==3?(i==0?0:i==1?8:4):0));
        CamWrite(r,off+8,write?values[i]:0);
    }
    CamWrite(r,base+wait,mask);CamWrite(r,base+enable,mask);CamWrite(r,base+control,0x10000);
    for(ULONG i=0;i<count;i++)if(CamRead(r,base+msg+12+i*stride)&0x10100u){status=STATUS_IO_DEVICE_ERROR;goto cleanup;}
    triggered=TRUE;CamWrite(r,base+control,0x1010000);
    ULONGLONG deadline=KeQueryInterruptTime()+1000000;
    do {
        done=TRUE;
        for(ULONG i=0;i<count;i++)if((CamRead(r,base+msg+12+i*stride)&0x10100u)!=0x10100u)done=FALSE;
        ULONG st=CamRead(r,base+state);
        if(done&&st&&st!=MAXULONG)break;
        done=FALSE;CamDelay(100);
    }while(KeQueryInterruptTime()<deadline);
    status=done?STATUS_SUCCESS:STATUS_IO_TIMEOUT;
    if(done&&!write)for(ULONG i=0;i<count;i++)values[i]=CamRead(r,base+msg+16+i*stride);
cleanup:
    if(!triggered||done) {
        CamWrite(r,base+control,0x10000);CamWrite(r,base+control,0);
        CamWrite(r,base+enable,0);CamWrite(r,base+wait,0);CamWrite(r,0xd08,1u<<slot);
        NTSTATUS clear=CamWait(r,0xd04,1u<<slot,0,10000);
        if(NT_SUCCESS(clear))CamWrite(r,0xd00,irq);
        else {p->Fault=TRUE;status=clear;}
    } else p->Fault=TRUE;
out:
    NTSTATUS released=PlatformLockRelease(&guard);return NT_SUCCESS(status)?released:status;
}

static NTSTATUS Vote(CAM *c,const CHAR *name,ULONG voltage,BOOLEAN arc) {
    POWER *p=c->PowerState;ULONG address,aux,length;
    RtlZeroMemory(c->Diag.Resource,sizeof(c->Diag.Resource));
    for(ULONG i=0;i<8&&name[i];i++)c->Diag.Resource[i]=name[i];
    if(p->Count>=RTL_NUMBER_OF(p->Votes))return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status=Find(c,name,&address,&aux,&length);
    if(!NT_SUCCESS(status))return status;
    if(((address>>16)&7)!=(arc?3u:4u))return STATUS_DEVICE_CONFIGURATION_ERROR;
    if(arc) {
        if(!length||(length&1)||length>64)return STATUS_DATA_ERROR;
        ULONG index;
        for(index=0;index<length/2;index++)if(Db16(c->Base[CamDb],aux+index*2)>=voltage)break;
        if(index==length/2)return STATUS_NOT_SUPPORTED;
        voltage=index;
    }
    VOTE *v=&p->Votes[p->Count];v->Address=address;v->Count=arc?1:3;
    status=Commands(c,address,v->Original,v->Count,FALSE);
    if(!NT_SUCCESS(status))return status;
    ULONG values[]={max(voltage,v->Original[0]),7,1};
    v->Changed=TRUE;p->Count++;
    status=Commands(c,address,values,v->Count,TRUE);
    CamDelay(1000);return status;
}

static NTSTATUS Bcm(CAM *c,const CHAR *name,BOOLEAN enableOnly) {
    POWER *p=c->PowerState;ULONG address,aux,length;
    RtlZeroMemory(c->Diag.Resource,sizeof(c->Diag.Resource));
    for(ULONG i=0;i<8&&name[i];i++)c->Diag.Resource[i]=name[i];
    NTSTATUS status=Find(c,name,&address,&aux,&length);
    if(!NT_SUCCESS(status))return status;
    if(((address>>16)&7)!=5||length<8||(aux&3)||p->Count>=RTL_NUMBER_OF(p->Votes))return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG unit=CamRead(c->Base[CamDb],aux),width=Db16(c->Base[CamDb],aux+4);
    if(!unit||!width)return STATUS_DATA_ERROR;
    VOTE *v=&p->Votes[p->Count];v->Address=address;v->Count=1;
    status=Commands(c,address,v->Original,1,FALSE);if(!NT_SUCCESS(status))return status;
    ULONG x=(v->Original[0]>>14)&0x3fff,y=v->Original[0]&0x3fff;
    ULONG vote=enableOnly?1:(ULONG)min((2000000000ULL*width/32+unit-1)/unit,0x3fffULL);
    ULONG value=0x60000000u|(x<<14)|(enableOnly?(y|1u):max(y,vote));
    ULONG slot=name[0]=='M'&&name[1]=='M'?(name[2]=='0'?0:1):name[0]=='C'?3:
        name[0]=='M'?5:name[2]=='0'?4:2;
    c->Trace.BusVotes[slot][0]=v->Original[0];c->Trace.BusVotes[slot][1]=value;
    v->Original[0]|=0x40000000u;v->Changed=TRUE;p->Count++;
    status=Commands(c,address,&value,1,TRUE);
    ULONG observed=0;
    if(NT_SUCCESS(status))status=Commands(c,address,&observed,1,FALSE);
    c->Trace.BusVotes[slot][2]=observed;
    return status;
}

NTSTATUS CamRailsOn(CAM *c) {
    POWER *p=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*p),'pCcQ');
    if(!p)return STATUS_INSUFFICIENT_RESOURCES;
    c->PowerState=p;p->Major=(CamRead(c->Base[CamRsc],0)>>16)&255;
    c->Diag.RscId=CamRead(c->Base[CamRsc],0);c->Diag.RscConfig=CamRead(c->Base[CamRsc],0xc);
    ULONG config=CamRead(c->Base[CamRsc],0xc);
    if((p->Major!=2&&p->Major!=3)||((config>>12)&63)<3||((config>>27)&31)<3)return STATUS_REVISION_MISMATCH;
    static const CHAR *const names[]={"cx.lvl","mx.lvl","mxc.lvl","mmcx.lvl","ldof1","ldog3","ldom5","ldom2","ldom3","ldom6","ldom7"};
    static const ULONG voltages[]={384,384,384,384,880,1200,1800,1100,2800,1800,2800};
    for(ULONG i=0;i<RTL_NUMBER_OF(names);i++) {
        c->Diag.Step=110+i;
        NTSTATUS status=Vote(c,names[i],voltages[i],i<4);
        if(!NT_SUCCESS(status))return status;
    }
    c->Diag.Step=120;NTSTATUS status=Bcm(c,"MM0",FALSE);
    if(!NT_SUCCESS(status))return status;
    c->Diag.Step=121;status=Bcm(c,"MM1",TRUE);
    if(NT_SUCCESS(status)){c->Diag.Step=122;status=Bcm(c,"SH1",TRUE);}
    if(NT_SUCCESS(status)){c->Diag.Step=123;status=Bcm(c,"CN0",TRUE);}
    if(NT_SUCCESS(status)){c->Diag.Step=124;status=Bcm(c,"SH0",FALSE);}
    if(NT_SUCCESS(status)){c->Diag.Step=125;status=Bcm(c,"MC0",FALSE);}
    return status;
}

VOID CamDiagnostics(CAM *c,QCAM_DIAG *diag) {
    *diag=c->Diag;diag->Size=sizeof(*diag);diag->Abi=QCAM_ABI;
    PUCHAR r=c->Base[CamRsc];ULONG major=(CamRead(r,0)>>16)&255;
    diag->RscId=CamRead(r,0);diag->RscConfig=CamRead(r,0xc);diag->DbMagic=CamRead(c->Base[CamDb],4);
    ULONG wait=major==3?0x20:0x10,msg=major==3?0x34:0x30;
    for(ULONG i=0;i<3;i++) {
        ULONG b=0xd00+i*0x2a0;
        diag->Tcs[i][0]=CamRead(r,b+wait);diag->Tcs[i][1]=CamRead(r,b+wait+4);
        diag->Tcs[i][2]=CamRead(r,b+wait+8);diag->Tcs[i][3]=CamRead(r,b+wait+12);
        for(ULONG j=0;j<5;j++)diag->Tcs[i][4+j]=CamRead(r,b+msg+j*4);
    }
    diag->SavedCount=c->SavedCount;
    diag->Faults=(c->Faulted?1u:0u)|(c->PowerState&&((POWER *)c->PowerState)->Fault?2u:0u);
}

NTSTATUS CamRailsOff(CAM *c) {
    POWER *p=c->PowerState;if(!p)return STATUS_SUCCESS;
    if(p->Fault)return STATUS_DEVICE_HARDWARE_ERROR;
    while(p->Count) {
        VOTE *v=&p->Votes[p->Count-1];
        NTSTATUS status=Commands(c,v->Address,v->Original,v->Count,TRUE);
        if(!NT_SUCCESS(status))return status;
        p->Count--;
    }
    ExFreePoolWithTag(p,'pCcQ');c->PowerState=NULL;return STATUS_SUCCESS;
}
