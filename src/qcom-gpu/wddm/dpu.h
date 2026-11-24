#pragma once
#include <ntifs.h>

/* The opaque adapter arguments avoid introducing an adapter.h include cycle.
 * Start/Stop run at PASSIVE_LEVEL. Queue is safe at the display DIRQL.
 * The caller owns both the pending and active scanout BO until a later TE
 * has confirmed their replacement. Physical addresses must be contiguous.
 */
NTSTATUS A830DpuInitialize(PVOID adapter);
BOOLEAN A830DpuCanRefresh(PVOID adapter);
NTSTATUS A830DpuSetRefresh(PVOID adapter,ULONG hz);
NTSTATUS A830DpuStop(PVOID adapter);
/* Crash/bugcheck path: MMIO only, no allocation, timer deletion or waits. */
VOID A830DpuRestoreRegisters(PVOID adapter);
BOOLEAN A830DpuEnabled(PVOID adapter);
BOOLEAN A830DpuReferences(PVOID adapter,ULONGLONG physical);
NTSTATUS A830DpuQueue(PVOID adapter,ULONGLONG physical,ULONG pitch,ULONGLONG wddmAddress,ULONG format,BOOLEAN hdr);
VOID A830DpuEnableVsync(PVOID adapter,BOOLEAN enabled);
VOID A830DpuWork(A830_ADAPTER *adapter);
