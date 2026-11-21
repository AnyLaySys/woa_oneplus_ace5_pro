#if defined(SENSOR_SEE)
static VOID SeeProximity(ULONG kind, ULONG id, SEE_PB payload) {
    ULONG near = MAXULONG, status = 0, tag;
    ULONGLONG value;
    SEE_PB data;
    while (payload.At < payload.End) {
        if (!SeeField(&payload, &tag, &data, &value)) return;
        if (id == 769) {
            if (tag == 1) near = (ULONG)value;
            else if (tag == 3) status = (ULONG)value;
        }
    }
    if (status <= 3 && near <= 1 && status) SeeValue(&Samples[kind], (LONG)near, 0, 0, STATUS_SUCCESS);
}
static const SEE_TYPE ProximitySource = { "proximity", QcomProximity, 514, SeeProximity };
#elif defined(SENSOR_NATIVE)
static const PROPERTYKEY ProxFields[] = { PKEY_SensorData_Timestamp, PKEY_SensorData_ProximityDetection };
static VOID InitProximity(SENSOR *s) {
    InitPropVariantFromBoolean(FALSE, &s->Data->List[1].Value);
    InitPropVariantFromUInt32(ProximityType_ObjectProximity, &s->Enumeration->List[11].Value);
}
static VOID ReportProximity(SENSOR *s, const QCOM_ROTATION *q) {
    InitPropVariantFromBoolean(q->Micro[0] != 0, &s->Data->List[1].Value);
}
static const NATIVE_CONFIG ProximityConfig = { QcomProximity, &GUID_SensorType_Proximity,
    &GUID_SensorCategory_PersonalActivity, L"Qualcomm Proximity", L"Qualcomm", ProxFields,
    &DEVPKEY_Sensor_ProximityType, 2, 0, 0, 0, 0, 0, FALSE, InitProximity, ReportProximity };
#endif
