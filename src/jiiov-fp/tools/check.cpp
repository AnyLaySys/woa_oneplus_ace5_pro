#include <windows.h>
#include <winbio.h>
#include <winbio_ioctl.h>
#include <stdio.h>
#include "../uapi.h"

static BOOL Query(HANDLE device, ULONG code, void *output, DWORD size) {
    DWORD bytes = 0;
    if (!DeviceIoControl(device, code, nullptr, 0, output, size, &bytes, nullptr)) {
        fwprintf(stderr, L"IOCTL 0x%08lx failed: %lu\n", code, GetLastError());
        return FALSE;
    }
    if (bytes != size) {
        fwprintf(stderr, L"IOCTL 0x%08lx returned %lu of %lu bytes\n", code, bytes, size);
        return FALSE;
    }
    return TRUE;
}

static BOOL Verify(HANDLE device) {
    DWORD bytes = 0, required = 0;
    if (!DeviceIoControl(device, IOCTL_BIOMETRIC_GET_ATTRIBUTES, nullptr, 0, &required,
        sizeof(required), &bytes, nullptr) || bytes != sizeof(required) || required != sizeof(WINBIO_SENSOR_ATTRIBUTES))
        return FALSE;
    UCHAR tiny[3] = {};
    if (DeviceIoControl(device, IOCTL_BIOMETRIC_GET_ATTRIBUTES, nullptr, 0, tiny, sizeof(tiny), &bytes, nullptr))
        return FALSE;
    ULONG invalid = 1;
    JFP_DIAGNOSTICS diagnostics = {};
    if (DeviceIoControl(device, JFP_QUERY, &invalid, sizeof(invalid), &diagnostics,
        sizeof(diagnostics), &bytes, nullptr)) return FALSE;
    if (DeviceIoControl(device, JFP_INITIALIZE, nullptr, 0, &required,
        sizeof(required), &bytes, nullptr)) return FALSE;
    WINBIO_CAPTURE_PARAMETERS parameters = {};
    parameters.PayloadSize = sizeof(parameters);
    parameters.Purpose = WINBIO_PURPOSE_ENROLL;
    parameters.Format.Owner = WINBIO_ANSI_381_FORMAT_OWNER;
    parameters.Format.Type = WINBIO_ANSI_381_FORMAT_TYPE;
    parameters.Flags = WINBIO_DATA_FLAG_RAW;
    WINBIO_CAPTURE_DATA capture = {};
    if (!DeviceIoControl(device, IOCTL_BIOMETRIC_CAPTURE_DATA, &parameters, sizeof(parameters),
        &capture, sizeof(capture), &bytes, nullptr) || bytes != sizeof(capture) ||
        capture.WinBioHresult != WINBIO_E_SENSOR_UNAVAILABLE || capture.CaptureData.Size ||
        capture.SensorStatus != WINBIO_SENSOR_FAILURE) return FALSE;
    parameters.PayloadSize--;
    if (DeviceIoControl(device, IOCTL_BIOMETRIC_CAPTURE_DATA, &parameters, sizeof(parameters),
        &capture, sizeof(capture), &bytes, nullptr)) return FALSE;
    wprintf(L"WBDI buffer, request and unavailable-capture checks passed\n");
    return TRUE;
}

