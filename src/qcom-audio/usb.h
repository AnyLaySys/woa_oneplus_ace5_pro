#pragma once
/* SET22: 0=DRP,1=sink,2=source. GET22: 0=not source,1=attached source. */
static const unsigned OtgProperties[]={0,1,9,17,18,24};
typedef struct {
 unsigned Phase,Step,Fault,Touched,Pending,Stop,Retries,Block;
 unsigned Saved[2],Live[4];
 unsigned long long Until,Last,Due,SourceAt;
} OTG_STATE;
typedef struct {unsigned Property,Value,Write;} OTG_REQUEST;
enum {OtgIdle,OtgRead,OtgConfigure,OtgWatch,OtgEnable,OtgSource,OtgDisable,OtgRestore,OtgDone,OtgFailed};
enum {OtgOk,OtgAbort,OtgInputPresent,OtgUnsafeState,OtgBatteryGuard,OtgTimeout,OtgFirmwareError,OtgRoleLost,OtgVoltageError,OtgRollbackError};
static __inline int OtgActive(const OTG_STATE*s){return s->Phase>=OtgRead&&s->Phase<=OtgRestore;}
static __inline void OtgStop(OTG_STATE*s,unsigned fault){if(!s->Stop){s->Fault=fault;s->Stop=1;}}
static __inline void OtgStart(OTG_STATE*s,unsigned long long now){s->Phase=OtgRead;s->Until=now+3000;}
static __inline void OtgShutdown(OTG_STATE*s,unsigned long long now){
 if(!OtgActive(s)&&!s->Touched){s->Phase=OtgDone;return;}
 s->Stop=2;if(!s->Fault)s->Fault=OtgAbort;
 if(s->Touched&&!OtgActive(s)){s->Phase=OtgRestore;s->Step=0;s->Retries=0;s->Pending=0;s->Until=now+5000;}
}
static __inline void OtgLive(OTG_STATE*s,unsigned p,unsigned v){
 if(p==22)s->Live[0]=v;
 else if(p==0)s->Live[1]=v;
 else if(p==1)s->Live[2]=v;
 else if(p==9)s->Live[3]=v;
}
static __inline void OtgReply(OTG_STATE*s,unsigned value,unsigned result,unsigned long long now){
 if(!s->Pending)return;
 s->Pending=0;
 if(result){
  if(s->Phase==OtgDisable||s->Phase==OtgRestore){if(++s->Retries>=3){s->Fault=OtgRollbackError;s->Phase=OtgFailed;}s->Due=now+100;return;}
  OtgStop(s,OtgFirmwareError);return;
 }
 if(s->Phase==OtgRead){
  unsigned step=s->Step++;
  if(step<3)s->Live[step+1]=value;else if(step<5)s->Saved[step-3]=value;
  if((step==0||step>=3)&&value>1){OtgStop(s,OtgUnsafeState);return;}
  if(s->Step==6){s->Step=0;s->Phase=OtgConfigure;s->Until=now+5000;}
 }else if(s->Phase==OtgConfigure){
  if(s->Step==1&&value){OtgStop(s,OtgUnsafeState);return;}
  if(++s->Step==5){s->Phase=OtgWatch;s->Step=0;s->Due=now;}
 }else if(s->Phase==OtgWatch){
  static const unsigned p[]={22,0,1,9};OtgLive(s,p[s->Step],value);
  if(!s->Step){if(value>1){OtgStop(s,OtgUnsafeState);return;}if(!value){s->Block=0;s->Fault=0;}}
  if(++s->Step==4){s->Step=0;s->Due=now+750;
   if(s->Live[0]==1&&!s->Block&&!s->Live[1]&&s->Live[2]<1000000&&!s->Live[3]){s->Phase=OtgEnable;s->Fault=0;s->Until=now+3000;s->Due=now;}}
 }else if(s->Phase==OtgEnable){
  static const unsigned p[]={64,0,1,9,22,24,24};if(s->Step!=0&&s->Step!=5)OtgLive(s,p[s->Step],value);
  if(s->Step==4&&value!=1){OtgStop(s,value?OtgUnsafeState:OtgRoleLost);return;}
  if((s->Step==1&&value)||(s->Step==2&&value>=1000000)){OtgStop(s,OtgInputPresent);return;}
  if((s->Step==3&&value)||(s->Step==6&&value!=1)){OtgStop(s,OtgUnsafeState);return;}
  if(++s->Step==7){s->Phase=OtgSource;s->Step=0;s->SourceAt=now;s->Until=now+1500;}
 }else if(s->Phase==OtgSource){
  static const unsigned p[]={22,0,1,9,24};OtgLive(s,p[s->Step],value);s->Until=now+1500;
  if(s->Step==0&&value!=1)OtgStop(s,value?OtgUnsafeState:OtgRoleLost);
  if(s->Step==1&&value)OtgStop(s,OtgInputPresent);
  if(s->Step==2&&(value>5500000||(value<4400000&&now-s->SourceAt>1000)))OtgStop(s,OtgVoltageError);
  if((s->Step==3&&value)||(s->Step==4&&value!=1))OtgStop(s,OtgUnsafeState);
  s->Step=(s->Step+1)%5;
 }else if(s->Phase==OtgDisable||s->Phase==OtgRestore){
  unsigned expected=s->Step<=5?0:s->Step==6?s->Saved[0]:s->Saved[1];
  if((s->Phase==OtgDisable&&s->Step==2&&value)||(s->Phase==OtgRestore&&s->Step>=4&&value!=expected)){
   if(++s->Retries>=3){s->Fault=OtgRollbackError;s->Phase=OtgFailed;}s->Due=now+100;return;
  }
  s->Retries=0;
  if(s->Phase==OtgDisable&&++s->Step==3){s->Phase=OtgWatch;s->Step=0;s->Block=s->Fault!=OtgRoleLost;s->Stop=0;s->Due=now+100;}
  else if(s->Phase==OtgRestore&&++s->Step==8){s->Touched=0;s->Phase=s->Fault&&s->Fault!=OtgAbort?OtgFailed:OtgDone;}
 }
}
static __inline int OtgNext(OTG_STATE*s,unsigned long long now,int batteryGood,OTG_REQUEST*r){
 if(!OtgActive(s))return 0;
 if(s->Pending){if(now-s->Last<500)return 0;s->Pending=0;
  if(s->Phase==OtgDisable||s->Phase==OtgRestore){if(++s->Retries>=3){s->Fault=OtgRollbackError;s->Phase=OtgFailed;return 0;}}
  else OtgStop(s,OtgTimeout);
 }
 if(s->Phase==OtgDisable||s->Phase==OtgRestore){if(now>=s->Until){s->Fault=OtgRollbackError;s->Phase=OtgFailed;return 0;}}
 else {
  if(s->Phase!=OtgWatch&&now>=s->Until)OtgStop(s,OtgTimeout);
  if((s->Phase==OtgEnable||s->Phase==OtgSource)&&!batteryGood)OtgStop(s,OtgBatteryGuard);
 }
 if(s->Stop&&((s->Phase!=OtgDisable&&s->Phase!=OtgRestore)||(s->Stop==2&&s->Phase==OtgDisable))){
  if(!s->Touched){s->Phase=s->Fault&&s->Fault!=OtgAbort?OtgFailed:OtgDone;return 0;}
  s->Phase=s->Stop==2?OtgRestore:OtgDisable;s->Step=0;s->Retries=0;s->Until=now+5000;s->Due=now;
 }
 if(now<s->Due||(s->Phase==OtgSource&&now-s->Last<50))return 0;
 r->Value=0;r->Write=0;
 if(s->Phase==OtgRead)r->Property=OtgProperties[s->Step];
 else if(s->Phase==OtgConfigure){static const unsigned p[]={24,24,17,18,22},v[]={0,0,1,1,0};r->Property=p[s->Step];r->Value=v[s->Step];r->Write=s->Step!=1;}
 else if(s->Phase==OtgWatch){static const unsigned p[]={22,0,1,9};r->Property=p[s->Step];}
 else if(s->Phase==OtgEnable){static const unsigned p[]={64,0,1,9,22,24,24};r->Property=p[s->Step];r->Value=s->Step?1:500;r->Write=!s->Step||s->Step==5;if(!r->Write)r->Value=0;}
 else if(s->Phase==OtgSource){static const unsigned p[]={22,0,1,9,24};r->Property=p[s->Step];}
 else if(s->Phase==OtgDisable){static const unsigned p[]={24,22,24};r->Property=p[s->Step];r->Write=s->Step<2;}
 else if(s->Phase==OtgRestore){static const unsigned p[]={24,22,17,18,24,22,17,18};r->Property=p[s->Step];r->Write=s->Step<4;r->Value=!r->Write||!s->Step?0:s->Step==1?1:s->Saved[s->Step==2?0:1];}
 else return 0;
 return 1;
}
static __inline void OtgSent(OTG_STATE*s,const OTG_REQUEST*r,unsigned long long now){s->Pending=1;s->Last=now;if(r->Write)s->Touched=1;}

#define SIA_USB_LCID 5u
#define SIA_USB_INTENTS 32u
typedef struct {
 unsigned Rcid,Count,Property,Write;
 unsigned Iid[SIA_USB_INTENTS],Size[SIA_USB_INTENTS];
 unsigned char Free[SIA_USB_INTENTS],Opening,Acked,Granted;
 unsigned long long OnlineAt,BatteryAt,BatteryUntil;
 OTG_STATE Otg;
} SIA_USB;
