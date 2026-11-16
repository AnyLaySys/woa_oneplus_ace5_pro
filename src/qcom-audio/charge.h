#pragma once
#include "charge-api.h"
#define QCH_UFCS_CAP_COUNT 7u
enum {ChargeNone,ChargeStop,ChargeAcquire,ChargeRelease,ChargeStart,ChargeNormal,ChargeKey};

typedef struct {
    ULONG Attempts,Hold,Active,Normal;
    ULONGLONG StableAt,OfflineAt,RetryAt,AcquireAt,PollAt;
} QCH_POLICY;
typedef struct {ULONGLONG Pdo[QCH_UFCS_CAP_COUNT];ULONG Count;} QCH_UFCS_CAPS;
typedef struct _QCH_LINK {
    QCH_POLICY Policy;
    ULONG Command;
    BOOLEAN Closing;
    BOOLEAN Waiting;
    ULONG Query,ExpectedOp,ExpectedProp,ExpectedValue;
    ULONG UsbOnline,UsbVoltage;
    ULONG SourceEnabled,SourceRole,Moisture;
    ULONGLONG SourceAt,RoleAt,MoistureAt;
    ULONG ReplyOp,ReplyProp,ReplyValue,ReplyResult;
    ULONG Phase,OffMask;
    ULONG Bridge,OtgPhase,OtgFault;
    ULONG Temperature,PackVoltage,PackCurrent,Remaining;
    BOOLEAN Running,ReplyReady,GuardGood,BatteryValid,UfcsDerated;
    ULONGLONG OnlineAt,VoltageAt,GuardAt,Deadline;
    ULONGLONG Last,SampleTimeMs,ExitAt;
    QCH_UFCS_CAPS Caps;
    ULONG AdapterType,SourceLo;
    ULONG Normal,UsbLimitUa,InputState,InputError,InputResult,CableOrientation;
    ULONGLONG InputAt,InputDeadline,CableAt;
    ULONG OffCheck,PdoRequests,VoocEnable;
    ULONG AuthVerified,AuthResult,AuthReplyValid;
    ULONG CpError,CpPrepared,CpActive,CpTouched,CpOffVerified;
    ULONG CellValid,Cell1Mv,Cell2Mv,BatteryStatus,GaugeCvMv,GaugeCcMa;
    ULONG LoadCv,LoadStopReason;
    ULONG LoadFvUv,LoadFvLimitMv,LoadReqMv,LoadReqMa,LoadMos,LoadReturn,LoadQuery,LoadBuckOff;
    ULONG LoadFccUa,LoadFccMaxUa;
    ULONGLONG LoadStartAt,LoadAt,LoadNextAt,LoadFvAt,LoadFccAt,LoadFccMaxAt;
    ULONG SessionValid,SessionDevHi,SessionRunning,SessionStatus;
    ULONG NotifyFlags,CapResponse,Fault,OffRetries,Soc;
    BOOLEAN NotifyAcked,UfcsStarted;
    ULONG PortOnline,PortVoltage,PortRole,PortMoisture,PeerRunning;
    ULONGLONG PortAt,InfoAt;
} QCH_LINK;
static __inline BOOLEAN QchBatteryFresh(const QCH_LINK *C,ULONGLONG Now)
{
    return C->BatteryValid && C->CellValid && C->SampleTimeMs && C->SampleTimeMs<=Now && Now-C->SampleTimeMs<=6000;
}
#define QCH_FAST_POLL(C) ((C)->Bridge==2 && ((C)->Running || (C)->OffMask || (C)->Phase==90))
#ifdef __cplusplus
extern "C" {
#endif
BOOLEAN QchTakeAuth(UCHAR *Key);
BOOLEAN QchAuthAvailable(VOID);
VOID QchDiscardAuth(VOID);
BOOLEAN QchInputSaved(VOID);
NTSTATUS QchInputSave(BOOLEAN Suspended);
BOOLEAN QchLoadAuth(VOID);
VOID QchPublish(const QCH_LINK *C);
BOOLEAN QchBatteryGuard(QCH_LINK *C);
VOID QchRemove(VOID);
NTSTATUS QchInit(PDRIVER_OBJECT Driver,PUNICODE_STRING RegistryPath);
#ifdef __cplusplus
}
#endif
