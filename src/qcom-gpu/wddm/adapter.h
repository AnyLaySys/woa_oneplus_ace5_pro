#pragma once
#define DXGKDDI_INTERFACE_VERSION 0xB004
#include "../gpu.h"
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "abi.h"
#include "kernel-api.h"
#include "fence-order.h"

#if defined(A830_RENDER_ONLY) || defined(A830_NATIVE_DISPLAY)
#define A830_SEGMENT_ADDRESS A830_KERNEL_BASE
#define A830_COMMAND_ADDRESS (A830_KERNEL_BASE+A830_SEGMENT_BYTES)
#else
#define A830_SEGMENT_ADDRESS 0x44000000ULL
#define A830_COMMAND_ADDRESS 0x48000000ULL
#endif
#define A830_QUEUE_DEPTH 32
typedef struct A830_CONTEXT A830_CONTEXT;
typedef struct A830_PREPARED A830_PREPARED;
typedef struct A830_ALLOCATION A830_ALLOCATION;
typedef struct A830_SHARED_BLOCK A830_SHARED_BLOCK;
typedef struct {
    KMUTEX Lock;
    ULONGLONG Bytes;
    SIZE_T PageCount,BlockCount;
    A830_SHARED_BLOCK **Blocks;
} A830_SHARED_MEMORY;
typedef struct { GPU_BO Segment,Commands; A830_SHARED_MEMORY Shared; } A830_MEMORY;
typedef struct {
    ULONG Operation,Offset,Bytes,Value;
    PVOID Source,Destination;
    PMDL SourceMdl,DestinationMdl;
    A830_ALLOCATION *SourceAllocation,*DestinationAllocation;
    ULONGLONG SourceShared,DestinationShared;
    SIZE_T SourceBytes,DestinationBytes;
    BOOLEAN SourceSharedMap,DestinationSharedMap,SourcePinned,DestinationPinned;
    ULONG Width,Height,SourcePitch,DestinationPitch;
    ULONGLONG PresentBytes;
    PEPROCESS Process;
    PKEVENT Completion;
    ULONGLONG Lease;
    A830_PREPARED *Prepared;
    A830_CONTEXT *Context;
    ULONGLONG Sequence;
    ULONG Count;
    union { GPU_SUBMIT Commands[256]; RECT Rects[256]; } Payload;
} A830_DMA;
typedef struct { LIST_ENTRY Entry; ULONGLONG QueuedAt,HardwareFence; ULONG Fence; BOOLEAN Dynamic; A830_DMA Dma; } A830_JOB;
typedef struct {
    GPU Hardware;
    PVOID Dpu,ScanoutPool,GpuScanout;
    PHYSICAL_ADDRESS GpuRegisters;
    ULONG GpuRegisterSize;
    DXGKRNL_INTERFACE Dxgk;
    A830_MEMORY Memory;
    DXGK_DISPLAY_INFORMATION DisplayInfo;
    PUCHAR Framebuffer;
    SIZE_T FramebufferSize;
    BOOLEAN Visible;
    BOOLEAN DisplayOnly;
    BOOLEAN RenderOnly;
    BOOLEAN BridgeAttached;
    volatile LONG FlipPending;
    volatile LONG VsyncEnabled;
    ULONGLONG ScanoutOffset;
    ULONG ScanoutPitch;
    ULONGLONG ScanoutAddress;
    PUCHAR ScanoutCpu;
    EX_RUNDOWN_REF ScanoutAccess;
    PFILE_OBJECT HardwareFile;
    A830_KERNEL_INTERFACE Kernel;
    ULONG PresentCount;
    ULONG RefreshRate;
    HANDLE Thread;
    PEX_TIMER CompletionTimer;
    KEVENT Wake;
    KDPC DisplayWake;
    GPU_TELEMETRY Telemetry;
    GPU_PATCH_DIAGNOSTICS PatchDiagnostics;
    KSPIN_LOCK PatchDiagnosticsLock;
    KSPIN_LOCK QueueLock;
    LIST_ENTRY Pending,Free;
    LIST_ENTRY Inflight;
    LIST_ENTRY Contexts;
    KSPIN_LOCK ContextLock;
    volatile LONG FaultFence;
    struct { ULONGLONG HardwareFence,Sequence; ULONG Fence; A830_CONTEXT *Context; } IrqRecords[128];
    volatile ULONG IrqRead,IrqWrite;
    volatile LONG WorkerBusy;
    volatile LONG ResetCount;
    volatile LONG64 CompletionPolls,PolledCompletions;
    A830_JOB Jobs[A830_QUEUE_DEPTH];
    volatile LONG Stopping,Failed,PreemptPending,SchedulerDpcPending,Generation,CpuBarriers;
    ULONG Completed,Preemption,LastSubmitted;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA Notify;
} A830_ADAPTER;
BOOLEAN A830RotationSupported(const A830_ADAPTER *adapter,D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rotation);
VOID A830SchedulerSnapshot(A830_ADAPTER *adapter,GPU_SCHEDULER_INFO *info);
VOID A830CompletionTick(PEX_TIMER timer,PVOID context);
static __forceinline VOID A830Count(GPU_U64 *field,ULONGLONG amount) {
    InterlockedAdd64((volatile LONG64 *)field,(LONG64)amount);
}
static __forceinline ULONG A830PublishFence(volatile ULONG *field,ULONG fence) {
    LONG previous=InterlockedCompareExchange((volatile LONG *)field,0,0);
    while(A830FenceAfter(fence,(ULONG)previous)) {
        LONG seen=InterlockedCompareExchange((volatile LONG *)field,(LONG)fence,previous);
        if(seen==previous)return fence;
        previous=seen;
    }
    return (ULONG)previous;
}
static __forceinline VOID A830Sample(GPU_U64 *histogram,ULONGLONG interval) {
    static const ULONG bounds[]={10000,20000,40000,80000,160000,200000,330000,670000};
    ULONG i=0;while(i<RTL_NUMBER_OF(bounds) && interval>=bounds[i])i++;
    A830Count(histogram+i,1);
}
static __forceinline VOID A830WakeDisplay(A830_ADAPTER *a) {
    if(KeGetCurrentIrql()<=DISPATCH_LEVEL)KeSetEvent(&a->Wake,IO_NO_INCREMENT,FALSE);
    else KeInsertQueueDpc(&a->DisplayWake,NULL,NULL);
}
typedef struct { A830_ADAPTER *Adapter; } A830_DEVICE;
struct A830_CONTEXT { A830_ADAPTER *Adapter; PEPROCESS Process; PKEVENT Completion; GPU_BO CompletionBuffer; PVOID CompletionMap; EX_RUNDOWN_REF Jobs; LIST_ENTRY Entry; ULONGLONG NotifiedSequence; KSPIN_LOCK TokenLock; LIST_ENTRY Rendered; BOOLEAN Closing; ULONGLONG ClientLease; };
struct A830_ALLOCATION { A830_RESOURCE Resource; LIST_ENTRY Entry; GPU_BO Buffer; ULONG References; ULONGLONG ReleaseFence; BOOLEAN ReleasePending; volatile LONG ScanoutReferences; };
NTSTATUS A830SharedCreate(A830_ALLOCATION *allocation);
VOID A830SharedDestroy(A830_ALLOCATION *allocation);
NTSTATUS APIENTRY A830Escape(HANDLE adapter,const DXGKARG_ESCAPE *args);
VOID A830FinishJob(A830_JOB *job,BOOLEAN signal);
VOID A830RecycleJob(A830_ADAPTER *adapter,A830_JOB *job);
NTSTATUS A830Prepare(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *commands,ULONG count,A830_PREPARED **prepared);
NTSTATUS A830Issue(A830_PREPARED *prepared,PULONGLONG fence);
VOID A830ReleasePrepared(A830_PREPARED *prepared);
VOID A830Retire(A830_ADAPTER *adapter);
BOOLEAN A830InterruptComplete(PVOID context);
NTSTATUS A830ContextStart(A830_CONTEXT *context,A830_CONTEXT_DATA *data);
VOID A830ContextStop(A830_CONTEXT *context);
VOID A830CancelRendered(A830_ADAPTER *adapter,const A830_DMA *token);
VOID A830CancelRenderedClient(A830_ADAPTER *adapter,PEPROCESS process,ULONGLONG lease);
ULONG A830FreezeFences(A830_ADAPTER *adapter);
A830_JOB *A830TakePending(A830_ADAPTER *adapter);

