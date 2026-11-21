#if defined(HID_SENSOR)
HID_SENSOR(10, QcomGeomagneticOrientation, 24, 20, 100, 100, 1000, (ULONG)-1000000, 1000000,
    QUATERNION_REPORT(10, 0xc1))
#elif defined(SENSOR_SEE)
static const SEE_TYPE GeomagneticOrientationSource = { "geomag_rv", QcomGeomagneticOrientation, 513,
    SeeOrientation };
#endif
