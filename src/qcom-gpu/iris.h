#pragma once
#include "gpu.h"

typedef struct IRIS IRIS;
NTSTATUS IrisCreate(GPU *gpu,PDRIVER_OBJECT driver,IRIS **out);
NTSTATUS IrisDelete(IRIS *iris);
NTSTATUS IrisStart(IRIS *iris,const GPU_VIDEO_CONFIG *config);
NTSTATUS IrisEncode(IRIS *iris,GPU_BO *input,ULONGLONG time,BOOLEAN key,ULONG bitrate,PUCHAR output,ULONG capacity,PULONG bytes,PBOOLEAN keyframe);
