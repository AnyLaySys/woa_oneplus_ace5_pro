#pragma once
#include <ntddk.h>
#include "abi.h"
typedef struct {
    ULONG Rcid,Node,Port,OwnNode,Iid[8],Size[8],Count,LocalIid,ResumeNode,ResumePort;
    BOOLEAN Opening,Acked,Requested,Free[8];
    USHORT Transaction;
    ULONGLONG Due;
} QCOM_SEE;
#ifdef __cplusplus
extern "C" {
#endif
NTSTATUS QcomSensorQuery(ULONG sensor,QCOM_ROTATION *value);
#ifdef __cplusplus
}
#endif
