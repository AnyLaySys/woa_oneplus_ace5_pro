#if defined(SENSOR_NATIVE)
static NTSTATUS HallDevice(WDFDEVICE device, BOOLEAN *hall) {
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &DEVPKEY_Device_HardwareIds);
    WCHAR ids[64] = { 0 };
    ULONG bytes;
    DEVPROPTYPE type;
    NTSTATUS status = WdfDeviceQueryPropertyEx(device, &property, sizeof(ids), ids, &bytes, &type);
    if (NT_SUCCESS(status)) *hall = type == DEVPROP_TYPE_STRING_LIST && !_wcsicmp(ids, L"ROOT\\AKMHALL");
    return status;
}
static const GUID HallType = { 0x9a123699, 0x2026, 0x1120,
    { 0xa1, 0x61, 0x41, 0x4b, 0x30, 0x39, 0x39, 0x70 } };
static const PROPERTYKEY HallFields[] = { PKEY_SensorData_Timestamp, PKEY_SensorData_CustomValue0,
    PKEY_SensorData_CustomValue1, PKEY_SensorData_CustomValue2, PKEY_SensorData_CustomValue3 };
static VOID InitHall(SENSOR *s) {
    for (ULONG i = 1; i < 4; i++) InitPropVariantFromInt32(0, &s->Data->List[i].Value);
    InitPropVariantFromUInt32(0, &s->Data->List[4].Value);
    InitPropVariantFromCLSID(HallType, &s->Enumeration->List[11].Value);
    for (ULONG i = 0; i < 3; i++) {
        s->Thresholds->List[i].Key = HallFields[i + 1];
        InitPropVariantFromInt32(1, &s->Thresholds->List[i].Value);
    }
}
static VOID ReportHall(SENSOR *s, const QCOM_ROTATION *q) {
    for (ULONG i = 0; i < 3; i++) InitPropVariantFromInt32(q->Micro[i], &s->Data->List[i + 1].Value);
    InitPropVariantFromUInt32((ULONG)q->Micro[3], &s->Data->List[4].Value);
}
static BOOLEAN ReadHall(HANDLE backend, QCOM_ROTATION *sample) {
    AKM_HALL_SAMPLE raw{};
    DWORD bytes = 0;
    if (!DeviceIoControl(backend, AKM_HALL_READ, NULL, 0, &raw, sizeof(raw), &bytes, NULL) ||
        bytes != sizeof(raw))
        return FALSE;
    sample->Time = raw.Time;
    sample->Status = STATUS_SUCCESS;
    sample->Micro[0] = raw.X;
    sample->Micro[1] = raw.Y;
    sample->Micro[2] = raw.Z;
    sample->Micro[3] = (LONG)raw.Position;
    return TRUE;
}
static const NATIVE_CONFIG HallConfig = { QcomHall, &GUID_SensorType_Custom, &GUID_SensorCategory_Other,
    L"AKM AK09970 Hall Sensor", L"AKM", HallFields, &DEVPKEY_Sensor_VendorDefinedSubType, 5, 3, 3, 1, -32768,
    32767, FALSE, InitHall, ReportHall };
