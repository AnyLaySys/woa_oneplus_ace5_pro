#include "adapter.h"

NTSTATUS A830KernelConnect(A830_ADAPTER *a) {
    UNICODE_STRING name;RtlInitUnicodeString(&name,L"\\Device\\A830Gpu");
    PDEVICE_OBJECT device=NULL;
    NTSTATUS status=IoGetDeviceObjectPointer(&name,FILE_READ_DATA,&a->HardwareFile,&device);
    if(!NT_SUCCESS(status))return status;
    KEVENT event;KeInitializeEvent(&event,NotificationEvent,FALSE);IO_STATUS_BLOCK io={0};
    PIRP irp=IoBuildDeviceIoControlRequest(A830_IOCTL_KERNEL_INTERFACE,device,NULL,0,
        &a->Kernel,sizeof(a->Kernel),TRUE,&event,&io);
    if(!irp)status=STATUS_INSUFFICIENT_RESOURCES;
    else {
        status=IoCallDriver(device,irp);
        if(status==STATUS_PENDING){KeWaitForSingleObject(&event,Executive,KernelMode,FALSE,NULL);status=io.Status;}
        if(NT_SUCCESS(status) && (io.Information!=sizeof(a->Kernel) || a->Kernel.Size!=sizeof(a->Kernel) ||
            a->Kernel.Version!=A830_KERNEL_INTERFACE_VERSION))status=STATUS_REVISION_MISMATCH;
        if(NT_SUCCESS(status))status=a->Kernel.Start();
    }
    if(!NT_SUCCESS(status))A830KernelDisconnect(a);
    return status;
}
VOID A830KernelDisconnect(A830_ADAPTER *a) {
    RtlZeroMemory(&a->Kernel,sizeof(a->Kernel));
    if(a->HardwareFile){ObDereferenceObject(a->HardwareFile);a->HardwareFile=NULL;}
}
NTSTATUS A830Map(A830_ADAPTER *a,GPU_BO *buffer,ULONGLONG address) {
    return a->RenderOnly?a->Kernel.Map(buffer,address):a->BridgeAttached?A830BridgeMap(buffer,address):GpuMap(&a->Hardware,0,buffer,address);
}
