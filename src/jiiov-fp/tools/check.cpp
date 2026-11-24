#include <windows.h>
#include <winbio.h>
#include <winbio_ioctl.h>
#include <stdio.h>
#include "../uapi.h"

static BOOL Query(HANDLE device, ULONG code, void *output, DWORD size, const void *input = nullptr, DWORD inputSize = 0) {
    DWORD bytes = 0;
    if (!DeviceIoControl(device, code, const_cast<void *>(input), inputSize, output, size, &bytes, nullptr)) {
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
    JFP_TEE_DIAGNOSTICS tee = {};
    ULONG uid = 0;
    if (DeviceIoControl(device, JFP_TEE_PROBE, &uid, sizeof(uid), &tee,
        sizeof(tee), &bytes, nullptr)) return FALSE;
    uid = 16;
    if (DeviceIoControl(device, JFP_TEE_PROBE, &uid, sizeof(uid), &required,
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

static BOOL LoadImage(HANDLE device, const wchar_t *path, JFP_TEE_DIAGNOSTICS *diagnostics, ULONG operation) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"Open TA image failed: %lu\n", GetLastError());
        return FALSE;
    }
    LARGE_INTEGER length = {};
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 64 || length.QuadPart > (operation >= 7 ? JFP_KM_MAX_IMAGE : operation == 4 || operation == 6 ? JFP_JV_MAX_IMAGE : JFP_TA_MAX_IMAGE)) {
        CloseHandle(file);
        return FALSE;
    }
    DWORD size = (DWORD)length.QuadPart;
    DWORD total = JFP_TA_HEADER_SIZE + size;
    JFP_TA_INPUT *payload = (JFP_TA_INPUT *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
    if (!payload) { CloseHandle(file); return FALSE; }
    payload->Size = total;
    payload->Version = JFP_VERSION;
    payload->ImageSize = size;
    payload->Reserved = operation;
    DWORD read = 0;
    BOOL ok = ReadFile(file, payload->Image, size, &read, nullptr) && read == size;
    CloseHandle(file);
    if (ok) ok = Query(device, JFP_TA_PROBE, diagnostics, sizeof(*diagnostics), payload, total);
    SecureZeroMemory(payload, total);
    HeapFree(GetProcessHeap(), 0, payload);
    return ok;
}

static BOOL Capture(HANDLE device, const wchar_t *spiPath, const wchar_t *jvPath,
    JFP_TEE_DIAGNOSTICS *diagnostics, ULONG operation) {
    HANDLE files[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    const wchar_t *paths[2] = {spiPath, jvPath};
    ULONG sizes[2] = {};
    BOOL ok = TRUE;
    for (ULONG i = 0; i < 2; i++) {
        files[i] = CreateFileW(paths[i], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size = {};
        if (files[i] == INVALID_HANDLE_VALUE || !GetFileSizeEx(files[i], &size) || size.QuadPart < 64 ||
            size.QuadPart > (i ? JFP_JV_MAX_IMAGE : JFP_TA_MAX_IMAGE)) { ok = FALSE; break; }
        sizes[i] = (ULONG)size.QuadPart;
    }
    ULONG total = JFP_CAPTURE_HEADER_SIZE + sizes[0] + sizes[1];
    JFP_CAPTURE_INPUT *payload = ok ? (JFP_CAPTURE_INPUT *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total) : nullptr;
    if (!payload) ok = FALSE;
    if (ok) {
        payload->Size = total;
        payload->Version = JFP_VERSION;
        payload->SpiSize = sizes[0];
        payload->JvSize = sizes[1];
        ULONG offset = 0;
        for (ULONG i = 0; ok && i < 2; i++) {
            DWORD read = 0;
            ok = ReadFile(files[i], payload->Images + offset, sizes[i], &read, nullptr) && read == sizes[i];
            offset += sizes[i];
        }
        if (ok) ok = Query(device, operation == 4 ? JFP_ENROLL_PREFLIGHT : operation == 3 ? JFP_SESSION_BEGIN : operation == 0 ? JFP_INIT_PROBE : operation == 2 ? JFP_FEATURE_PROBE : JFP_CAPTURE_PROBE,
            diagnostics, sizeof(*diagnostics), payload, total);
    }
    for (HANDLE file : files) if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (payload) { SecureZeroMemory(payload, total); HeapFree(GetProcessHeap(), 0, payload); }
    return ok;
}

static int Session(HANDLE device, const wchar_t *spi, const wchar_t *jv, BOOL sample) {
    JFP_TEE_DIAGNOSTICS tee = {};
    BOOL ok = Capture(device, spi, jv, &tee, 3);
    LONG first = ok ? tee.Status : (LONG)0xc0000185;
    wprintf(L"SessionBegin=%08lx InitResult=%08lx AuthStartResult=%08lx Active=%lu\n",
        (ULONG)first, tee.InitResult, tee.AuthStartResult, tee.SessionActive);
    HANDLE observer = INVALID_HANDLE_VALUE;
    if (ok && first >= 0) {
        observer = CreateFileW(L"\\\\.\\JiiovFp", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD bytes = 0;
        JFP_TEE_DIAGNOSTICS other = {};
        BOOL isolated = observer != INVALID_HANDLE_VALUE;
        const ULONG operations[] = {JFP_SESSION_SAMPLE, JFP_SESSION_END, JFP_INITIALIZE};
        for (ULONG operation : operations) {
            if (!isolated) break;
            isolated = !DeviceIoControl(observer, operation, nullptr, 0, &other, sizeof(other), &bytes, nullptr)
                && GetLastError() == ERROR_BUSY;
        }
        wprintf(L"SessionIsolation=%lu\n", isolated);
        if (!isolated) first = (LONG)0xc0000186;
        if (sample && isolated) for (ULONG i = 0; i < 2; i++) {
            ok = Query(device, JFP_SESSION_SAMPLE, &tee, sizeof(tee));
            wprintf(L"Sample=%lu Status=%08lx WakeResult=%08lx CaptureResult=%08lx FeatureCommandResult=%08lx FeatureResult=%08lx FeatureValue=%08lx Count=%lu\n",
                i + 1, ok ? (ULONG)tee.Status : 0xc0000185u, tee.WakeResult, tee.CaptureResult,
                tee.FeatureCommandResult, tee.FeatureResult, tee.FeatureValue, tee.SampleCount);
            if (first >= 0 && (!ok || tee.Status < 0)) first = ok ? tee.Status : (LONG)0xc0000185;
            if (!ok || tee.Pinned == 0) break;
            if (i == 0) Sleep(250);
        }
    }
    if (sample && tee.SessionActive) {
        ok = Query(device, JFP_SESSION_END, &tee, sizeof(tee));
        if (first >= 0 && (!ok || tee.Status < 0)) first = ok ? tee.Status : (LONG)0xc0000185;
    }
    /* The init-only test exercises cleanup when the client closes its handle. */
    CloseHandle(device);
    if (observer != INVALID_HANDLE_VALUE) {
        ok = Query(observer, JFP_SESSION_STATUS, &tee, sizeof(tee));
        if (first >= 0 && (!ok || tee.Pinned || tee.LiveObjects || tee.SessionActive || tee.CleanupStatus < 0))
            first = (LONG)0xc0000186;
        CloseHandle(observer);
    }
    wprintf(L"TeeStatus=%08lx CancelResult=%08lx Cleanup=%08lx Pinned=%lu LiveObjects=%lu Active=%lu Samples=%lu\n",
        (ULONG)first, tee.CancelResult, (ULONG)tee.CleanupStatus, tee.Pinned, tee.LiveObjects,
        tee.SessionActive, tee.SampleCount);
    return first >= 0 ? 0 : 1;
}

int wmain(int argc, wchar_t **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    BOOL initializeOnly = argc >= 2 && !wcscmp(argv[1], L"ta-init");
    BOOL preflight = argc >= 2 && !wcscmp(argv[1], L"enroll-preflight");
    BOOL featureMode = argc >= 2 && !wcscmp(argv[1], L"sample-feature");
    BOOL sessionMode = argc >= 2 && (!wcscmp(argv[1], L"session-feature") || !wcscmp(argv[1], L"session-init"));
    BOOL captureMode = preflight || initializeOnly || featureMode || sessionMode || (argc >= 2 && !wcscmp(argv[1], L"capture"));
    BOOL parameters = argc >= 2 && !wcscmp(argv[1], L"keymaster-params");
    BOOL sharing = argc >= 2 && !wcscmp(argv[1], L"keymaster-sharing");
    BOOL keymaster = parameters || sharing || (argc >= 2 && !wcscmp(argv[1], L"keymaster-auth"));
    BOOL taMode = keymaster || (argc >= 2 && (!wcscmp(argv[1], L"spi-ta") || !wcscmp(argv[1], L"spi-init") || !wcscmp(argv[1], L"spi-chip") || !wcscmp(argv[1], L"spi-power") || !wcscmp(argv[1], L"jv-ta") || !wcscmp(argv[1], L"jv-config") || !wcscmp(argv[1], L"spi-clock")));
    if ((captureMode && argc != 4) || (!captureMode && (argc > 3 || (taMode && argc != 3) || (!taMode && (argc > 2 ||
        (argc == 2 && wcscmp(argv[1], L"init") && wcscmp(argv[1], L"verify") &&
        wcscmp(argv[1], L"status") && wcscmp(argv[1], L"spi-service") && wcscmp(argv[1], L"tee") && wcscmp(argv[1], L"loader") && wcscmp(argv[1], L"feature") && wcscmp(argv[1], L"resume"))))))) {
        fwprintf(stderr, L"Usage: jiiov_fp_check [init|verify|status|tee|loader|feature|resume|spi-service] | [spi-ta|spi-init|spi-chip|spi-power|spi-clock|jv-ta|jv-config|keymaster-auth] <TA.elf> | [ta-init|capture|sample-feature|session-init|session-feature|enroll-preflight] <uff_spi.elf> <uff_jv.elf>\n");
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
    if (keymaster) {
        JFP_TEE_DIAGNOSTICS tee = {};
        BOOL ok = LoadImage(device, argv[2], &tee, parameters ? 8 : sharing ? 9 : 7);
        CloseHandle(device);
        if (!ok) return 1;
        wprintf(L"DiagnosticOnly=1 TeeStatus=%08lx Stage=%lu Command=%04lx KeymasterResult=%08lx ResponseSize=%lu EnvelopeOffset=%lu EnvelopeSize=%lu\n",
            (ULONG)tee.Status, tee.Stage, tee.Command, (ULONG)tee.CommandStatus,
            tee.ResponseSize, tee.Response[1], tee.Response[2]);
        wprintf(L"Cleanup=%08lx Pinned=%lu LiveObjects=%lu HATIssued=0 EnrollmentCompleted=0\n",
            (ULONG)tee.CleanupStatus, tee.Pinned, tee.LiveObjects);
        if (sharing) wprintf(L"GetParametersResult=%08lx ComputeSharingResult=%08lx AuthKeyResult=%08lx\n",
            (ULONG)tee.Flow[0], (ULONG)tee.Flow[1], (ULONG)tee.Flow[2]);
        if (parameters && tee.Status >= 0) {
            wprintf(L"PublicSharingParameters=");
            const UCHAR *bytes = reinterpret_cast<const UCHAR *>(tee.Data);
            for (ULONG i = 0; i < tee.ResponseSize && i < sizeof(tee.Data); i++) wprintf(L"%02x", bytes[i]);
            wprintf(L"\n");
        }
        return tee.Status >= 0 && tee.CleanupStatus >= 0 && !tee.Pinned && !tee.LiveObjects ? 0 : 1;
    }
    if (sessionMode) return Session(device, argv[2], argv[3], !wcscmp(argv[1], L"session-feature"));
    if (preflight) {
        JFP_TEE_DIAGNOSTICS tee = {};
        BOOL ok = Capture(device, argv[2], argv[3], &tee, 4);
        CloseHandle(device);
        if (!ok) return 1;
        wprintf(L"DiagnosticOnly=1 InitResult=%08lx PreEnrollResult=%08lx ChallengePresent=%lu MissingAuthResult=%08lx\n",
            tee.InitResult, tee.PreEnrollResult, tee.ChallengePresent, tee.MissingAuthResult);
        wprintf(L"TeeStatus=%08lx CancelResult=%08lx PostEnrollResult=%08lx Cleanup=%08lx Pinned=%lu LiveObjects=%lu Samples=%lu\n",
            (ULONG)tee.Status, tee.CancelResult, tee.PostEnrollResult, (ULONG)tee.CleanupStatus,
            tee.Pinned, tee.LiveObjects, tee.SampleCount);
        /* Expected rejection validates only the missing-authorization boundary.
           It cannot prove enrollment, calibration sufficiency or matching. */
        BOOL rejected = tee.Status == (LONG)0xc0000185u && !tee.InitResult && !tee.PreEnrollResult &&
            tee.ChallengePresent && tee.MissingAuthResult == 0x69 && !tee.CancelResult &&
            !tee.PostEnrollResult && tee.CleanupStatus >= 0 && !tee.Pinned && !tee.LiveObjects && !tee.SampleCount;
        wprintf(L"ExpectedMissingAuthRejection=%lu EnrollmentCompleted=0\n", rejected);
        return rejected ? 0 : 1;
    }
    if (captureMode || taMode || (argc == 2 && (!wcscmp(argv[1], L"spi-service") || !wcscmp(argv[1], L"tee") || !wcscmp(argv[1], L"loader") || !wcscmp(argv[1], L"feature") || !wcscmp(argv[1], L"resume")))) {
        JFP_TEE_DIAGNOSTICS tee = {};
        ULONG uid = !wcscmp(argv[1], L"loader") ? 122 : !wcscmp(argv[1], L"feature") ? 2033 : 16;
        BOOL ok = captureMode ? Capture(device, argv[2], argv[3], &tee, initializeOnly ? 0 : featureMode ? 2 : 1) : taMode ? LoadImage(device, argv[2], &tee, !wcscmp(argv[1], L"jv-config") ? 6 : !wcscmp(argv[1], L"spi-clock") ? 5 : !wcscmp(argv[1], L"jv-ta") ? 4 : !wcscmp(argv[1], L"spi-power") ? 3 : !wcscmp(argv[1], L"spi-chip") ? 2 : !wcscmp(argv[1], L"spi-init") ? 1 : 0) : !wcscmp(argv[1], L"spi-service") ? Query(device, JFP_SPI_SERVICE_PROBE, &tee, sizeof(tee)) : !wcscmp(argv[1], L"resume") ? Query(device, JFP_TEE_CONTINUE, &tee, sizeof(tee)) : Query(device, JFP_TEE_PROBE, &tee, sizeof(tee), &uid, sizeof(uid));
        if (ok) {
            wprintf(L"TeeStatus=%08lx Stage=%lu Calls=%lu CallId=%08lx ScmStatus=%08lx ObjectStatus=%08lx Response=%lu\n",
                (ULONG)tee.Status, tee.Stage, tee.Calls, tee.CallId, (ULONG)tee.ScmStatus,
                (ULONG)tee.ObjectStatus, tee.ResponseType);
            wprintf(L"Available=%lu Uid=%lu Client=%08lx Service=%08lx Callbacks=%lu Pinned=%lu Cleanup=%08lx\n",
                tee.Available, tee.Uid, tee.Client, tee.Service, tee.Callbacks, tee.Pinned, (ULONG)tee.CleanupStatus);
            wprintf(L"Return=%016llx,%016llx,%016llx,%016llx\n", tee.Return[0], tee.Return[1], tee.Return[2], tee.Return[3]);
            wprintf(L"App=%08lx ImageSize=%lu Maps=%lu Releases=%lu LiveObjects=%lu\n",
                tee.App, tee.ImageSize, tee.Maps, tee.Releases, tee.LiveObjects);
            wprintf(L"Region=%08lx\n", tee.Region);
            for (const JFP_CONFIG_DIAGNOSTICS &config : tee.Config) {
                if (!config.Size) continue;
                wprintf(L"Config=%04lx Size=%lu Status=%08lx Result=%08lx Data=", config.Id,
                    config.Size, (ULONG)config.Status, (ULONG)config.Result);
                for (ULONG i = 0; i < (config.Size + 3) / 4 && i < 32; i++) wprintf(L"%08lx,", config.Data[i]);
                wprintf(L"\n");
            }
            if (captureMode) {
                wprintf(L"Flow=");
                for (LONG step : tee.Flow) wprintf(L"%08lx,", (ULONG)step);
                wprintf(L"\n");
                if (!initializeOnly) wprintf(L"CaptureResult=%08lx\n", tee.CaptureResult);
                if (featureMode) wprintf(L"AuthStartResult=%08lx FeatureResult=%08lx CancelResult=%08lx\n",
                    tee.AuthStartResult, tee.FeatureResult, tee.CancelResult);
                if (featureMode) wprintf(L"FeatureCommandResult=%08lx FeatureValue=%08lx\n",
                    tee.FeatureCommandResult, tee.FeatureValue);
            }
            wprintf(L"AuditStatus=%08lx\n", (ULONG)tee.AuditStatus);
            wprintf(L"TaLogSize=%lu TaLogStatus=%08lx\n", tee.TaLogSize, (ULONG)tee.TaLogStatus);
            wprintf(L"StorageStatus=%08lx Requests=%lu Command=%lu IoStatus=%08lx InitResult=%08lx\n",
                (ULONG)tee.StorageStatus, tee.StorageRequests, tee.StorageCommand,
                (ULONG)tee.StorageIoStatus, tee.InitResult);
            wprintf(L"WakeResult=%08lx\n", tee.WakeResult);
            if (!wcscmp(argv[1], L"spi-service")) wprintf(L"SpiService Connect=%08lx Open=%08lx Transfer=%08lx Chip=%04lx\n",
                (ULONG)tee.Flow[0], (ULONG)tee.Flow[1], (ULONG)tee.Flow[2], tee.Data[0]);
            wprintf(L"Callback=%08lx/%08lx Counts=%04lx Command=%08lx Result=%08lx ResponseSize=%lu\n",
                tee.CallbackContext, tee.CallbackOperation, tee.CallbackCounts, tee.Command,
                (ULONG)tee.CommandStatus, tee.ResponseSize);
            wprintf(L"PowerAddress=%08lx Original=%lu,%lu,%lu PowerStatus=%08lx\n",
                tee.PowerAddress, tee.SavedPower[0], tee.SavedPower[1], tee.SavedPower[2], (ULONG)tee.PowerStatus);
            wprintf(L"ScmRegisters=");
            for (ULONGLONG reg : tee.ScmRegisters) wprintf(L"%016llx,", reg);
            wprintf(L"\nResponse=");
            for (ULONG word : tee.Response) wprintf(L"%08lx,", word);
            wprintf(L"\nData=");
            for (ULONG word : tee.Data) wprintf(L"%08lx,", word);
            wprintf(L"\n");
        }
        CloseHandle(device);
        return ok && tee.Status >= 0 ? 0 : 1;
    }
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
