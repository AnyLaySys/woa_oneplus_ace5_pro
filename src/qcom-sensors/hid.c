#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include "abi.h"
#pragma pack(push, 1)
typedef struct {
    UCHAR Id, Connection, Reporting, Power, State, Primary;
    ULONG Interval, MinimumInterval;
    USHORT Sensitivity;
    ULONG Minimum, Maximum;
} FEATURE;
typedef struct {
    UCHAR Id, State, Event;
    LONG Value[5];
} REPORT;
#pragma pack(pop)
#define COMMON_FEATURE(minimum)                                                                              \
    0x75, 0x08, 0x95, 0x01, 0x15, 0x01, 0x25, 0x03, 0x0a, 0x09, 0x03, 0xa1, 0x02, 0x1a, 0x30, 0x08, 0x2a,    \
        0x32, 0x08, 0xb1, 0x00, 0xc0, 0x25, 0x06, 0x0a, 0x16, 0x03, 0xa1, 0x02, 0x1a, 0x40, 0x08, 0x2a,      \
        0x45, 0x08, 0xb1, 0x00, 0xc0, 0x0a, 0x19, 0x03, 0xa1, 0x02, 0x1a, 0x50, 0x08, 0x2a, 0x55, 0x08,      \
        0xb1, 0x00, 0xc0, 0x25, 0x07, 0x0a, 0x01, 0x02, 0xa1, 0x02, 0x1a, 0x00, 0x08, 0x2a, 0x06, 0x08,      \
        0xb1, 0x00, 0xc0, 0x15, 0x00, 0x25, 0x01, 0x0a, 0x1e, 0x03, 0xb1, 0x02, 0x75, 0x20, 0x15, minimum,   \
        0x26, 0x60, 0xea, 0x65, 0x19, 0x55, 0x00, 0x0a, 0x0e, 0x03, 0xb1, 0x02, 0x0a, 0x04, 0x03, 0xb1, 0x02
#define COMMON_INPUT                                                                                         \
    0x75, 0x08, 0x15, 0x01, 0x25, 0x07, 0x65, 0x00, 0x55, 0x00, 0x0a, 0x01, 0x02, 0xa1, 0x02, 0x1a, 0x00,    \
        0x08, 0x2a, 0x06, 0x08, 0x81, 0x00, 0xc0, 0x25, 0x0c, 0x0a, 0x02, 0x02, 0xa1, 0x02, 0x1a, 0x10,      \
        0x08, 0x2a, 0x1b, 0x08, 0x81, 0x00, 0xc0
#define LE32(n)                                                                                              \
    (UCHAR)((ULONG)(n) & 255u), (UCHAR)(((ULONG)(n) >> 8) & 255u), (UCHAR)(((ULONG)(n) >> 16) & 255u),       \
        (UCHAR)((ULONG)(n) >> 24)
#define MOTION_REPORT(id, type, unit, field, limit)                                                          \
    0x09, type, 0xa1, 0x00, 0x85, id, COMMON_FEATURE(20), 0x75, 0x10, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00,    \
        0x00, 0x65, unit, 0x55, 0x0d, 0x0a, field, 0x14, 0xb1, 0x02, 0x75, 0x20, 0x17, LE32(-(limit)), 0x27, \
        LE32(limit), 0x0a, field, 0x34, 0xb1, 0x02, 0x0a, field, 0x24, 0xb1, 0x02, COMMON_INPUT, 0x75, 0x20, \
        0x17, LE32(-(limit)), 0x27, LE32(limit), 0x65, unit, 0x55, 0x0d, 0x0a, (field) + 1, 0x04, 0x81,      \
        0x02, 0x0a, (field) + 2, 0x04, 0x81, 0x02, 0x0a, (field) + 3, 0x04, 0x81, 0x02, 0xc0
#define QUATERNION_REPORT(id, type)                                                                          \
    0x65, 0x00, 0x55, 0x00, 0x09, type, 0xa1, 0x00, 0x85, id, COMMON_FEATURE(100), 0x75, 0x10, 0x15, 0x00,   \
        0x27, 0xff, 0xff, 0x00, 0x00, 0x65, 0x00, 0x55, 0x0a, 0x0a, 0x83, 0x14, 0xb1, 0x02, 0x75, 0x20,      \
        0x17, LE32(-1000000), 0x27, LE32(1000000), 0x0a, 0x83, 0x34, 0xb1, 0x02, 0x0a, 0x83, 0x24, 0xb1,     \
        0x02, COMMON_INPUT, 0x75, 0x20, 0x95, 0x04, 0x17, LE32(-1000000), 0x27, LE32(1000000), 0x65, 0x00,   \
        0x55, 0x0a, 0x0a, 0x83, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x15, 0x01, 0x25, 0x03, 0x65,      \
        0x00, 0x55, 0x00, 0x0a, 0x88, 0x04, 0xa1, 0x02, 0x1a, 0xe0, 0x08, 0x2a, 0xe2, 0x08, 0x81, 0x00,      \
        0xc0, 0xc0
