#define NOMINMAX
#include <windows.h>
#include <wdf.h>
#include <nfccx.h>
struct NFC {
 WDFDEVICE Device;WDFWAITLOCK Lock;WDFMEMORY Memory;WDFWORKITEM Work;
 PUCHAR Base;ULONG Depth;volatile LONG Online;UCHAR Packet[258];
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(NFC,Context);
static ULONG Read(NFC*c,ULONG o){return WDF_READ_REGISTER_ULONG(c->Device,(PULONG)(c->Base+o));}
static VOID Write(NFC*c,ULONG o,ULONG v){WDF_WRITE_REGISTER_ULONG(c->Device,(PULONG)(c->Base+o),v);}
static NTSTATUS On(NFC*c){
 if(((Read(c,0x68)>>8)&255)!=3 || (Read(c,0x64)&1))return STATUS_DEVICE_CONFIGURATION_ERROR;
 c->Depth=(Read(c,0xe24)>>16)&255;if(!c->Depth)return STATUS_DEVICE_CONFIGURATION_ERROR;
 Write(c,0x28,Read(c,0x28)|0x7f);Write(c,0xe30,Read(c,0xe30)|15);
 Write(c,0x24,0x7f);Write(c,0x20,1);Write(c,0x258,0);Write(c,0xe18,0);
 Write(c,0x614,0);Write(c,0x644,0);Write(c,0x618,0xffffffff);Write(c,0x648,0xffffffff);
 Write(c,0x260,0x7f8fe);Write(c,0x264,0xffefe);Write(c,0x284,0x7f8fe);Write(c,0x288,0xffefe);
 Write(c,0x254,0);Write(c,0x80c,0);Write(c,0x810,1);Write(c,0x814,c->Depth);
 Write(c,0x7c,0);Write(c,0x48,0x21);Write(c,0x278,(5u<<20)|(11u<<10)|22u);
 InterlockedExchange(&c->Online,TRUE);return STATUS_SUCCESS;
}
static VOID Failure(WDFWORKITEM work){NFC*c=Context(WdfWorkItemGetParentObject(work));WdfDeviceSetFailed(c->Device,WdfDeviceFailedNoRestart);}
static VOID Fault(NFC*c){if(InterlockedExchange(&c->Online,FALSE))WdfWorkItemEnqueue(c->Work);}
static NTSTATUS Transfer(NFC*c,PUCHAR bytes,ULONG count,BOOLEAN receive){
 if(!c->Online)return STATUS_DEVICE_NOT_READY;
 if(Read(c,0x40)&1)return STATUS_DEVICE_BUSY;
 Write(c,0x618,0xffffffff);Write(c,receive?0x270:0x26c,count);Write(c,0x600,((receive?2u:1u)<<27)|(0x28u<<9));
 ULONG done=0;NTSTATUS status=STATUS_IO_TIMEOUT;ULONGLONG end=GetTickCount64()+200;
 do{
  ULONG irq=Read(c,0x610);
  if(irq&0x3300363e){status=STATUS_IO_DEVICE_ERROR;break;}
  if(receive){ULONG words=Read(c,0x804)&0x1ffffffu;while(words-- && done<count){ULONG v=Read(c,0x780);for(ULONG i=0;i<4 && done<count;i++)bytes[done++]=(UCHAR)(v>>(i*8));}}
  else{ULONG used=Read(c,0x800)&0xfffffffu;while(used++<c->Depth && done<count){ULONG v=0;for(ULONG i=0;i<4 && done<count;i++)v|=(ULONG)bytes[done++]<<(i*8);Write(c,0x700,v);}}
  if(irq&1){Write(c,0x618,irq);return done==count?STATUS_SUCCESS:STATUS_DEVICE_PROTOCOL_ERROR;}
  Write(c,0x618,irq);Sleep(0);
 }while(GetTickCount64()<end);
 Write(c,0x604,2);end=GetTickCount64()+100;while((Read(c,0x40)&1)&&GetTickCount64()<end)Sleep(1);Write(c,0x618,0xffffffff);return status;
}
static EVT_NFC_CX_WRITE_NCI_PACKET Send;
static VOID Send(WDFDEVICE device,WDFREQUEST request){
 NFC*c=Context(device);PUCHAR data;size_t bytes;NTSTATUS status=WdfRequestRetrieveInputBuffer(request,3,(PVOID*)&data,&bytes);
 if(NT_SUCCESS(status)){if(bytes>258 || data[2]+3u!=bytes)status=STATUS_INVALID_PARAMETER;else{WdfWaitLockAcquire(c->Lock,NULL);status=Transfer(c,data,(ULONG)bytes,FALSE);WdfWaitLockRelease(c->Lock);if(!NT_SUCCESS(status))Fault(c);}}
 WdfRequestCompleteWithInformation(request,status,NT_SUCCESS(status)?bytes:0);
}
static EVT_WDF_INTERRUPT_ISR Receive;
static BOOLEAN Receive(WDFINTERRUPT interrupt,ULONG message){
 UNREFERENCED_PARAMETER(message);WDFDEVICE device=WdfInterruptGetDevice(interrupt);NFC*c=Context(device);if(!c->Online)return TRUE;
 WdfWaitLockAcquire(c->Lock,NULL);NTSTATUS status=Transfer(c,c->Packet,3,TRUE);ULONG bytes=3;
 if(NT_SUCCESS(status)){bytes+=c->Packet[2];if(bytes>3)status=Transfer(c,c->Packet+3,bytes-3,TRUE);}WdfWaitLockRelease(c->Lock);
 if(NT_SUCCESS(status)){status=WdfMemoryAssignBuffer(c->Memory,c->Packet,bytes);if(NT_SUCCESS(status))status=NfcCxNciReadNotification(device,c->Memory);}
 if(!NT_SUCCESS(status))Fault(c);return TRUE;
}
static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST resources){
 UNREFERENCED_PARAMETER(resources);NFC*c=Context(device);c->Online=FALSE;WdfWorkItemFlush(c->Work);if(c->Base){WdfDeviceUnmapIoSpace(device,c->Base,0x4000);c->Base=NULL;}return STATUS_SUCCESS;
}
static NTSTATUS Prepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated){
 NFC*c=Context(device);WDF_INTERRUPT_CONFIG config;WDF_INTERRUPT_CONFIG_INIT(&config,Receive,NULL);config.PassiveHandling=TRUE;config.AutomaticSerialization=FALSE;
 for(ULONG i=0;i<WdfCmResourceListGetCount(translated);i++){
  PCM_PARTIAL_RESOURCE_DESCRIPTOR r=WdfCmResourceListGetDescriptor(translated,i);
  if(r->Type==CmResourceTypeMemory && r->u.Memory.Start.QuadPart==0x00a80000 && r->u.Memory.Length>=0x4000){NTSTATUS status=WdfDeviceMapIoSpace(device,r->u.Memory.Start,0x4000,MmNonCached,(PVOID*)&c->Base);if(!NT_SUCCESS(status))return status;}
  else if(r->Type==CmResourceTypeInterrupt){config.InterruptTranslated=r;config.InterruptRaw=WdfCmResourceListGetDescriptor(raw,i);}
 }
 if(!c->Base || !config.InterruptTranslated)return STATUS_DEVICE_CONFIGURATION_ERROR;
 WDFINTERRUPT interrupt;return WdfInterruptCreate(device,&config,WDF_NO_OBJECT_ATTRIBUTES,&interrupt);
}
static NTSTATUS Enter(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous){UNREFERENCED_PARAMETER(previous);return On(Context(device));}
static NTSTATUS Start(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous){UNREFERENCED_PARAMETER(previous);NFC_CX_HARDWARE_EVENT e={STATUS_SUCCESS,HostActionStart};return NfcCxHardwareEvent(device,&e);}
static NTSTATUS Stop(WDFDEVICE device,WDF_POWER_DEVICE_STATE next){UNREFERENCED_PARAMETER(next);NFC_CX_HARDWARE_EVENT e={STATUS_SUCCESS,HostActionStop};return NfcCxHardwareEvent(device,&e);}
static NTSTATUS Exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE next){UNREFERENCED_PARAMETER(next);Context(device)->Online=FALSE;return STATUS_SUCCESS;}
static NTSTATUS Add(WDFDRIVER driver,PWDFDEVICE_INIT init){
 UNREFERENCED_PARAMETER(driver);NFC_CX_CLIENT_CONFIG config;NFC_CX_CLIENT_CONFIG_INIT(&config,NFC_CX_TRANSPORT_I2C);config.EvtNfcCxWriteNciPacket=Send;
 config.DriverFlags=NFC_CX_DRIVER_ENABLE_EEPROM_WRITE_PROTECTION|NFC_CX_DRIVER_DISABLE_HOST_CARD_EMULATION|NFC_CX_DRIVER_DISABLE_NFCEE_DISCOVERY;
 NTSTATUS status=NfcCxDeviceInitConfig(init,&config);if(!NT_SUCCESS(status))return status;
 WDF_PNPPOWER_EVENT_CALLBACKS p;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);p.EvtDevicePrepareHardware=Prepare;p.EvtDeviceReleaseHardware=Release;p.EvtDeviceD0Entry=Enter;p.EvtDeviceD0EntryPostInterruptsEnabled=Start;p.EvtDeviceD0ExitPreInterruptsDisabled=Stop;p.EvtDeviceD0Exit=Exit;WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
 WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,NFC);a.SynchronizationScope=WdfSynchronizationScopeNone;a.ExecutionLevel=WdfExecutionLevelPassive;WDFDEVICE device;status=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(status))return status;
 NFC*c=Context(device);c->Device=device;WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=device;status=WdfWaitLockCreate(&a,&c->Lock);if(!NT_SUCCESS(status))return status;
 WDF_WORKITEM_CONFIG w;WDF_WORKITEM_CONFIG_INIT(&w,Failure);w.AutomaticSerialization=FALSE;status=WdfWorkItemCreate(&w,&a,&c->Work);if(!NT_SUCCESS(status))return status;
 status=WdfMemoryCreatePreallocated(&a,c->Packet,sizeof(c->Packet),&c->Memory);if(!NT_SUCCESS(status))return status;
 status=NfcCxDeviceInitialize(device);if(!NT_SUCCESS(status))return status;
 NFC_CX_RF_DISCOVERY_CONFIG rf;NFC_CX_RF_DISCOVERY_CONFIG_INIT(&rf);rf.PollConfig=NFC_CX_POLL_NFC_A|NFC_CX_POLL_NFC_B|NFC_CX_POLL_NFC_F_212|NFC_CX_POLL_NFC_F_424|NFC_CX_POLL_NFC_15693;rf.NfcIPMode=0;rf.NfcIPTgtMode=0;rf.NfcCEMode=0;return NfcCxSetRfDiscoveryConfig(device,&rf);
}
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path){WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,Add);return WdfDriverCreate(driver,path,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);}
