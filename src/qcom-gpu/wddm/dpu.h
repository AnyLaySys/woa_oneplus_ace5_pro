#pragma once
#include <ntifs.h>

/* The opaque adapter arguments avoid introducing an adapter.h include cycle.
 * Start/Stop run at PASSIVE_LEVEL. Queue is safe at the display DIRQL.
 * The caller owns both the pending and active scanout BO until a later TE
 * has confirmed their replacement. Physical addresses must be contiguous.
 */
NTSTATUS A830DpuStart(PVOID adapter);
NTSTATUS A830DpuInitialize(PVOID adapter);
NTSTATUS A830DpuStop(PVOID adapter);
/* Crash/bugcheck path: MMIO only, no allocation, timer deletion or waits. */
VOID A830DpuRestoreRegisters(PVOID adapter);
BOOLEAN A830DpuEnabled(PVOID adapter);
BOOLEAN A830DpuReferences(PVOID adapter,ULONGLONG physical);
/* RAM snapshot of the last driver-confirmed latch, never a pending queue.
 * Caller keeps the allocation pool alive while using the returned address. */
BOOLEAN A830DpuActive(PVOID adapter,PULONGLONG physical,PULONG pitch,PULONGLONG address,PULONG frame);
VOID A830DpuTraceSnapshot(PVOID output);
NTSTATUS A830DpuQueue(PVOID adapter,ULONGLONG physical,ULONG pitch,ULONGLONG wddmAddress);
VOID A830DpuEnableVsync(PVOID adapter,BOOLEAN enabled);
NTSTATUS A830DisplayClocks(PVOID adapter,PVOID request,PVOID output);
NTSTATUS A830DisplayBusInfo(PVOID adapter,PVOID output);
NTSTATUS A830DisplayActivation(PVOID adapter,PVOID request,PVOID output);
/* Worker-only dynamic initialization; TRUE holds the display ordering barrier. */
BOOLEAN A830DpuWork(PVOID adapter);
