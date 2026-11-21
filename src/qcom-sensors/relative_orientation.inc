#if defined(HID_SENSOR)
HID_SENSOR(
    9, QcomRelativeOrientation, 24, 20, 100, 100, 1000, (ULONG)-1000000, 1000000, QUATERNION_REPORT(9, 0x8e))
#elif defined(SENSOR_SEE)
static const SEE_TYPE RelativeOrientationSource = { "game_rv", QcomRelativeOrientation, 513, SeeOrientation };
#endif
