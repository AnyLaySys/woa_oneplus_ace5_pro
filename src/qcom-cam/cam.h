#pragma once
#include <ntddk.h>
#include <wdf.h>
#include "uapi.h"
#include "pdaf.h"
#include "video/raw-row.h"

enum { CamGcc,CamCc,CamMclk,CamCci,CamPhy,CamCsid,CamTfe,CamCpas,CamNoc,CamSmmu,CamTlmm,CamSpmi,CamOwners,CamObserve,CamChannels,CamRsc,CamDb,CamCrm,CamCdm,CamHfTbu,CamSfTbu,CamMnoc,CamRpmh,CamCci1,CamPhy1,CamPhy3,CamPhy4,CamRegionCount };
typedef struct { ULONG Address,Value,Delay; } CAM_REG;
typedef struct { USHORT Address; UCHAR Value; } SENSOR_REG;
typedef struct { PUCHAR Base; ULONG Offset,Original,Config; BOOLEAN Rcg; } CAM_SAVE;
typedef struct { PVOID Owner; ULONG Index,First,End; HANDLE Thread; KEVENT Ready,Done; USHORT Workspace[QCAM_VIDEO_MAX_WIDTH*6]; } CAM_VIDEO_WORKER;
typedef struct {
    WDFDEVICE Device;
    ULONG Sensor;
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
    ULONG CrmBwStates[2],CrmPerfStates[3][2];
    BOOLEAN CrmOwned,CrmPending;
    BOOLEAN ScmPending;
    BOOLEAN StaticQosReady;
    BOOLEAN ScmResumeValid;
    ULONGLONG ScmResumeArgs[8];
    ULONG ScmAuditSequence;
    BOOLEAN CdmPending;
    BOOLEAN TbuPending;
    BOOLEAN ScmWaitqKnown,ScmFastWaitq;
    BOOLEAN ScmIoKnown,ScmIoAvailable,ScmNocReadable;
    BOOLEAN ReceiverVerified;
    WDFFILEOBJECT StreamFile;
    BOOLEAN LensReady;
    ULONG FocusPosition,FocusTarget,FocusFlags,FocusWait;
    LONG FocusStatus;
    CAM_PDAF_CAL FocusCalibration,MainFocusCalibration;
    UCHAR MainQsc[3072];BOOLEAN MainQscValid;
    CAM_PDAF_RESULT FocusResult;
    LONG FocusWorkspace[2520];
    USHORT VideoWorkspace[QCAM_VIDEO_MAX_WIDTH*6];
    CAM_VIDEO_WORKER VideoWorkers[3];
    UCHAR VideoColors[3][1024];
    ULONG VideoGains[3];
    ULONG VideoZoom,VideoWidth,VideoHeight,VideoBytes,VideoRows[QCAM_VIDEO_MAX_HEIGHT];
    CAM_RAW_BLOCK VideoBlocks[QCAM_VIDEO_MAX_WIDTH/8];
    CAM_RAW_BLOCK VideoDense[QCAM_VIDEO_MAX_WIDTH/16];
    ULONG VideoColumns[QCAM_VIDEO_MAX_WIDTH];
    BOOLEAN VideoCached;
    ULONG VideoLinearGains[3];
    BOOLEAN VideoStopping;
    const UCHAR *VideoSource;
    PUCHAR VideoOutput;
    ULONG FocusSequence,FocusStable,FocusMisses,FocusSamples;
    BOOLEAN FocusComplete,FocusWindowValid;
    ULONG FocusWindow;
    QCAM_ROI_OPTIONS FocusRoi;
    ULONG FocusRoiX,FocusRoiY;
    BOOLEAN FlashAvailable,FlashOwned;
    ULONG FlashMode,FlashPower,FlashTimeout;
    ULONGLONG FlashDeadline;
    UCHAR FlashSaved[10];
    ULONG FrameSlot,FrameStamp;
    QCAM_FRAME_TIME FrameTime;
    BOOLEAN PdafEnabled;
    ULONG PdafSlot,PdafCompletedSlot,PdafCompletedId;
    ULONGLONG PdafStamp,PdafCompletedSof,PdafCompletedEof;
    BOOLEAN PdafPending;
    PVOID PdafAllocation,Pdaf,PdafLatest;
    PHYSICAL_ADDRESS PdafAddress;
    PMDL PdafMdl[4];
    QCAM_PDAF_FRAME PdafInfo;
    PUCHAR PdafSource;
    BOOLEAN PdafCopied,PdafFocusCopied;
    BOOLEAN FrameRouteOwned;
    ULONG FrameRoute[4];
    PUCHAR ResetRead,ResetWrite;
    UCHAR ResetSaved[7];
    ULONG MclkPllSaved[11];BOOLEAN MclkPllOwned;
    BOOLEAN ResetOwned,Powered,Configured,DmaActive,RxActive,Faulted,BusOn,PhyOn,ScmAvailable;
    volatile LONG Online;
    WDFREQUEST Request;
    WDFTIMER Timer;
    WDFINTERRUPT CsidInterrupt,TfeInterrupt;
    ULONGLONG Deadline;
    WDFDMAENABLER DmaEnabler;
    WDFCOMMONBUFFER FrameBuffer;
    BOOLEAN FrameContiguous;
    BOOLEAN FrameMapped;
    ULONG FrameMapBank;
    PVOID FrameTables;
    PHYSICAL_ADDRESS FrameTableAddress;
    PVOID Frame;
    PVOID FrameAllocation;
    PMDL FrameMdl[4];
    PHYSICAL_ADDRESS FrameAddress;
    WDFCOMMONBUFFER CommandBuffer;
    PVOID Command;
    PMDL CommandMdl;
    PHYSICAL_ADDRESS CommandAddress;
    BOOLEAN CommandContiguous;
    PVOID PowerState;
} CAM;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CAM,CamContext);

