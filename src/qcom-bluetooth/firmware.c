#include "bluetooth.h"
typedef struct { PUCHAR Data; ULONG Size; } BLOB;
typedef struct { QBT*Hw; UCHAR Pending[256],Frame[512]; ULONG At,Have,Length; BOOLEAN TmelReady; } BOOT;
static ULONG Le32(const UCHAR*p){return (ULONG)p[0]|((ULONG)p[1]<<8)|((ULONG)p[2]<<16)|((ULONG)p[3]<<24);}
static NTSTATUS Load(PCWSTR name,BLOB*b){
 WCHAR path[128];UNICODE_STRING u;RtlInitEmptyUnicodeString(&u,path,sizeof(path));NTSTATUS status=RtlAppendUnicodeToString(&u,QBT_FW_ROOT);if(!NT_SUCCESS(status))return status;status=RtlAppendUnicodeToString(&u,name);if(!NT_SUCCESS(status))return status;
 OBJECT_ATTRIBUTES oa;InitializeObjectAttributes(&oa,&u,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,NULL,NULL);IO_STATUS_BLOCK io;HANDLE f;
 status=ZwCreateFile(&f,FILE_READ_DATA|SYNCHRONIZE,&oa,&io,NULL,0,FILE_SHARE_READ,FILE_OPEN,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,NULL,0);if(!NT_SUCCESS(status))return status;
 FILE_STANDARD_INFORMATION info;status=ZwQueryInformationFile(f,&io,&info,sizeof(info),FileStandardInformation);
 if(NT_SUCCESS(status)){
  if(info.EndOfFile.QuadPart<=0||info.EndOfFile.QuadPart>524288)status=STATUS_INVALID_IMAGE_FORMAT;
  else {b->Size=(ULONG)info.EndOfFile.QuadPart;b->Data=ExAllocatePool2(POOL_FLAG_NON_PAGED,b->Size,QBT_TAG);if(!b->Data)status=STATUS_INSUFFICIENT_RESOURCES;
   else {status=ZwReadFile(f,NULL,NULL,NULL,&io,b->Data,b->Size,NULL,NULL);if(NT_SUCCESS(status)&&io.Information!=b->Size)status=STATUS_END_OF_FILE;}}
 }
 ZwClose(f);return status;
}
static NTSTATUS PatchNvm(BLOB*b,const UCHAR*address){
 if(b->Size<4||(Le32(b->Data)&0xff)!=2||(Le32(b->Data)>>8)!=b->Size-4)return STATUS_INVALID_IMAGE_FORMAT;
 BOOLEAN baud=FALSE,addr=address==NULL;
 for(ULONG at=4;at<b->Size;){
  if(b->Size-at<12)return STATUS_INVALID_IMAGE_FORMAT;
  ULONG tag=b->Data[at]|((ULONG)b->Data[at+1]<<8),n=b->Data[at+2]|((ULONG)b->Data[at+3]<<8);at+=12;if(n>b->Size-at)return STATUS_INVALID_IMAGE_FORMAT;
  if(tag==17){if(baud||n<2)return STATUS_INVALID_IMAGE_FORMAT;b->Data[at+1]=0x15;baud=TRUE;}
  if(tag==2&&address){if(addr||n!=6)return STATUS_INVALID_IMAGE_FORMAT;RtlCopyMemory(b->Data+at,address,6);addr=TRUE;}
  at+=n;
 }
 return baud&&addr?STATUS_SUCCESS:STATUS_INVALID_IMAGE_FORMAT;
}
static NTSTATUS Byte(BOOT*b,UCHAR*v){
 ULONGLONG until=KeQueryInterruptTime()+20000000;
 while(b->At==b->Have){if(KeQueryInterruptTime()>=until||KeQueryInterruptTime()>=b->Hw->BootDeadline)return STATUS_IO_TIMEOUT;
  b->At=0;b->Have=QbtReceive(b->Hw,b->Pending,sizeof(b->Pending));if(!b->Have)KeStallExecutionProcessor(20);
 }
 *v=b->Pending[b->At++];return STATUS_SUCCESS;
}
static NTSTATUS Frame(BOOT*b){
 for(ULONG tries=0;tries<256;tries++){
  UCHAR*p=b->Frame;NTSTATUS s=Byte(b,p);if(!NT_SUCCESS(s))return s;
  if(p[0]==0xfc||p[0]==0xfe)continue;
  if(p[0]==0xfd){UCHAR ack=0xfc;s=QbtSend(b->Hw,&ack,1);if(!NT_SUCCESS(s))return s;continue;}
  ULONG h=p[0]==0x34?4:p[0]==4?3:p[0]==2?5:0;if(!h)return STATUS_DEVICE_PROTOCOL_ERROR;
  for(ULONG i=1;i<h;i++){s=Byte(b,p+i);if(!NT_SUCCESS(s))return s;}
  ULONG n=p[0]==2?(ULONG)p[3]+((ULONG)p[4]<<8):p[h-1];if(n>sizeof(b->Frame)-h)return STATUS_BUFFER_OVERFLOW;
  for(ULONG i=0;i<n;i++){s=Byte(b,p+h+i);if(!NT_SUCCESS(s))return s;}
  if(p[0]==2)continue;
  b->Length=h+n;
  if(b->Length==8&&RtlCompareMemory(p,"\x34\x00\xff\x04\xf0\x03\x00\x03",8)==8)b->TmelReady=TRUE;
  return STATUS_SUCCESS;
 }
 return STATUS_DEVICE_PROTOCOL_ERROR;
}
static NTSTATUS Wait(BOOT*b,const UCHAR*prefix,ULONG length){
 for(ULONG i=0;i<32;i++){NTSTATUS s=Frame(b);if(!NT_SUCCESS(s))return s;if(b->Length>=length&&RtlCompareMemory(b->Frame,prefix,length)==length)return STATUS_SUCCESS;}
 return STATUS_DEVICE_PROTOCOL_ERROR;
}
static NTSTATUS Command(BOOT*b,const UCHAR*wire,ULONG n,const UCHAR*reply,ULONG size){NTSTATUS s=QbtSend(b->Hw,wire,n);return NT_SUCCESS(s)?Wait(b,reply,size):s;}
static NTSTATUS PeriCommand(BOOT*b,UCHAR command,UCHAR subsystem,UCHAR length,UCHAR eventLength){
 const UCHAR wire[]={0x31,0,0xf0,0xff,length,command,subsystem,0},reply[]={0x34,0,0xff,eventLength,0xf0,1,1,0xf0,0xff,0,command,subsystem};
 return Command(b,wire,5+length,reply,11+(length>1));
}
static NTSTATUS Wake(BOOT*b){
 UCHAR wake=0xfd;NTSTATUS s=QbtSend(b->Hw,&wake,1);if(!NT_SUCCESS(s))return s;
 for(ULONG i=0;i<32;i++){UCHAR v;s=Byte(b,&v);if(!NT_SUCCESS(s))return s;if(v==0xfc)return STATUS_SUCCESS;if(v==0xfd){v=0xfc;s=QbtSend(b->Hw,&v,1);if(!NT_SUCCESS(s))return s;}}
 return STATUS_DEVICE_PROTOCOL_ERROR;
}
static NTSTATUS PeriDownload(BOOT*b,const BLOB*f,UCHAR ss,BOOLEAN ackEach){
 UCHAR wire[251]={0x31,0,0xf0,0xff,0,6,ss,0};
 for(ULONG at=0;at<f->Size;){ULONG n=min(243,f->Size-at);wire[4]=(UCHAR)(n+3);wire[7]=(UCHAR)n;RtlCopyMemory(wire+8,f->Data+at,n);at+=n;
  NTSTATUS s=QbtSend(b->Hw,wire,n+8);if(!NT_SUCCESS(s))return s;
  if(ackEach||at==f->Size){const UCHAR reply[]={0x34,0,0xff,8,0xf0,1,1,0xf0,0xff,0,6,ss};s=Wait(b,reply,sizeof(reply));if(!NT_SUCCESS(s))return s;}}
 return STATUS_SUCCESS;
}
static NTSTATUS Hci(BOOT*b,USHORT opcode,const UCHAR*params,UCHAR length,UCHAR sub){
 UCHAR wire[259]={1,(UCHAR)opcode,(UCHAR)(opcode>>8),length};if(length)RtlCopyMemory(wire+4,params,length);NTSTATUS s=QbtSend(b->Hw,wire,length+4);if(!NT_SUCCESS(s))return s;
 for(ULONG i=0;i<32;i++){s=Frame(b);if(!NT_SUCCESS(s))return s;UCHAR*p=b->Frame;
  if(b->Length>=7&&p[0]==4&&p[1]==0x0e&&p[4]==(UCHAR)opcode&&p[5]==(UCHAR)(opcode>>8)){
   if(p[6]||(opcode==0xfc00&&(b->Length<8||p[7]!=sub)))return STATUS_DEVICE_PROTOCOL_ERROR;return STATUS_SUCCESS;
  }
 }
 return STATUS_DEVICE_PROTOCOL_ERROR;
}
static NTSTATUS BtDownload(BOOT*b,const BLOB*f,BOOLEAN ackEach){
 UCHAR payload[245]={0x1e};
 for(ULONG at=0;at<f->Size;){ULONG n=min(243,f->Size-at);payload[1]=(UCHAR)n;RtlCopyMemory(payload+2,f->Data+at,n);at+=n;NTSTATUS s;
  if(ackEach||at==f->Size)s=Hci(b,0xfc00,payload,(UCHAR)(n+2),0x1e);
  else {UCHAR wire[249]={1,0,0xfc,(UCHAR)(n+2)};RtlCopyMemory(wire+4,payload,n+2);s=QbtSend(b->Hw,wire,n+6);}
  if(!NT_SUCCESS(s))return s;
 }
 return STATUS_SUCCESS;
}
NTSTATUS QbtFirmware(QBT*c){
 static PCWSTR names[]={L"brhperifw20.mbn",L"brhperinv20.bin",L"tmel_peach_20.elf",L"brhbtfw20.mbn",L"brhbtnv20.bin",L"address.bin"};
 BLOB f[6]={0};BOOT*b=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*b),QBT_TAG);if(!b)return STATUS_INSUFFICIENT_RESOURCES;b->Hw=c;NTSTATUS status=STATUS_SUCCESS;
