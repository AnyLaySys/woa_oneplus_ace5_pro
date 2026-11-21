#if defined(SENSOR_SEE)
static ULONG SeeActivityState(const ULONG *states, ULONG count) {
    if (count == 1) return states[0] == 1 ? 2u : states[0] == 4 ? 32u : 1u;
    if (count == 2) {
        if (states[0] == 2) return states[1] == 5 ? 8u : states[1] == 6 ? 16u : 1u;
        if (states[0] == 3 && states[1] == 7) return 64u;
        if (states[0] == 4) return states[1] == 1 ? 2u : states[1] == 8 || states[1] == 9 ? 32u : 1u;
    }
    if (count == 3 && states[2] == 1 &&
        ((states[0] == 3 && states[1] == 7) || (states[0] == 4 && states[1] == 8)))
        return 2u;
    return 1u;
}
static VOID SeeActivity(SEE_PB payload) {
    ULONG states[3], count = 0, tag;
    ULONGLONG value;
    SEE_PB data;
    while (payload.At < payload.End) {
        if (!SeeField(&payload, &tag, &data, &value)) return;
        if (tag != 1) continue;
        if (data.At) {
            while (data.At < data.End) {
                if (count == RTL_NUMBER_OF(states) || !SeeVar(&data, &value) || value > 9) return;
                states[count++] = (ULONG)value;
            }
        } else {
            if (count == RTL_NUMBER_OF(states) || value > 9) return;
            states[count++] = (ULONG)value;
        }
    }
    if (count) SeeValue(&Activity, (LONG)SeeActivityState(states, count), 0, 0, STATUS_SUCCESS);
}
static VOID SeeMotionActivity(ULONG kind, ULONG id, SEE_PB payload) {
    ULONG state = 0, tag;
    BOOLEAN seen = FALSE;
    ULONGLONG value;
    SEE_PB data;
    while (payload.At < payload.End) {
        if (!SeeField(&payload, &tag, &data, &value)) return;
        if (id == 772 && tag == 1 && !data.At && value <= 2) {
            state = value == 1 ? 2u : 1u;
            seen = TRUE;
        }
    }
    if (seen) SeeValue(&Samples[kind], (LONG)state, 0, 0, STATUS_SUCCESS);
}
static VOID SeeClassifier(ULONG kind, ULONG id, SEE_PB payload) {
    UNREFERENCED_PARAMETER(kind);
    if (id == 800) SeeActivity(payload);
}
static const SEE_TYPE MotionActivitySource = { "rmd", QcomActivity, 514, SeeMotionActivity };
static const SEE_TYPE ActivitySource = { "activity_recognition", QcomActivity, 514, SeeClassifier };
#elif defined(SENSOR_NATIVE)
static const PROPERTYKEY ActFields[] = { PKEY_SensorData_Timestamp, PKEY_SensorData_CurrentActivityState,
    PKEY_SensorData_CurrentActivityStateConfidence_Percentage };
static VOID InitActivity(SENSOR *s) {
    InitPropVariantFromUInt32(1, &s->Data->List[1].Value);
    InitPropVariantFromUInt16(0, &s->Data->List[2].Value);
    InitPropVariantFromUInt32(QCOM_ACTIVITY_SUPPORTED, &s->Enumeration->List[11].Value);
    InitPropVariantFromUInt32(QCOM_ACTIVITY_SUPPORTED, &s->Properties->List[8].Value);
    s->Thresholds->List[0].Key = PKEY_SensorData_SubscribedActivityStates;
    InitPropVariantFromUInt32(QCOM_ACTIVITY_SUPPORTED, &s->Thresholds->List[0].Value);
    s->Thresholds->List[1].Key = PKEY_SensorData_ActivityStream;
    InitPropVariantFromBoolean(TRUE, &s->Thresholds->List[1].Value);
    s->Thresholds->List[2].Key = PKEY_SensorData_ConfidenceThreshold_Percentage;
    InitPropVariantFromUInt16(0, &s->Thresholds->List[2].Value);
}
static VOID ReportActivity(SENSOR *s, const QCOM_ROTATION *q) {
    ULONG state = (ULONG)q->Micro[0] & s->Subscribed;
    if (!state) state = ActivityState_Unknown;
    InitPropVariantFromUInt32(state, &s->Data->List[1].Value);
    InitPropVariantFromUInt16(0, &s->Data->List[2].Value);
}
static const NATIVE_CONFIG ActivityConfig = { QcomActivity, &GUID_SensorType_ActivityDetection,
    &GUID_SensorCategory_PersonalActivity, L"Qualcomm Activity Detection", L"Qualcomm", ActFields,
    &PKEY_SensorData_SupportedActivityStates, 3, 2, 3, 1, 0, 100, TRUE, InitActivity, ReportActivity };
#endif
