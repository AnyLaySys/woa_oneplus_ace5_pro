#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef uint16_t USHORT;
typedef uint8_t UCHAR;
typedef char CHAR;
typedef uint8_t BOOLEAN;
typedef UCHAR *PUCHAR;
typedef uint64_t PFN_NUMBER;
typedef PFN_NUMBER *PPFN_NUMBER;
typedef ULONGLONG *PULONGLONG;
typedef int32_t NTSTATUS;
#define VOID void
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_IO_DEVICE_ERROR -1
#define STATUS_IO_TIMEOUT -2
#define STATUS_INVALID_PARAMETER -3
#define STATUS_DEVICE_HARDWARE_ERROR -4
#define STATUS_DEVICE_BUSY -5
#define STATUS_REVISION_MISMATCH -6
#define STATUS_DATA_ERROR -7
#define STATUS_NOT_SUPPORTED -8
#define STATUS_DEVICE_NOT_READY -9
#define MAXULONG UINT32_MAX
#define NT_SUCCESS(s) ((s)>=0)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
enum { CamCci,CamCpas,CamCrm,CamCc,CamGcc,CamSmmu };
#include "uapi.h"
typedef struct {
    PUCHAR Base[6]; QCAM_INFO Info; BOOLEAN Faulted,Powered,DmaActive,RxActive,ScmAvailable,ScmPending,CdmPending,CrmPending,ReceiverVerified,ScmWaitqKnown,ScmFastWaitq;
    QCAM_DIAG Diag; QCAM_TRACE Trace;
} CAM;
ULONG CamRead(PUCHAR base,ULONG offset);
VOID CamWrite(PUCHAR base,ULONG offset,ULONG value);
VOID CamDelay(ULONG us);
ULONGLONG KeQueryInterruptTime(void);
NTSTATUS CamWait(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us);
NTSTATUS CamSave(CAM *c,PUCHAR base,ULONG offset,ULONG value);
NTSTATUS CamCciInit(CAM *c);
NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes);
NTSTATUS CamCciWrite(CAM *c,UCHAR slave,USHORT address,const UCHAR *buffer,ULONG bytes);
NTSTATUS CamScmProbe(CAM *c);
NTSTATUS CamScmQos(CAM *c);
NTSTATUS CamScmQosDiagnostic(CAM *c);
NTSTATUS CamScmWaitqProbe(CAM *c);
NTSTATUS CamScmWaitqContext(CAM *c);
NTSTATUS CamScmQosRaw(CAM *c);
NTSTATUS CamScmIoProbe(CAM *c);
