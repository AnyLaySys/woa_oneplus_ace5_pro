#include "fingerprint.h"
#include <initguid.h>
typedef ULONG DWORD;
typedef UCHAR BYTE;
typedef LONG HRESULT;
typedef LONG BOOL;
typedef struct { LONG x, y; } POINT;
typedef struct { LONG left, top, right, bottom; } RECT;
#define SECURITY_MAX_SID_SIZE 68
#include <winbio_ioctl.h>
#include <winbio_err.h>

static NTSTATUS Output(WDFREQUEST request, const VOID *data, SIZE_T size) {
    PVOID output = NULL;
    SIZE_T length = 0;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(request, sizeof(ULONG), &output, &length);
    if (!NT_SUCCESS(status)) return status;
    if (length < size) {
        *(PULONG)output = (ULONG)size;
        WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(ULONG));
        return STATUS_PENDING;
    }
    RtlCopyMemory(output, data, size);
    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, size);
    return STATUS_PENDING;
}

static VOID Control(WDFQUEUE queue, WDFREQUEST request, SIZE_T output, SIZE_T input, ULONG code) {
    UNREFERENCED_PARAMETER(output);
    JFP_DEVICE *device = JfpContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    if (device->SessionOwner && code != IOCTL_BIOMETRIC_GET_ATTRIBUTES &&
        code != IOCTL_BIOMETRIC_GET_SENSOR_STATUS && code != JFP_QUERY && code != JFP_SESSION_STATUS &&
        ((code != JFP_SESSION_SAMPLE && code != JFP_SESSION_END) ||
            WdfRequestGetFileObject(request) != device->SessionOwner)) {
        WdfRequestComplete(request, STATUS_DEVICE_BUSY);
        return;
    }
    switch (code) {
    case IOCTL_BIOMETRIC_GET_ATTRIBUTES: {
        WINBIO_SENSOR_ATTRIBUTES attributes = {0};
        attributes.PayloadSize = sizeof(attributes);
        attributes.WinBioVersion.MajorVersion = WINBIO_WBDI_MAJOR_VERSION;
        attributes.WinBioVersion.MinorVersion = WINBIO_WBDI_MINOR_VERSION;
        attributes.SensorType = WINBIO_TYPE_FINGERPRINT;
        attributes.SensorSubType = WINBIO_SENSOR_SUBTYPE_UNKNOWN;
        attributes.Capabilities = WINBIO_CAPABILITY_SENSOR;
        RtlCopyMemory(attributes.ManufacturerName, L"Jiiov", sizeof(L"Jiiov"));
        RtlCopyMemory(attributes.ModelName, L"JV0301", sizeof(L"JV0301"));
        RtlCopyMemory(attributes.SerialNumber, L"hummer-24811", sizeof(L"hummer-24811"));
        attributes.FirmwareVersion.MajorVersion = 1;
        attributes.SupportedFormatEntries = 1;
        attributes.SupportedFormat[0].Owner = WINBIO_ANSI_381_FORMAT_OWNER;
        attributes.SupportedFormat[0].Type = WINBIO_ANSI_381_FORMAT_TYPE;
        status = Output(request, &attributes, sizeof(attributes));
        break;
    }
    case IOCTL_BIOMETRIC_GET_SENSOR_STATUS: {
        WINBIO_DIAGNOSTICS diagnostics = {0};
        diagnostics.PayloadSize = sizeof(diagnostics);
        diagnostics.WinBioHresult = WINBIO_E_SENSOR_UNAVAILABLE;
        diagnostics.SensorStatus = device->Info.Flags & JFP_FLAG_CHIP
            ? WINBIO_SENSOR_NOT_CALIBRATED : WINBIO_SENSOR_FAILURE;
        status = Output(request, &diagnostics, sizeof(diagnostics));
        break;
    }
    case IOCTL_BIOMETRIC_RESET: {
        WINBIO_BLANK_PAYLOAD result = {0};
        result.PayloadSize = sizeof(result);
        result.WinBioHresult = WINBIO_E_SENSOR_UNAVAILABLE;
        status = Output(request, &result, sizeof(result));
        break;
    }
    case IOCTL_BIOMETRIC_CALIBRATE: {
        WINBIO_CALIBRATION_INFO result = {0};
        result.PayloadSize = sizeof(result);
        result.WinBioHresult = WINBIO_E_SENSOR_UNAVAILABLE;
        status = Output(request, &result, sizeof(result));
        break;
    }
    case IOCTL_BIOMETRIC_CAPTURE_DATA: {
        WINBIO_CAPTURE_PARAMETERS *parameters = NULL;
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*parameters), (PVOID *)&parameters, NULL);
        if (!NT_SUCCESS(status)) break;
        if (parameters->PayloadSize != sizeof(*parameters) || input != sizeof(*parameters)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        WINBIO_CAPTURE_DATA result = {0};
        result.PayloadSize = sizeof(result);
        result.WinBioHresult = WINBIO_E_SENSOR_UNAVAILABLE;
        result.SensorStatus = WINBIO_SENSOR_FAILURE;
        status = Output(request, &result, sizeof(result));
        break;
    }
    case JFP_QUERY:
    case JFP_INITIALIZE:
        if (input) { status = STATUS_INVALID_PARAMETER; break; }
        if (code == JFP_INITIALIZE) {
            PVOID buffer = NULL;
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Info), &buffer, NULL);
            if (!NT_SUCCESS(status)) break;
            device->Info.Status = JfpInitialize(device);
        } else if (output >= sizeof(device->Info)) JfpInspect(device);
        status = Output(request, &device->Info, sizeof(device->Info));
        break;
    case JFP_INIT_PROBE:
    case JFP_ENROLL_PREFLIGHT:
    case JFP_FEATURE_PROBE:
    case JFP_SESSION_BEGIN:
    case JFP_CAPTURE_PROBE: {
        JFP_CAPTURE_INPUT *payload = NULL;
        PVOID buffer = NULL;
        if (code == JFP_SESSION_BEGIN && !WdfRequestGetFileObject(request)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (input < JFP_CAPTURE_HEADER_SIZE + 128 || input > JFP_CAPTURE_HEADER_SIZE + JFP_TA_MAX_IMAGE + JFP_JV_MAX_IMAGE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = WdfRequestRetrieveInputBuffer(request, input, (PVOID *)&payload, NULL);
        if (!NT_SUCCESS(status)) break;
        if (payload->Size != input || payload->Version != JFP_VERSION || payload->SpiSize < 64 ||
            payload->SpiSize > JFP_TA_MAX_IMAGE || payload->JvSize < 64 || payload->JvSize > JFP_JV_MAX_IMAGE ||
            payload->SpiSize + payload->JvSize != input - JFP_CAPTURE_HEADER_SIZE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Tee.Info), &buffer, NULL);
        if (!NT_SUCCESS(status)) break;
        device->Tee.Info.Status = code == JFP_INIT_PROBE ?
            JfpQteeInitProbe(device, payload->Images, payload->SpiSize,
                payload->Images + payload->SpiSize, payload->JvSize) :
            code == JFP_ENROLL_PREFLIGHT ? JfpQteeEnrollPreflight(device, payload->Images, payload->SpiSize,
                payload->Images + payload->SpiSize, payload->JvSize) :
            code == JFP_SESSION_BEGIN ? JfpQteeSessionBegin(device, payload->Images, payload->SpiSize,
                payload->Images + payload->SpiSize, payload->JvSize) :
            code == JFP_FEATURE_PROBE ? JfpQteeFeatureProbe(device, payload->Images, payload->SpiSize,
                payload->Images + payload->SpiSize, payload->JvSize) :
            JfpQteeCaptureProbe(device, payload->Images, payload->SpiSize,
                payload->Images + payload->SpiSize, payload->JvSize);
        if (code == JFP_SESSION_BEGIN && NT_SUCCESS(device->Tee.Info.Status))
            device->SessionOwner = WdfRequestGetFileObject(request);
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    }
    case JFP_SESSION_STATUS:
        if (input) { status = STATUS_INVALID_PARAMETER; break; }
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    case JFP_SESSION_SAMPLE:
    case JFP_SESSION_END: {
        PVOID buffer = NULL;
        if (input) { status = STATUS_INVALID_PARAMETER; break; }
        if (!device->SessionOwner || WdfRequestGetFileObject(request) != device->SessionOwner) {
            status = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Tee.Info), &buffer, NULL);
        if (!NT_SUCCESS(status)) break;
        device->Tee.Info.Status = code == JFP_SESSION_SAMPLE ? JfpQteeSessionSample(device) :
            JfpQteeSessionEnd(device);
        if (code == JFP_SESSION_END) device->SessionOwner = NULL;
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    }
    case JFP_SPI_SERVICE_PROBE: {
        PVOID buffer = NULL;
        if (input) { status = STATUS_INVALID_PARAMETER; break; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Tee.Info), &buffer, NULL);
        if (!NT_SUCCESS(status)) break;
        device->Tee.Info.Status = JfpQteeSpiServiceProbe(device);
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    }
    case JFP_TEE_CONTINUE: {
        PVOID buffer = NULL;
        if (input) { status = STATUS_INVALID_PARAMETER; break; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Tee.Info), &buffer, NULL);
        if (!NT_SUCCESS(status)) break;
        device->Tee.Info.Status = JfpQteeContinue(device);
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    }
    case JFP_TEE_PROBE: {
        PVOID buffer = NULL;
        ULONG *uid = NULL;
        if (input != sizeof(*uid)) { status = STATUS_INVALID_PARAMETER; break; }
        status = WdfRequestRetrieveInputBuffer(request, sizeof(*uid), (PVOID *)&uid, NULL);
        if (!NT_SUCCESS(status)) break;
        if (*uid != 16 && *uid != 122 && *uid != 2033) { status = STATUS_INVALID_PARAMETER; break; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Tee.Info), &buffer, NULL);
        if (!NT_SUCCESS(status)) break;
        device->Tee.Info.Status = JfpQteeProbe(device, *uid);
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    }
    case JFP_TA_PROBE: {
        JFP_TA_INPUT *payload = NULL;
        PVOID buffer = NULL;
        if (input < JFP_TA_HEADER_SIZE + 64 || input > JFP_TA_HEADER_SIZE + JFP_JV_MAX_IMAGE) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        status = WdfRequestRetrieveInputBuffer(request, input, (PVOID *)&payload, NULL);
        if (!NT_SUCCESS(status)) break;
        if (payload->Size != input || payload->Version != JFP_VERSION || payload->Reserved > 9 ||
            payload->ImageSize != input - JFP_TA_HEADER_SIZE) { status = STATUS_INVALID_PARAMETER; break; }
        status = WdfRequestRetrieveOutputBuffer(request, sizeof(device->Tee.Info), &buffer, NULL);
        if (!NT_SUCCESS(status)) break;
        device->Tee.Info.Status = payload->Reserved >= 7 ?
            JfpQteeKeymasterProbe(device, payload->Image, payload->ImageSize, payload->Reserved) : payload->Reserved == 6 ?
            JfpQteeConfigProbe(device, payload->Image, payload->ImageSize) : payload->Reserved == 4 ?
            JfpQteeJvProbe(device, payload->Image, payload->ImageSize) : payload->Reserved ?
            JfpQteeSpiProbe(device, payload->Image, payload->ImageSize, payload->Reserved) :
            JfpQteeLoadProbe(device, payload->Image, payload->ImageSize);
        status = Output(request, &device->Tee.Info, sizeof(device->Tee.Info));
        break;
    }
    default:
        break;
    }
    if (status != STATUS_PENDING) WdfRequestComplete(request, status);
}

static NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(raw);
    UNREFERENCED_PARAMETER(translated);
    return JfpPrepare(JfpContext(device));
}

static NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    JfpRelease(JfpContext(device));
    return STATUS_SUCCESS;
}

static VOID FileCleanup(WDFFILEOBJECT file) {
    JFP_DEVICE *device = JfpContext(WdfFileObjectGetDevice(file));
    if (device->SessionOwner == file) {
        JfpQteeSessionEnd(device);
        device->SessionOwner = NULL;
    }
}

static NTSTATUS Add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\JiiovFp");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\JiiovFp");
    DECLARE_CONST_UNICODE_STRING(security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");
    WdfDeviceInitSetDeviceType(init, FILE_DEVICE_BIOMETRIC);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WDF_FILEOBJECT_CONFIG files;
    WDF_FILEOBJECT_CONFIG_INIT(&files, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, FileCleanup);
    files.FileObjectClass = WdfFileObjectWdfCannotUseFsContexts;
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&fileAttributes);
    fileAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    fileAttributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    WdfDeviceInitSetFileObjectConfig(init, &files, &fileAttributes);
    NTSTATUS status = WdfDeviceInitAssignSDDLString(init, &security);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    WDF_PNPPOWER_EVENT_CALLBACKS callbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&callbacks);
    callbacks.EvtDevicePrepareHardware = Prepare;
    callbacks.EvtDeviceReleaseHardware = Release;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &callbacks);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, JFP_DEVICE);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    JfpContext(device)->Device = device;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_BIOMETRIC_READER, NULL);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG queue;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queue, WdfIoQueueDispatchSequential);
    queue.EvtIoDeviceControl = Control;
    return WdfIoQueueCreate(device, &queue, WDF_NO_OBJECT_ATTRIBUTES, NULL);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Add);
    return WdfDriverCreate(driver, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
