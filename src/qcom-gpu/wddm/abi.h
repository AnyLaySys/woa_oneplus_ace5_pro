#pragma once
#include "../uapi.h"
#define A830_ABI_VERSION 4
#define A830_RESOURCE_OWNED 2u
#define A830_RESOURCE_PRIMARY 4u
#define A830_SHARED_BASE 0xC8000000ULL
#define A830_SHARED_END GPU_CLIENT_VA_BASE
#define A830_OP_FILL 1
#define A830_OP_CLIENT 3
#define A830_DMA_BYTES 8192
typedef struct { unsigned Version,Flags,Status,Chip; } A830_ADAPTER_INFO;
typedef struct { unsigned Version,Reserved; GPU_U64 CompletionEvent,CompletionPage; } A830_CONTEXT_DATA;
typedef struct { GPU_U64 Submitted,Completed; unsigned Status,Generation; } A830_COMPLETION;
typedef struct { unsigned Version,Reserved; GPU_U64 CompletionPage; } A830_CONTEXT_QUERY;
typedef struct { unsigned Version,Size,Width,Height,Pitch,Format,Flags,Rotation; } A830_RESOURCE;
static inline unsigned A830PixelBytes(unsigned format) {
    switch(format) {
    case D3DDDIFMT_A8R8G8B8:case D3DDDIFMT_X8R8G8B8:case D3DDDIFMT_A8B8G8R8:
    case D3DDDIFMT_A2R10G10B10:case D3DDDIFMT_A2B10G10R10:return 4;
    case D3DDDIFMT_A16B16G16R16F:return 8;
    default:return 0;
    }
}
typedef struct { unsigned Version,Allocation; GPU_U64 Address; unsigned Size,Pitch,Format,Reserved; } A830_ALLOCATION_ADDRESS;
typedef struct { unsigned Version,Operation,Allocation,Offset,Bytes,Value; } A830_COMMAND;
typedef struct {
    unsigned Version,Operation,Count,Reserved;
    GPU_U64 Lease;
    GPU_U64 Fence;
    GPU_SUBMIT Commands[256];
} A830_BATCH_COMMAND;
