#if defined(SENSOR_SEE)
static VOID SeePedometer(ULONG kind, ULONG id, SEE_PB payload) {
    ULONG count = 0, tag;
    BOOLEAN seen = FALSE;
    ULONGLONG value;
    SEE_PB data;
    while (payload.At < payload.End) {
        if (!SeeField(&payload, &tag, &data, &value)) return;
        if ((id == 1028 || id == 775) && tag == 1 && !data.At && value <= MAXULONG) {
            count = (ULONG)value;
            seen = TRUE;
        }
    }
    if (seen) SeeValue(&Samples[kind], (LONG)count, 0, 0, STATUS_SUCCESS);
}
static const SEE_TYPE PedometerSource = { "pedometer", QcomPedometer, 514, SeePedometer };
#elif defined(SENSOR_NATIVE)
static const PROPERTYKEY StepFields[] = { PKEY_SensorData_Timestamp, PKEY_SensorData_PedometerReset,
    PKEY_SensorData_PedometerStepType, PKEY_SensorData_PedometerStepCount,
    PKEY_SensorData_PedometerStepDuration_Ms };
static VOID InitPedometer(SENSOR *s) {
    InitPropVariantFromBoolean(TRUE, &s->Data->List[1].Value);
    InitPropVariantFromUInt32(1, &s->Data->List[2].Value);
    InitPropVariantFromUInt32(0, &s->Data->List[3].Value);
    InitPropVariantFromInt64(0, &s->Data->List[4].Value);
    InitPropVariantFromUInt32(1, &s->Enumeration->List[11].Value);
    InitPropVariantFromUInt32(1, &s->Properties->List[8].Value);
    s->Thresholds->List[0].Key = PKEY_SensorData_PedometerStepCount;
    InitPropVariantFromUInt32(1, &s->Thresholds->List[0].Value);
}
static VOID ReportPedometer(SENSOR *s, const QCOM_ROTATION *q) {
    InitPropVariantFromBoolean(
        s->First || (ULONG)q->Micro[0] < s->Data->List[3].Value.ulVal, &s->Data->List[1].Value);
    InitPropVariantFromUInt32(PedometerStepType_Unknown, &s->Data->List[2].Value);
    InitPropVariantFromUInt32((ULONG)q->Micro[0], &s->Data->List[3].Value);
    InitPropVariantFromInt64(0, &s->Data->List[4].Value);
}
static const NATIVE_CONFIG PedometerConfig = { QcomPedometer, &GUID_SensorType_Pedometer,
    &GUID_SensorCategory_PersonalActivity, L"Qualcomm Pedometer", L"Qualcomm", StepFields,
    &PKEY_SensorData_SupportedStepTypes, 5, 4, 1, 1, 0, (float)0xffffffffu, TRUE, InitPedometer,
    ReportPedometer };
#endif
