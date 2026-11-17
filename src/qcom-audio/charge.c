#include <ntifs.h>
#include <wdmsec.h>
#include "sia9177.h"
#include "..\qcom-battery\power.h"
static HANDLE InputKey;
static BOOLEAN InputSaved;
BOOLEAN QchInputSaved(VOID){return InputSaved;}
NTSTATUS QchInputSave(BOOLEAN Suspended){
    UNICODE_STRING Name=RTL_CONSTANT_STRING(L"InputSuspended");ULONG Value=Suspended;
    NTSTATUS Status=InputKey?ZwSetValueKey(InputKey,&Name,0,REG_DWORD,&Value,sizeof(Value)):STATUS_DEVICE_NOT_READY;
    if(NT_SUCCESS(Status))InputSaved=Suspended;return Status;
}
static KSPIN_LOCK Lock;
static PDEVICE_OBJECT Control;
static BOOLEAN ControlDeleted;
static PDRIVER_DISPATCH Prev[IRP_MJ_MAXIMUM_FUNCTION+1];
static PDRIVER_UNLOAD PrevUnload;
static UNICODE_STRING CredentialPath;
static QCH_LINK Snapshot;
static UCHAR AuthKey[16];
static BOOLEAN AuthReady;
BOOLEAN QchAuthAvailable(VOID) {KIRQL I;KeAcquireSpinLock(&Lock,&I);BOOLEAN R=AuthReady;KeReleaseSpinLock(&Lock,I);return R;}
VOID QchDiscardAuth(VOID) {KIRQL I;KeAcquireSpinLock(&Lock,&I);RtlSecureZeroMemory(AuthKey,sizeof(AuthKey));AuthReady=FALSE;KeReleaseSpinLock(&Lock,I);}
BOOLEAN QchTakeAuth(UCHAR *Key) {
    KIRQL I;KeAcquireSpinLock(&Lock,&I);BOOLEAN R=AuthReady;
    if(R){RtlCopyMemory(Key,AuthKey,sizeof(AuthKey));RtlSecureZeroMemory(AuthKey,sizeof(AuthKey));AuthReady=FALSE;}
    KeReleaseSpinLock(&Lock,I);return R;
}
VOID QchPublish(const QCH_LINK *C){KIRQL I;KeAcquireSpinLock(&Lock,&I);Snapshot=*C;KeReleaseSpinLock(&Lock,I);}
static const GUID BatteryGuid={0x72631e54,0x78a4,0x11d0,{0xbc,0xf7,0x00,0xaa,0x00,0xb7,0xb3,0x2a}};
static NTSTATUS BatteryIo(PDEVICE_OBJECT Device,ULONG Code,VOID *In,ULONG InSize,VOID *Out,ULONG OutSize)
{
    KEVENT Event;IO_STATUS_BLOCK Io;LARGE_INTEGER Timeout;
    KeInitializeEvent(&Event,NotificationEvent,FALSE);
    PIRP Irp=IoBuildDeviceIoControlRequest(Code,Device,In,InSize,Out,OutSize,TRUE,&Event,&Io);
    if(!Irp) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS Status=IoCallDriver(Device,Irp);
    if(Status==STATUS_PENDING) {
        Timeout.QuadPart=-10000000LL;
        Status=KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,&Timeout);
        if(Status==STATUS_TIMEOUT) { IoCancelIrp(Irp);KeWaitForSingleObject(&Event,Executive,KernelMode,FALSE,NULL);return STATUS_IO_TIMEOUT; }
        Status=Io.Status;
    }
    return Status;
}
BOOLEAN QchBatteryGuard(QCH_LINK *C)
{
    PWSTR Names=NULL;PFILE_OBJECT File=NULL;PDEVICE_OBJECT Device=NULL;UNICODE_STRING Name;
    QCB_SOURCE Source={C->OnlineAt,1,C->UsbOnline};QCB_POWER P={0};
    ULONGLONG Now=KeQueryInterruptTime()/10000;
    Source.Valid=C->OnlineAt<=Now && Now-C->OnlineAt<=QCB_POWER_MAX_AGE_MS;
    NTSTATUS Status=IoGetDeviceInterfaces(&BatteryGuid,NULL,0,&Names);
    if(NT_SUCCESS(Status)) {
        if(!Names[0]) Status=STATUS_DEVICE_NOT_READY;
        else {
            RtlInitUnicodeString(&Name,Names);
            Status=IoGetDeviceObjectPointer(&Name,FILE_READ_DATA,&File,&Device);
            if(NT_SUCCESS(Status)) {
                Status=BatteryIo(Device,QCB_POWER_EXCHANGE,&Source,sizeof(Source),&P,sizeof(P));

                ObDereferenceObject(File);
            }
        }
        ExFreePool(Names);
    }
    C->CellValid=P.CellValid==1 && P.Cell1Mv>=2500 && P.Cell1Mv<=5000 && P.Cell2Mv>=2500 && P.Cell2Mv<=5000 &&
        P.Cell1Mv+P.Cell2Mv+100>=P.VoltageMv && P.Cell1Mv+P.Cell2Mv<=P.VoltageMv+100;C->Cell1Mv=P.Cell1Mv;C->Cell2Mv=P.Cell2Mv;
    C->BatteryStatus=P.BatteryStatus;C->GaugeCvMv=P.ChargingVoltageMv;C->GaugeCcMa=P.ChargingCurrentMa;
    C->GuardAt=Now;
    if(NT_SUCCESS(Status)) {
        C->SampleTimeMs=P.SampleTimeMs;C->Temperature=P.TemperatureDeciK;C->PackVoltage=P.VoltageMv;
        C->PackCurrent=(ULONG)P.CurrentMa;C->Remaining=P.RemainingMah;C->Soc=P.Soc;
    }
    C->BatteryValid=NT_SUCCESS(Status) && P.Valid && P.SampleTimeMs && P.SampleTimeMs<=Now && Now-P.SampleTimeMs<=QCB_POWER_MAX_AGE_MS;
    C->GuardGood=C->BatteryValid &&
        P.TemperatureDeciK<ALS_CHARGE_TEMP_LIMIT_DK && C->CellValid && P.VoltageMv<=9200 && P.Soc<=100;
    return C->GuardGood;
}

