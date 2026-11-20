#pragma once
#include <ntddk.h>
#include <acpiioct.h>
static NTSTATUS PlatformVote(PDEVICE_OBJECT device,ULONG mask) {
 ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER in={0};ACPI_EVAL_OUTPUT_BUFFER out={0};
 in.Signature=ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE;RtlCopyMemory(in.MethodName,"QVOT",4);in.IntegerArgument=mask;
 KEVENT event;IO_STATUS_BLOCK io;KeInitializeEvent(&event,NotificationEvent,FALSE);
 PIRP irp=IoBuildDeviceIoControlRequest(IOCTL_ACPI_EVAL_METHOD,device,&in,sizeof(in),&out,sizeof(out),FALSE,&event,&io);
 if(!irp)return STATUS_INSUFFICIENT_RESOURCES;
 NTSTATUS status=IoCallDriver(device,irp);if(status==STATUS_PENDING){KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL);status=io.Status;}
 return status;
}