static __forceinline ULONG CamRead(PUCHAR base,ULONG offset) { return READ_REGISTER_ULONG((PULONG)(base+offset)); }
static __forceinline VOID CamWrite(PUCHAR base,ULONG offset,ULONG value) { WRITE_REGISTER_ULONG((PULONG)(base+offset),value); }
static __forceinline ULONGLONG CamRead64(PUCHAR base,ULONG offset) { return READ_REGISTER_ULONG64((PULONG64)(base+offset)); }
static __forceinline VOID CamWrite64(PUCHAR base,ULONG offset,ULONGLONG value) { WRITE_REGISTER_ULONG64((PULONG64)(base+offset),value); }
VOID CamDelay(ULONG us);
NTSTATUS CamWait(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG us);
NTSTATUS CamSave(CAM *c,PUCHAR base,ULONG offset,ULONG value);
NTSTATUS CamPowerOn(CAM *c);
NTSTATUS CamPowerOff(CAM *c);
NTSTATUS CamReset(CAM *c,BOOLEAN high);
NTSTATUS CamPmic(CAM *c,ULONG ppid,UCHAR reg,UCHAR *value,BOOLEAN write);
NTSTATUS CamFlashSet(CAM *c,const QCAM_FLASH_OPTIONS *options);
NTSTATUS CamFlashStop(CAM *c);
VOID CamFlashFrame(CAM *c);
NTSTATUS CamCciInit(CAM *c);
NTSTATUS CamCciRead(CAM *c,UCHAR slave,USHORT address,PUCHAR buffer,ULONG bytes);
NTSTATUS CamCciWrite(CAM *c,UCHAR slave,USHORT address,const UCHAR *buffer,ULONG bytes);
VOID CamOtherSensorSnapshot(CAM *c);
NTSTATUS CamOtherSensorInit(CAM *c);
NTSTATUS CamOtherSensorStream(CAM *c,BOOLEAN on);
NTSTATUS CamOtherSensorExposure(CAM *c,ULONG lines,ULONG gain);
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
NTSTATUS CamCrmOn(CAM *c);
NTSTATUS CamMnocQos(CAM *c,BOOLEAN apply);
VOID CamDiagnostics(CAM *c,QCAM_DIAG *diag);
NTSTATUS CamScmProbe(CAM *c);
NTSTATUS CamScmQos(CAM *c);
NTSTATUS CamEnsureStaticQos(CAM *c);
NTSTATUS CamScmQosDiagnostic(CAM *c);
NTSTATUS CamScmWaitqProbe(CAM *c);
NTSTATUS CamScmWaitqContext(CAM *c);
NTSTATUS CamScmQosRaw(CAM *c);
NTSTATUS CamScmContinue(CAM *c);
NTSTATUS CamScmAudit(CAM *c,ULONG phase,const ULONGLONG *registers,NTSTATUS result);
NTSTATUS CamScmQosSmart(CAM *c);
NTSTATUS CamScmQosRdi(CAM *c);
NTSTATUS CamScmQosCdm(CAM *c);
NTSTATUS CamScmNocFaults32(CAM *c);
NTSTATUS CamCameraBusProbe(CAM *c);
NTSTATUS CamNocRegisters(CAM *c);
NTSTATUS CamScmNocFaults(CAM *c);
NTSTATUS CamMeasureClocks(CAM *c);
NTSTATUS CamScmIoProbe(CAM *c);
NTSTATUS CamCdmConfigure(CAM *c);
NTSTATUS CamCdmFetch(CAM *c);
NTSTATUS CamCdmTranslate(CAM *c);
NTSTATUS CamFrameTranslate(CAM *c);
NTSTATUS CamFrameRoutePrepare(CAM *c);
NTSTATUS CamCdmPrepare(CAM *c,ULONG limit);
NTSTATUS CamCdmAbort(CAM *c);

NTSTATUS CamCciReadByte(CAM *c,UCHAR slave,UCHAR address,PUCHAR buffer,ULONG bytes);
NTSTATUS CamCciWriteByte(CAM *c,UCHAR slave,UCHAR address,const UCHAR *buffer,ULONG bytes);

NTSTATUS CamFocusInit(CAM *c);
NTSTATUS CamFocusStop(CAM *c);
NTSTATUS CamFocusSet(CAM *c,const QCAM_FOCUS_OPTIONS *options);
VOID CamFocusFrame(CAM *c);
ULONG CamFocusState(CAM *c);
NTSTATUS CamPdafFrame(CAM *c);
NTSTATUS CamPdafPrepare(CAM *c);
NTSTATUS CamPdafCopy(CAM *c);
VOID CamDmaInvalidate(PVOID base,ULONG span,ULONG step,ULONG count,ULONG offset);

static __forceinline PUCHAR CamCciBase(CAM *c) {return c->Base[c->Sensor==2?CamCci1:CamCci];}
static __forceinline PUCHAR CamPhyBase(CAM *c) {return c->Base[c->Sensor==1?CamPhy1:c->Sensor==2?CamPhy3:c->Sensor==3?CamPhy4:CamPhy];}
static __forceinline ULONG CamCciMaster(CAM *c) {return c->Sensor?0:1;}
