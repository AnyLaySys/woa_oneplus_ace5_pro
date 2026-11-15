#pragma once
#include "../gpu.h"
#include "../uapi.h"
#include "memory-layout.h"

#define A830_KERNEL_INTERFACE_VERSION 1
#define A830_KERNEL_BASE 0x70000000ULL
#define A830_KERNEL_END (A830_KERNEL_BASE+A830_SEGMENT_BYTES+A830_COMMAND_BYTES)
#define A830_IOCTL_KERNEL_INTERFACE CTL_CODE(FILE_DEVICE_UNKNOWN,0x900,METHOD_BUFFERED,FILE_READ_DATA)

static __forceinline BOOLEAN A830KernelRange(ULONGLONG address,SIZE_T bytes) {
    if(!bytes)return FALSE;
    if(address>=A830_KERNEL_BASE && address<A830_KERNEL_END)return bytes<=A830_KERNEL_END-address;
    return address>=A830_SHARED_ADDRESS && address<GPU_VA_END && bytes<=GPU_VA_END-address;
}

typedef struct {
    ULONG Size,Version;
    NTSTATUS (*Start)(VOID);
    NTSTATUS (*Reset)(VOID);
    NTSTATUS (*Map)(GPU_BO *buffer,ULONGLONG address);
    NTSTATUS (*Unmap)(GPU_BO *buffer);
    NTSTATUS (*Execute)(const GPU_COMMAND *commands,ULONG count);
    NTSTATUS (*ExecuteClient)(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *commands,ULONG count);
    NTSTATUS (*Timestamp)(PULONGLONG counter);
} A830_KERNEL_INTERFACE;
