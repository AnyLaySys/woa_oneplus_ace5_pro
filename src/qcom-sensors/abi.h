#pragma once
#include <basetsd.h>
#define QCOM_SENSOR_READ 0x00226098UL
#define QCOM_ACTIVITY_SUPPORTED 0x0000007BUL
typedef enum {
    QcomLight,
    QcomSimpleOrientation,
    QcomGyro,
    QcomAccel,
    QcomRotation,
    QcomGravity,
    QcomLinearAccel,
    QcomMagnetometer,
    QcomRelativeOrientation,
    QcomGeomagneticOrientation,
    QcomProximity,
    QcomPedometer,
    QcomActivity,
    QcomHall,
    QcomSensorCount
} QCOM_SENSOR;
typedef struct {
    INT32 Micro[4];
    UINT32 Accuracy;
    INT32 Status;
    UINT64 Time;
} QCOM_ROTATION;
#define AKM_HALL_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x833, METHOD_BUFFERED, FILE_READ_DATA)
typedef struct {
    UINT64 Time;
    INT32 X, Y, Z;
    UINT32 Position;
} AKM_HALL_SAMPLE;
C_ASSERT(sizeof(AKM_HALL_SAMPLE) == 24);
