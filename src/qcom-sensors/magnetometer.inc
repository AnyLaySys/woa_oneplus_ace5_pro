#if defined(SENSOR_SEE)
static VOID SeeMagnetometer(ULONG kind, ULONG id, SEE_PB payload) {
    UNREFERENCED_PARAMETER(id);
    SEE_MEASUREMENT m;
    if (SeeMeasurement(payload, 0, 3, FALSE, &m))
        SeeVector(kind, m.Bits, 655360000, 100000000, max(m.Status, 1u));
}
static const SEE_TYPE MagnetometerSource = { "mag", QcomMagnetometer, 513, SeeMagnetometer };
#elif defined(SENSOR_NATIVE)
static const PROPERTYKEY MagFields[] = { PKEY_SensorData_Timestamp,
    PKEY_SensorData_MagneticFieldStrengthX_Microteslas, PKEY_SensorData_MagneticFieldStrengthY_Microteslas,
    PKEY_SensorData_MagneticFieldStrengthZ_Microteslas, PKEY_SensorData_MagnetometerAccuracy };
static VOID InitMagnetometer(SENSOR *s) {
    for (ULONG i = 1; i < 4; i++) InitPropVariantFromFloat(0, &s->Data->List[i].Value);
    InitPropVariantFromUInt32(0, &s->Data->List[4].Value);
    for (ULONG i = 0; i < 3; i++) {
        s->Thresholds->List[i].Key = MagFields[i + 1];
        InitPropVariantFromFloat(0, &s->Thresholds->List[i].Value);
    }
}
static VOID ReportMagnetometer(SENSOR *s, const QCOM_ROTATION *q) {
    for (ULONG i = 0; i < 3; i++)
        InitPropVariantFromFloat(q->Micro[i] / 10000.0f, &s->Data->List[i + 1].Value);
    InitPropVariantFromUInt32(q->Accuracy, &s->Data->List[4].Value);
}
static const NATIVE_CONFIG MagnetometerConfig = { QcomMagnetometer, &GUID_SensorType_Magnetometer3D,
    &GUID_SensorCategory_Orientation, L"Qualcomm Magnetometer", L"Qualcomm", MagFields, NULL, 5, 4, 3,
    0.0001f, -10000, 10000, FALSE, InitMagnetometer, ReportMagnetometer };
#endif
