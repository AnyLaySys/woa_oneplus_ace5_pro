#pragma once
#include "gpu.h"

typedef struct IRIS IRIS;
NTSTATUS IrisCreate(GPU *gpu,PDRIVER_OBJECT driver,IRIS **out);
NTSTATUS IrisDelete(IRIS *iris);
NTSTATUS IrisStart(IRIS *iris,const GPU_VIDEO_CONFIG *config);
NTSTATUS IrisEncode(IRIS *iris,GPU_BO *input,ULONGLONG time,BOOLEAN key,ULONG bitrate,PUCHAR output,ULONG capacity,PULONG bytes,PBOOLEAN keyframe);
NTSTATUS IrisDecodeStart(IRIS *iris,const GPU_DECODE_CONFIG *config);
NTSTATUS IrisDecodeInput(IRIS *iris,const UCHAR *data,ULONG bytes,ULONGLONG time);
NTSTATUS IrisDecodeOutput(IRIS *iris,GPU_DECODE_OUTPUT *output,ULONG capacity,PULONG bytes);
NTSTATUS IrisDecodeDrain(IRIS *iris);
NTSTATUS IrisDecodeSurface(IRIS *iris,GPU_BO *buffer,const GPU_DECODE_SURFACE *surface);
NTSTATUS IrisDecodeComplete(IRIS *iris,GPU_DECODE_OUTPUT *output);
NTSTATUS IrisDecodeDequeue(IRIS *iris,GPU_DECODE_COMPLETION *output);
