#pragma once
#include "../gpu.h"
NTSTATUS A830ScanoutStart(PVOID adapter);
VOID A830ScanoutStop(PVOID adapter);
NTSTATUS A830ScanoutAllocate(PVOID adapter,GPU_BO *buffer,ULONG bytes);
BOOLEAN A830ScanoutFree(PVOID adapter,GPU_BO *buffer);

BOOLEAN A830ScanoutStagingReady(PVOID adapter);
NTSTATUS A830ScanoutStaging(PVOID adapter,ULONG index,GPU_BO *buffer);
