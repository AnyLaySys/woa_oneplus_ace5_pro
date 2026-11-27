#pragma once
#define ALS_CHARGE_STATUS 0x222040u
#define ALS_CHARGE_ABI 1u
#define ALS_CHARGE_STATUS_BASE_SIZE 208u
#define ALS_CHARGE_TEMP_LIMIT_DK 3182u
#define ALS_CHARGE_TEMP_RESUME_DK 3163u
#define ALS_CHARGE_UFCS_DERATE_DK 3173u
#define ALS_CHARGE_STOP_TEMPERATURE 4u
#define ALS_CHARGE_DEVICE L"\\\\.\\AlsCharge"
typedef struct _ALS_CHARGE_STATUS_DATA {
    ULONG Abi,Revision,Bridge,Phase,Running,Fault;
    ULONG Pending,AuthReady,AuthVerified,Online,UsbMv,SourceEnabled;
    ULONG SourceRole,Moisture,Adapter,OnlineAge,VoltageAge,SourceAge;
    ULONG RoleAge,MoistureAge,BatteryAge,Guard,CellValid,Temperature;
    ULONG PackMv,PackMa,Soc,Remaining,Cell1,Cell2;
    ULONG BatteryStatus,Mos,CpActive,CpOff,CpTouched,CpError;
    ULONG Cv,StopReason,FvMv,RequestedMv,RequestedMa,PortOnline;
    ULONG PortMv,PortRole,PortMoisture,PortAge,OtgPhase,OtgFault;
    ULONG UfcsStarted,PeerRunning,SourceInfoLow,SourceInfoAge;
    ULONG BypassState,BypassError,BypassResult,InputLimitUa,InputAge,CableOrientation,CableAge;
} ALS_CHARGE_STATUS_DATA;
