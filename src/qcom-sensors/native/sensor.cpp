#define NOMINMAX
#include <windows.h>
#include <wdf.h>
#include <initguid.h>
#include <devpkey.h>
#include <SensorsCx.h>
#include <SensorsUtils.h>
#include <propvarutil.h>
#include <cmath>
#include "../abi.h"
struct SENSOR;
struct NATIVE_CONFIG {
    ULONG Kind;
    const GUID *Type, *Category;
    const WCHAR *Name, *Manufacturer;
    const PROPERTYKEY *Fields, *Extra;
    ULONG Count, RangeCount, ThresholdCount;
    float Resolution, Minimum, Maximum;
    BOOLEAN Personal;
    VOID (*Initialize)(SENSOR *);
    VOID (*Report)(SENSOR *, const QCOM_ROTATION *);
};
struct SENSOR {
    WDFDEVICE Device;
    WDFWAITLOCK Lock;
    WDFTIMER Timer;
    const NATIVE_CONFIG *Config;
    ULONG Interval, Subscribed;
    BOOLEAN Started, First;
    ULONGLONG Last;
    PSENSOR_COLLECTION_LIST Enumeration, Properties, Data, Thresholds, Range;
    PSENSOR_PROPERTY_LIST Fields;
};
struct DEVICE {
    HANDLE Backend;
    WDFWAITLOCK Lock;
    SENSOROBJECT Sensors[5];
    BOOLEAN Powered, HallOnly;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SENSOR, Sensor);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE, Device);
#define SENSOR_NATIVE
#include "../magnetometer.inc"
#include "../pedometer.inc"
#include "../activity.inc"
#include "../proximity.inc"
#include "../hall.inc"
#undef SENSOR_NATIVE
static const NATIVE_CONFIG *const Configs[] = { &MagnetometerConfig, &PedometerConfig, &ActivityConfig,
    &ProximityConfig, &HallConfig };