#define TRY(call) do {status=(call);if(!NT_SUCCESS(status))goto done;}while(0)
 for(ULONG i=0;i<RTL_NUMBER_OF(f);i++)TRY(Load(names[i],f+i));
 if(f[5].Size!=6){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
 UCHAR any=0,all=0xff;for(ULONG i=0;i<6;i++){any|=f[5].Data[i];all&=f[5].Data[i];}if(!any||all==0xff){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
 const ULONG patches[]={0,2,3};for(ULONG n=0;n<RTL_NUMBER_OF(patches);n++){ULONG i=patches[n];if(f[i].Size<52||RtlCompareMemory(f[i].Data,"\x7f""ELF\x01\x01",6)!=6||f[i].Data[18]!=0xf3||f[i].Data[19]!=0){status=STATUS_INVALID_IMAGE_FORMAT;goto done;}}
 TRY(PatchNvm(f+1,NULL));TRY(PatchNvm(f+4,f[5].Data));TRY(Wake(b));
 TRY(PeriCommand(b,5,0,2,0x15));
 if(b->Length!=25||Le32(b->Frame+12)!=0x21||b->Frame[18]!=0||b->Frame[19]!=2||Le32(b->Frame+20)!=0x40210200){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
 const UCHAR baud[]={0x31,0,0xf1,0xff,2,2,0x15},baudReply[]={0x34,0,0xff,7,0xf0,1,1,0xf1,0xff,1,2};
 QbtWrite(c->U,0x2ac,0x80000002);TRY(QbtSend(c,baud,sizeof(baud)));LARGE_INTEGER delay;delay.QuadPart=-200000;KeDelayExecutionThread(KernelMode,FALSE,&delay);TRY(QbtFastClock(c));TRY(Wait(b,baudReply,sizeof(baudReply)));
 TRY(PeriCommand(b,8,0,3,8));TRY(PeriDownload(b,f,0,FALSE));
 TRY(PeriCommand(b,0x0a,0,1,0x0a));TRY(PeriDownload(b,f+1,0,TRUE));
 const UCHAR active[]={0x31,0,0xf1,0xff,1,3},activeReply[]={0x34,0,0xff,7,0xf0,1,1,0xf1,0xff,0,3};
 TRY(Command(b,active,sizeof(active),activeReply,sizeof(activeReply)));TRY(Wake(b));
 TRY(PeriCommand(b,8,3,3,8));
 TRY(PeriCommand(b,5,3,2,0x15));TRY(PeriDownload(b,f+2,3,FALSE));
 const UCHAR tmelDone[]={0x34,0,0xff,4,0xf0,3,0,3};if(!b->TmelReady)TRY(Wait(b,tmelDone,sizeof(tmelDone)));
 const UCHAR btActive[]={0x31,0,0xf1,0xff,3,0,1,1},btActiveReply[]={0x34,0,0xff,6,0xf0,0,0,1,0xf1,0xff},btDone[]={0x34,0,0xff,4,0xf0,2,1,1};
 TRY(Command(b,btActive,sizeof(btActive),btActiveReply,sizeof(btActiveReply)));TRY(Wait(b,btDone,sizeof(btDone)));delay.QuadPart=-200000;KeDelayExecutionThread(KernelMode,FALSE,&delay);
 UCHAR ver=0x19;TRY(Hci(b,0xfc00,&ver,1,0x19));TRY(BtDownload(b,f+3,FALSE));UCHAR board=0x23;TRY(Hci(b,0xfc00,&board,1,0x23));
 if(b->Length!=11||b->Frame[8]!=2||b->Frame[9]||b->Frame[10]){status=STATUS_DEVICE_CONFIGURATION_ERROR;goto done;}
 TRY(BtDownload(b,f+4,TRUE));UCHAR logging[]={0x14,0};TRY(Hci(b,0xfc17,logging,sizeof(logging),0));TRY(Hci(b,0x0c03,NULL,0,0));
 done:
 if(NT_SUCCESS(status)){while(b->At<b->Have){c->Ring[c->RingWrite++]=b->Pending[b->At++];}}
 for(ULONG i=0;i<RTL_NUMBER_OF(f);i++)if(f[i].Data)ExFreePoolWithTag(f[i].Data,QBT_TAG);ExFreePoolWithTag(b,QBT_TAG);return status;
#undef TRY
}
