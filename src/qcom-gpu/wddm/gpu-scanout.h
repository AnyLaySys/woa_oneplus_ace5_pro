#pragma once
#include <ntifs.h>
/* Temporary GPU copy into the inherited firmware scanout, not DPU flip/TE.
 * All calls run at PASSIVE_LEVEL in the scheduler's submission barrier.
 * Issue requires all earlier GPU fences retired and a held source BO lease.
 * Keep that lease until Retire returns SUCCESS or Stop has quiesced hardware.
 */
NTSTATUS A830GpuScanoutStart(PVOID adapter);
NTSTATUS A830GpuScanoutIssue(PVOID adapter,ULONGLONG source,ULONG sourceBytes,ULONG pitch,ULONGLONG address,PULONGLONG fence);
NTSTATUS A830GpuScanoutRetire(PVOID adapter);
NTSTATUS A830GpuScanoutStop(PVOID adapter);
NTSTATUS A830GpuScanoutRemap(PVOID adapter);
/* Only after a full GPU reset has stopped all old-generation DMA. */
NTSTATUS A830GpuScanoutAfterReset(PVOID adapter);
BOOLEAN A830GpuScanoutEnabled(PVOID adapter);
NTSTATUS A830GpuScanoutQueue(PVOID adapter,PVOID allocation,ULONGLONG offset,ULONG pitch,ULONGLONG address);
/* Pump runs only in the scheduler worker; TRUE holds the display barrier. */
BOOLEAN A830GpuScanoutPump(PVOID adapter);

BOOLEAN A830GpuScanoutStagingReady(PVOID adapter);
BOOLEAN A830GpuScanoutBusy(PVOID adapter);
VOID A830GpuScanoutSnapshot(PVOID adapter,PVOID output);
NTSTATUS A830GpuScanoutReserveStaging(PVOID adapter);
