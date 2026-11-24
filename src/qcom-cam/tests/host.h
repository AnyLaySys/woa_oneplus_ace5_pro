#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef uint32_t ULONG;
typedef int32_t LONG;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
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
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
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
#define STATUS_NO_SUCH_DEVICE -10
#define MAXULONG UINT32_MAX
#define NT_SUCCESS(s) ((s)>=0)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
enum { CamCci,CamCpas,CamCrm,CamCc,CamGcc,CamSmmu };
#include "uapi.h"
#include "pdaf.h"
typedef struct {
    PUCHAR Base[6]; QCAM_INFO Info; BOOLEAN Faulted,Powered,DmaActive,RxActive,ScmAvailable,ScmPending,CdmPending,CrmPending,ReceiverVerified,ScmWaitqKnown,ScmFastWaitq,StaticQosReady;
    QCAM_DIAG Diag; QCAM_TRACE Trace;
    BOOLEAN FrameRouteOwned,TbuPending;
    BOOLEAN ScmResumeValid;
    ULONGLONG ScmResumeArgs[8];
    ULONG ScmAuditSequence;
    ULONG FrameRoute[4];
    ULONG Sensor;
    BOOLEAN LensReady;
    ULONG FocusPosition,FocusTarget,FocusFlags,FocusWait;
    LONG FocusStatus;
    CAM_PDAF_CAL FocusCalibration,MainFocusCalibration;
    CAM_PDAF_RESULT FocusResult;
    LONG FocusWorkspace[2520];
    ULONG FocusSequence,FocusStable,FocusMisses,FocusSamples;
    BOOLEAN FocusComplete,FocusWindowValid;
    ULONG FocusWindow;
    QCAM_ROI_OPTIONS FocusRoi;
    ULONG FocusRoiX,FocusRoiY;
    BOOLEAN FlashAvailable,FlashOwned;
    ULONG FlashMode,FlashPower,FlashTimeout;
    ULONGLONG FlashDeadline;
    UCHAR FlashSaved[10];
    ULONG VideoZoom;
    UCHAR *PdafLatest;QCAM_PDAF_FRAME PdafInfo;
} CAM;
ULONGLONG KeQueryInterruptTime(void);
NTSTATUS CamPmic(CAM *c,ULONG ppid,UCHAR reg,UCHAR *value,BOOLEAN write);
NTSTATUS CamFlashStop(CAM *c);
ULONG CamRead(PUCHAR base,ULONG offset);
VOID CamWrite(PUCHAR base,ULONG offset,ULONG value);
ULONGLONG CamRead64(PUCHAR base,ULONG offset);
VOID CamWrite64(PUCHAR base,ULONG offset,ULONGLONG value);
VOID CamDelay(ULONG us);
ULONGLONG KeQueryInterruptTime(void);
NTSTATUS CamWait(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us);
NTSTATUS CamSave(CAM *c,PUCHAR base,ULONG offset,ULONG value);
NTSTATUS CamCciInit(CAM *c);
NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes);
NTSTATUS CamCciWrite(CAM *c,UCHAR slave,USHORT address,const UCHAR *buffer,ULONG bytes);
NTSTATUS CamScmProbe(CAM *c);
NTSTATUS CamScmQos(CAM *c);
NTSTATUS CamEnsureStaticQos(CAM *c);
NTSTATUS CamScmQosDiagnostic(CAM *c);
NTSTATUS CamScmWaitqProbe(CAM *c);
NTSTATUS CamScmWaitqContext(CAM *c);
NTSTATUS CamScmQosRaw(CAM *c);
NTSTATUS CamScmContinue(CAM *c);
NTSTATUS CamScmAudit(CAM *c,ULONG phase,const ULONGLONG *registers,NTSTATUS result);
NTSTATUS CamScmIoProbe(CAM *c);

NTSTATUS CamCciReadByte(CAM *c,UCHAR slave,UCHAR address,PUCHAR buffer,ULONG bytes);
NTSTATUS CamCciWriteByte(CAM *c,UCHAR slave,UCHAR address,const UCHAR *buffer,ULONG bytes);

NTSTATUS CamPdafPrepare(CAM *c);

static inline PUCHAR CamCciBase(CAM *c) {return c->Base[CamCci];}
static inline ULONG CamCciMaster(CAM *c) {return c->Sensor?0:1;}
