#pragma once
#include <ntifs.h>
/* Temporary GPU copy into the inherited firmware scanout, not DPU flip/TE.
 * All calls run at PASSIVE_LEVEL in the scheduler's submission barrier.
 * Issue requires all earlier GPU fences retired and a held source BO lease.
 * Keep that lease until Retire returns SUCCESS or Stop has quiesced hardware.
 */
NTSTATUS QcomGpuScanoutStart(PVOID adapter);
NTSTATUS QcomGpuScanoutIssue(PVOID adapter,ULONGLONG source,ULONG sourceBytes,ULONG pitch,ULONGLONG address,PULONGLONG fence);
NTSTATUS QcomGpuScanoutRetire(PVOID adapter);
NTSTATUS QcomGpuScanoutStop(PVOID adapter);
NTSTATUS QcomGpuScanoutRemap(PVOID adapter);
/* Only after a full GPU reset has stopped all old-generation DMA. */
NTSTATUS QcomGpuScanoutAfterReset(PVOID adapter);
BOOLEAN QcomGpuScanoutEnabled(PVOID adapter);
NTSTATUS QcomGpuScanoutQueue(PVOID adapter,PVOID allocation,ULONGLONG offset,ULONG pitch,ULONGLONG address,ULONGLONG presentId);
NTSTATUS QcomGpuScanoutQueueFirmware(PVOID adapter,ULONGLONG presentId);
/* Pump runs only in the scheduler worker; TRUE holds the display barrier. */
BOOLEAN QcomGpuScanoutPump(PVOID adapter);

BOOLEAN QcomGpuScanoutStagingReady(PVOID adapter);
BOOLEAN QcomGpuScanoutBusy(PVOID adapter);
NTSTATUS QcomGpuScanoutReserveStaging(PVOID adapter);
