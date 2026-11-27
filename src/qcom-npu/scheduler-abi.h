#pragma once
#include "uapi.h"
#define NPU_SCHEDULER_MAGIC 0x4d55504eu
#define NPU_SCHEDULER_ABI 1u
typedef struct { QCOM_NPU_U32 Magic, Version; QCOM_NPU_U64 Token; } NPU_SCHEDULER_COMMAND;
typedef struct { QCOM_NPU_U32 Magic, Version; QCOM_NPU_U64 Event, Cookie; } NPU_SCHEDULER_CONTEXT;
typedef struct { QCOM_NPU_U32 Magic, Version, RuntimeVersion, Reserved; } NPU_SCHEDULER_INFO;
typedef struct { QCOM_NPU_U32 Magic, Id, Bytes, Owner; QCOM_NPU_U64 Address, Cookie; } NPU_SCHEDULER_MEMORY;
#ifdef _KERNEL_MODE
#ifdef NPU_SCHEDULER_EXPORTS
#define NPU_SCHEDULER_DECL __declspec(dllexport)
#else
#define NPU_SCHEDULER_DECL __declspec(dllimport)
#endif
NPU_SCHEDULER_DECL BOOLEAN NpuSchedulerReady(VOID);
NPU_SCHEDULER_DECL NTSTATUS NpuSchedulerRun(ULONGLONG token, ULONG owner, const volatile LONG *preempt);
NPU_SCHEDULER_DECL VOID NpuSchedulerCancel(ULONGLONG token, ULONG owner);
NPU_SCHEDULER_DECL NTSTATUS NpuSchedulerReset(VOID);
NPU_SCHEDULER_DECL BOOLEAN NpuSchedulerMemoryReference(const NPU_SCHEDULER_MEMORY *memory, BOOLEAN acquire);
NPU_SCHEDULER_DECL ULONGLONG NpuSchedulerMemoryBudget(VOID);
NPU_SCHEDULER_DECL VOID NpuSchedulerMemoryPurge(ULONG owner, ULONGLONG cookie);
#endif
