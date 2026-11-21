#if defined(HID_SENSOR)
HID_SENSOR(
    3, QcomGyro, 24, 15, 20, 20, 100, (ULONG)-2000000, 2000000, MOTION_REPORT(3, 0x76, 0x15, 0x56, 2000000))
#elif defined(SENSOR_SEE)
static VOID SeeGyroscope(ULONG kind, ULONG id, SEE_PB payload) {
    UNREFERENCED_PARAMETER(id);
    SEE_MEASUREMENT m;
    if (SeeMeasurement(payload, 0, 3, TRUE, &m)) SeeVector(kind, m.Bits, 3754936206u, 2000000, 0);
}
static const SEE_TYPE GyroscopeSource = { "gyro", QcomGyro, 513, SeeGyroscope };
#endif
