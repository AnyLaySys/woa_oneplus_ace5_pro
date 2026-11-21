#if defined(HID_SENSOR)
HID_SENSOR(2, QcomSimpleOrientation, 14, 4, 200, 100, 100, 0, 16000, 0x65, 0x00, 0x55, 0x00, 0x09, 0x8f, 0xa1,
    0x00, 0x85, 0x02, COMMON_FEATURE(100), COMMON_INPUT, 0x15, 0x00, 0x25, 0x05, 0x0a, 0x89, 0x04, 0x81, 0x02,
    0xc0)
#elif defined(SENSOR_SEE)
static VOID SeeSimpleOrientation(ULONG kind, ULONG id, SEE_PB payload) {
    UNREFERENCED_PARAMETER(id);
    SEE_MEASUREMENT m;
    if (!SeeMeasurement(payload, 0, 3, TRUE, &m)) return;
    SeeAccelerometer(m.Bits);
    ULONG axis = 0;
    for (ULONG j = 1; j < 3; j++)
        if ((m.Bits[j] & 0x7fffffffu) > (m.Bits[axis] & 0x7fffffffu)) axis = j;
    if ((m.Bits[axis] & 0x7fffffffu) < 0x40f00000u) return;
    ULONG negative = m.Bits[axis] >> 31;
    SeeValue(&Samples[kind],
        axis == 1   ? negative * 2
        : axis == 0 ? 1 + negative * 2
                    : 4 + negative,
        0, 0, STATUS_SUCCESS);
}
static const SEE_TYPE SimpleOrientationSource = { "accel", QcomSimpleOrientation, 513, SeeSimpleOrientation };
#endif
