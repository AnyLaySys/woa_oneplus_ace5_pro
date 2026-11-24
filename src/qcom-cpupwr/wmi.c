#include "cpupower.h"
#include <wmilib.h>
#include <wmidata.h>
#include <wmistr.h>

static const GUID CpuGuid={0x41dcb50c,0x1eb3,0x4d70,{0xa8,0x68,0xc0,0xa5,0xea,0x8f,0x41,0x81}};
static const GUID ThermalGuid=MSAcpi_ThermalZoneTemperatureGuid;
static WMIGUIDREGINFO Guids[]={{&CpuGuid,2,0},{&ThermalGuid,2,0}};
static PDEVICE_OBJECT Physical;
static UNICODE_STRING RegistryPath=RTL_CONSTANT_STRING(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\qcom_cpupwr");
typedef struct {ULONG Domain,FrequencyKhz;LONG TemperatureDeciC;ULONG Valid;} CPU_DATA;

static NTSTATUS RegInfo(PDEVICE_OBJECT device,PULONG flags,PUNICODE_STRING name,
    PUNICODE_STRING *path,PUNICODE_STRING mof,PDEVICE_OBJECT *physical) {
    UNREFERENCED_PARAMETER(device);UNREFERENCED_PARAMETER(name);
    *flags=WMIREG_FLAG_INSTANCE_PDO;*physical=Physical;*path=&RegistryPath;
    RtlInitUnicodeString(mof,L"CpuMof");return STATUS_SUCCESS;
}
static NTSTATUS Query(PDEVICE_OBJECT device,PIRP irp,ULONG guid,ULONG first,ULONG count,
    PULONG lengths,ULONG available,PUCHAR buffer) {
    if(guid>=RTL_NUMBER_OF(Guids))return WmiCompleteRequest(device,irp,STATUS_WMI_GUID_NOT_FOUND,0,0);
    if(first>=2 || !count || count>2-first)return WmiCompleteRequest(device,irp,STATUS_WMI_INSTANCE_NOT_FOUND,0,0);
    ULONG size=guid?MSAcpi_ThermalZoneTemperature_SIZE:sizeof(CPU_DATA),stride=(size+7)&~7u;
    ULONG needed=(count-1)*stride+size;
    if(!lengths || !buffer || available<needed)return WmiCompleteRequest(device,irp,STATUS_BUFFER_TOO_SMALL,needed,0);
    RtlZeroMemory(buffer,needed);
    for(ULONG i=0;i<count;i++) {
        ULONG d=first+i;LONG temperature=InterlockedCompareExchange(&Sm.Temperature[d],0,0);
        if(!guid) {
            CPU_DATA *data=(CPU_DATA *)(buffer+i*stride);SM_REPLY reply;
            data->Domain=d;data->TemperatureDeciC=temperature;
            data->Valid=temperature!=MINLONG?2u:0;
            if(!Sm.Stopping && NT_SUCCESS(SmScmiRequest(8,d,0,&reply)) && reply.Count && reply.Words[0]<SM_LEVEL_COUNT) {
                data->FrequencyKhz=SmFrequencyKhz[d][reply.Words[0]];data->Valid|=1;
            }
        } else {
            if(temperature==MINLONG)return WmiCompleteRequest(device,irp,STATUS_DEVICE_NOT_READY,0,0);
            PMSAcpi_ThermalZoneTemperature data=(PMSAcpi_ThermalZoneTemperature)(buffer+i*stride);
            data->CurrentTemperature=(ULONG)(temperature+2732);
            data->PassiveTripPoint=3722;
        }
        lengths[i]=size;
    }
    return WmiCompleteRequest(device,irp,STATUS_SUCCESS,needed,0);
}
static WMILIB_CONTEXT Wmi={RTL_NUMBER_OF(Guids),Guids,RegInfo,Query,NULL,NULL,NULL,NULL};

NTSTATUS SmWmiRegister(PDEVICE_OBJECT device,PDEVICE_OBJECT physical) {
    Physical=physical;return IoWMIRegistrationControl(device,WMIREG_ACTION_REGISTER);
}
NTSTATUS SmWmiControl(PDEVICE_OBJECT device,PIRP irp) {
    SYSCTL_IRP_DISPOSITION disposition;NTSTATUS status=WmiSystemControl(&Wmi,device,irp,&disposition);
    if(disposition==IrpProcessed)return status;
    if(disposition==IrpNotCompleted){IoCompleteRequest(irp,0);return status;}
    return SmForward(device,irp);
}
