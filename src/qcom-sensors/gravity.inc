#if defined(HID_SENSOR)
HID_SENSOR(7, QcomLinearAccel, 24, 15, 20, 20, 5, (ULONG)-16000, 16000, 0x65, 0x00, 0x55, 0x00,
    MOTION_REPORT(7, 0x7c, 0x1a, 0x52, 16000))
#elif defined(SENSOR_SEE)
static VOID SeeGravity(ULONG kind, ULONG id, SEE_PB payload) {
    UNREFERENCED_PARAMETER(id);
    SEE_MEASUREMENT m;
    if (!SeeMeasurement(payload, 2, 6, FALSE, &m)) return;
    SeeVector(kind, m.Bits, 6682812, 16000, 0);
    SeeLinearAcceleration(m.Bits + 3);
}
static const SEE_TYPE GravitySource = { "gravity", QcomGravity, 513, SeeGravity };
#endif
