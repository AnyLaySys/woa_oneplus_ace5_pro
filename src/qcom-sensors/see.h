#pragma once
#include <ntddk.h>
#define QCOM_LIGHT_READ CTL_CODE(FILE_DEVICE_UNKNOWN,0x820,METHOD_BUFFERED,FILE_READ_DATA)
#define QCOM_ORIENTATION_READ CTL_CODE(FILE_DEVICE_UNKNOWN,0x821,METHOD_BUFFERED,FILE_READ_DATA)
typedef struct { ULONG LuxMilli; NTSTATUS Status; ULONGLONG Time; } QCOM_LIGHT;
typedef struct { ULONG Direction; NTSTATUS Status; ULONGLONG Time; } QCOM_ORIENTATION;
typedef struct {
    ULONG Rcid,Node,Port,OwnNode,Iid[8],Size[8],Count,LocalIid,ResumeNode,ResumePort;
    BOOLEAN Opening,Acked,Requested,Free[8];
    USHORT Transaction;
    ULONGLONG Due;
} QCOM_SEE;
#ifdef __cplusplus
extern "C" {
#endif
VOID QcomLightQuery(QCOM_LIGHT *value);
VOID QcomOrientationQuery(QCOM_ORIENTATION *value);
#ifdef __cplusplus
}
#endif