#define HID_SENSOR(id, kind, featureSize, reportSize, initial, minimum, sensitivity, low, high, ...)         \
    __VA_ARGS__,
static const UCHAR Descriptor[] = {
#include "light.inc"
#include "simple_orientation.inc"
#include "gyroscope.inc"
#include "accelerometer.inc"
#include "orientation.inc"
#include "linear_acceleration.inc"
#include "gravity.inc"
#include "relative_orientation.inc"
#include "geomagnetic_orientation.inc"
};
#undef HID_SENSOR
static const HID_DESCRIPTOR Hid = { 9, 0x21, 0x0111, 0, 1, { 0x22, sizeof(Descriptor) } };
static const HID_DEVICE_ATTRIBUTES Attributes = { sizeof(HID_DEVICE_ATTRIBUTES), 0x17cb, 0x8750, 0x0101 };
typedef struct {
    FEATURE Feature;
    UCHAR FeatureSize, ReportSize;
    ULONG Kind;
} HID_CONFIG;
#define HID_SENSOR(id, kind, featureSize, reportSize, initial, minimum, sensitivity, low, high, ...)         \
    { { id, 1, 1, 2, 4, 1, initial, minimum, sensitivity, low, high }, featureSize, reportSize, kind },
static const HID_CONFIG Configs[] = {
#include "light.inc"
#include "simple_orientation.inc"
#include "gyroscope.inc"
#include "accelerometer.inc"
#include "orientation.inc"
#include "linear_acceleration.inc"
#include "gravity.inc"
#include "relative_orientation.inc"
#include "geomagnetic_orientation.inc"
};
#undef HID_SENSOR
typedef struct {
    FEATURE Feature;
    REPORT Report;
    ULONGLONG Time, Due, ReportDue;
} SENSOR;
typedef struct {
    WDFQUEUE Reads;
    WDFTIMER Timer;
    WDFIOTARGET Target;
    BOOLEAN Open, On;
    SENSOR Sensors[RTL_NUMBER_OF(Configs)];
} HUB;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(HUB, Context);
static BOOLEAN Quaternion(ULONG kind) {
    return kind == QcomRotation || kind == QcomRelativeOrientation || kind == QcomGeomagneticOrientation;
}
static NTSTATUS Copy(WDFREQUEST request, const VOID *data, size_t size) {
    WDFMEMORY memory;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(request, &memory);
    if (NT_SUCCESS(status)) status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)data, size);
    if (NT_SUCCESS(status)) WdfRequestSetInformation(request, size);
    return status;
}
static BOOLEAN Polling(const SENSOR *s, ULONG kind) {
    return (s->Feature.Power == 2 || s->Feature.Power == 3) &&
           (kind <= QcomRotation || (s->Feature.Reporting != 1 && s->Feature.Reporting != 4));
}
static VOID Schedule(HUB *a) {
    if (!a->On) return;
    ULONGLONG due = MAXULONGLONG, now = KeQueryInterruptTime();
    for (ULONG i = 0; i < RTL_NUMBER_OF(a->Sensors); i++)
        if (Polling(&a->Sensors[i], Configs[i].Kind)) due = min(due, a->Sensors[i].Due);
    if (due != MAXULONGLONG) WdfTimerStart(a->Timer, -(LONGLONG)max(due > now ? due - now : 0, 1ULL));
}
static VOID Poll(WDFTIMER timer) {
    HUB *a = Context(WdfTimerGetParentObject(timer));
    if (!a->On) return;
    ULONGLONG now = KeQueryInterruptTime();
    for (ULONG i = 0; i < RTL_NUMBER_OF(a->Sensors); i++) {
        SENSOR *s = &a->Sensors[i];
        if (!Polling(s, Configs[i].Kind) || now < s->Due) continue;
        s->Due = now + (ULONGLONG)min(max(s->Feature.Interval, s->Feature.MinimumInterval), 2000u) * 10000;
        if (!a->Open) {
            UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\QcomSensorData");
            WDF_IO_TARGET_OPEN_PARAMS p;
            WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&p, &name, GENERIC_READ);
            p.ShareAccess = FILE_SHARE_READ | FILE_SHARE_WRITE;
            a->Open = NT_SUCCESS(WdfIoTargetOpen(a->Target, &p));
        }
        ULONG kind = Configs[i].Kind;
        QCOM_ROTATION sample = { 0 };
        NTSTATUS ready = STATUS_DEVICE_NOT_READY;
        ULONGLONG time = 0;
        LONG values[5] = { 0 };
        if (a->Open) {
            WDF_MEMORY_DESCRIPTOR input, output;
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, &kind, sizeof(kind));
            WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, &sample, sizeof(sample));
            WDF_REQUEST_SEND_OPTIONS options;
            WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
            WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(100));
            NTSTATUS status = WdfIoTargetSendIoctlSynchronously(
                a->Target, NULL, QCOM_SENSOR_READ, &input, &output, &options, NULL);
            if (!NT_SUCCESS(status)) {
                WdfIoTargetClose(a->Target);
                a->Open = FALSE;
                ready = status;
            } else {
                ready = sample.Status;
                time = sample.Time;
                RtlCopyMemory(values, sample.Micro, sizeof(sample.Micro));
            }
        }
        if (Quaternion(kind)) {
            for (ULONG j = 0; j < 4; j++) values[j] = sample.Micro[(j + 1) % 4];
            values[4] = (LONG)sample.Accuracy;
        }
        s->Feature.State =
            NT_SUCCESS(ready) && time && (kind != QcomSimpleOrientation || (ULONG)values[0] <= 5) ? 2 : 4;
        REPORT report = { Configs[i].Feature.Id, s->Feature.State, 4, { 0 } };
        RtlCopyMemory(report.Value, s->Feature.State == 2 ? values : s->Report.Value, sizeof(values));
        if (report.State != s->Report.State) report.Event = 2;
        else if (s->Feature.Reporting == 1 || s->Feature.Reporting == 4) continue;
        else if (report.State != 2 || time == s->Time || now < s->ReportDue) continue;
        else if (s->Time && (s->Feature.Reporting == 3 || s->Feature.Reporting == 6)) {
            ULONGLONG difference = 0;
            for (ULONG j = 0; j < (Quaternion(kind) ? 4u
                                      : kind == QcomGyro || kind == QcomAccel || kind == QcomGravity ||
                                              kind == QcomLinearAccel
                                          ? 3u
                                          : 1u);
                j++) {
                LONGLONG d = (LONGLONG)report.Value[j] - s->Report.Value[j];
                difference = max(difference, (ULONGLONG)(d < 0 ? -d : d));
            }
            if (!(Quaternion(kind) && report.Value[4] != s->Report.Value[4]) &&
                (kind == QcomLight
                        ? report.Value[1] == s->Report.Value[1] && difference * 10000 <
                              max((ULONGLONG)s->Report.Value[0] * s->Feature.Sensitivity, 1ULL)
                        : difference <
                              (kind == QcomSimpleOrientation ? 1u : max(s->Feature.Sensitivity, 1u))))
                continue;
            report.Event = 6;
        }
        WDFREQUEST request;
        if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(a->Reads, &request))) {
            NTSTATUS status = Copy(request, &report, Configs[i].ReportSize);
            if (NT_SUCCESS(status)) {
                s->Report = report;
                s->Time = report.Event == 2 ? 0 : time;
                s->ReportDue = s->Time ? now + (ULONGLONG)s->Feature.Interval * 10000 : 0;
            }
            WdfRequestComplete(request, status);
        }
    }
    Schedule(a);
}
static NTSTATUS Start(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    HUB *a = Context(device);
    a->On = TRUE;
    for (ULONG i = 0; i < RTL_NUMBER_OF(a->Sensors); i++) {
        a->Sensors[i].Due = a->Sensors[i].ReportDue = a->Sensors[i].Time = 0;
    }
    Schedule(a);
    return STATUS_SUCCESS;
}
static NTSTATUS Stop(WDFDEVICE device, WDF_POWER_DEVICE_STATE next) {
    UNREFERENCED_PARAMETER(next);
    HUB *a = Context(device);
    a->On = FALSE;
    WdfTimerStop(a->Timer, TRUE);
    if (a->Open) {
        WdfIoTargetClose(a->Target);
        a->Open = FALSE;
    }
    return STATUS_SUCCESS;
}
static VOID Control(WDFQUEUE queue, WDFREQUEST request, size_t output, size_t input, ULONG code) {
    HUB *a = Context(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    switch (code) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = Copy(request, &Hid, sizeof(Hid));
        break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = Copy(request, &Attributes, sizeof(Attributes));
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = Copy(request, Descriptor, sizeof(Descriptor));
        break;
    case IOCTL_HID_READ_REPORT:
        status = WdfRequestForwardToIoQueue(request, a->Reads);
        if (NT_SUCCESS(status)) return;
        break;
    case IOCTL_HID_GET_FEATURE:
    case IOCTL_HID_SET_FEATURE:
    case IOCTL_HID_GET_INPUT_REPORT: {
        PHID_XFER_PACKET p = WdfRequestWdmGetIrp(request)->UserBuffer;
        if ((code == IOCTL_HID_SET_FEATURE ? input : output) < sizeof(HID_XFER_PACKET) || !p ||
            !p->reportBuffer) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        ULONG i = 0;
        while (i < RTL_NUMBER_OF(a->Sensors) && Configs[i].Feature.Id != p->reportId) ++i;
        if (i == RTL_NUMBER_OF(a->Sensors)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        SENSOR *s = &a->Sensors[i];
        size_t size = code == IOCTL_HID_GET_INPUT_REPORT ? Configs[i].ReportSize : Configs[i].FeatureSize;
        if (p->reportBufferLen < size) {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (code == IOCTL_HID_SET_FEATURE) {
            FEATURE f = { 0 };
            RtlCopyMemory(&f, p->reportBuffer, size);
            if (f.Id != p->reportId || f.Reporting < 1 || f.Reporting > 6 || f.Power < 1 || f.Power > 6 ||
                f.Interval < s->Feature.MinimumInterval || f.Interval > 60000 ||
                (!i && f.Sensitivity > 10000)) {
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            s->Feature.Reporting = f.Reporting;
            s->Feature.Power = f.Power;
            s->Feature.Interval = f.Interval;
            s->Feature.Sensitivity = f.Sensitivity;
            s->Due = s->ReportDue = s->Time = 0;
            WdfTimerStop(a->Timer, FALSE);
            Schedule(a);
        } else if (code == IOCTL_HID_GET_INPUT_REPORT) {
            REPORT r = s->Report;
            r.Event = 5;
            RtlCopyMemory(p->reportBuffer, &r, size);
        } else RtlCopyMemory(p->reportBuffer, &s->Feature, size);
        if (code != IOCTL_HID_SET_FEATURE) WdfRequestSetInformation(request, size);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_HID_GET_STRING: {
        WDF_REQUEST_PARAMETERS p;
        WDF_REQUEST_PARAMETERS_INIT(&p);
        WdfRequestGetParameters(request, &p);
        ULONG id = PtrToUlong(p.Parameters.DeviceIoControl.Type3InputBuffer) & 0xffff;
        static const WCHAR vendor[] = L"Qualcomm", product[] = L"Qualcomm Sensors", serial[] = L"QCOMALS";
        status = id == HID_STRING_ID_IMANUFACTURER   ? Copy(request, vendor, sizeof(vendor))
                 : id == HID_STRING_ID_IPRODUCT      ? Copy(request, product, sizeof(product))
                 : id == HID_STRING_ID_ISERIALNUMBER ? Copy(request, serial, sizeof(serial))
                                                     : STATUS_NOT_SUPPORTED;
        break;
    }
    }
    WdfRequestComplete(request, status);
}
static NTSTATUS Add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WdfFdoInitSetFilter(init);
    WDF_PNPPOWER_EVENT_CALLBACKS p;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDeviceD0Entry = Start;
    p.EvtDeviceD0Exit = Stop;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &p);
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, HUB);
    attrs.SynchronizationScope = WdfSynchronizationScopeDevice;
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&init, &attrs, &device);
    if (!NT_SUCCESS(status)) return status;
    HUB *a = Context(device);
    for (ULONG i = 0; i < RTL_NUMBER_OF(a->Sensors); i++) {
        SENSOR *v = &a->Sensors[i];
        v->Feature = Configs[i].Feature;
        v->Report = (REPORT){ Configs[i].Feature.Id, 4, 1, { 0 } };
    }
    WDF_IO_QUEUE_CONFIG q;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q, WdfIoQueueDispatchParallel);
    q.PowerManaged = WdfFalse;
    q.EvtIoInternalDeviceControl = Control;
    status = WdfIoQueueCreate(device, &q, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG_INIT(&q, WdfIoQueueDispatchManual);
    q.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &q, WDF_NO_OBJECT_ATTRIBUTES, &a->Reads);
    if (!NT_SUCCESS(status)) return status;
    status = WdfIoTargetCreate(device, WDF_NO_OBJECT_ATTRIBUTES, &a->Target);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG t;
    WDF_TIMER_CONFIG_INIT(&t, Poll);
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    attrs.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&t, &attrs, &a->Timer);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Add);
    return WdfDriverCreate(driver, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
