#pragma once
#include "gpu.h"

typedef struct IRIS IRIS;
NTSTATUS IrisCreate(GPU *gpu,PDRIVER_OBJECT driver,IRIS **out);
NTSTATUS IrisDelete(IRIS *iris);