NTSTATUS A830MemoryStart(A830_ADAPTER *adapter);
VOID A830MemoryStop(A830_ADAPTER *adapter);
NTSTATUS APIENTRY A830Query(HANDLE adapter,const DXGKARG_QUERYADAPTERINFO *args);
NTSTATUS APIENTRY A830CreateAllocation(HANDLE adapter,DXGKARG_CREATEALLOCATION *args);
NTSTATUS APIENTRY A830DestroyAllocation(HANDLE adapter,const DXGKARG_DESTROYALLOCATION *args);
NTSTATUS APIENTRY A830Paging(HANDLE adapter,DXGKARG_BUILDPAGINGBUFFER *args);
NTSTATUS A830SharedStart(A830_ADAPTER *adapter);
VOID A830SharedStop(A830_ADAPTER *adapter);
NTSTATUS A830SharedMap(A830_ADAPTER *adapter,SIZE_T page,SIZE_T count,PMDL source,ULONG offset);
NTSTATUS A830SharedUnmap(A830_ADAPTER *adapter,SIZE_T page,SIZE_T count);
NTSTATUS A830SharedRestore(A830_ADAPTER *adapter);
NTSTATUS A830MapSharedCpu(A830_ADAPTER *adapter,ULONGLONG address,SIZE_T bytes,PVOID *map,PMDL *mapping);
NTSTATUS APIENTRY A830StandardAllocation(HANDLE adapter,DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *args);
NTSTATUS APIENTRY A830Render(HANDLE context,DXGKARG_RENDER *args);
NTSTATUS APIENTRY A830Patch(HANDLE adapter,const DXGKARG_PATCH *args);
NTSTATUS APIENTRY A830Submit(HANDLE adapter,const DXGKARG_SUBMITCOMMAND *args);
VOID A830Worker(PVOID context);
VOID A830Record(PCWSTR label,ULONG value);
NTSTATUS A830DisplayStart(A830_ADAPTER *adapter);
NTSTATUS A830DisplayLayout(const DXGK_DISPLAY_INFORMATION *display,SIZE_T *bytes);
ULONG A830RefreshRate(const A830_ADAPTER *adapter);
D3DKMDT_VIDEO_SIGNAL_INFO A830Signal(const A830_ADAPTER *adapter);
NTSTATUS A830AdapterStop(PVOID context);
VOID A830DisplayStop(A830_ADAPTER *adapter);
VOID A830DisplayDdi(DRIVER_INITIALIZATION_DATA *data);
VOID A830DisplayOnlyDdi(KMDDOD_INITIALIZATION_DATA *data);
NTSTATUS A830BlitFrame(PUCHAR destination,ULONG pitch,ULONG width,ULONG height,const VOID *source,LONG sourcePitch,const RECT *rect);
NTSTATUS A830MoveFrame(PUCHAR destination,ULONG pitch,ULONG width,ULONG height,const POINT *source,const RECT *rect);
NTSTATUS A830BridgeInitialize(PDRIVER_OBJECT object);
VOID A830BridgeDestroy(VOID);
VOID A830BridgeAttach(A830_ADAPTER *adapter);
VOID A830BridgeDetach(A830_ADAPTER *adapter);
VOID A830BridgeCollectShared(VOID);
BOOLEAN A830PathSupport(const A830_ADAPTER *adapter,D3DKMDT_VIDPN_PRESENT_PATH *path,const DXGKARG_ENUMVIDPNCOFUNCMODALITY *args);
NTSTATUS A830KernelConnect(A830_ADAPTER *adapter);
VOID A830KernelDisconnect(A830_ADAPTER *adapter);
NTSTATUS A830Map(A830_ADAPTER *adapter,GPU_BO *buffer,ULONGLONG address);
NTSTATUS APIENTRY A830Present(HANDLE context,DXGKARG_PRESENT *args);
NTSTATUS A830CopyPresent(const A830_DMA *dma);
NTSTATUS A830PinPresent(A830_DMA *dma);
NTSTATUS A830MapPresent(A830_ADAPTER *adapter,A830_DMA *dma);
VOID A830ReleasePresent(A830_DMA *dma);
NTSTATUS A830CopyPresentToFrame(const A830_DMA *dma,PUCHAR destination,ULONG pitch,ULONG width,ULONG height);
NTSTATUS A830BridgeExecuteClient(PEPROCESS process,ULONGLONG lease,const GPU_SUBMIT *commands,ULONG count);
NTSTATUS A830BridgeExecute(const GPU_COMMAND *commands,ULONG count);
NTSTATUS A830BridgeMap(GPU_BO *buffer,ULONGLONG address);
NTSTATUS A830BridgeUnmap(GPU_BO *buffer);
NTSTATUS A830BridgeReset(VOID);
VOID A830BridgeStopHardware(A830_ADAPTER *adapter);
NTSTATUS A830BridgeTimestamp(PULONGLONG counter);

#include "dpu.h"
#include "scanout.h"
#include "gpu-scanout.h"
NTSTATUS A830BridgeReserveDisplay(PVOID adapter,GPU_BO buffers[2]);