static NTSTATUS Collection(SENSOROBJECT parent, ULONG count, PSENSOR_COLLECTION_LIST *list) {
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = parent;
    WDFMEMORY memory;
    ULONG bytes = SENSOR_COLLECTION_LIST_SIZE(count);
    NTSTATUS status = WdfMemoryCreate(&a, PagedPool, 0, bytes, &memory, (PVOID *)list);
    if (NT_SUCCESS(status)) {
        SENSOR_COLLECTION_LIST_INIT(*list, bytes);
        (*list)->Count = count;
    }
    return status;
}
static VOID Clear(PSENSOR_COLLECTION_LIST p) {
    if (p)
        for (ULONG i = 0; i < p->Count; i++) PropVariantClear(&p->List[i].Value);
}
static VOID CleanupSensor(WDFOBJECT object) {
    SENSOR *s = Sensor(object);
    if (s->Timer) WdfTimerStop(s->Timer, TRUE);
    Clear(s->Enumeration);
    Clear(s->Properties);
    Clear(s->Data);
    Clear(s->Thresholds);
    Clear(s->Range);
}
static VOID CleanupDevice(WDFOBJECT object) {
    DEVICE *d = Device(object);
    if (d->Backend != INVALID_HANDLE_VALUE) CloseHandle(d->Backend);
}
static NTSTATUS CopyCollection(PSENSOR_COLLECTION_LIST source, PSENSOR_COLLECTION_LIST output, PULONG size) {
    if (!size) return STATUS_INVALID_PARAMETER;
    *size = CollectionsListGetMarshalledSize(source);
    if (!output) return STATUS_SUCCESS;
    if (output->AllocatedSizeInBytes < *size) return STATUS_BUFFER_TOO_SMALL;
    return CollectionsListCopyAndMarshall(output, source);
}
static EVT_SENSOR_DRIVER_GET_PROPERTIES GetProperties;
static NTSTATUS GetProperties(SENSOROBJECT object, PSENSOR_COLLECTION_LIST output, PULONG size) {
    SENSOR *s = Sensor(object);
    WdfWaitLockAcquire(s->Lock, NULL);
    NTSTATUS status = CopyCollection(s->Properties, output, size);
    WdfWaitLockRelease(s->Lock);
    return status;
}
static EVT_SENSOR_DRIVER_GET_SUPPORTED_DATA_FIELDS GetFields;
static NTSTATUS GetFields(SENSOROBJECT object, PSENSOR_PROPERTY_LIST output, PULONG size) {
    SENSOR *s = Sensor(object);
    if (!size) return STATUS_INVALID_PARAMETER;
    *size = s->Fields->AllocatedSizeInBytes;
    if (!output) return STATUS_SUCCESS;
    if (output->AllocatedSizeInBytes < *size) return STATUS_BUFFER_TOO_SMALL;
    return PropertiesListCopy(output, s->Fields);
}
static EVT_SENSOR_DRIVER_GET_DATA_FIELD_PROPERTIES GetRange;
static NTSTATUS GetRange(
    SENSOROBJECT object, const PROPERTYKEY *key, PSENSOR_COLLECTION_LIST output, PULONG size) {
    SENSOR *s = Sensor(object);
    if (!key) return STATUS_NOT_SUPPORTED;
    for (ULONG i = 1; i <= s->Config->RangeCount; i++)
        if (*key == s->Fields->List[i]) return CopyCollection(s->Range, output, size);
    return STATUS_NOT_SUPPORTED;
}
static EVT_SENSOR_DRIVER_GET_DATA_INTERVAL GetInterval;
static NTSTATUS GetInterval(SENSOROBJECT object, PULONG interval) {
    if (!interval) return STATUS_INVALID_PARAMETER;
    *interval = Sensor(object)->Interval;
    return STATUS_SUCCESS;
}
static EVT_SENSOR_DRIVER_SET_DATA_INTERVAL SetInterval;
static NTSTATUS SetInterval(SENSOROBJECT object, ULONG interval) {
    if (!interval) interval = 100;
    if (interval < 100 || interval > 60000) return STATUS_INVALID_PARAMETER;
    SENSOR *s = Sensor(object);
    WdfWaitLockAcquire(s->Lock, NULL);
    s->Interval = interval;
    WdfWaitLockRelease(s->Lock);
    return STATUS_SUCCESS;
}
static EVT_SENSOR_DRIVER_GET_DATA_THRESHOLDS GetThresholds;
static NTSTATUS GetThresholds(SENSOROBJECT object, PSENSOR_COLLECTION_LIST output, PULONG size) {
    SENSOR *s = Sensor(object);
    WdfWaitLockAcquire(s->Lock, NULL);
    NTSTATUS status = CopyCollection(s->Thresholds, output, size);
    WdfWaitLockRelease(s->Lock);
    return status;
}
static EVT_SENSOR_DRIVER_SET_DATA_THRESHOLDS SetThresholds;
static NTSTATUS SetThresholds(SENSOROBJECT object, PSENSOR_COLLECTION_LIST input) {
    if (!input) return STATUS_INVALID_PARAMETER;
    SENSOR *s = Sensor(object);
    WdfWaitLockAcquire(s->Lock, NULL);
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG i = 0; i < input->Count; i++) {
        ULONG j = 0;
        for (; j < s->Thresholds->Count; j++)
            if (input->List[i].Key == s->Thresholds->List[j].Key) break;
        if (j == s->Thresholds->Count) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        PROPVARIANT *v = &input->List[i].Value;
        if (v->vt != s->Thresholds->List[j].Value.vt) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (v->vt == VT_I4 && v->lVal < 0) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (v->vt == VT_R4 && (!std::isfinite(v->fltVal) || v->fltVal < 0)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (input->List[i].Key == PKEY_SensorData_SubscribedActivityStates &&
            (v->ulVal & ~QCOM_ACTIVITY_SUPPORTED)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }
    if (NT_SUCCESS(status))
        for (ULONG i = 0; i < input->Count; i++)
            for (ULONG j = 0; j < s->Thresholds->Count; j++)
                if (input->List[i].Key == s->Thresholds->List[j].Key) {
                    s->Thresholds->List[j].Value = input->List[i].Value;
                    if (input->List[i].Key == PKEY_SensorData_SubscribedActivityStates)
                        s->Subscribed = input->List[i].Value.ulVal;
                }
    WdfWaitLockRelease(s->Lock);
    return status;
}
static BOOLEAN Read(SENSOR *s, QCOM_ROTATION *sample) {
    DEVICE *d = Device(s->Device);
    WdfWaitLockAcquire(d->Lock, NULL);
    if (d->Backend == INVALID_HANDLE_VALUE)
        d->Backend = CreateFileW(d->HallOnly ? L"\\\\.\\QcomHaptics" : L"\\\\.\\QcomSensorData", GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD bytes = 0;
    BOOLEAN ok = d->Backend != INVALID_HANDLE_VALUE &&
                 (d->HallOnly ? ReadHall(d->Backend, sample)
                              : DeviceIoControl(d->Backend, QCOM_SENSOR_READ, (PVOID)&s->Config->Kind,
                                    sizeof(s->Config->Kind), sample, sizeof(*sample), &bytes, NULL) &&
                                    bytes == sizeof(*sample));
    if (!ok && d->Backend != INVALID_HANDLE_VALUE) {
        CloseHandle(d->Backend);
        d->Backend = INVALID_HANDLE_VALUE;
    }
    WdfWaitLockRelease(d->Lock);
    return ok && sample->Status >= 0 && sample->Time;
}
static EVT_WDF_TIMER Poll;
static VOID Poll(WDFTIMER timer) {
    SENSOROBJECT object = WdfTimerGetParentObject(timer);
    SENSOR *s = Sensor(object);
    WdfWaitLockAcquire(s->Lock, NULL);
    if (!s->Started || !Device(s->Device)->Powered) {
        WdfWaitLockRelease(s->Lock);
        return;
    }
    QCOM_ROTATION q{};
    if (Read(s, &q) && (s->First || q.Time != s->Last)) {
        FILETIME time;
        GetSystemTimePreciseAsFileTime(&time);
        InitPropVariantFromFileTime(&time, &s->Data->List[0].Value);
        s->Config->Report(s, &q);
        InitPropVariantFromUInt32(SensorState_Active, &s->Properties->List[0].Value);
        SensorsCxSensorDataReady(object, s->Data);
        s->Last = q.Time;
        s->First = FALSE;
    }
    WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(s->Interval));
    WdfWaitLockRelease(s->Lock);
}
static EVT_SENSOR_DRIVER_START_SENSOR Start;
static NTSTATUS Start(SENSOROBJECT object) {
    SENSOR *s = Sensor(object);
    WdfWaitLockAcquire(s->Lock, NULL);
    s->Started = TRUE;
    s->First = TRUE;
    s->Last = 0;
    InitPropVariantFromUInt32(SensorState_Active, &s->Properties->List[0].Value);
    WdfTimerStart(s->Timer, WDF_REL_TIMEOUT_IN_MS(1));
    WdfWaitLockRelease(s->Lock);
    return STATUS_SUCCESS;
}
static EVT_SENSOR_DRIVER_STOP_SENSOR Stop;
static NTSTATUS Stop(SENSOROBJECT object) {
    SENSOR *s = Sensor(object);
    if (s->Lock) WdfWaitLockAcquire(s->Lock, NULL);
    s->Started = FALSE;
    if (s->Properties) InitPropVariantFromUInt32(SensorState_Idle, &s->Properties->List[0].Value);
    if (s->Lock) WdfWaitLockRelease(s->Lock);
    if (s->Timer) WdfTimerStop(s->Timer, TRUE);
    return STATUS_SUCCESS;
}
static NTSTATUS Initialize(WDFDEVICE device, SENSOROBJECT object, ULONG index) {
    SENSOR *s = Sensor(object);
    s->Device = device;
    s->Config = Configs[index];
    const NATIVE_CONFIG *c = s->Config;
    s->Interval = 100;
    s->Subscribed = QCOM_ACTIVITY_SUPPORTED;
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = object;
    NTSTATUS status = WdfWaitLockCreate(&a, &s->Lock);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG timer;
    WDF_TIMER_CONFIG_INIT(&timer, Poll);
    timer.AutomaticSerialization = FALSE;
    a.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timer, &a, &s->Timer);
    if (!NT_SUCCESS(status)) return status;
    status = Collection(object, c->Count, &s->Data);
    if (!NT_SUCCESS(status)) return status;
    for (ULONG i = 0; i < c->Count; i++) s->Data->List[i].Key = c->Fields[i];
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    InitPropVariantFromFileTime(&now, &s->Data->List[0].Value);
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = object;
    WDFMEMORY memory;
    ULONG bytes = SENSOR_PROPERTY_LIST_SIZE(c->Count);
    status = WdfMemoryCreate(&a, PagedPool, 0, bytes, &memory, (PVOID *)&s->Fields);
    if (!NT_SUCCESS(status)) return status;
    SENSOR_PROPERTY_LIST_INIT(s->Fields, bytes);
    s->Fields->Count = c->Count;
    for (ULONG i = 0; i < c->Count; i++) s->Fields->List[i] = c->Fields[i];
    status = Collection(object, c->Extra ? 12 : 11, &s->Enumeration);
    if (!NT_SUCCESS(status)) return status;
    const PROPERTYKEY enumkeys[] = { DEVPKEY_Sensor_Type, DEVPKEY_Sensor_Manufacturer, DEVPKEY_Sensor_Model,
        DEVPKEY_Sensor_ConnectionType, DEVPKEY_Sensor_PersistentUniqueId, DEVPKEY_Sensor_IsPrimary,
        DEVPKEY_Sensor_Name, DEVPKEY_Sensor_Category, PKEY_Sensor_Power_Milliwatts,
        PKEY_SensorHistory_MaxSize_Bytes, PKEY_Sensor_MinimumDataInterval_Ms };
    for (ULONG i = 0; i < RTL_NUMBER_OF(enumkeys); i++) s->Enumeration->List[i].Key = enumkeys[i];
    if (c->Extra) s->Enumeration->List[11].Key = *c->Extra;
    InitPropVariantFromCLSID(*c->Type, &s->Enumeration->List[0].Value);
    InitPropVariantFromString(c->Manufacturer, &s->Enumeration->List[1].Value);
    InitPropVariantFromString(c->Name, &s->Enumeration->List[2].Value);
    InitPropVariantFromUInt32(0, &s->Enumeration->List[3].Value);
    GUID id = { 0x9a123600u + c->Kind, 0x2026, 0x1119, { 0xa1, 0x61, 0x53, 0x45, 0x4e, 0x53, 0x4f, 0x52 } };
    InitPropVariantFromCLSID(id, &s->Enumeration->List[4].Value);
    InitPropVariantFromBoolean(TRUE, &s->Enumeration->List[5].Value);
    InitPropVariantFromString(c->Name, &s->Enumeration->List[6].Value);
    InitPropVariantFromCLSID(*c->Category, &s->Enumeration->List[7].Value);
    InitPropVariantFromFloat(0, &s->Enumeration->List[8].Value);
    InitPropVariantFromUInt32(0, &s->Enumeration->List[9].Value);
    InitPropVariantFromUInt32(100, &s->Enumeration->List[10].Value);
    status = Collection(object, c->Personal ? 9 : 4, &s->Properties);
    if (!NT_SUCCESS(status)) return status;
    const PROPERTYKEY props[] = { PKEY_Sensor_State, PKEY_Sensor_MinimumDataInterval_Ms,
        PKEY_Sensor_MaximumDataFieldSize_Bytes, PKEY_Sensor_Type, PKEY_Sensor_Power_Milliwatts,
        PKEY_SensorHistory_MaxSize_Bytes, PKEY_SensorHistory_Interval_Ms,
        PKEY_SensorHistory_MaximumRecordSize_Bytes };
    for (ULONG i = 0; i < (c->Personal ? 8u : 4u); i++) s->Properties->List[i].Key = props[i];
    if (c->Personal) s->Properties->List[8].Key = *c->Extra;
    InitPropVariantFromUInt32(SensorState_Idle, &s->Properties->List[0].Value);
    InitPropVariantFromUInt32(100, &s->Properties->List[1].Value);
    InitPropVariantFromCLSID(*c->Type, &s->Properties->List[3].Value);
    if (c->Personal) {
        InitPropVariantFromFloat(0, &s->Properties->List[4].Value);
        for (ULONG i = 5; i < 8; i++) InitPropVariantFromUInt32(0, &s->Properties->List[i].Value);
    }
    if (c->RangeCount) {
        status = Collection(object, 3, &s->Range);
        if (!NT_SUCCESS(status)) return status;
        const PROPERTYKEY ranges[] = { PKEY_SensorDataField_Resolution, PKEY_SensorDataField_RangeMinimum,
            PKEY_SensorDataField_RangeMaximum };
        for (ULONG i = 0; i < 3; i++) s->Range->List[i].Key = ranges[i];
        InitPropVariantFromFloat(c->Resolution, &s->Range->List[0].Value);
        InitPropVariantFromFloat(c->Minimum, &s->Range->List[1].Value);
        InitPropVariantFromFloat(c->Maximum, &s->Range->List[2].Value);
    }
    status = Collection(object, c->ThresholdCount, &s->Thresholds);
    if (!NT_SUCCESS(status)) return status;
    c->Initialize(s);
    InitPropVariantFromUInt32(CollectionsListGetMarshalledSize(s->Data), &s->Properties->List[2].Value);
    SENSOR_CONFIG config;
    SENSOR_CONFIG_INIT(&config);
    config.pEnumerationList = s->Enumeration;
    return SensorsCxSensorInitialize(object, &config);
}
static NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST) {
    DEVICE *d = Device(device);
    for (ULONG i = d->HallOnly ? 4 : 0; i < (d->HallOnly ? 5u : 4u); i++) {
        WDF_OBJECT_ATTRIBUTES a;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, SENSOR);
        a.EvtCleanupCallback = CleanupSensor;
        NTSTATUS status = SensorsCxSensorCreate(device, &a, &d->Sensors[i]);
        if (!NT_SUCCESS(status)) return status;
        status = Initialize(device, d->Sensors[i], i);
        if (!NT_SUCCESS(status)) return status;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST) {
    DEVICE *d = Device(device);
    for (auto &object : d->Sensors)
        if (object) {
            Stop(object);
            WdfObjectDelete(object);
            object = NULL;
        }
    return STATUS_SUCCESS;
}
static NTSTATUS Enter(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DEVICE *d = Device(device);
    d->Powered = TRUE;
    for (auto object : d->Sensors)
        if (object && Sensor(object)->Started) WdfTimerStart(Sensor(object)->Timer, WDF_REL_TIMEOUT_IN_MS(1));
    return STATUS_SUCCESS;
}
static NTSTATUS Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    DEVICE *d = Device(device);
    d->Powered = FALSE;
    for (auto object : d->Sensors)
        if (object) WdfTimerStop(Sensor(object)->Timer, TRUE);
    return STATUS_SUCCESS;
}
static NTSTATUS Add(WDFDRIVER, PWDFDEVICE_INIT init) {
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, DEVICE);
    a.EvtCleanupCallback = CleanupDevice;
    NTSTATUS status = SensorsCxDeviceInitConfig(init, &a, 0);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS p;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDevicePrepareHardware = Prepare;
    p.EvtDeviceReleaseHardware = Release;
    p.EvtDeviceD0Entry = Enter;
    p.EvtDeviceD0Exit = Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &p);
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    DEVICE *d = Device(device);
    d->Backend = INVALID_HANDLE_VALUE;
    status = HallDevice(device, &d->HallOnly);
    if (!NT_SUCCESS(status)) return status;
    WDF_DEVICE_PNP_CAPABILITIES caps;
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&caps);
    caps.NoDisplayInUI = d->HallOnly ? WdfFalse : WdfTrue;
    WdfDeviceSetPnpCapabilities(device, &caps);
    WDF_OBJECT_ATTRIBUTES lock;
    WDF_OBJECT_ATTRIBUTES_INIT(&lock);
    lock.ParentObject = device;
    status = WdfWaitLockCreate(&lock, &d->Lock);
    if (!NT_SUCCESS(status)) return status;
    SENSOR_CONTROLLER_CONFIG c;
    SENSOR_CONTROLLER_CONFIG_INIT(&c);
    c.DriverIsPowerPolicyOwner = WdfUseDefault;
    c.EvtSensorStart = Start;
    c.EvtSensorStop = Stop;
    c.EvtSensorGetSupportedDataFields = GetFields;
    c.EvtSensorGetProperties = GetProperties;
    c.EvtSensorGetDataFieldProperties = GetRange;
    c.EvtSensorGetDataInterval = GetInterval;
    c.EvtSensorSetDataInterval = SetInterval;
    c.EvtSensorGetDataThresholds = GetThresholds;
    c.EvtSensorSetDataThresholds = SetThresholds;
    return SensorsCxDeviceInitialize(device, &c);
}
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG c;
    WDF_DRIVER_CONFIG_INIT(&c, Add);
    return WdfDriverCreate(driver, path, WDF_NO_OBJECT_ATTRIBUTES, &c, WDF_NO_HANDLE);
}
