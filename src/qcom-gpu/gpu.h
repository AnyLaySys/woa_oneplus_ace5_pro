#pragma once
#include <ntifs.h>
#include "uapi.h"
#include "../qcom-dvfs.h"

#define GPU_TAG '083G'
#define GPU_WINDOW 0xE0000
#define GPU_BUFFER_LIMIT 0x20000000
#define GPU_MAX_COMMANDS 256
#define GPU_PERF_LEVELS 16
typedef struct { PMDL Mdl; PUCHAR Cpu; ULONGLONG Physical,Address; ULONG Size; BOOLEAN Contiguous,Cached; } GPU_BO;
typedef struct { GPU_BO Tables; ULONG Used,Bank; } GPU_VM;
typedef struct { GPU_BO *Buffer; ULONG Offset,Words; } GPU_COMMAND;
typedef struct { ULONGLONG Address; ULONG Words; } GPU_IB;
typedef VOID (*GPU_SYNCHRONIZE)(PVOID context);
typedef struct {
    PUCHAR Registers,Gcc,Rsc,Db,Thermal;
    GPU_VM Vm[2];
    GPU_BO Cache,Dummy,Debug,Log,Hfi,Sqe,Aqe,Ring,Memory,Prr;
    KEVENT Event;
    GPU_SYNCHRONIZE Synchronize;
    PVOID Host;
    ULONG Stage,Fault,Sequence,WriteIndex,ClockMask,Gdscr;
    ULONGLONG Submitted;
    KSPIN_LOCK RingLock;
    KMUTEX MappingLock,PowerLock;
    BOOLEAN Online,Powered,GmuResetReleased;
    ULONG CbBase;
    ULONG SpeedBin,PerfCount,PerfIndex,FixedKhz,MaxKhz;
    ULONG PerfKhz[GPU_PERF_LEVELS],PerfBw[GPU_PERF_LEVELS];
    ULONGLONG PerfLast,BusyLast;
    ULONG CoolingBand,SweetBand,DownSamples;
    LONG TemperatureAverage;
    BOOLEAN LastContextValid;
    ULONGLONG LastContextCookie;
    ULONGLONG LastRoot;
    ULONGLONG LastSubmitTime,PendingSince,PendingCompleted,RaisedAt;
} GPU;
VOID GpuInitialize(GPU *gpu,GPU_SYNCHRONIZE synchronize,PVOID host);
NTSTATUS GpuBind(GPU *gpu,PHYSICAL_ADDRESS registers,ULONG length);
VOID GpuUnbind(GPU *gpu);
BOOLEAN GpuInterrupt(GPU *gpu,ULONG index);
VOID GpuDpc(GPU *gpu);
NTSTATUS GpuTimestamp(GPU *gpu,PULONGLONG counter);
ULONG GpuRead(PUCHAR base,ULONG offset);
VOID GpuWrite(PUCHAR base,ULONG offset,ULONG value);
NTSTATUS GpuPoll(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG timeout);
NTSTATUS GpuAllocate(GPU_BO *bo,ULONG size);
NTSTATUS GpuAllocateCached(GPU_BO *bo,ULONG size);
VOID GpuFree(GPU_BO *bo);
NTSTATUS GpuMap(GPU *gpu,ULONG vm,GPU_BO *bo,ULONGLONG address);
VOID GpuUnmap(GPU *gpu,ULONG vm,GPU_BO *bo);
NTSTATUS GpuMapRange(GPU *gpu,ULONG vm,ULONGLONG address,ULONGLONG size,GPU_BO *bo,ULONGLONG offset,BOOLEAN zero,BOOLEAN flush);
NTSTATUS GpuFlushMappings(GPU *gpu,ULONG vm);
NTSTATUS GpuMmuStart(GPU *gpu);
VOID GpuMmuStop(GPU *gpu);
NTSTATUS GpuPower(GPU *gpu,BOOLEAN enable);
NTSTATUS GpuDb(GPU *gpu,PCSTR name,PULONG address,PULONG auxiliary,PULONG length);
NTSTATUS GpuArc(GPU *gpu,PCSTR primary,PCSTR secondary,ULONG level,PULONG vote);
NTSTATUS GpuRscWrite(GPU *gpu,ULONG address,ULONG value);
NTSTATUS GpuFirmware(HANDLE directory,PCWSTR name,ULONG maximum,PUCHAR *data,PULONG size);
NTSTATUS GpuStart(GPU *gpu);
VOID GpuStop(GPU *gpu);
NTSTATUS GpuSubmit(GPU *gpu,GPU_BO *bo,ULONG offset,ULONG words);
NTSTATUS GpuSubmitBatch(GPU *gpu,const GPU_COMMAND *entries,ULONG count);
NTSTATUS GpuSubmitContextBatch(GPU *gpu,const GPU_COMMAND *entries,ULONG count,ULONGLONG cookie);
NTSTATUS GpuSubmitRootBatch(GPU *gpu,const GPU_IB *entries,ULONG count,ULONGLONG root,ULONGLONG cookie);
NTSTATUS GpuWait(GPU *gpu,ULONGLONG fence,ULONGLONG timeout);
VOID GpuSmc(ULONGLONG *registers);
NTSTATUS GpuGmuStart(GPU *gpu);
VOID GpuGmuStop(GPU *gpu);
NTSTATUS GpuSetFrequency(GPU *gpu,ULONG index);
VOID GpuGovern(GPU *gpu);
LONG GpuTemperature(GPU *gpu);
static __forceinline ULONG Gr(GPU *g,ULONG word) { return GpuRead(g->Registers,word*4); }
static __forceinline VOID Gw(GPU *g,ULONG word,ULONG value) { GpuWrite(g->Registers,word*4,value); }
static __forceinline VOID Gw64(GPU *g,ULONG word,ULONGLONG value) { Gw(g,word,(ULONG)value);Gw(g,word+1,(ULONG)(value>>32)); }
