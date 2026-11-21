#if defined(HID_SENSOR)
HID_SENSOR(6, QcomGravity, 24, 15, 20, 20, 5, (ULONG)-16000, 16000, 0x65, 0x00, 0x55, 0x00,
    MOTION_REPORT(6, 0x7b, 0x1a, 0x52, 16000))
#elif defined(SENSOR_SEE)
static VOID SeeLinearAcceleration(const ULONG *bits) { SeeVector(QcomLinearAccel, bits, 6682812, 16000, 0); }
#endif
