#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "uapi.h"

enum { CamGcc,CamCc,CamMclk,CamCci,CamPhy,CamCsid,CamTfe,CamCpas,CamNoc,CamSmmu,CamTlmm,CamSpmi,CamOwners,CamObserve,CamChannels,CamRsc,CamDb,CamCrm,CamRegionCount };
typedef struct { ULONG Address,Value,Delay; } CAM_REG;
typedef struct { USHORT Address; UCHAR Value; } SENSOR_REG;
typedef struct { PUCHAR Base; ULONG Offset,Original,Config; BOOLEAN Rcg; } CAM_SAVE;
typedef struct {
    WDFDEVICE Device;
    PUCHAR Base[CamRegionCount];
    WDFWAITLOCK Lock;
    QCAM_INFO Info;
    QCAM_DIAG Diag;
    QCAM_TRACE Trace;
    CAM_SAVE Saved[128];
    ULONG SavedCount;
    ULONG ProbeLimit;
    ULONG PipelineLimit;
    ULONG CrmOriginal;
    ULONG CrmPerfOriginal[3],CrmPerfMask,CrmPendingIrq;
    BOOLEAN CrmOwned,CrmPending;
    BOOLEAN ScmPending;
    PUCHAR ResetRead,ResetWrite;
    UCHAR ResetSaved[7];
    BOOLEAN ResetOwned,Powered,Configured,DmaActive,RxActive,Faulted,BusOn,PhyOn,ScmAvailable;
    volatile LONG Online;
    WDFREQUEST Request;
    WDFTIMER Timer;
    WDFINTERRUPT CsidInterrupt,TfeInterrupt;
    ULONGLONG Deadline;
    WDFDMAENABLER DmaEnabler;
    WDFCOMMONBUFFER FrameBuffer;
    PVOID Frame;
    PMDL FrameMdl;
    PHYSICAL_ADDRESS FrameAddress;
    PVOID PowerState;
} CAM;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CAM,CamContext);

static __forceinline ULONG CamRead(PUCHAR base,ULONG offset) { return READ_REGISTER_ULONG((PULONG)(base+offset)); }
static __forceinline VOID CamWrite(PUCHAR base,ULONG offset,ULONG value) { WRITE_REGISTER_ULONG((PULONG)(base+offset),value); }
VOID CamDelay(ULONG us);
NTSTATUS CamWait(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us);
NTSTATUS CamSave(CAM *c,PUCHAR base,ULONG offset,ULONG value);
NTSTATUS CamPowerOn(CAM *c);
NTSTATUS CamPowerOff(CAM *c);
NTSTATUS CamReset(CAM *c,BOOLEAN high);
NTSTATUS CamCciInit(CAM *c);
NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes);
NTSTATUS CamCciWrite(CAM *c,UCHAR slave,USHORT address,const UCHAR *buffer,ULONG bytes);
NTSTATUS CamSensorInit(CAM *c);
NTSTATUS CamSensorStream(CAM *c,BOOLEAN on);
NTSTATUS CamSensorExposure(CAM *c,ULONG lines,ULONG gain);
NTSTATUS CamCaptureStart(CAM *c,const QCAM_CAPTURE_OPTIONS *options);
NTSTATUS CamCaptureStop(CAM *c);
BOOLEAN CamCapturePoll(CAM *c,NTSTATUS *status);
VOID CamSnapshot(CAM *c);
VOID CamDmaFree(CAM *c);
NTSTATUS CamBusOn(CAM *c);
NTSTATUS CamBusOff(CAM *c);
VOID CamBusSnapshot(CAM *c);
NTSTATUS CamCrmOff(CAM *c);
VOID CamDiagnostics(CAM *c,QCAM_DIAG *diag);
NTSTATUS CamScmProbe(CAM *c);
NTSTATUS CamScmQos(CAM *c);
NTSTATUS CamScmIoProbe(CAM *c);
