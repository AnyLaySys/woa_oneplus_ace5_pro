#include "bluetooth.h"
static const ULONG UartOffsets[]={0x20,0x24,0x28,0x48,0x4c,0x7c,0x22c,0x254,0x258,0x25c,0x260,0x264,0x268,0x26c,0x280,0x284,0x288,0x28c,0x294,0x2a4,0x2a8,0x2ac,0x614,0x644,0x80c,0x810,0x814,0xe18,0xe1c,0xe30};
C_ASSERT(RTL_NUMBER_OF(UartOffsets)==30);
static BOOLEAN Poll(QBT*c,PUCHAR p,ULONG o,ULONG mask,ULONG expected,ULONG tries){
 for(ULONG i=0;i<tries;i++){if(c->BootDeadline&&KeQueryInterruptTime()>=c->BootDeadline)return FALSE;if((QbtRead(p,o)&mask)==expected)return TRUE;KeStallExecutionProcessor(10);}return FALSE;
}
static VOID Delay(ULONG ms){LARGE_INTEGER t;t.QuadPart=-(LONGLONG)ms*10000;KeDelayExecutionThread(KernelMode,FALSE,&t);}
static PUCHAR Map(ULONGLONG address,SIZE_T n){PHYSICAL_ADDRESS p;p.QuadPart=address;return MmMapIoSpaceEx(p,n,PAGE_READWRITE|PAGE_NOCACHE);}
static NTSTATUS Gpio(QBT*c,BOOLEAN write,ULONG reg,UCHAR*v){
 PUCHAR p=write?c->Ch:c->Obs;
 if(write)QbtWrite(p,0x10,*v);
 QbtWrite(p,0,(write?0:0x08000000)|(reg<<4));
 if(!Poll(c,p,8,1,1,100))return STATUS_IO_TIMEOUT;
 if((QbtRead(p,8)&7)!=1)return STATUS_ACCESS_DENIED;
 if(!write)*v=(UCHAR)QbtRead(p,0x18);return STATUS_SUCCESS;
}
NTSTATUS QbtMap(QBT*c,WDFCMRESLIST resources){
 for(ULONG i=0;i<WdfCmResourceListGetCount(resources);i++){
  PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(resources,i);
  if(r->Type==CmResourceTypeMemory&&r->u.Memory.Start.QuadPart==0x898000&&r->u.Memory.Length>=0x4000)c->U=Map(r->u.Memory.Start.QuadPart,0x4000);
 }
 if(!c->U)return STATUS_DEVICE_CONFIGURATION_ERROR;
 c->G=Map(0x100000,0x60000);c->Pins=Map(0xf118000,0x4000);c->Core=Map(0xc400000,0x3000);c->Cfg=Map(0xc42d000,0x4000);
 if(!c->G||!c->Pins||!c->Core||!c->Cfg)return STATUS_INSUFFICIENT_RESOURCES;
 ULONG version=QbtRead(c->Core,0),count=QbtRead(c->Core,4)&0x7ff;
 if(version<0x70000000||version>=0x80000000||count>1024)return STATUS_NOT_SUPPORTED;
 for(ULONG i=0;i<count;i++)if(((QbtRead(c->Core,0x2000+4*i)>>8)&0xfff)==0x58a){
  if((QbtRead(c->Cfg,0x700+4*i)&7)!=0)return STATUS_ACCESS_DENIED;
  c->Obs=Map(0xc440000+0x20*i,0x20);c->Ch=Map(0xc500000+0x1000*i,0x20);
  return c->Obs&&c->Ch?STATUS_SUCCESS:STATUS_INSUFFICIENT_RESOURCES;
 }
 return STATUS_DEVICE_CONFIGURATION_ERROR;
}
VOID QbtUnmap(QBT*c){
#define UNMAP(p,n) if(c->p){MmUnmapIoSpace(c->p,n);c->p=NULL;}
 UNMAP(Ch,0x20);UNMAP(Obs,0x20);UNMAP(Cfg,0x4000);UNMAP(Core,0x3000);UNMAP(Pins,0x4000);UNMAP(U,0x4000);UNMAP(G,0x60000);
#undef UNMAP
}
ULONG QbtReceive(QBT*c,UCHAR*b,ULONG capacity){
 ULONG status=QbtRead(c->U,0x804),words=status&0x1ffffff,n=0;
 if(words>48||capacity<192)return 0;
 for(ULONG i=0;i<words;i++){
  ULONG value=QbtRead(c->U,0x780),valid=4;
  if(i+1==words&&(status&0x80000000)){ULONG last=(status>>28)&7;if(last&&last<4)valid=last;}
  for(ULONG k=0;k<valid;k++)b[n++]=(UCHAR)(value>>(k*8));
 }
 QbtWrite(c->U,0x648,QbtRead(c->U,0x640));QbtWrite(c->U,0x618,QbtRead(c->U,0x610));return n;
}
NTSTATUS QbtSend(QBT*c,const UCHAR*b,ULONG n){
 if(KeQueryInterruptTime()>=c->BootDeadline)return STATUS_IO_TIMEOUT;
 if(QbtRead(c->U,0x40)&1)return STATUS_DEVICE_BUSY;
 QbtWrite(c->U,0x618,0xffffffff);QbtWrite(c->U,0x270,n);QbtWrite(c->U,0x600,0x08000000);
 for(ULONG at=0;at<n;at+=4){
  ULONG tries=0;while((QbtRead(c->U,0x800)&0xfffffff)>=48){if(++tries>10000||KeQueryInterruptTime()>=c->BootDeadline)return STATUS_IO_TIMEOUT;KeStallExecutionProcessor(10);}
  ULONG v=0;for(ULONG k=0;k<4&&at+k<n;k++)v|=(ULONG)b[at+k]<<(8*k);QbtWrite(c->U,0x700,v);
 }
 return Poll(c,c->U,0x610,1,1,5000)?STATUS_SUCCESS:STATUS_IO_TIMEOUT;
}
NTSTATUS QbtFastClock(QBT*c){
 QbtWrite(c->G,0x1e784,16);QbtWrite(c->G,0x1e788,0xffc4);QbtWrite(c->G,0x1e78c,0xffb4);QbtWrite(c->G,0x1e780,(QbtRead(c->G,0x1e780)&~0x371fu)|0x2101);QbtWrite(c->G,0x1e77c,QbtRead(c->G,0x1e77c)|1);
 if(!Poll(c,c->G,0x1e77c,1,0,1000))return STATUS_IO_TIMEOUT;
 /* Changing the clocks must not cancel the live receive sequencer. */
 QbtWrite(c->U,0x48,0x11);QbtWrite(c->U,0x4c,0x11);QbtWrite(c->U,0x7c,0);QbtWrite(c->U,0x2ac,0x80000001);return STATUS_SUCCESS;
}
NTSTATUS QbtStart(QBT*c){
 if(!(QbtRead(c->G,0x1e76c)&0x80000000))return STATUS_DEVICE_BUSY;
 c->BootDeadline=KeQueryInterruptTime()+200000000;
 c->AddedVotes=0x40f&~QbtRead(c->G,0x52010);QbtWrite(c->G,0x52010,QbtRead(c->G,0x52010)|0x40f);c->Voted=TRUE;
 if(!Poll(c,c->G,0x1e76c,0x80000000,0,1000))return STATUS_DEVICE_POWER_FAILURE;
 if(((QbtRead(c->U,0x68)>>8)&0xff)!=2||(QbtRead(c->U,0x40)&0x1001))return STATUS_DEVICE_BUSY;
 UCHAR mode,enable,value;NTSTATUS status=Gpio(c,FALSE,0x40,&mode);if(!NT_SUCCESS(status))return status;
 status=Gpio(c,FALSE,0x46,&enable);if(!NT_SUCCESS(status))return status;
 status=Gpio(c,FALSE,0x44,&value);if(!NT_SUCCESS(status))return status;
 if(mode!=1||!(enable&0x80)||(value&0x7f))return STATUS_DEVICE_CONFIGURATION_ERROR;
 for(ULONG i=0;i<RTL_NUMBER_OF(UartOffsets);i++)c->OldUart[i]=QbtRead(c->U,UartOffsets[i]);c->UartSaved=TRUE;
 for(ULONG i=0;i<4;i++)c->OldPins[i]=QbtRead(c->Pins,0x1000*i);c->PinsSaved=TRUE;
 for(ULONG i=0;i<4;i++)QbtWrite(c->Pins,0x1000*i,i<2?5:7);
 for(ULONG i=0;i<4;i++)c->OldRoot[i]=QbtRead(c->G,0x1e780+4*i);c->OldDfs=QbtRead(c->G,0x1e790);c->RootSaved=TRUE;
 QbtWrite(c->G,0x1e790,c->OldDfs&~1u);QbtWrite(c->G,0x1e784,384);QbtWrite(c->G,0x1e788,0xc476);QbtWrite(c->G,0x1e78c,0xc2f6);QbtWrite(c->G,0x1e780,(c->OldRoot[0]&~0x371fu)|0x2601);QbtWrite(c->G,0x1e77c,QbtRead(c->G,0x1e77c)|1);
 if(!Poll(c,c->G,0x1e77c,1,0,1000))return STATUS_IO_TIMEOUT;
 QbtIrqOff(c);QbtWrite(c->U,0xe18,0);QbtWrite(c->U,0x258,0);
 QbtWrite(c->U,0x28,QbtRead(c->U,0x28)|0x7f);QbtWrite(c->U,0xe30,QbtRead(c->U,0xe30)|0xf);QbtWrite(c->U,0x24,0x7f);QbtWrite(c->U,0x20,1);
 QbtWrite(c->U,0x48,0x41);QbtWrite(c->U,0x4c,0x41);QbtWrite(c->U,0x7c,0);QbtWrite(c->U,0x22c,0);QbtWrite(c->U,0x254,0);
 QbtWrite(c->U,0x25c,0);QbtWrite(c->U,0x280,0);QbtWrite(c->U,0x2a4,0);QbtWrite(c->U,0x2a8,0);QbtWrite(c->U,0x268,8);QbtWrite(c->U,0x28c,8);QbtWrite(c->U,0x26c,0);QbtWrite(c->U,0x294,160);QbtWrite(c->U,0x2ac,0);
 QbtWrite(c->U,0x260,0x4380e);QbtWrite(c->U,0x264,0xc3e0e);QbtWrite(c->U,0x284,0x4380e);QbtWrite(c->U,0x288,0xc3e0e);
 QbtWrite(c->U,0x80c,8);QbtWrite(c->U,0x810,16);QbtWrite(c->U,0x814,44);QbtWrite(c->U,0x618,0xffffffff);QbtWrite(c->U,0x648,0xffffffff);QbtWrite(c->U,0x630,0x08000000);
 value=0;status=Gpio(c,TRUE,0x44,&value);if(!NT_SUCCESS(status))return status;Delay(20);
 value=0x80;status=Gpio(c,TRUE,0x44,&value);if(!NT_SUCCESS(status))return status;c->ResetReleased=TRUE;Delay(100);
 UCHAR stale[256];QbtReceive(c,stale,sizeof(stale));Delay(2);QbtReceive(c,stale,sizeof(stale));
 status=QbtFirmware(c);if(NT_SUCCESS(status))QbtWrite(c->U,0x2ac,0);return status;
}
VOID QbtStop(QBT*c){
 c->BootDeadline=0;
 if(c->ResetReleased){UCHAR value=0;Gpio(c,TRUE,0x44,&value);c->ResetReleased=FALSE;}
 if(c->UartSaved){
  QbtIrqOff(c);
  QbtWrite(c->U,0x634,4);Poll(c,c->U,0x640,16,16,1000);
  UCHAR discard[256];QbtReceive(c,discard,sizeof(discard));
  if(QbtRead(c->U,0x40)&0x1000){QbtWrite(c->U,0x634,2);Poll(c,c->U,0x634,2,0,1000);QbtWrite(c->U,0x20,1);}
  if(QbtRead(c->U,0x40)&1){QbtWrite(c->U,0x604,2);Poll(c,c->U,0x604,2,0,1000);}
  QbtWrite(c->U,0x618,0xffffffff);QbtWrite(c->U,0x648,0xffffffff);
  for(ULONG i=0;i<RTL_NUMBER_OF(UartOffsets);i++)QbtWrite(c->U,UartOffsets[i],c->OldUart[i]);c->UartSaved=FALSE;
 }
 if(c->RootSaved){for(ULONG i=0;i<4;i++)QbtWrite(c->G,0x1e780+4*i,c->OldRoot[i]);QbtWrite(c->G,0x1e77c,QbtRead(c->G,0x1e77c)|1);Poll(c,c->G,0x1e77c,1,0,1000);QbtWrite(c->G,0x1e790,c->OldDfs);c->RootSaved=FALSE;}
 if(c->PinsSaved){for(ULONG i=0;i<4;i++)QbtWrite(c->Pins,0x1000*i,c->OldPins[i]);c->PinsSaved=FALSE;}
 if(c->Voted){QbtWrite(c->G,0x52010,QbtRead(c->G,0x52010)&~c->AddedVotes);c->Voted=FALSE;}
}
VOID QbtIrqOff(QBT*c){QbtWrite(c->U,0xe1c,0);QbtWrite(c->U,0x614,0);QbtWrite(c->U,0x644,0);}
VOID QbtRuntimeFill(QBT*c){
 while(c->TxAt<c->TxLength&&(QbtRead(c->U,0x800)&0xfffffff)<48){
  ULONG v=0;for(ULONG k=0;k<4&&c->TxAt<c->TxLength;k++)v|=(ULONG)c->TxBytes[c->TxAt++]<<(k*8);QbtWrite(c->U,0x700,v);
 }
 QbtWrite(c->U,0x614,0x3300003d|(c->TxAt<c->TxLength?0x40000000:0));
}
static VOID DrainRx(QBT*c){
 ULONG status=QbtRead(c->U,0x804),words=status&0x1ffffff;
 if(words>48)InterlockedCompareExchange(&c->Fault,STATUS_DEVICE_DATA_ERROR,0);
 else for(ULONG i=0;i<words;i++){
  ULONG value=QbtRead(c->U,0x780),valid=4;
  if(i+1==words&&(status&0x80000000)){ULONG n=(status>>28)&7;if(n&&n<4)valid=n;}
  for(ULONG k=0;k<valid;k++){
   ULONG next=(c->RingWrite+1)&(QBT_RING_SIZE-1);
   if(next==c->RingRead){InterlockedCompareExchange(&c->Fault,STATUS_BUFFER_OVERFLOW,0);break;}
   c->Ring[c->RingWrite]=(UCHAR)(value>>(k*8));c->RingWrite=next;
  }
 }
 ULONG free=(c->RingRead-c->RingWrite-1)&(QBT_RING_SIZE-1);
 if(free<512&&!c->Paused){QbtWrite(c->U,0x2ac,0x80000002);c->Paused=TRUE;}
}
BOOLEAN QbtIsr(WDFINTERRUPT interrupt,ULONG message){
 UNREFERENCED_PARAMETER(message);QBT*c=QbtDevice(WdfInterruptGetDevice(interrupt));if(!c->Online)return FALSE;
 ULONG rawM=QbtRead(c->U,0x610),rawS=QbtRead(c->U,0x640);if(!rawM&&!rawS)return FALSE;
 ULONG m=rawM&QbtRead(c->U,0x614),s=rawS&QbtRead(c->U,0x644);
 QbtWrite(c->U,0x618,rawM);QbtWrite(c->U,0x648,rawS);DrainRx(c);
 if((s&0x0300060c)||(m&0x3300003c))InterlockedCompareExchange(&c->Fault,STATUS_DEVICE_DATA_ERROR,0);
 if(c->TxBytes){if(m&1){c->TxDone=TRUE;QbtWrite(c->U,0x614,0x3300003c);}else if(m&0x40000000)QbtRuntimeFill(c);}
 QbtWrite(c->U,0x618,rawM&0x40000000);
 if(c->Fault){QbtIrqOff(c);}
 WdfDpcEnqueue(c->Dpc);return TRUE;
}
NTSTATUS QbtInterruptEnable(WDFINTERRUPT interrupt,WDFDEVICE device){
 UNREFERENCED_PARAMETER(device);QBT*c=QbtDevice(WdfInterruptGetDevice(interrupt));
 QbtWrite(c->U,0x618,0xffffffff);QbtWrite(c->U,0x648,0xffffffff);QbtWrite(c->U,0x614,0);QbtWrite(c->U,0x644,0x0f00063c);InterlockedExchange(&c->Online,1);DrainRx(c);QbtWrite(c->U,0xe1c,0x0c);WdfDpcEnqueue(c->Dpc);return STATUS_SUCCESS;
}
NTSTATUS QbtInterruptDisable(WDFINTERRUPT interrupt,WDFDEVICE device){
 UNREFERENCED_PARAMETER(device);QBT*c=QbtDevice(WdfInterruptGetDevice(interrupt));InterlockedExchange(&c->Online,0);QbtIrqOff(c);return STATUS_SUCCESS;
}