static ULONG Age(ULONGLONG Now,ULONGLONG At){return At&&At<=Now?(ULONG)min(Now-At,0xffffffffull):0xffffffffu;}
static NTSTATUS Dispatch(PDEVICE_OBJECT Device,PIRP Irp)
{
    PIO_STACK_LOCATION S=IoGetCurrentIrpStackLocation(Irp);
    if(Device!=Control)return Prev[S->MajorFunction](Device,Irp);
    if(S->MajorFunction==IRP_MJ_CLEANUP)QcomRpcDetach(S->FileObject);
    NTSTATUS Status=STATUS_INVALID_DEVICE_REQUEST;ULONG Bytes=0;
    if(S->MajorFunction==IRP_MJ_CREATE||S->MajorFunction==IRP_MJ_CLOSE||S->MajorFunction==IRP_MJ_CLEANUP)Status=STATUS_SUCCESS;
    else if(S->MajorFunction==IRP_MJ_DEVICE_CONTROL && S->Parameters.DeviceIoControl.IoControlCode==ALS_CHARGE_STATUS){
        if(S->Parameters.DeviceIoControl.InputBufferLength)Status=STATUS_INVALID_PARAMETER;
        else if(S->Parameters.DeviceIoControl.OutputBufferLength<ALS_CHARGE_STATUS_BASE_SIZE)Status=STATUS_BUFFER_TOO_SMALL;
        else{
            KIRQL I;KeAcquireSpinLock(&Lock,&I);QCH_LINK C=Snapshot;BOOLEAN Ready=AuthReady;KeReleaseSpinLock(&Lock,I);
            ULONGLONG Now=KeQueryInterruptTime()/10000;
            ALS_CHARGE_STATUS_DATA V={ALS_CHARGE_ABI,12,C.Bridge,C.Phase,C.Running,C.Fault,C.Command||C.Bridge==1||C.Waiting||(C.InputState&&C.InputState!=7),Ready,C.AuthVerified,C.UsbOnline,C.UsbVoltage/1000,C.SourceEnabled,C.SourceRole,C.Moisture,C.AdapterType,Age(Now,C.OnlineAt),Age(Now,C.VoltageAt),Age(Now,C.SourceAt),Age(Now,C.RoleAt),Age(Now,C.MoistureAt),Age(Now,C.SampleTimeMs),C.GuardGood,C.CellValid,C.Temperature,C.PackVoltage,C.PackCurrent,C.Soc,C.Remaining,C.Cell1Mv,C.Cell2Mv,C.BatteryStatus,C.LoadMos,C.CpActive,C.CpOffVerified,C.CpTouched,C.CpError,C.LoadCv,C.LoadStopReason,C.LoadFvLimitMv,C.LoadReqMv,C.LoadReqMa,C.PortOnline,C.PortVoltage/1000,C.PortRole,C.PortMoisture,Age(Now,C.PortAt),C.OtgPhase,C.OtgFault,C.UfcsStarted,C.PeerRunning,C.SourceLo,Age(Now,C.InfoAt),C.InputState,C.InputError,C.InputResult,C.UsbLimitUa,Age(Now,C.InputAt),C.CableOrientation,Age(Now,C.CableAt)};
            Bytes=S->Parameters.DeviceIoControl.OutputBufferLength>=sizeof(V)?sizeof(V):ALS_CHARGE_STATUS_BASE_SIZE;
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer,&V,Bytes);Status=STATUS_SUCCESS;
        }
    }
    if(S->MajorFunction==IRP_MJ_DEVICE_CONTROL && (S->Parameters.DeviceIoControl.IoControlCode==QCOM_LIGHT_READ || S->Parameters.DeviceIoControl.IoControlCode==QCOM_ORIENTATION_READ)){
        if(S->Parameters.DeviceIoControl.InputBufferLength)Status=STATUS_INVALID_PARAMETER;
        else if(S->Parameters.DeviceIoControl.OutputBufferLength<sizeof(QCOM_LIGHT))Status=STATUS_BUFFER_TOO_SMALL;
        else {if(S->Parameters.DeviceIoControl.IoControlCode==QCOM_LIGHT_READ)QcomLightQuery(Irp->AssociatedIrp.SystemBuffer);else QcomOrientationQuery(Irp->AssociatedIrp.SystemBuffer);Bytes=sizeof(QCOM_LIGHT);Status=STATUS_SUCCESS;}
    }
    if(S->MajorFunction==IRP_MJ_DEVICE_CONTROL && S->Parameters.DeviceIoControl.IoControlCode==QH_VOLUME) {
        if(S->Parameters.DeviceIoControl.InputBufferLength)Status=STATUS_INVALID_PARAMETER;
        else if(S->Parameters.DeviceIoControl.OutputBufferLength<sizeof(ULONG))Status=STATUS_BUFFER_TOO_SMALL;
        else {*(ULONG*)Irp->AssociatedIrp.SystemBuffer=SiaVolumeLimits();Bytes=sizeof(ULONG);Status=STATUS_SUCCESS;}
    }
    if(S->MajorFunction==IRP_MJ_DEVICE_CONTROL && S->Parameters.DeviceIoControl.IoControlCode==QCOM_RPC_INVOKE)Status=QcomRpcInvoke(Irp,&Bytes);
    Irp->IoStatus.Status=Status;Irp->IoStatus.Information=Bytes;IoCompleteRequest(Irp,IO_NO_INCREMENT);return Status;
}
VOID QchRemove(VOID)
{
    QchDiscardAuth();
    if(Control && !ControlDeleted) {
        UNICODE_STRING Link=RTL_CONSTANT_STRING(L"\\DosDevices\\AlsCharge");
        IoDeleteSymbolicLink(&Link);ControlDeleted=TRUE;IoDeleteDevice(Control);
    }
}
static VOID Unload(PDRIVER_OBJECT Driver)
{
    QchDiscardAuth();
    UNICODE_STRING Link=RTL_CONSTANT_STRING(L"\\DosDevices\\AlsCharge");
    IoDeleteSymbolicLink(&Link);
    if(Control && !ControlDeleted) IoDeleteDevice(Control);
    Control=NULL;
    if(InputKey){ZwClose(InputKey);InputKey=NULL;}
    if(CredentialPath.Buffer){ExFreePoolWithTag(CredentialPath.Buffer,'aChQ');RtlZeroMemory(&CredentialPath,sizeof(CredentialPath));}
    if(PrevUnload) PrevUnload(Driver);
}
BOOLEAN QchLoadAuth(VOID)
{
    if(!CredentialPath.Buffer || KeGetCurrentIrql()!=PASSIVE_LEVEL)return FALSE;
    OBJECT_ATTRIBUTES Attributes;IO_STATUS_BLOCK Io;HANDLE File;UCHAR Key[16]={0};FILE_STANDARD_INFORMATION Info;
    InitializeObjectAttributes(&Attributes,&CredentialPath,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    NTSTATUS Status=ZwCreateFile(&File,FILE_READ_DATA|SYNCHRONIZE,&Attributes,&Io,NULL,FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,FILE_OPEN,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,NULL,0);
    if(!NT_SUCCESS(Status))return FALSE;
    Status=ZwQueryInformationFile(File,&Io,&Info,sizeof(Info),FileStandardInformation);
    if(NT_SUCCESS(Status) && Info.EndOfFile.QuadPart==sizeof(Key)){
        LARGE_INTEGER Offset;Offset.QuadPart=0;
        Status=ZwReadFile(File,NULL,NULL,NULL,&Io,Key,sizeof(Key),&Offset,NULL);
        if(NT_SUCCESS(Status) && Io.Information!=sizeof(Key))Status=STATUS_INFO_LENGTH_MISMATCH;
    }else Status=STATUS_INFO_LENGTH_MISMATCH;
    ZwClose(File);
    if(NT_SUCCESS(Status)){
        KIRQL I;KeAcquireSpinLock(&Lock,&I);RtlCopyMemory(AuthKey,Key,sizeof(AuthKey));AuthReady=TRUE;KeReleaseSpinLock(&Lock,I);
    }
    RtlSecureZeroMemory(Key,sizeof(Key));return NT_SUCCESS(Status);
}
NTSTATUS QchInit(PDRIVER_OBJECT Driver,PUNICODE_STRING RegistryPath)
{
    static const GUID Guid={0x61bf8950,0x44ec,0x4b25,{0xa2,0x4c,0x99,0xa5,0xf1,0x3c,0xe2,0x72}};
    UNICODE_STRING Name=RTL_CONSTANT_STRING(L"\\Device\\AlsCharge");
    UNICODE_STRING Link=RTL_CONSTANT_STRING(L"\\DosDevices\\AlsCharge");
    UNICODE_STRING Sddl=RTL_CONSTANT_STRING(L"D:P(A;;GR;;;SY)(A;;GR;;;BA)");
    OBJECT_ATTRIBUTES Attributes;InitializeObjectAttributes(&Attributes,RegistryPath,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    NTSTATUS Status=ZwOpenKey(&InputKey,KEY_QUERY_VALUE|KEY_SET_VALUE,&Attributes);
    if(!NT_SUCCESS(Status))return Status;
    UNICODE_STRING ValueName=RTL_CONSTANT_STRING(L"InputSuspended");UCHAR ValueBuffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION)+sizeof(ULONG)];ULONG ValueBytes;
    if(NT_SUCCESS(ZwQueryValueKey(InputKey,&ValueName,KeyValuePartialInformation,ValueBuffer,sizeof(ValueBuffer),&ValueBytes))){
        PKEY_VALUE_PARTIAL_INFORMATION V=(PKEY_VALUE_PARTIAL_INFORMATION)ValueBuffer;
        InputSaved=V->Type==REG_DWORD && V->DataLength==sizeof(ULONG) && *(ULONG*)V->Data==1;
    }
    KeInitializeSpinLock(&Lock);
    UNICODE_STRING Image;
    Status=IoQueryFullDriverPath(Driver,&Image);
    if(!NT_SUCCESS(Status))goto Failed;
    USHORT Length=Image.Length;
    while(Length && Image.Buffer[Length/sizeof(WCHAR)-1]!=L'\\')Length-=sizeof(WCHAR);
    SIZE_T Size=(SIZE_T)Length+sizeof(L"credential.bin");
    if(!Length || Size>MAXUSHORT){ExFreePool(Image.Buffer);Status=STATUS_NAME_TOO_LONG;goto Failed;}
    CredentialPath.Buffer=ExAllocatePool2(POOL_FLAG_PAGED,Size,'aChQ');
    if(CredentialPath.Buffer){
        RtlCopyMemory(CredentialPath.Buffer,Image.Buffer,Length);
        RtlCopyMemory((PUCHAR)CredentialPath.Buffer+Length,L"credential.bin",sizeof(L"credential.bin"));
        CredentialPath.Length=(USHORT)(Size-sizeof(WCHAR));CredentialPath.MaximumLength=(USHORT)Size;
    }
    ExFreePool(Image.Buffer);
    if(!CredentialPath.Buffer){Status=STATUS_INSUFFICIENT_RESOURCES;goto Failed;}
    Status=IoCreateDeviceSecure(Driver,0,&Name,FILE_DEVICE_UNKNOWN,FILE_DEVICE_SECURE_OPEN,FALSE,&Sddl,&Guid,&Control);
    if(!NT_SUCCESS(Status))goto Failed;
    Status=IoCreateSymbolicLink(&Link,&Name);
    if(!NT_SUCCESS(Status)){IoDeleteDevice(Control);Control=NULL;goto Failed;}
    for(ULONG I=0;I<=IRP_MJ_MAXIMUM_FUNCTION;I++){Prev[I]=Driver->MajorFunction[I];Driver->MajorFunction[I]=Dispatch;}
    PrevUnload=Driver->DriverUnload;Driver->DriverUnload=Unload;
    ControlDeleted=FALSE;Control->Flags&=~DO_DEVICE_INITIALIZING;return STATUS_SUCCESS;
Failed:
    if(CredentialPath.Buffer){ExFreePoolWithTag(CredentialPath.Buffer,'aChQ');RtlZeroMemory(&CredentialPath,sizeof(CredentialPath));}
    ZwClose(InputKey);InputKey=NULL;return Status;
}
