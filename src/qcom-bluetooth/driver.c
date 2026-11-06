#include "bluetooth.h"
#include "h4.h"
static EVT_WDF_DRIVER_DEVICE_ADD QbtAdd;
static EVT_WDF_DEVICE_PREPARE_HARDWARE QbtPrepare;
static EVT_WDF_DEVICE_RELEASE_HARDWARE QbtRelease;
static EVT_WDF_DEVICE_D0_ENTRY QbtD0Entry;
static EVT_WDF_DEVICE_D0_EXIT QbtD0Exit;
static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL QbtIo, QbtChildIo;
static EVT_WDF_DPC QbtDpc;
static EVT_WDF_TIMER QbtWatchdog;
static NTSTATUS Queue(WDFDEVICE device,WDFQUEUE*q){
 WDF_IO_QUEUE_CONFIG config;WDF_IO_QUEUE_CONFIG_INIT(&config,WdfIoQueueDispatchManual);config.PowerManaged=WdfFalse;return WdfIoQueueCreate(device,&config,WDF_NO_OBJECT_ATTRIBUTES,q);
}
static VOID FreePackets(QBT*c){
 for(ULONG i=0;i<2;i++)while(!IsListEmpty(c->Packets+i)){PLIST_ENTRY e=RemoveHeadList(c->Packets+i);ExFreePoolWithTag(CONTAINING_RECORD(e,QBT_PACKET,Link),QBT_TAG);}
 c->PacketCount=0;
}
static VOID CompleteWrite(WDFREQUEST request,NTSTATUS status){
 ULONG_PTR info=0;if(NT_SUCCESS(status)){PULONG out;status=WdfRequestRetrieveOutputBuffer(request,sizeof(ULONG),(PVOID*)&out,NULL);if(NT_SUCCESS(status)){*out=QbtRequest(request)->Length-1;info=sizeof(ULONG);}}
 WdfRequestCompleteWithInformation(request,status,info);
}
static ULONG Take(QBT*c,UCHAR*data,ULONG wanted){
 WdfInterruptAcquireLock(c->Interrupt);ULONG count=0;
 while(count<wanted&&c->RingRead!=c->RingWrite){data[count++]=c->Ring[c->RingRead];c->RingRead=(c->RingRead+1)&(QBT_RING_SIZE-1);}
 if(c->Paused&&((c->RingRead-c->RingWrite-1)&(QBT_RING_SIZE-1))>=1024){QbtWrite(c->U,0x2ac,0);c->Paused=FALSE;}
 WdfInterruptReleaseLock(c->Interrupt);return count;
}
static VOID Deliver(QBT*c){
 for(ULONG type=0;type<2;type++)while(!IsListEmpty(c->Packets+type)){
  WDFREQUEST request;if(!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(c->Reads[type],&request)))break;
  QBT_PACKET*p=CONTAINING_RECORD(c->Packets[type].Flink,QBT_PACKET,Link);
  PBTHX_HCI_READ_WRITE_CONTEXT out;NTSTATUS status=WdfRequestRetrieveOutputBuffer(request,FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT,Data)+p->Length,(PVOID*)&out,NULL);
  if(NT_SUCCESS(status)){out->DataLen=p->Length;out->Type=type?HciPacketAclData:HciPacketEvent;RtlCopyMemory(out->Data,p->Data,p->Length);RemoveEntryList(&p->Link);c->PacketCount--;ULONG_PTR n=FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT,Data)+p->Length;ExFreePoolWithTag(p,QBT_TAG);WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,n);}
  else WdfRequestComplete(request,status);
 }
}
static VOID Parse(QBT*c){
 while(c->PacketCount<QBT_RX_LIMIT&&!c->Fault){
  ULONG need=QbtWireLength(c->Frame,c->FrameHave);
  if(!need){InterlockedCompareExchange(&c->Fault,STATUS_DEVICE_PROTOCOL_ERROR,0);break;}
  ULONG n=Take(c,c->Frame+c->FrameHave,need-c->FrameHave);if(!n)break;c->FrameHave+=n;
  if(c->FrameHave!=QbtWireLength(c->Frame,c->FrameHave))continue;
  UCHAR type=c->Frame[0];
  if(type==0xfd)c->AckPending=TRUE;
  QbtIbsReceived(&c->Ibs,type,KeQueryInterruptTime());
  if(type==4||type==2){
   /* 0x2edc is the controller's diagnostic ACL handle, not a connection. */
   if(type!=2||c->Frame[1]!=0xdc||c->Frame[2]!=0x2e){
    ULONG length=need-1;QBT_PACKET*p=ExAllocatePool2(POOL_FLAG_NON_PAGED,FIELD_OFFSET(QBT_PACKET,Data)+length,QBT_TAG);
    if(!p){InterlockedCompareExchange(&c->Fault,STATUS_INSUFFICIENT_RESOURCES,0);break;}
    p->Length=length;RtlCopyMemory(p->Data,c->Frame+1,length);InsertTailList(c->Packets+(type==2),&p->Link);c->PacketCount++;
   }
  }
  c->FrameHave=0;
 }
}
static VOID StartWire(QBT*c,PUCHAR bytes,ULONG length,WDFREQUEST request){
 c->TxBytes=bytes;c->TxLength=length;c->TxAt=0;c->TxRequest=request;c->TxDone=FALSE;c->TxDeadline=KeQueryInterruptTime()+20000000;
 QbtWrite(c->U,0x618,0xffffffff);QbtWrite(c->U,0x270,length);QbtWrite(c->U,0x600,0x08000000);QbtRuntimeFill(c);
}
static VOID QbtDpc(WDFDPC dpc){
 QBT*c=QbtDevice(WdfDpcGetParentObject(dpc));WdfSpinLockAcquire(c->WorkLock);
 if(!c->Online){WdfSpinLockRelease(c->WorkLock);return;}
 WDFREQUEST complete=NULL;WdfInterruptAcquireLock(c->Interrupt);
 if(c->TxDone){c->Ibs.ChangedAt=KeQueryInterruptTime();complete=c->TxRequest;c->TxRequest=NULL;c->TxBytes=NULL;c->TxDone=FALSE;}
 WdfInterruptReleaseLock(c->Interrupt);if(complete)CompleteWrite(complete,c->Fault?c->Fault:STATUS_SUCCESS);
 if(!c->Fault){Deliver(c);Parse(c);Deliver(c);}
 if(c->Fault){
  InterlockedExchange(&c->Online,0);WdfInterruptAcquireLock(c->Interrupt);QbtIrqOff(c);if(c->TxBytes)QbtWrite(c->U,0x604,2);complete=c->TxRequest;c->TxRequest=NULL;c->TxBytes=NULL;WdfInterruptReleaseLock(c->Interrupt);
  if(complete)CompleteWrite(complete,c->Fault);
  if(c->PendingWrite){CompleteWrite(c->PendingWrite,c->Fault);c->PendingWrite=NULL;}
  WdfIoQueuePurge(c->Writes,NULL,NULL);WdfIoQueuePurge(c->Reads[0],NULL,NULL);WdfIoQueuePurge(c->Reads[1],NULL,NULL);
  WdfDeviceSetFailed(c->Device,WdfDeviceFailedNoRestart);
  WdfSpinLockRelease(c->WorkLock);return;
 }
 WdfInterruptAcquireLock(c->Interrupt);BOOLEAN idle=c->TxBytes==NULL;WdfInterruptReleaseLock(c->Interrupt);
 if(idle){
  if(!c->PendingWrite)WdfIoQueueRetrieveNextRequest(c->Writes,&c->PendingWrite);
  UCHAR control=0;
  if(c->AckPending){c->AckPending=FALSE;control=0xfc;}
  else control=QbtIbsAction(&c->Ibs,KeQueryInterruptTime(),c->PendingWrite!=NULL);
  if(control==0xff){InterlockedCompareExchange(&c->Fault,STATUS_IO_TIMEOUT,0);WdfDpcEnqueue(c->Dpc);}
  else if(control||c->PendingWrite&&c->Ibs.State==0){
   WdfInterruptAcquireLock(c->Interrupt);
   if(c->Online){if(control){c->AckByte=control;StartWire(c,&c->AckByte,1,NULL);}else {QBT_REQUEST*r=QbtRequest(c->PendingWrite);StartWire(c,r->Bytes,r->Length,c->PendingWrite);c->PendingWrite=NULL;}}
   WdfInterruptReleaseLock(c->Interrupt);
  }
 }
 WdfSpinLockRelease(c->WorkLock);
}
static VOID QbtWatchdog(WDFTIMER timer){
 QBT*c=QbtDevice(WdfTimerGetParentObject(timer));if(!c->Online)return;
 WdfSpinLockAcquire(c->WorkLock);ULONGLONG now=KeQueryInterruptTime();
 WdfInterruptAcquireLock(c->Interrupt);BOOLEAN expired=c->TxBytes&&now>=c->TxDeadline;BOOLEAN idle=c->TxBytes==NULL;WdfInterruptReleaseLock(c->Interrupt);
 if(expired)InterlockedCompareExchange(&c->Fault,STATUS_IO_TIMEOUT,0);
 BOOLEAN service=expired||idle&&(c->Ibs.State==2||c->Ibs.State==0&&now-c->Ibs.ChangedAt>=QBT_IBS_IDLE_TICKS);
 WdfSpinLockRelease(c->WorkLock);if(service)WdfDpcEnqueue(c->Dpc);
}
static VOID QbtIo(WDFQUEUE queue,WDFREQUEST request,size_t outLength,size_t inLength,ULONG code){
 QBT*c=QbtDevice(WdfIoQueueGetDevice(queue));PVOID in=NULL,out=NULL;NTSTATUS status=STATUS_INVALID_PARAMETER;size_t offset=FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT,Data);
 if(WdfRequestGetRequestorMode(request)!=KernelMode){WdfRequestComplete(request,STATUS_ACCESS_DENIED);return;}
 if(inLength){status=WdfRequestRetrieveInputBuffer(request,inLength,&in,NULL);if(!NT_SUCCESS(status))goto done;}
 if(outLength){status=WdfRequestRetrieveOutputBuffer(request,outLength,&out,NULL);if(!NT_SUCCESS(status))goto done;}
 if(code==IOCTL_BTHX_GET_VERSION){if(out&&outLength>=sizeof(BTHX_VERSION)){((PBTHX_VERSION)out)->Version=BTHX_DDI_VERSION_1;WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(BTHX_VERSION));return;}goto invalid;}
 if(code==IOCTL_BTHX_SET_VERSION){status=in&&inLength>=sizeof(BTHX_VERSION)&&((PBTHX_VERSION)in)->Version==BTHX_DDI_VERSION_1?STATUS_SUCCESS:STATUS_NOT_SUPPORTED;goto done;}
 if(code==IOCTL_BTHX_QUERY_CAPABILITIES){
  if(!out||outLength<sizeof(BTHX_CAPABILITIES))goto invalid;
  PBTHX_CAPABILITIES caps=out;RtlZeroMemory(caps,sizeof(*caps));caps->MaxAclTransferInSize=QBT_PACKET_MAX;caps->ScoSupport=ScoSupportHCIBypass;caps->MaxScoChannels=1;WdfRequestCompleteWithInformation(request,STATUS_SUCCESS,sizeof(*caps));return;
 }
 if(!c->Online){status=STATUS_DEVICE_POWERED_OFF;goto done;}
 if(code==IOCTL_BTHX_READ_HCI){
  if(!in||inLength!=sizeof(BTHX_HCI_PACKET_TYPE)||!out||outLength<sizeof(BTHX_HCI_READ_WRITE_CONTEXT))goto invalid;
  BTHX_HCI_PACKET_TYPE type=*(BTHX_HCI_PACKET_TYPE*)in;if(type!=HciPacketEvent&&type!=HciPacketAclData)goto invalid;
  status=WdfRequestForwardToIoQueue(request,c->Reads[type==HciPacketAclData]);if(NT_SUCCESS(status)){WdfDpcEnqueue(c->Dpc);return;}goto done;
 }
 if(code==IOCTL_BTHX_WRITE_HCI){
  if(!in||inLength<offset||!out||outLength!=sizeof(ULONG))goto invalid;
  PBTHX_HCI_READ_WRITE_CONTEXT h=in;ULONG n=h->DataLen;
  if(n>QBT_PACKET_MAX||n>inLength-offset)goto invalid;
  if(h->Type==HciPacketCommand){if(n<3||n!=(ULONG)h->Data[2]+3)goto invalid;}
  else if(h->Type==HciPacketAclData){if(n<4||n!=(ULONG)h->Data[2]+((ULONG)h->Data[3]<<8)+4)goto invalid;}
  else goto invalid;
  WDF_OBJECT_ATTRIBUTES attrs;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs,QBT_REQUEST);status=WdfObjectAllocateContext(request,&attrs,NULL);if(!NT_SUCCESS(status))goto done;
  WDF_OBJECT_ATTRIBUTES_INIT(&attrs);attrs.ParentObject=request;WDFMEMORY memory;QBT_REQUEST*r=QbtRequest(request);
  status=WdfMemoryCreate(&attrs,NonPagedPoolNx,QBT_TAG,n+1,&memory,(PVOID*)&r->Bytes);if(!NT_SUCCESS(status))goto done;
  r->Length=n+1;r->Bytes[0]=h->Type;RtlCopyMemory(r->Bytes+1,h->Data,n);
  status=WdfRequestForwardToIoQueue(request,c->Writes);if(NT_SUCCESS(status)){WdfDpcEnqueue(c->Dpc);return;}goto done;
 }
 status=STATUS_INVALID_DEVICE_REQUEST;goto done;
 invalid:status=STATUS_INVALID_PARAMETER;
 done:WdfRequestComplete(request,status);
}
static VOID QbtChildIo(WDFQUEUE queue,WDFREQUEST request,size_t outLength,size_t inLength,ULONG code){
 UNREFERENCED_PARAMETER(outLength);UNREFERENCED_PARAMETER(inLength);UNREFERENCED_PARAMETER(code);
 WDF_REQUEST_FORWARD_OPTIONS options;WDF_REQUEST_FORWARD_OPTIONS_INIT(&options);options.Flags=WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET;
 WDFDEVICE parent=WdfPdoGetParent(WdfIoQueueGetDevice(queue));NTSTATUS status=WdfRequestForwardToParentDeviceIoQueue(request,WdfDeviceGetDefaultQueue(parent),&options);if(!NT_SUCCESS(status))WdfRequestComplete(request,status);
}
static NTSTATUS Child(WDFDEVICE parent){
 PWDFDEVICE_INIT init=WdfPdoInitAllocate(parent);if(!init)return STATUS_INSUFFICIENT_RESOURCES;
 DECLARE_CONST_UNICODE_STRING(id,L"AnyLaySys\\WCN7861_BLUETOOTH");DECLARE_CONST_UNICODE_STRING(compatible,L"MS_BTHX_BTHMINI");DECLARE_CONST_UNICODE_STRING(instance,L"0");DECLARE_CONST_UNICODE_STRING(text,L"Qualcomm WCN7861 Bluetooth");DECLARE_CONST_UNICODE_STRING(location,L"Qualcomm GENI UART");
 NTSTATUS status=WdfPdoInitAssignDeviceID(init,&id);if(!NT_SUCCESS(status))goto failed;
 status=WdfPdoInitAddHardwareID(init,&id);if(!NT_SUCCESS(status))goto failed;status=WdfPdoInitAddCompatibleID(init,&compatible);if(!NT_SUCCESS(status))goto failed;
 status=WdfPdoInitAssignInstanceID(init,&instance);if(!NT_SUCCESS(status))goto failed;status=WdfPdoInitAddDeviceText(init,&text,&location,0x409);if(!NT_SUCCESS(status))goto failed;
 WdfPdoInitSetDefaultLocale(init,0x409);WdfPdoInitAllowForwardingRequestToParent(init);WdfDeviceInitSetDeviceType(init,FILE_DEVICE_BLUETOOTH);
 WDFDEVICE child;status=WdfDeviceCreate(&init,WDF_NO_OBJECT_ATTRIBUTES,&child);if(!NT_SUCCESS(status))goto failed;
 WDF_DEVICE_PNP_CAPABILITIES pnp;WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnp);pnp.Removable=WdfFalse;pnp.SurpriseRemovalOK=WdfFalse;pnp.UniqueID=WdfTrue;pnp.Address=0;pnp.UINumber=0;WdfDeviceSetPnpCapabilities(child,&pnp);
 WDF_DEVICE_POWER_CAPABILITIES power;WDF_DEVICE_POWER_CAPABILITIES_INIT(&power);power.DeviceD1=WdfFalse;power.DeviceD2=WdfFalse;power.DeviceState[PowerSystemWorking]=PowerDeviceD0;for(ULONG i=PowerSystemSleeping1;i<PowerSystemMaximum;i++)power.DeviceState[i]=PowerDeviceD3;WdfDeviceSetPowerCapabilities(child,&power);
 WDF_IO_QUEUE_CONFIG config;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config,WdfIoQueueDispatchParallel);config.PowerManaged=WdfFalse;config.EvtIoDeviceControl=QbtChildIo;config.EvtIoInternalDeviceControl=QbtChildIo;
 status=WdfIoQueueCreate(child,&config,WDF_NO_OBJECT_ATTRIBUTES,NULL);if(!NT_SUCCESS(status)){WdfObjectDelete(child);return status;}
 status=WdfFdoAddStaticChild(parent,child);if(!NT_SUCCESS(status))WdfObjectDelete(child);return status;
 failed:if(init)WdfDeviceInitFree(init);return status;
}
static NTSTATUS QbtPrepare(WDFDEVICE device,WDFCMRESLIST raw,WDFCMRESLIST translated){
 UNREFERENCED_PARAMETER(raw);QBT*c=QbtDevice(device);NTSTATUS status=QbtMap(c,translated);if(!NT_SUCCESS(status))QbtUnmap(c);return status;
}
static NTSTATUS QbtRelease(WDFDEVICE device,WDFCMRESLIST resources){UNREFERENCED_PARAMETER(resources);QbtUnmap(QbtDevice(device));return STATUS_SUCCESS;}
static NTSTATUS QbtD0Entry(WDFDEVICE device,WDF_POWER_DEVICE_STATE previous){
 UNREFERENCED_PARAMETER(previous);QBT*c=QbtDevice(device);c->Fault=0;c->RingRead=c->RingWrite=0;c->FrameHave=0;c->Paused=FALSE;c->AckPending=FALSE;c->TxDone=FALSE;
 c->Ibs.State=0;
 WdfIoQueueStart(c->Writes);WdfIoQueueStart(c->Reads[0]);WdfIoQueueStart(c->Reads[1]);NTSTATUS status=QbtStart(c);if(!NT_SUCCESS(status)){QbtStop(c);return status;}c->Ibs.ChangedAt=KeQueryInterruptTime();WdfTimerStart(c->Timer,WDF_REL_TIMEOUT_IN_MS(100));return STATUS_SUCCESS;
}
static NTSTATUS QbtD0Exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE target){
 UNREFERENCED_PARAMETER(target);QBT*c=QbtDevice(device);InterlockedExchange(&c->Online,0);WdfTimerStop(c->Timer,TRUE);WdfDpcCancel(c->Dpc,TRUE);
 if(c->TxRequest){CompleteWrite(c->TxRequest,STATUS_DEVICE_POWERED_OFF);c->TxRequest=NULL;}c->TxBytes=NULL;if(c->PendingWrite){CompleteWrite(c->PendingWrite,STATUS_DEVICE_POWERED_OFF);c->PendingWrite=NULL;}
 WdfIoQueuePurgeSynchronously(c->Writes);WdfIoQueuePurgeSynchronously(c->Reads[0]);WdfIoQueuePurgeSynchronously(c->Reads[1]);FreePackets(c);QbtStop(c);return STATUS_SUCCESS;
}
static NTSTATUS QbtAdd(WDFDRIVER driver,PWDFDEVICE_INIT init){
 UNREFERENCED_PARAMETER(driver);WdfDeviceInitSetDeviceType(init,FILE_DEVICE_BUS_EXTENDER);
 WDF_PNPPOWER_EVENT_CALLBACKS pnp;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);pnp.EvtDevicePrepareHardware=QbtPrepare;pnp.EvtDeviceReleaseHardware=QbtRelease;pnp.EvtDeviceD0Entry=QbtD0Entry;pnp.EvtDeviceD0Exit=QbtD0Exit;WdfDeviceInitSetPnpPowerEventCallbacks(init,&pnp);
 WDF_OBJECT_ATTRIBUTES attrs;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs,QBT);WDFDEVICE device;NTSTATUS status=WdfDeviceCreate(&init,&attrs,&device);if(!NT_SUCCESS(status))return status;
 QBT*c=QbtDevice(device);c->Device=device;InitializeListHead(c->Packets);InitializeListHead(c->Packets+1);
 WDF_OBJECT_ATTRIBUTES_INIT(&attrs);attrs.ParentObject=device;status=WdfSpinLockCreate(&attrs,&c->WorkLock);if(!NT_SUCCESS(status))return status;
 WDF_IO_QUEUE_CONFIG queue;WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue,WdfIoQueueDispatchParallel);queue.PowerManaged=WdfFalse;queue.EvtIoDeviceControl=QbtIo;queue.EvtIoInternalDeviceControl=QbtIo;status=WdfIoQueueCreate(device,&queue,WDF_NO_OBJECT_ATTRIBUTES,NULL);if(!NT_SUCCESS(status))return status;
 status=Queue(device,&c->Writes);if(!NT_SUCCESS(status))return status;for(ULONG i=0;i<2;i++){status=Queue(device,c->Reads+i);if(!NT_SUCCESS(status))return status;}
 WDF_DPC_CONFIG dpc;WDF_DPC_CONFIG_INIT(&dpc,QbtDpc);dpc.AutomaticSerialization=FALSE;status=WdfDpcCreate(&dpc,&attrs,&c->Dpc);if(!NT_SUCCESS(status))return status;
 WDF_TIMER_CONFIG timer;WDF_TIMER_CONFIG_INIT_PERIODIC(&timer,QbtWatchdog,100);timer.AutomaticSerialization=FALSE;status=WdfTimerCreate(&timer,&attrs,&c->Timer);if(!NT_SUCCESS(status))return status;
 WDF_INTERRUPT_CONFIG interrupt;WDF_INTERRUPT_CONFIG_INIT(&interrupt,QbtIsr,NULL);interrupt.EvtInterruptEnable=QbtInterruptEnable;interrupt.EvtInterruptDisable=QbtInterruptDisable;status=WdfInterruptCreate(device,&interrupt,WDF_NO_OBJECT_ATTRIBUTES,&c->Interrupt);if(!NT_SUCCESS(status))return status;
 return Child(device);
}
DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object,PUNICODE_STRING registry){WDF_DRIVER_CONFIG config;WDF_DRIVER_CONFIG_INIT(&config,QbtAdd);return WdfDriverCreate(object,registry,WDF_NO_OBJECT_ATTRIBUTES,&config,WDF_NO_HANDLE);}
