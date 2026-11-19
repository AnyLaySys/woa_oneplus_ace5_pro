#pragma once
#include <basetsd.h>
#define QCOM_SENSOR_READ 0x00226098UL
// SensorsCx activity bits: Unknown, Stationary, Walking, Running, InVehicle, Biking.
#define QCOM_ACTIVITY_SUPPORTED 0x0000007BUL
typedef enum {QcomLight,QcomSimpleOrientation,QcomGyro,QcomAccel,QcomRotation,QcomGravity,QcomLinearAccel,QcomMagnetometer,QcomRelativeOrientation,QcomGeomagneticOrientation,QcomProximity,QcomPedometer,QcomActivity,QcomSensorCount} QCOM_SENSOR;
typedef struct { INT32 Micro[4]; UINT32 Accuracy; INT32 Status; UINT64 Time; } QCOM_ROTATION;