#elif defined(SENSOR_SERVICE)
static DWORD WINAPI HallWorker(PVOID) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    IMMDeviceEnumerator *enumerator = nullptr;
    HANDLE device = INVALID_HANDLE_VALUE;
    ULONG position = 0;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        (void **)&enumerator);
    while (WaitForSingleObject(Stop, 200) == WAIT_TIMEOUT) {
        if (device == INVALID_HANDLE_VALUE)
            device = CreateFileW(L"\\\\.\\QcomHaptics", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, 0, nullptr);
        AKM_HALL_SAMPLE sample{};
        DWORD bytes = 0;
        if (device == INVALID_HANDLE_VALUE) continue;
        if (!DeviceIoControl(device, AKM_HALL_READ, nullptr, 0, &sample, sizeof(sample), &bytes, nullptr) ||
            bytes != sizeof(sample)) {
            CloseHandle(device);
            device = INVALID_HANDLE_VALUE;
            continue;
        }
        if (sample.Position < 1 || sample.Position > 3 || sample.Position == position) continue;
        if (!enumerator)
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                (void **)&enumerator);
        IMMDevice *endpoint = nullptr;
        IAudioEndpointVolume *volume = nullptr;
        if (enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint))) {
            if (SUCCEEDED(endpoint->Activate(
                    __uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void **)&volume))) {
                if (SUCCEEDED(volume->SetMute(sample.Position == 1, nullptr))) position = sample.Position;
                volume->Release();
            }
            endpoint->Release();
        }
    }
    if (device != INVALID_HANDLE_VALUE) CloseHandle(device);
    if (enumerator) enumerator->Release();
    CoUninitialize();
    return 0;
}
#elif defined(SENSOR_HARDWARE)
typedef struct {
    PUCHAR Base;
    ULONG Saved[2][2], Config[2];
    BOOLEAN Configured;
    NTSTATUS Status;
    AKM_HALL_SAMPLE Sample;
    UCHAR Candidate, Confirm;
} HALL;
static ULONG HallRegister(HALL *h, ULONG pin, ULONG offset) {
    return READ_REGISTER_ULONG((PULONG)(h->Base + pin * 0x1000 + offset));
}
static VOID HallPin(HALL *h, ULONG pin, BOOLEAN low) {
    if (low) WRITE_REGISTER_ULONG((PULONG)(h->Base + pin * 0x1000 + 4), 0);
    WRITE_REGISTER_ULONG((PULONG)(h->Base + pin * 0x1000), h->Config[pin] | (low ? 0x200u : 0));
    KeStallExecutionProcessor(5);
}
static BOOLEAN HallClock(HALL *h) {
    HallPin(h, 1, FALSE);
    for (ULONG i = 0; i < 500; i++) {
        if (HallRegister(h, 1, 4) & 1) return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}
static BOOLEAN HallStartBit(HALL *h) {
    HallPin(h, 0, FALSE);
    if (!HallClock(h)) return FALSE;
    HallPin(h, 0, TRUE);
    HallPin(h, 1, TRUE);
    return TRUE;
}
static VOID HallStopBit(HALL *h) {
    HallPin(h, 0, TRUE);
    HallClock(h);
    HallPin(h, 0, FALSE);
}
static BOOLEAN HallByte(HALL *h, UCHAR value) {
    for (ULONG i = 0; i < 8; i++) {
        HallPin(h, 0, (value & 0x80) == 0);
        if (!HallClock(h)) return FALSE;
        HallPin(h, 1, TRUE);
        value <<= 1;
    }
    HallPin(h, 0, FALSE);
    if (!HallClock(h)) return FALSE;
    BOOLEAN ack = !(HallRegister(h, 0, 4) & 1);
    HallPin(h, 1, TRUE);
    return ack;
}
static NTSTATUS HallTransfer(HALL *h, UCHAR reg, PUCHAR data, ULONG count, BOOLEAN write) {
    BOOLEAN ok = HallStartBit(h) && HallByte(h, 0x20) && HallByte(h, reg);
    if (!write && ok) ok = HallStartBit(h) && HallByte(h, 0x21);
    for (ULONG i = 0; ok && i < count; i++) {
        if (write) {
            ok = HallByte(h, data[i]);
            continue;
        }
        UCHAR value = 0;
        HallPin(h, 0, FALSE);
        for (ULONG bit = 0; bit < 8; bit++) {
            if (!(ok = HallClock(h))) break;
            value = (UCHAR)((value << 1) | (HallRegister(h, 0, 4) & 1));
            HallPin(h, 1, TRUE);
        }
        if (!ok) break;
        data[i] = value;
        HallPin(h, 0, i + 1 < count);
        ok = HallClock(h);
        HallPin(h, 1, TRUE);
    }
    HallStopBit(h);
    return ok ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}
static VOID HallStart(HALL *h) {
    h->Status = STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(&h->Sample, sizeof(h->Sample));
    h->Candidate = h->Confirm = 0;
    if (!h->Base) return;
    for (ULONG i = 0; i < 2; i++) {
        h->Saved[i][0] = HallRegister(h, i, 0);
        h->Saved[i][1] = HallRegister(h, i, 4) & 2;
        h->Config[i] = (h->Saved[i][0] & ~0x3ffu) | 0x1003;
        HallPin(h, i, FALSE);
    }
    h->Configured = TRUE;
    UCHAR id[2];
    h->Status = HallTransfer(h, 0, id, 2, FALSE);
    if (!NT_SUCCESS(h->Status)) return;
    if (id[0] != 0x48 || id[1] != 0xc1) {
        h->Status = STATUS_NO_SUCH_DEVICE;
        return;
    }
    UCHAR value = 1;
    h->Status = HallTransfer(h, 0x30, &value, 1, TRUE);
    if (!NT_SUCCESS(h->Status)) return;
    LARGE_INTEGER delay;
    delay.QuadPart = -50000;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    value = 0x2a;
    h->Status = HallTransfer(h, 0x21, &value, 1, TRUE);
}
static VOID HallStop(HALL *h) {
    if (!h->Configured) return;
    if (NT_SUCCESS(h->Status)) {
        UCHAR value = 0;
        HallTransfer(h, 0x21, &value, 1, TRUE);
    }
    for (ULONG i = 0; i < 2; i++) {
        WRITE_REGISTER_ULONG((PULONG)(h->Base + i * 0x1000 + 4), h->Saved[i][1]);
        WRITE_REGISTER_ULONG((PULONG)(h->Base + i * 0x1000), h->Saved[i][0]);
    }
    h->Configured = FALSE;
    h->Status = STATUS_DEVICE_NOT_READY;
    h->Sample.Time = 0;
}
static NTSTATUS HallRead(HALL *h, AKM_HALL_SAMPLE *sample) {
    if (!NT_SUCCESS(h->Status)) return h->Status;
    ULONGLONG now = KeQueryInterruptTime();
    if (!h->Sample.Time || now - h->Sample.Time >= 1000000) {
        UCHAR bytes[7];
        NTSTATUS status = HallTransfer(h, 0x17, bytes, sizeof(bytes), FALSE);
        if (!NT_SUCCESS(status)) {
            h->Confirm = 0;
            return status;
        }
        if (bytes[0] & 1) {
            h->Sample.Z = (SHORT)(((ULONG)bytes[1] << 8) | bytes[2]);
            h->Sample.Y = (SHORT)(((ULONG)bytes[3] << 8) | bytes[4]);
            h->Sample.X = (SHORT)(((ULONG)bytes[5] << 8) | bytes[6]);
            h->Sample.Time = now;
            LONG x = h->Sample.Y < 0 ? -h->Sample.X : h->Sample.X;
            UCHAR position = x > 3000 ? 1 : x < -3000 ? 3 : 2;
            if (position != h->Candidate) {
                h->Candidate = position;
                h->Confirm = 1;
            } else if (h->Confirm < 2) h->Confirm++;
            if (h->Confirm == 2) h->Sample.Position = position;
        }
    }
    if (!h->Sample.Time || now - h->Sample.Time > 5000000) {
        h->Confirm = 0;
        return STATUS_DEVICE_NOT_READY;
    }
    *sample = h->Sample;
    return STATUS_SUCCESS;
}
#endif