int wmain(int argc, wchar_t **argv) {
    if (argc > 2 || (argc == 2 && wcscmp(argv[1], L"init") && wcscmp(argv[1], L"verify") && wcscmp(argv[1], L"status"))) {
        fwprintf(stderr, L"Usage: jiiov_fp_check [init|verify|status]\n");
        return 2;
    }
    HANDLE device = CreateFileW(L"\\\\.\\JiiovFp", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"Open JiiovFp failed: %lu\n", GetLastError());
        return 1;
    }
    WINBIO_SENSOR_ATTRIBUTES attributes = {};
    if (!Query(device, IOCTL_BIOMETRIC_GET_ATTRIBUTES, &attributes, sizeof(attributes))) {
        CloseHandle(device);
        return 1;
    }
    wprintf(L"Manufacturer=%ls Model=%ls Capabilities=0x%08lx\n", attributes.ManufacturerName,
        attributes.ModelName, attributes.Capabilities);
    if (argc == 2 && !wcscmp(argv[1], L"status")) {
        WINBIO_DIAGNOSTICS sensor = {};
        BOOL ok = Query(device, IOCTL_BIOMETRIC_GET_SENSOR_STATUS, &sensor, sizeof(sensor));
        if (ok) wprintf(L"WinBioHresult=0x%08lx SensorStatus=%lu\n", (ULONG)sensor.WinBioHresult, sensor.SensorStatus);
        CloseHandle(device);
        return ok ? 0 : 1;
    }
    JFP_DIAGNOSTICS info = {};
    BOOL initialize = argc == 2 && !wcscmp(argv[1], L"init");
    BOOL ok = Query(device, initialize ? JFP_INITIALIZE : JFP_QUERY, &info, sizeof(info));
    if (ok) {
        wprintf(L"Status=0x%08lx Flags=0x%08lx Spi=0x%08lx ClockVote=0x%08lx ClockState=0x%08lx\n",
            (ULONG)info.Status, info.Flags, info.SpiAddress, info.ClockVote, info.ClockState);
        wprintf(L"Firmware=0x%08lx InterfaceDisable=0x%08lx Hardware=0x%08lx Sequencer=0x%08lx Irq=0x%08lx\n",
            info.Firmware, info.InterfaceDisable, info.HardwareParameters, info.SequencerStatus, info.IrqStatus);
        wprintf(L"Pins=%08lx,%08lx,%08lx,%08lx Reset=%08lx,%08lx PowerAddress=%08lx ChipId=%04lx Transfers=%lu\n",
            info.Pins[0], info.Pins[1], info.Pins[2], info.Pins[3], info.ResetConfig, info.ResetValue,
            info.PowerAddress, info.ChipId, info.TransferCount);
        wprintf(L"LastTx=%02x,%02x,%02x,%02x LastRx=%02x,%02x,%02x,%02x\n",
            info.LastTx[0], info.LastTx[1], info.LastTx[2], info.LastTx[3],
            info.LastRx[0], info.LastRx[1], info.LastRx[2], info.LastRx[3]);
        wprintf(L"RegisterAddress=%08lx RegisterStatus=%08lx Scm=%016llx,%016llx,%016llx,%016llx\n",
            info.RegisterAddress, (ULONG)info.RegisterStatus, info.ScmReturn[0], info.ScmReturn[1],
            info.ScmReturn[2], info.ScmReturn[3]);
    }
    WINBIO_DIAGNOSTICS sensor = {};
    if (Query(device, IOCTL_BIOMETRIC_GET_SENSOR_STATUS, &sensor, sizeof(sensor)))
        wprintf(L"WinBioHresult=0x%08lx SensorStatus=%lu\n", (ULONG)sensor.WinBioHresult, sensor.SensorStatus);
    if (argc == 2 && !wcscmp(argv[1], L"verify")) ok = Verify(device) && ok;
    CloseHandle(device);
    WINBIO_UNIT_SCHEMA *units = nullptr;
    SIZE_T count = 0;
    HRESULT result = WinBioEnumBiometricUnits(WINBIO_TYPE_FINGERPRINT, &units, &count);
    wprintf(L"WinBioEnumBiometricUnits=0x%08lx Units=%llu\n", (ULONG)result, (ULONGLONG)count);
    if (SUCCEEDED(result)) {
        for (SIZE_T i = 0; i < count; i++) wprintf(L"Unit=%lu Description=%ls\n", units[i].UnitId, units[i].Description);
        WinBioFree(units);
    }
    return ok && (!initialize || info.Status >= 0) ? 0 : 1;
}
