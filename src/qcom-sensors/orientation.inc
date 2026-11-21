#if defined(HID_SENSOR)
HID_SENSOR(5, QcomRotation, 24, 20, 100, 100, 1000, (ULONG)-1000000, 1000000, QUATERNION_REPORT(5, 0x8a))
#elif defined(SENSOR_SEE)
static const SEE_TYPE OrientationSource = { "rotv", QcomRotation, 513, SeeOrientation };
#endif
