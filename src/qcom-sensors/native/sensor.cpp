#define NOMINMAX
#include <windows.h>
#include <wdf.h>
#include <initguid.h>
#include <SensorsCx.h>
#include <SensorsUtils.h>
#include <propvarutil.h>
#include <cmath>
#include "../abi.h"
struct SENSOR {
    WDFDEVICE Device;WDFWAITLOCK Lock;WDFTIMER Timer;ULONG Kind,Interval,Subscribed;
    BOOLEAN Started,First;ULONGLONG Last;
    PSENSOR_COLLECTION_LIST Enumeration,Properties,Data,Thresholds,Range;
    PSENSOR_PROPERTY_LIST Fields;
};
struct DEVICE {HANDLE Backend;WDFWAITLOCK Lock;SENSOROBJECT Sensors[4];BOOLEAN Powered;};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SENSOR,Sensor);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE,Device);
static const ULONG Kinds[]={QcomMagnetometer,QcomPedometer,QcomActivity,QcomProximity};
static const GUID*Types[]={&GUID_SensorType_Magnetometer3D,&GUID_SensorType_Pedometer,&GUID_SensorType_ActivityDetection,&GUID_SensorType_Proximity};
static const WCHAR*Names[]={L"Qualcomm Magnetometer",L"Qualcomm Pedometer",L"Qualcomm Activity Detection",L"Qualcomm Proximity"};
static const PROPERTYKEY MagFields[]={PKEY_SensorData_Timestamp,PKEY_SensorData_MagneticFieldStrengthX_Microteslas,PKEY_SensorData_MagneticFieldStrengthY_Microteslas,PKEY_SensorData_MagneticFieldStrengthZ_Microteslas,PKEY_SensorData_MagnetometerAccuracy};
static const PROPERTYKEY StepFields[]={PKEY_SensorData_Timestamp,PKEY_SensorData_PedometerReset,PKEY_SensorData_PedometerStepType,PKEY_SensorData_PedometerStepCount,PKEY_SensorData_PedometerStepDuration_Ms};
static const PROPERTYKEY ActFields[]={PKEY_SensorData_Timestamp,PKEY_SensorData_CurrentActivityState,PKEY_SensorData_CurrentActivityStateConfidence_Percentage};
static const PROPERTYKEY ProxFields[]={PKEY_SensorData_Timestamp,PKEY_SensorData_ProximityDetection};
static NTSTATUS Collection(SENSOROBJECT parent,ULONG count,PSENSOR_COLLECTION_LIST*list) {
    WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=parent;WDFMEMORY memory;
    ULONG bytes=SENSOR_COLLECTION_LIST_SIZE(count);NTSTATUS status=WdfMemoryCreate(&a,PagedPool,0,bytes,&memory,(PVOID*)list);
    if(NT_SUCCESS(status)){SENSOR_COLLECTION_LIST_INIT(*list,bytes);(*list)->Count=count;}return status;
}
static VOID Clear(PSENSOR_COLLECTION_LIST p){if(p)for(ULONG i=0;i<p->Count;i++)PropVariantClear(&p->List[i].Value);}
static VOID CleanupSensor(WDFOBJECT object){SENSOR*s=Sensor(object);if(s->Timer)WdfTimerStop(s->Timer,TRUE);Clear(s->Enumeration);Clear(s->Properties);Clear(s->Data);Clear(s->Thresholds);Clear(s->Range);}
static VOID CleanupDevice(WDFOBJECT object){DEVICE*d=Device(object);if(d->Backend!=INVALID_HANDLE_VALUE)CloseHandle(d->Backend);}
static NTSTATUS CopyCollection(PSENSOR_COLLECTION_LIST source,PSENSOR_COLLECTION_LIST output,PULONG size){
    if(!size)return STATUS_INVALID_PARAMETER;*size=CollectionsListGetMarshalledSize(source);if(!output)return STATUS_SUCCESS;
    if(output->AllocatedSizeInBytes<*size)return STATUS_BUFFER_TOO_SMALL;return CollectionsListCopyAndMarshall(output,source);
}
static EVT_SENSOR_DRIVER_GET_PROPERTIES GetProperties;
static NTSTATUS GetProperties(SENSOROBJECT object,PSENSOR_COLLECTION_LIST output,PULONG size){SENSOR*s=Sensor(object);WdfWaitLockAcquire(s->Lock,NULL);NTSTATUS status=CopyCollection(s->Properties,output,size);WdfWaitLockRelease(s->Lock);return status;}
static EVT_SENSOR_DRIVER_GET_SUPPORTED_DATA_FIELDS GetFields;
static NTSTATUS GetFields(SENSOROBJECT object,PSENSOR_PROPERTY_LIST output,PULONG size){SENSOR*s=Sensor(object);if(!size)return STATUS_INVALID_PARAMETER;*size=s->Fields->AllocatedSizeInBytes;if(!output)return STATUS_SUCCESS;if(output->AllocatedSizeInBytes<*size)return STATUS_BUFFER_TOO_SMALL;return PropertiesListCopy(output,s->Fields);}
static EVT_SENSOR_DRIVER_GET_DATA_FIELD_PROPERTIES GetRange;
static NTSTATUS GetRange(SENSOROBJECT object,const PROPERTYKEY*key,PSENSOR_COLLECTION_LIST output,PULONG size){SENSOR*s=Sensor(object);if(!key || s->Kind==QcomProximity || *key==PKEY_SensorData_Timestamp)return STATUS_NOT_SUPPORTED;for(ULONG i=1;i<s->Fields->Count;i++)if(*key==s->Fields->List[i])return CopyCollection(s->Range,output,size);return STATUS_NOT_SUPPORTED;}
static EVT_SENSOR_DRIVER_GET_DATA_INTERVAL GetInterval;
static NTSTATUS GetInterval(SENSOROBJECT object,PULONG interval){if(!interval)return STATUS_INVALID_PARAMETER;*interval=Sensor(object)->Interval;return STATUS_SUCCESS;}
static EVT_SENSOR_DRIVER_SET_DATA_INTERVAL SetInterval;
static NTSTATUS SetInterval(SENSOROBJECT object,ULONG interval){if(!interval)interval=100;if(interval<100 || interval>60000)return STATUS_INVALID_PARAMETER;SENSOR*s=Sensor(object);WdfWaitLockAcquire(s->Lock,NULL);s->Interval=interval;WdfWaitLockRelease(s->Lock);return STATUS_SUCCESS;}
static EVT_SENSOR_DRIVER_GET_DATA_THRESHOLDS GetThresholds;
static NTSTATUS GetThresholds(SENSOROBJECT object,PSENSOR_COLLECTION_LIST output,PULONG size){SENSOR*s=Sensor(object);WdfWaitLockAcquire(s->Lock,NULL);NTSTATUS status=CopyCollection(s->Thresholds,output,size);WdfWaitLockRelease(s->Lock);return status;}
static EVT_SENSOR_DRIVER_SET_DATA_THRESHOLDS SetThresholds;
static NTSTATUS SetThresholds(SENSOROBJECT object,PSENSOR_COLLECTION_LIST input){
    if(!input)return STATUS_INVALID_PARAMETER;SENSOR*s=Sensor(object);WdfWaitLockAcquire(s->Lock,NULL);NTSTATUS status=STATUS_SUCCESS;
    for(ULONG i=0;i<input->Count;i++){
        ULONG j=0;for(;j<s->Thresholds->Count;j++)if(input->List[i].Key==s->Thresholds->List[j].Key)break;
        if(j==s->Thresholds->Count){status=STATUS_INVALID_PARAMETER;break;}
        PROPVARIANT*v=&input->List[i].Value;if(v->vt!=s->Thresholds->List[j].Value.vt){status=STATUS_INVALID_PARAMETER;break;}
        if(v->vt==VT_R4 && (!std::isfinite(v->fltVal) || v->fltVal<0)){status=STATUS_INVALID_PARAMETER;break;}
        if(input->List[i].Key==PKEY_SensorData_SubscribedActivityStates && (v->ulVal&~QCOM_ACTIVITY_SUPPORTED)){status=STATUS_INVALID_PARAMETER;break;}
    }
    if(NT_SUCCESS(status))for(ULONG i=0;i<input->Count;i++)for(ULONG j=0;j<s->Thresholds->Count;j++)if(input->List[i].Key==s->Thresholds->List[j].Key){s->Thresholds->List[j].Value=input->List[i].Value;if(input->List[i].Key==PKEY_SensorData_SubscribedActivityStates)s->Subscribed=input->List[i].Value.ulVal;}
    WdfWaitLockRelease(s->Lock);return status;
}
static BOOLEAN Read(SENSOR*s,QCOM_ROTATION*sample){
    DEVICE*d=Device(s->Device);WdfWaitLockAcquire(d->Lock,NULL);
    if(d->Backend==INVALID_HANDLE_VALUE)d->Backend=CreateFileW(L"\\\\.\\QcomSensorData",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    DWORD bytes=0;BOOLEAN ok=d->Backend!=INVALID_HANDLE_VALUE && DeviceIoControl(d->Backend,QCOM_SENSOR_READ,&s->Kind,sizeof(s->Kind),sample,sizeof(*sample),&bytes,NULL) && bytes==sizeof(*sample);
    if(!ok && d->Backend!=INVALID_HANDLE_VALUE){CloseHandle(d->Backend);d->Backend=INVALID_HANDLE_VALUE;}
    WdfWaitLockRelease(d->Lock);return ok && sample->Status>=0 && sample->Time;
}
static EVT_WDF_TIMER Poll;
static VOID Poll(WDFTIMER timer){
    SENSOROBJECT object=WdfTimerGetParentObject(timer);SENSOR*s=Sensor(object);WdfWaitLockAcquire(s->Lock,NULL);
    if(!s->Started || !Device(s->Device)->Powered){WdfWaitLockRelease(s->Lock);return;}
    QCOM_ROTATION q{};if(Read(s,&q) && (s->First || q.Time!=s->Last)){
        FILETIME time;GetSystemTimePreciseAsFileTime(&time);InitPropVariantFromFileTime(&time,&s->Data->List[0].Value);
        if(s->Kind==QcomMagnetometer){for(ULONG j=0;j<3;j++)InitPropVariantFromFloat(q.Micro[j]/10000.0f,&s->Data->List[j+1].Value);InitPropVariantFromUInt32(q.Accuracy,&s->Data->List[4].Value);}
        else if(s->Kind==QcomPedometer){InitPropVariantFromBoolean(s->First || (ULONG)q.Micro[0]<s->Data->List[3].Value.ulVal,&s->Data->List[1].Value);InitPropVariantFromUInt32(PedometerStepType_Unknown,&s->Data->List[2].Value);InitPropVariantFromUInt32((ULONG)q.Micro[0],&s->Data->List[3].Value);InitPropVariantFromInt64(0,&s->Data->List[4].Value);}
        else if(s->Kind==QcomActivity){ULONG state=(ULONG)q.Micro[0]&s->Subscribed;if(!state)state=ActivityState_Unknown;InitPropVariantFromUInt32(state,&s->Data->List[1].Value);InitPropVariantFromUInt16(0,&s->Data->List[2].Value);}
        else InitPropVariantFromBoolean(q.Micro[0]!=0,&s->Data->List[1].Value);
        InitPropVariantFromUInt32(SensorState_Active,&s->Properties->List[0].Value);SensorsCxSensorDataReady(object,s->Data);s->Last=q.Time;s->First=FALSE;
    }
    WdfTimerStart(timer,WDF_REL_TIMEOUT_IN_MS(s->Interval));WdfWaitLockRelease(s->Lock);
}
static EVT_SENSOR_DRIVER_START_SENSOR Start;
static NTSTATUS Start(SENSOROBJECT object){SENSOR*s=Sensor(object);WdfWaitLockAcquire(s->Lock,NULL);s->Started=TRUE;s->First=TRUE;s->Last=0;InitPropVariantFromUInt32(SensorState_Active,&s->Properties->List[0].Value);WdfTimerStart(s->Timer,WDF_REL_TIMEOUT_IN_MS(1));WdfWaitLockRelease(s->Lock);return STATUS_SUCCESS;}
static EVT_SENSOR_DRIVER_STOP_SENSOR Stop;
static NTSTATUS Stop(SENSOROBJECT object){SENSOR*s=Sensor(object);if(s->Lock)WdfWaitLockAcquire(s->Lock,NULL);s->Started=FALSE;if(s->Properties)InitPropVariantFromUInt32(SensorState_Idle,&s->Properties->List[0].Value);if(s->Lock)WdfWaitLockRelease(s->Lock);if(s->Timer)WdfTimerStop(s->Timer,TRUE);return STATUS_SUCCESS;}
static NTSTATUS Initialize(WDFDEVICE device,SENSOROBJECT object,ULONG index){
    SENSOR*s=Sensor(object);s->Device=device;s->Kind=Kinds[index];s->Interval=100;s->Subscribed=QCOM_ACTIVITY_SUPPORTED;
    WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=object;NTSTATUS status=WdfWaitLockCreate(&a,&s->Lock);if(!NT_SUCCESS(status))return status;
    WDF_TIMER_CONFIG timer;WDF_TIMER_CONFIG_INIT(&timer,Poll);timer.AutomaticSerialization=FALSE;a.ExecutionLevel=WdfExecutionLevelPassive;status=WdfTimerCreate(&timer,&a,&s->Timer);if(!NT_SUCCESS(status))return status;
    BOOLEAN personal=index==1 || index==2;
    ULONG count=index==2?3:index==3?2:5;const PROPERTYKEY*keys=index==0?MagFields:index==1?StepFields:index==2?ActFields:ProxFields;
    status=Collection(object,count,&s->Data);if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<count;i++)s->Data->List[i].Key=keys[i];FILETIME now;GetSystemTimePreciseAsFileTime(&now);InitPropVariantFromFileTime(&now,&s->Data->List[0].Value);
    if(index==0){for(ULONG i=1;i<4;i++)InitPropVariantFromFloat(0,&s->Data->List[i].Value);InitPropVariantFromUInt32(0,&s->Data->List[4].Value);}
    if(index==1){InitPropVariantFromBoolean(TRUE,&s->Data->List[1].Value);InitPropVariantFromUInt32(1,&s->Data->List[2].Value);InitPropVariantFromUInt32(0,&s->Data->List[3].Value);InitPropVariantFromInt64(0,&s->Data->List[4].Value);}
    if(index==2){InitPropVariantFromUInt32(1,&s->Data->List[1].Value);InitPropVariantFromUInt16(0,&s->Data->List[2].Value);}
    if(index==3)InitPropVariantFromBoolean(FALSE,&s->Data->List[1].Value);
    WDF_OBJECT_ATTRIBUTES_INIT(&a);a.ParentObject=object;WDFMEMORY memory;ULONG bytes=SENSOR_PROPERTY_LIST_SIZE(count);status=WdfMemoryCreate(&a,PagedPool,0,bytes,&memory,(PVOID*)&s->Fields);if(!NT_SUCCESS(status))return status;SENSOR_PROPERTY_LIST_INIT(s->Fields,bytes);s->Fields->Count=count;for(ULONG i=0;i<count;i++)s->Fields->List[i]=keys[i];
    status=Collection(object,index?12:11,&s->Enumeration);if(!NT_SUCCESS(status))return status;
    const PROPERTYKEY enumkeys[]={DEVPKEY_Sensor_Type,DEVPKEY_Sensor_Manufacturer,DEVPKEY_Sensor_Model,DEVPKEY_Sensor_ConnectionType,DEVPKEY_Sensor_PersistentUniqueId,DEVPKEY_Sensor_IsPrimary,DEVPKEY_Sensor_Name,DEVPKEY_Sensor_Category,PKEY_Sensor_Power_Milliwatts,PKEY_SensorHistory_MaxSize_Bytes,PKEY_Sensor_MinimumDataInterval_Ms,index==1?PKEY_SensorData_SupportedStepTypes:index==2?PKEY_SensorData_SupportedActivityStates:DEVPKEY_Sensor_ProximityType};
    for(ULONG i=0;i<s->Enumeration->Count;i++)s->Enumeration->List[i].Key=enumkeys[i];
    InitPropVariantFromCLSID(*Types[index],&s->Enumeration->List[0].Value);InitPropVariantFromString(L"Qualcomm",&s->Enumeration->List[1].Value);InitPropVariantFromString(Names[index],&s->Enumeration->List[2].Value);InitPropVariantFromUInt32(0,&s->Enumeration->List[3].Value);
    GUID id={0x9a123600u+s->Kind,0x2026,0x1119,{0xa1,0x61,0x53,0x45,0x4e,0x53,0x4f,0x52}};InitPropVariantFromCLSID(id,&s->Enumeration->List[4].Value);InitPropVariantFromBoolean(TRUE,&s->Enumeration->List[5].Value);InitPropVariantFromString(Names[index],&s->Enumeration->List[6].Value);
    InitPropVariantFromCLSID(index==0?GUID_SensorCategory_Orientation:GUID_SensorCategory_PersonalActivity,&s->Enumeration->List[7].Value);InitPropVariantFromFloat(0,&s->Enumeration->List[8].Value);InitPropVariantFromUInt32(0,&s->Enumeration->List[9].Value);InitPropVariantFromUInt32(100,&s->Enumeration->List[10].Value);if(index)InitPropVariantFromUInt32(index==1?1:index==2?QCOM_ACTIVITY_SUPPORTED:ProximityType_ObjectProximity,&s->Enumeration->List[11].Value);
    status=Collection(object,personal?9:4,&s->Properties);if(!NT_SUCCESS(status))return status;
    const PROPERTYKEY props[]={PKEY_Sensor_State,PKEY_Sensor_MinimumDataInterval_Ms,PKEY_Sensor_MaximumDataFieldSize_Bytes,PKEY_Sensor_Type,PKEY_Sensor_Power_Milliwatts,PKEY_SensorHistory_MaxSize_Bytes,PKEY_SensorHistory_Interval_Ms,PKEY_SensorHistory_MaximumRecordSize_Bytes,index==1?PKEY_SensorData_SupportedStepTypes:PKEY_SensorData_SupportedActivityStates};
    for(ULONG i=0;i<s->Properties->Count;i++)s->Properties->List[i].Key=props[i];
    InitPropVariantFromUInt32(SensorState_Idle,&s->Properties->List[0].Value);InitPropVariantFromUInt32(100,&s->Properties->List[1].Value);InitPropVariantFromUInt32(CollectionsListGetMarshalledSize(s->Data),&s->Properties->List[2].Value);InitPropVariantFromCLSID(*Types[index],&s->Properties->List[3].Value);
    if(personal){InitPropVariantFromFloat(0,&s->Properties->List[4].Value);InitPropVariantFromUInt32(0,&s->Properties->List[5].Value);InitPropVariantFromUInt32(0,&s->Properties->List[6].Value);InitPropVariantFromUInt32(0,&s->Properties->List[7].Value);InitPropVariantFromUInt32(index==1?1:QCOM_ACTIVITY_SUPPORTED,&s->Properties->List[8].Value);}
    if(index!=3){status=Collection(object,3,&s->Range);if(!NT_SUCCESS(status))return status;const PROPERTYKEY ranges[]={PKEY_SensorDataField_Resolution,PKEY_SensorDataField_RangeMinimum,PKEY_SensorDataField_RangeMaximum};for(ULONG i=0;i<3;i++)s->Range->List[i].Key=ranges[i];InitPropVariantFromFloat(index==0?0.0001f:1,&s->Range->List[0].Value);InitPropVariantFromFloat(index==0?-10000.0f:0,&s->Range->List[1].Value);InitPropVariantFromFloat(index==0?10000.0f:index==1?(float)0xffffffffu:100,&s->Range->List[2].Value);}
    status=Collection(object,index==1?1:index==3?0:3,&s->Thresholds);if(!NT_SUCCESS(status))return status;
    if(index==0){for(ULONG i=0;i<3;i++){s->Thresholds->List[i].Key=keys[i+1];InitPropVariantFromFloat(0,&s->Thresholds->List[i].Value);}}
    if(index==1){s->Thresholds->List[0].Key=PKEY_SensorData_PedometerStepCount;InitPropVariantFromUInt32(1,&s->Thresholds->List[0].Value);}
    if(index==2){s->Thresholds->List[0].Key=PKEY_SensorData_SubscribedActivityStates;InitPropVariantFromUInt32(QCOM_ACTIVITY_SUPPORTED,&s->Thresholds->List[0].Value);s->Thresholds->List[1].Key=PKEY_SensorData_ActivityStream;InitPropVariantFromBoolean(TRUE,&s->Thresholds->List[1].Value);s->Thresholds->List[2].Key=PKEY_SensorData_ConfidenceThreshold_Percentage;InitPropVariantFromUInt16(0,&s->Thresholds->List[2].Value);}
    SENSOR_CONFIG config;SENSOR_CONFIG_INIT(&config);config.pEnumerationList=s->Enumeration;return SensorsCxSensorInitialize(object,&config);
}
static NTSTATUS Prepare(WDFDEVICE device,WDFCMRESLIST,WDFCMRESLIST){DEVICE*d=Device(device);for(ULONG i=0;i<RTL_NUMBER_OF(Kinds);i++){WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,SENSOR);a.EvtCleanupCallback=CleanupSensor;NTSTATUS status=SensorsCxSensorCreate(device,&a,&d->Sensors[i]);if(!NT_SUCCESS(status))return status;status=Initialize(device,d->Sensors[i],i);if(!NT_SUCCESS(status))return status;}return STATUS_SUCCESS;}
static NTSTATUS Release(WDFDEVICE device,WDFCMRESLIST){DEVICE*d=Device(device);for(auto &object:d->Sensors)if(object){Stop(object);WdfObjectDelete(object);object=NULL;}return STATUS_SUCCESS;}
static NTSTATUS Enter(WDFDEVICE device,WDF_POWER_DEVICE_STATE){DEVICE*d=Device(device);d->Powered=TRUE;for(auto object:d->Sensors)if(object && Sensor(object)->Started)WdfTimerStart(Sensor(object)->Timer,WDF_REL_TIMEOUT_IN_MS(1));return STATUS_SUCCESS;}
static NTSTATUS Exit(WDFDEVICE device,WDF_POWER_DEVICE_STATE){DEVICE*d=Device(device);d->Powered=FALSE;for(auto object:d->Sensors)if(object)WdfTimerStop(Sensor(object)->Timer,TRUE);return STATUS_SUCCESS;}
static NTSTATUS Add(WDFDRIVER,PWDFDEVICE_INIT init){
    WDF_OBJECT_ATTRIBUTES a;WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a,DEVICE);a.EvtCleanupCallback=CleanupDevice;NTSTATUS status=SensorsCxDeviceInitConfig(init,&a,0);if(!NT_SUCCESS(status))return status;
    WDF_PNPPOWER_EVENT_CALLBACKS p;WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);p.EvtDevicePrepareHardware=Prepare;p.EvtDeviceReleaseHardware=Release;p.EvtDeviceD0Entry=Enter;p.EvtDeviceD0Exit=Exit;WdfDeviceInitSetPnpPowerEventCallbacks(init,&p);
    WDFDEVICE device;status=WdfDeviceCreate(&init,&a,&device);if(!NT_SUCCESS(status))return status;
    WDF_DEVICE_PNP_CAPABILITIES caps;WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);caps.NoDisplayInUI=WdfTrue;WdfDeviceSetPnpCapabilities(device,&caps);
    DEVICE*d=Device(device);d->Backend=INVALID_HANDLE_VALUE;WDF_OBJECT_ATTRIBUTES lock;WDF_OBJECT_ATTRIBUTES_INIT(&lock);lock.ParentObject=device;status=WdfWaitLockCreate(&lock,&d->Lock);if(!NT_SUCCESS(status))return status;
    SENSOR_CONTROLLER_CONFIG c;SENSOR_CONTROLLER_CONFIG_INIT(&c);c.DriverIsPowerPolicyOwner=WdfUseDefault;c.EvtSensorStart=Start;c.EvtSensorStop=Stop;c.EvtSensorGetSupportedDataFields=GetFields;c.EvtSensorGetProperties=GetProperties;c.EvtSensorGetDataFieldProperties=GetRange;c.EvtSensorGetDataInterval=GetInterval;c.EvtSensorSetDataInterval=SetInterval;c.EvtSensorGetDataThresholds=GetThresholds;c.EvtSensorSetDataThresholds=SetThresholds;return SensorsCxDeviceInitialize(device,&c);
}
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver,PUNICODE_STRING path){WDF_DRIVER_CONFIG c;WDF_DRIVER_CONFIG_INIT(&c,Add);return WdfDriverCreate(driver,path,WDF_NO_OBJECT_ATTRIBUTES,&c,WDF_NO_HANDLE);}
