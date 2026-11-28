#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WDDM3_0
#include <ntifs.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include "../scheduler-abi.h"
#define TAG 'mnNQ'
#define MEMORY_BYTES (231u * PAGE_SIZE)
#define QUEUE_COUNT 16u
#define NPU_ALLOCATIONS 256u
#define VA_BITS 36u
#define TABLE_ENTRIES 256u
#define TABLE_LEVELS 3u

typedef struct ADAPTER ADAPTER;
typedef struct { ADAPTER *Adapter; ULONG Owner; } PROCESS;
typedef struct { ADAPTER *Adapter; ULONG Owner; ULONGLONG Cookie; LIST_ENTRY Entry; } DEVICE;
typedef struct { ADAPTER *Adapter; ULONG Owner; ULONGLONG Cookie; PKEVENT Completion; EX_RUNDOWN_REF Jobs; BOOLEAN System; D3DGPU_PHYSICAL_ADDRESS Root; UINT RootEntries; } NPU_CONTEXT;
typedef struct {
    ULONG Operation, Bytes, Value;
    PVOID Source, Destination;
    NPU_SCHEDULER_COMMAND Command;
    D3DGPU_VIRTUAL_ADDRESS Address;
    D3DGPU_PHYSICAL_ADDRESS Root;
    UINT RootEntries;
} DMA;
typedef struct { LIST_ENTRY Entry; ULONG Fence; NPU_CONTEXT *Context; DMA Data; } JOB;
typedef struct { NPU_SCHEDULER_MEMORY Memory; BOOLEAN Registered; } ALLOCATION;
struct ADAPTER {
    DXGKRNL_INTERFACE Dxgk;
    HANDLE Thread;
    KEVENT Wake;
    KSPIN_LOCK Lock;
    LIST_ENTRY Pending, Free, Devices;
    JOB Jobs[QUEUE_COUNT];
    PUCHAR Memory;
    PHYSICAL_ADDRESS Physical;
    volatile LONG Stopping, Failed, Busy, Preempt;
    ULONG Completed, Preemption;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA Notify;
};
// Windows owns these page tables. The command worker translates its process's
// GPU VA before dispatching the corresponding isolated FastRPC protection domain.
static NTSTATUS ReadPhysical(ADAPTER *a, UINT segment, UINT64 address, PVOID destination, SIZE_T bytes) {
    if (segment == 1) {
        if (address > MEMORY_BYTES || bytes > MEMORY_BYTES - address) return STATUS_ACCESS_VIOLATION;
        RtlCopyMemory(destination, a->Memory + address, bytes); return STATUS_SUCCESS;
    }
    if (segment != 0 && segment != 3) return STATUS_ACCESS_VIOLATION;
    MM_COPY_ADDRESS source; source.PhysicalAddress.QuadPart = address; SIZE_T copied = 0;
    NTSTATUS status = MmCopyMemory(destination, source, bytes, MM_COPY_MEMORY_PHYSICAL, &copied);
    return NT_SUCCESS(status) && copied == bytes ? STATUS_SUCCESS : STATUS_ACCESS_VIOLATION;
}
static NTSTATUS ReadCommand(ADAPTER *a, const DMA *dma, NPU_SCHEDULER_COMMAND *command) {
    UINT64 address = dma->Address;
    if (address >= (1ULL << VA_BITS) || (address & (PAGE_SIZE - 1)) > PAGE_SIZE - sizeof(*command)) return STATUS_ACCESS_VIOLATION;
    UINT segment = dma->Root.SegmentId; UINT64 table = dma->Root.SegmentOffset;
    for (UINT level = TABLE_LEVELS; level; level--) {
        UINT index = (UINT)(address >> (12 + 8 * (level - 1))) & (TABLE_ENTRIES - 1);
        if (level == TABLE_LEVELS && index >= dma->RootEntries) return STATUS_ACCESS_VIOLATION;
        DXGK_PTE entry;
        NTSTATUS status = ReadPhysical(a, segment, table + index * sizeof(entry), &entry, sizeof(entry));
        if (!NT_SUCCESS(status) || !entry.Valid || entry.Zero || entry.LargePage || entry.PhysicalAdapterIndex || entry.PageTablePageSize)
            return STATUS_ACCESS_VIOLATION;
        // DXGK_PTE carries a page number, unlike the root's byte offset.
        segment = (UINT)entry.Segment; table = entry.PageAddress << PAGE_SHIFT;
        if (level == 1 && entry.NoExecute) return STATUS_ACCESS_VIOLATION;
    }
    return ReadPhysical(a, segment, table + (address & (PAGE_SIZE - 1)), command, sizeof(*command));
}
static BOOLEAN Notify(PVOID pointer) {
    ADAPTER *a = pointer;
    a->Dxgk.DxgkCbNotifyInterrupt(a->Dxgk.DeviceHandle, &a->Notify);
    return a->Dxgk.DxgkCbQueueDpc(a->Dxgk.DeviceHandle);
}
static VOID Complete(ADAPTER *a, ULONG fence) {
    RtlZeroMemory(&a->Notify, sizeof(a->Notify)); a->Notify.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    a->Notify.DmaCompleted.SubmissionFenceId = fence; a->Notify.DmaCompleted.NodeOrdinal = 0; a->Notify.DmaCompleted.EngineOrdinal = 0;
    BOOLEAN result = FALSE;
    NTSTATUS status = a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle, Notify, a, 0, &result);
    if (!NT_SUCCESS(status)) InterlockedExchange(&a->Failed, status);
    a->Completed = fence;
}
static VOID Recycle(ADAPTER *a, JOB *job, ULONG reason) {
    if (job->Context) {
        BOOLEAN work = job->Data.Operation == 1 || job->Data.Operation == 5;
        if (reason == 1 && work) NpuSchedulerCancel(job->Data.Command.Token, job->Context->Owner);
        if (reason != 2 && work && job->Context->Completion) KeSetEvent(job->Context->Completion, IO_NO_INCREMENT, FALSE);
        ExReleaseRundownProtection(&job->Context->Jobs);
    }
    ExInterlockedInsertTailList(&a->Free, &job->Entry, &a->Lock);
}
static VOID CancelQueue(ADAPTER *a, BOOLEAN preempt) {
    PLIST_ENTRY entry;
    // Preemption returns unexecuted packets to Windows for resubmission. Their
    // transport tickets and completion events must remain pending.
    while ((entry = ExInterlockedRemoveHeadList(&a->Pending, &a->Lock))) Recycle(a, CONTAINING_RECORD(entry, JOB, Entry), preempt ? 2 : 1);
}
static VOID Worker(PVOID pointer) {
    ADAPTER *a = pointer;
    for (;;) {
        KeWaitForSingleObject(&a->Wake, Executive, KernelMode, FALSE, NULL);
        if (a->Stopping) break;
        if (InterlockedExchange(&a->Preempt, 0)) {
            CancelQueue(a, TRUE); RtlZeroMemory(&a->Notify, sizeof(a->Notify)); a->Notify.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
            a->Notify.DmaPreempted.PreemptionFenceId = a->Preemption; a->Notify.DmaPreempted.LastCompletedFenceId = a->Completed;
            BOOLEAN result = FALSE;
            a->Dxgk.DxgkCbSynchronizeExecution(a->Dxgk.DeviceHandle, Notify, a, 0, &result);
        }
        PLIST_ENTRY entry;
        while (!a->Stopping && !a->Preempt && (entry = ExInterlockedRemoveHeadList(&a->Pending, &a->Lock))) {
            JOB *job = CONTAINING_RECORD(entry, JOB, Entry); InterlockedExchange(&a->Busy, 1);
            NTSTATUS status = STATUS_SUCCESS;
            if (job->Data.Operation == 1) status = NpuSchedulerRun(job->Data.Command.Token, job->Context->Owner, &a->Preempt);
            else if (job->Data.Operation == 5) {
                NPU_SCHEDULER_COMMAND command;
                status = ReadCommand(a, &job->Data, &command);
                if (NT_SUCCESS(status) && RtlCompareMemory(&command, &job->Data.Command, sizeof(command)) != sizeof(command)) status = STATUS_ACCESS_DENIED;
                if (NT_SUCCESS(status)) status = NpuSchedulerRun(command.Token, job->Context->Owner, &a->Preempt);
                else NpuSchedulerCancel(job->Data.Command.Token, job->Context->Owner);
            }
            else if (job->Data.Operation == 2) RtlCopyMemory(job->Data.Destination, job->Data.Source, job->Data.Bytes);
            else if (job->Data.Operation == 3) {
                PULONG p = job->Data.Destination;
                for (ULONG i = 0; i < job->Data.Bytes / 4; i++) p[i] = job->Data.Value;
            }
            if (status == STATUS_DEVICE_BUSY && a->Preempt) {
                Recycle(a, job, 2); InterlockedExchange(&a->Busy, 0); continue;
            }
            // User model errors still complete their packet. Timeout cannot
            // release DMA; the transport retains it until process release.
            if (status == STATUS_IO_TIMEOUT || status == STATUS_DEVICE_NOT_CONNECTED) InterlockedExchange(&a->Failed, status);
            Complete(a, job->Fence);
            Recycle(a, job, 0); InterlockedExchange(&a->Busy, 0);
        }
    }
    CancelQueue(a, FALSE); PsTerminateSystemThread(STATUS_SUCCESS);
}
static NTSTATUS Add(PDEVICE_OBJECT device, PVOID *context) {
    UNREFERENCED_PARAMETER(device);
    ADAPTER *a = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), TAG);
    if (!a) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeSpinLock(&a->Lock); KeInitializeEvent(&a->Wake, SynchronizationEvent, FALSE);
    InitializeListHead(&a->Pending); InitializeListHead(&a->Free); InitializeListHead(&a->Devices);
    for (ULONG i = 0; i < QUEUE_COUNT; i++) InsertTailList(&a->Free, &a->Jobs[i].Entry);
    *context = a; return STATUS_SUCCESS;
}
static NTSTATUS Start(PVOID context, PDXGK_START_INFO start, PDXGKRNL_INTERFACE dxgk, PULONG sources, PULONG children) {
    UNREFERENCED_PARAMETER(start); ADAPTER *a = context; a->Dxgk = *dxgk; *sources = *children = 0;
    // PnP can start this dependent adapter while FastRPC is negotiating its
    // link. Bound the wait and let the transport make progress between polls.
    BOOLEAN ready = FALSE; LARGE_INTEGER interval; interval.QuadPart = -100000;
    for (ULONG i = 0; i < 200; i++) {
        if ((ready = NpuSchedulerReady())) break;
        KeDelayExecutionThread(KernelMode, FALSE, &interval);
    }
    if (!ready) return STATUS_DEVICE_NOT_READY;
    PHYSICAL_ADDRESS low = {0}, high, boundary = {0}; high.QuadPart = MAXLONGLONG;
    a->Memory = MmAllocateContiguousMemorySpecifyCache(MEMORY_BYTES, low, high, boundary, MmNonCached);
    if (!a->Memory) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(a->Memory, MEMORY_BYTES); a->Physical = MmGetPhysicalAddress(a->Memory);
    a->Stopping = a->Failed = 0;
    OBJECT_ATTRIBUTES attributes; InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    NTSTATUS status = PsCreateSystemThread(&a->Thread, SYNCHRONIZE, &attributes, NULL, NULL, Worker, a);
    if (!NT_SUCCESS(status)) { MmFreeContiguousMemorySpecifyCache(a->Memory, MEMORY_BYTES, MmNonCached); a->Memory = NULL; }
    return status;
}
static NTSTATUS Stop(PVOID context) {
    ADAPTER *a = context; InterlockedExchange(&a->Stopping, 1); KeSetEvent(&a->Wake, 0, FALSE);
    if (a->Thread) { ZwWaitForSingleObject(a->Thread, FALSE, NULL); ZwClose(a->Thread); a->Thread = NULL; }
    if (a->Memory) { MmFreeContiguousMemorySpecifyCache(a->Memory, MEMORY_BYTES, MmNonCached); a->Memory = NULL; }
    return STATUS_SUCCESS;
}
static NTSTATUS Remove(PVOID context) { ExFreePoolWithTag(context, TAG); return STATUS_SUCCESS; }
static VOID Unload(VOID) {}
// The port's initialization validator requires this callback even for a
// compute adapter. No legacy video IOCTL is supported by the NPU.
static NTSTATUS Dispatch(PVOID context, ULONG source, PVIDEO_REQUEST_PACKET packet) {
    UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(source);
    packet->StatusBlock->Status = STATUS_NOT_SUPPORTED; packet->StatusBlock->Information = 0;
    return STATUS_NOT_SUPPORTED;
}
static BOOLEAN Interrupt(PVOID context, ULONG message) { UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(message); return FALSE; }
static VOID Dpc(PVOID context) { ADAPTER *a = context; a->Dxgk.DxgkCbNotifyDpc(a->Dxgk.DeviceHandle); }
static NTSTATUS Children(PVOID context, PDXGK_CHILD_DESCRIPTOR data, ULONG bytes) { UNREFERENCED_PARAMETER(context); RtlZeroMemory(data, bytes); return STATUS_SUCCESS; }
static NTSTATUS ChildStatus(PVOID context, PDXGK_CHILD_STATUS data, BOOLEAN nondestructive) { UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(data); UNREFERENCED_PARAMETER(nondestructive); return STATUS_NOT_SUPPORTED; }
static NTSTATUS Descriptor(PVOID context, ULONG child, PDXGK_DEVICE_DESCRIPTOR data) { UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(child); UNREFERENCED_PARAMETER(data); return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA; }
static NTSTATUS Power(PVOID context, ULONG uid, DEVICE_POWER_STATE state, POWER_ACTION action) { UNREFERENCED_PARAMETER(context); UNREFERENCED_PARAMETER(uid); UNREFERENCED_PARAMETER(state); UNREFERENCED_PARAMETER(action); return STATUS_SUCCESS; }
static VOID ResetDevice(PVOID context) { ADAPTER *a = context; InterlockedExchange(&a->Failed, STATUS_DEVICE_HARDWARE_ERROR); }
static NTSTATUS APIENTRY Query(HANDLE context, const DXGKARG_QUERYADAPTERINFO *p) {
    ADAPTER *a = context; if (!p->pOutputData) return STATUS_INVALID_PARAMETER;
    if (p->Type == DXGKQAITYPE_DRIVERCAPS) {
        if (p->OutputDataSize < sizeof(DXGK_DRIVERCAPS)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_DRIVERCAPS *caps = p->pOutputData; RtlZeroMemory(caps, sizeof(*caps));
        caps->HighestAcceptableAddress.QuadPart = MAXLONGLONG;
        caps->SchedulingCaps.MultiEngineAware = caps->SchedulingCaps.PreemptionAware = caps->SchedulingCaps.LowIrqlPreemptCommand = 1;
        caps->GpuEngineTopology.NbAsymetricProcessingNodes = 1; caps->WDDMVersion = DXGKDDI_WDDMv3_0;
        caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        caps->SupportPerEngineTDR = TRUE; caps->MiscCaps.ComputeOnly = 1;
        caps->MemoryManagementCaps.VirtualAddressingSupported = caps->MemoryManagementCaps.GpuMmuSupported = 1;
        return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_GPUMMUCAPS) {
        if (p->OutputDataSize < sizeof(DXGK_GPUMMUCAPS)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_GPUMMUCAPS *caps = p->pOutputData; RtlZeroMemory(caps, sizeof(*caps));
        caps->CacheCoherentMemorySupported = caps->PageTableUpdateRequireAddressSpaceIdle = 1;
        caps->ReadOnlyMemorySupported = caps->NoExecuteMemorySupported = 1;
        caps->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
        caps->VirtualAddressBitCount = VA_BITS; caps->PageTableLevelCount = TABLE_LEVELS;
        return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_PAGETABLELEVELDESC) {
        if (!p->pInputData || p->InputDataSize < sizeof(DXGK_QUERYPAGETABLELEVELDESCIN)) return STATUS_INVALID_PARAMETER;
        const DXGK_QUERYPAGETABLELEVELDESCIN *input = p->pInputData;
        if (input->LevelIndex >= TABLE_LEVELS || input->PhysicalAdapterIndex) return STATUS_INVALID_PARAMETER;
        if (p->OutputDataSize < sizeof(DXGK_PAGE_TABLE_LEVEL_DESC)) return STATUS_BUFFER_TOO_SMALL;
        *(DXGK_PAGE_TABLE_LEVEL_DESC*)p->pOutputData = (DXGK_PAGE_TABLE_LEVEL_DESC){8, 3, 3, PAGE_SIZE, PAGE_SIZE};
        return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_WDDMDEVICECAPS) {
        if (p->OutputDataSize < sizeof(DXGK_WDDMDEVICECAPS)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_WDDMDEVICECAPS *caps = p->pOutputData; RtlZeroMemory(caps, sizeof(*caps)); caps->WDDMVersion = DXGKDDI_WDDMv3_0; return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_UMDRIVERPRIVATE) {
        if (p->OutputDataSize < sizeof(NPU_SCHEDULER_INFO)) return STATUS_BUFFER_TOO_SMALL;
        *(NPU_SCHEDULER_INFO*)p->pOutputData = (NPU_SCHEDULER_INFO){NPU_SCHEDULER_MAGIC, NPU_SCHEDULER_ABI, QCOM_NPU_VERSION, 0}; return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_NUMPOWERCOMPONENTS) {
        if (p->OutputDataSize < sizeof(UINT)) return STATUS_BUFFER_TOO_SMALL;
        *(UINT*)p->pOutputData = 0; return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_PHYSICAL_MEMORY_CAPS) {
        if (p->OutputDataSize < sizeof(DXGK_PHYSICAL_MEMORY_CAPS)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_PHYSICAL_MEMORY_CAPS *caps = p->pOutputData; RtlZeroMemory(caps, sizeof(*caps));
        // KMT command and paging buffers are consumed by the CPU worker. DSP
        // addresses are separately validated and mapped by the transport.
        caps->HighestVisibleAddress.QuadPart = MAXLONGLONG; return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_NODEPERFDATA) {
        // Global node statistics also query this telemetry block. Rejecting
        // it prevents Task Manager from reading the scheduler's running time.
        // Zero denotes unavailable frequency/voltage telemetry; utilization
        // itself is computed by Windows from actual submitted/completed work.
        if (!p->pInputData || p->InputDataSize != sizeof(ULONG) || *(const ULONG*)p->pInputData)
            return STATUS_INVALID_PARAMETER;
        if (p->OutputDataSize < sizeof(DXGK_NODE_PERFDATA)) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(p->pOutputData, sizeof(DXGK_NODE_PERFDATA)); return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_HISTORYBUFFERPRECISION) {
        if (p->OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION)) return STATUS_BUFFER_TOO_SMALL;
        DXGKARG_HISTORYBUFFERPRECISION *info = p->pOutputData; RtlZeroMemory(info, sizeof(*info)); info->PrecisionBits = 64; return STATUS_SUCCESS;
    }
    if (p->Type == DXGKQAITYPE_QUERYSEGMENT4) {
        if (p->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT4)) return STATUS_BUFFER_TOO_SMALL;
        DXGK_QUERYSEGMENTOUT4 *info = p->pOutputData;
        if (!info->pSegmentDescriptor) { info->NbSegment = 2; return STATUS_SUCCESS; }
        if (info->NbSegment < 2 || info->SegmentDescriptorStride < sizeof(DXGK_SEGMENTDESCRIPTOR4) || !a->Memory) return STATUS_INVALID_PARAMETER;
        DXGK_SEGMENTDESCRIPTOR4 *segment = (PVOID)info->pSegmentDescriptor; RtlZeroMemory(segment, sizeof(*segment));
        segment->Flags.CpuVisible = 1; segment->Flags.CacheCoherent = 1; segment->CpuTranslatedAddress = a->Physical; segment->Size = segment->CommitLimit = MEMORY_BYTES;
        segment = (PVOID)((PUCHAR)info->pSegmentDescriptor + info->SegmentDescriptorStride);
        RtlZeroMemory(segment, sizeof(*segment)); segment->Flags.Aperture = 1; segment->Flags.CacheCoherent = 1;
        segment->Size = segment->CommitLimit = (SIZE_T)NpuSchedulerMemoryBudget();
        info->PagingBufferSegmentId = 0; info->PagingBufferSize = PAGE_SIZE; info->PagingBufferPrivateDataSize = sizeof(DMA);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}
static NTSTATUS APIENTRY CreateDevice(HANDLE adapter, DXGKARG_CREATEDEVICE *p) {
    PROCESS *process = p->hKmdProcess;
    if (!process || process->Adapter != adapter) return STATUS_INVALID_HANDLE;
    DEVICE *device = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*device), TAG); if (!device) return STATUS_INSUFFICIENT_RESOURCES;
    device->Adapter = adapter; device->Owner = process->Owner; p->hDevice = device;
    ExInterlockedInsertTailList(&device->Adapter->Devices, &device->Entry, &device->Adapter->Lock); return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DestroyDevice(HANDLE handle) {
    DEVICE *device = handle; KIRQL old;
    KeAcquireSpinLock(&device->Adapter->Lock, &old); RemoveEntryList(&device->Entry); KeReleaseSpinLock(&device->Adapter->Lock, old);
    // The port has released this device's allocation backing stores. A free
    // allocation callback alone does not fence its outstanding MDL references.
    NpuSchedulerMemoryPurge(device->Owner, device->Cookie);
    ExFreePoolWithTag(device, TAG); return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY CreateContext(HANDLE device, DXGKARG_CREATECONTEXT *p) {
    DEVICE *d = device; if (p->NodeOrdinal) return STATUS_NOT_SUPPORTED;
    NPU_CONTEXT *c = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), TAG); if (!c) return STATUS_INSUFFICIENT_RESOURCES;
    c->Adapter = d->Adapter; c->Owner = d->Owner; c->System = p->Flags.SystemContext != 0; ExInitializeRundownProtection(&c->Jobs);
    if (p->PrivateDriverDataSize) {
        if (p->PrivateDriverDataSize != 16 && p->PrivateDriverDataSize != sizeof(NPU_SCHEDULER_CONTEXT)) { ExFreePoolWithTag(c, TAG); return STATUS_INVALID_PARAMETER; }
        NPU_SCHEDULER_CONTEXT data = {0};
        __try { RtlCopyMemory(&data, p->pPrivateDriverData, p->PrivateDriverDataSize); }
        __except (EXCEPTION_EXECUTE_HANDLER) { ExFreePoolWithTag(c, TAG); return GetExceptionCode(); }
        if (data.Magic != NPU_SCHEDULER_MAGIC || !data.Event ||
            !((data.Version == 1 && p->PrivateDriverDataSize == 16) || (data.Version == 2 && data.Cookie))) { ExFreePoolWithTag(c, TAG); return STATUS_INVALID_PARAMETER; }
        if (data.Cookie) {
            if (d->Cookie && d->Cookie != data.Cookie) { ExFreePoolWithTag(c, TAG); return STATUS_INVALID_PARAMETER; }
            d->Cookie = data.Cookie;
        }
        NTSTATUS status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)data.Event, EVENT_MODIFY_STATE, *ExEventObjectType, UserMode, (PVOID*)&c->Completion, NULL);
        if (!NT_SUCCESS(status)) { ExFreePoolWithTag(c, TAG); return status; }
    }
    c->Cookie = d->Cookie; p->hContext = c; RtlZeroMemory(&p->ContextInfo, sizeof(p->ContextInfo));
    p->ContextInfo.DmaBufferSize = PAGE_SIZE; p->ContextInfo.DmaBufferPrivateDataSize = sizeof(DMA);
    p->ContextInfo.Caps.DriverManagesResidency = p->Flags.VirtualAddressing;
    // Keep more than VidMm's three-entry minimum (bugcheck 10e/16), and
    // reserve enough entries to reference every mapped model buffer.
    p->ContextInfo.AllocationListSize = NPU_ALLOCATIONS; p->ContextInfo.PatchLocationListSize = 1; return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DestroyContext(HANDLE context) {
    NPU_CONTEXT *c = context; ExWaitForRundownProtectionRelease(&c->Jobs);
    if (c->Completion) ObDereferenceObject(c->Completion); ExFreePoolWithTag(c, TAG); return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY CreateProcess(HANDLE adapter, DXGKARG_CREATEPROCESS *p) {
    if (p->Flags.VirtualMachineProcess) return STATUS_NOT_SUPPORTED;
    PROCESS *process = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*process), TAG);
    if (!process) return STATUS_INSUFFICIENT_RESOURCES;
    process->Adapter = adapter; process->Owner = HandleToUlong(PsGetCurrentProcessId()); p->hKmdProcess = process; return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY DestroyProcess(HANDLE adapter, HANDLE handle) {
    PROCESS *process = handle; if (!process || process->Adapter != adapter) return STATUS_INVALID_HANDLE;
    ExFreePoolWithTag(process, TAG); return STATUS_SUCCESS;
}
static SIZE_T APIENTRY RootSize(HANDLE adapter, DXGKARG_GETROOTPAGETABLESIZE *p) {
    UNREFERENCED_PARAMETER(adapter); p->NumberOfPte = TABLE_ENTRIES; return PAGE_SIZE;
}
static VOID APIENTRY SetRoot(HANDLE adapter, const DXGKARG_SETROOTPAGETABLE *p) {
    NPU_CONTEXT *c = p->hContext;
    if (c && c->Adapter == adapter) { c->Root = p->Address; c->RootEntries = p->NumEntries; }
}
static NTSTATUS APIENTRY Render(HANDLE context, DXGKARG_RENDER *p) {
    NPU_CONTEXT *owner = context;
    if (p->CommandLength != sizeof(NPU_SCHEDULER_COMMAND) || p->DmaSize < p->CommandLength || p->DmaBufferPrivateDataSize < sizeof(DMA) || p->AllocationListSize > NPU_ALLOCATIONS || p->PatchLocationListInSize)
        return STATUS_INVALID_PARAMETER;
    for (UINT i=0; i<p->AllocationListSize; i++) {
        ALLOCATION *allocation = p->pAllocationList[i].hDeviceSpecificAllocation;
        if (!allocation || !allocation->Memory.Id || allocation->Memory.Owner != owner->Owner || allocation->Memory.Cookie != owner->Cookie) return STATUS_ACCESS_DENIED;
    }
    NPU_SCHEDULER_COMMAND command;
    __try { RtlCopyMemory(&command, p->pCommand, sizeof(command)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
    if (command.Magic != NPU_SCHEDULER_MAGIC || command.Version != 1 || !command.Token) return STATUS_INVALID_PARAMETER;
    DMA *dma = p->pDmaBufferPrivateData; RtlZeroMemory(dma, sizeof(*dma)); dma->Operation = 1; dma->Command = command;
    RtlCopyMemory(p->pDmaBuffer, &command, sizeof(command)); p->pDmaBuffer = (PUCHAR)p->pDmaBuffer + sizeof(command); return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Patch(HANDLE adapter, const DXGKARG_PATCH *p) { UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(p); return STATUS_SUCCESS; }
static NTSTATUS APIENTRY Submit(HANDLE context, const DXGKARG_SUBMITCOMMAND *p) {
    ADAPTER *a = context; if (a->Stopping || a->Failed) return STATUS_DEVICE_NOT_READY;
    if (p->EngineOrdinal || p->NodeOrdinal) return STATUS_INVALID_PARAMETER;
    DMA data = {0};
    if (!p->Flags.ContextSwitch) {
        ULONG offset = p->DmaBufferPrivateDataSubmissionStartOffset;
        if (!p->pDmaBufferPrivateData || offset > p->DmaBufferPrivateDataSize || sizeof(DMA) > p->DmaBufferPrivateDataSize - offset) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(&data, (PUCHAR)p->pDmaBufferPrivateData + offset, sizeof(data));
    }
    NPU_CONTEXT *c = p->hContext;
    // The scheduler supplies no driver context for paging packets.
    if (c && c->Adapter != a) return STATUS_INVALID_HANDLE;
    if (((data.Operation == 1 || data.Operation == 5) && (!c || p->Flags.Paging)) ||
        (data.Operation > 1 && data.Operation != 5 && !p->Flags.Paging)) return STATUS_INVALID_PARAMETER;
    if (c && !ExAcquireRundownProtection(&c->Jobs)) return STATUS_DEVICE_REMOVED;
    PLIST_ENTRY entry = ExInterlockedRemoveHeadList(&a->Free, &a->Lock);
    if (!entry) { if (c) ExReleaseRundownProtection(&c->Jobs); return STATUS_INSUFFICIENT_RESOURCES; }
    JOB *job = CONTAINING_RECORD(entry, JOB, Entry); job->Fence = p->SubmissionFenceId; job->Context = c;
    job->Data = data;
    ExInterlockedInsertTailList(&a->Pending, entry, &a->Lock); KeSetEvent(&a->Wake, 0, FALSE); return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY SubmitVirtual(HANDLE adapter, const DXGKARG_SUBMITCOMMANDVIRTUAL *p) {
    NPU_CONTEXT *c = p->hContext;
    DXGKARG_SUBMITCOMMAND submit = {0}; submit.hContext = c;
    submit.SubmissionFenceId = p->SubmissionFenceId; submit.Flags = p->Flags; submit.EngineOrdinal = p->EngineOrdinal; submit.NodeOrdinal = p->NodeOrdinal;
    if (p->Flags.ContextSwitch) return Submit(adapter, &submit);
    if (!c || c->Adapter != adapter || p->Flags.Paging || p->DmaBufferSize != sizeof(NPU_SCHEDULER_COMMAND) ||
        !p->pDmaBufferPrivateData || p->DmaBufferUmdPrivateDataSize != sizeof(NPU_SCHEDULER_COMMAND) ||
        p->DmaBufferPrivateDataSize < sizeof(NPU_SCHEDULER_COMMAND)) return STATUS_INVALID_PARAMETER;
    DMA data = {0}; data.Operation = 5; data.Address = p->DmaBufferVirtualAddress; data.Root = c->Root; data.RootEntries = c->RootEntries;
    RtlCopyMemory(&data.Command, p->pDmaBufferPrivateData, sizeof(data.Command));
    if (data.Command.Magic != NPU_SCHEDULER_MAGIC || data.Command.Version != 1 || !data.Command.Token || !data.RootEntries || data.RootEntries > TABLE_ENTRIES)
        return STATUS_INVALID_PARAMETER;
    submit.pDmaBufferPrivateData = &data; submit.DmaBufferPrivateDataSize = sizeof(data);
    return Submit(adapter, &submit);
}
static NTSTATUS APIENTRY Preempt(HANDLE adapter, const DXGKARG_PREEMPTCOMMAND *p) {
    ADAPTER *a = adapter; if (p->NodeOrdinal || p->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    a->Preemption = p->PreemptionFenceId; InterlockedExchange(&a->Preempt, 1); KeSetEvent(&a->Wake, 0, FALSE); return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Metadata(HANDLE adapter, UINT ordinal, DXGKARG_GETNODEMETADATA *p) {
    UNREFERENCED_PARAMETER(adapter); if (ordinal) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(p, sizeof(*p)); p->EngineType = DXGK_ENGINE_TYPE_OTHER; p->GpuMmuSupported = 1;
    return RtlStringCchCopyW(p->FriendlyName, RTL_NUMBER_OF(p->FriendlyName), L"Hexagon V79 NPU");
}
static NTSTATUS APIENTRY Clock(HANDLE adapter, UINT32 node, UINT32 engine, DXGKARG_CALIBRATEGPUCLOCK *p) {
    UNREFERENCED_PARAMETER(adapter); if (node || engine) return STATUS_INVALID_PARAMETER;
    LARGE_INTEGER frequency, counter = KeQueryPerformanceCounter(&frequency); RtlZeroMemory(p, sizeof(*p));
    // Completion is observed by the FastRPC worker, in the CPU clock domain.
    p->GpuFrequency = frequency.QuadPart; p->GpuClockCounter = p->CpuClockCounter = counter.QuadPart; return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY History(HANDLE adapter, DXGKARG_FORMATHISTORYBUFFER *p) { UNREFERENCED_PARAMETER(adapter); p->NumTimestamps = 0; return STATUS_SUCCESS; }
static NTSTATUS APIENTRY EngineStatus(HANDLE adapter, DXGKARG_QUERYENGINESTATUS *p) {
    ADAPTER *a = adapter; if (p->NodeOrdinal || p->EngineOrdinal) return STATUS_INVALID_PARAMETER;
    p->EngineStatus.Value = 0; p->EngineStatus.Responsive = !a->Failed; return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Group(HANDLE adapter, DXGKARG_QUERYDEPENDENTENGINEGROUP *p) { UNREFERENCED_PARAMETER(adapter); if (p->NodeOrdinal || p->EngineOrdinal) return STATUS_INVALID_PARAMETER; p->DependentNodeOrdinalMask = 1; return STATUS_SUCCESS; }
static NTSTATUS APIENTRY Reset(HANDLE adapter) {
    ADAPTER *a = adapter; NTSTATUS status = NpuSchedulerReset();
    if (NT_SUCCESS(status)) InterlockedExchange(&a->Failed, 0); return status;
}
static NTSTATUS APIENTRY Restart(HANDLE adapter) { ADAPTER *a = adapter; return a->Failed ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS; }
static NTSTATUS APIENTRY ResetEngine(HANDLE adapter, DXGKARG_RESETENGINE *p) { ADAPTER *a = adapter; if (p->NodeOrdinal || p->EngineOrdinal) return STATUS_INVALID_PARAMETER; NTSTATUS status = Reset(a); p->LastAbortedFenceId = a->Completed; return status; }
static NTSTATUS APIENTRY Stable(HANDLE adapter, const DXGKARG_SETSTABLEPOWERSTATE *p) { UNREFERENCED_PARAMETER(p); ADAPTER *a = adapter; return a->Stopping ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS; }
static NTSTATUS APIENTRY VirtualMachine(HANDLE adapter, const DXGKARG_SETVIRTUALMACHINEDATA *p) { UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(p); return STATUS_NOT_SUPPORTED; }
static NTSTATUS APIENTRY Debug(HANDLE adapter, const DXGKARG_COLLECTDBGINFO *p) { UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(p); return STATUS_NOT_SUPPORTED; }
static VOID ReleaseMemoryAllocation(ALLOCATION *allocation) {
    if (allocation->Registered) NpuSchedulerMemoryReference(&allocation->Memory, FALSE);
    ExFreePoolWithTag(allocation, TAG);
}
static NTSTATUS APIENTRY Open(HANDLE device, const DXGKARG_OPENALLOCATION *p) {
    DEVICE *owner = device; UINT created = 0; NTSTATUS status = STATUS_SUCCESS;
    for (; created < p->NumAllocations; created++) {
        DXGK_OPENALLOCATIONINFO *info = &p->pOpenAllocation[created];
        if (info->PrivateDriverDataSize && info->PrivateDriverDataSize != sizeof(NPU_SCHEDULER_MEMORY) &&
            !(info->PrivateDriverDataSize == sizeof(ULONG) && *(const ULONG*)info->pPrivateDriverData == NPU_SCHEDULER_MAGIC)) { status = STATUS_INVALID_PARAMETER; break; }
        ALLOCATION *allocation = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*allocation), TAG);
        if (!allocation) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
        if (info->PrivateDriverDataSize == sizeof(NPU_SCHEDULER_MEMORY)) {
            RtlCopyMemory(&allocation->Memory, info->pPrivateDriverData, sizeof(allocation->Memory));
            if (allocation->Memory.Owner != owner->Owner || allocation->Memory.Cookie != owner->Cookie ||
                !NpuSchedulerMemoryReference(&allocation->Memory, TRUE)) {
                ExFreePoolWithTag(allocation, TAG); status = STATUS_ACCESS_DENIED; break;
            }
            NpuSchedulerMemoryReference(&allocation->Memory, FALSE);
        }
        info->hDeviceSpecificAllocation = allocation;
    }
    if (!NT_SUCCESS(status)) for (UINT i=0; i<created; i++) {
        ReleaseMemoryAllocation(p->pOpenAllocation[i].hDeviceSpecificAllocation); p->pOpenAllocation[i].hDeviceSpecificAllocation = NULL;
    }
    return status;
}
static NTSTATUS APIENTRY Close(HANDLE device, const DXGKARG_CLOSEALLOCATION *p) {
    UNREFERENCED_PARAMETER(device);
    for (UINT i=0; i<p->NumAllocations; i++) ReleaseMemoryAllocation(p->pOpenHandleList[i]);
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Describe(HANDLE adapter, DXGKARG_DESCRIBEALLOCATION *p) {
    UNREFERENCED_PARAMETER(adapter); ALLOCATION *allocation = p->hAllocation;
    if (!allocation) return STATUS_INVALID_HANDLE;
    p->Width = allocation->Memory.Id ? allocation->Memory.Bytes : PAGE_SIZE; p->Height = 1;
    p->Format = D3DDDIFMT_UNKNOWN; p->MultisampleMethod.NumSamples = 1; p->MultisampleMethod.NumQualityLevels = 0;
    p->RefreshRate.Numerator = 0; p->RefreshRate.Denominator = 1; p->PrivateDriverFormatAttribute = 0;
    p->Rotation = D3DDDI_ROTATION_IDENTITY; return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Standard(HANDLE adapter, DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA *p) { UNREFERENCED_PARAMETER(adapter); UNREFERENCED_PARAMETER(p); return STATUS_NOT_SUPPORTED; }
static BOOLEAN MemoryDevice(ADAPTER *adapter, const NPU_SCHEDULER_MEMORY *memory) {
    BOOLEAN found = FALSE; KIRQL old; KeAcquireSpinLock(&adapter->Lock, &old);
    for (PLIST_ENTRY entry=adapter->Devices.Flink; entry!=&adapter->Devices; entry=entry->Flink) {
        DEVICE *device = CONTAINING_RECORD(entry, DEVICE, Entry);
        if (memory->Cookie && device->Owner == memory->Owner && device->Cookie == memory->Cookie) { found = TRUE; break; }
    }
    KeReleaseSpinLock(&adapter->Lock, old); return found;
}
static NTSTATUS APIENTRY Allocate(HANDLE adapter, DXGKARG_CREATEALLOCATION *p) {
    for (UINT i = 0; i < p->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *info = &p->pAllocationInfo[i];
        ALLOCATION *allocation = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*allocation), TAG);
        if (!allocation) return STATUS_INSUFFICIENT_RESOURCES;
        if (info->PrivateDriverDataSize && !(info->PrivateDriverDataSize == sizeof(ULONG) && *(const ULONG*)info->pPrivateDriverData == NPU_SCHEDULER_MAGIC)) {
            if (info->PrivateDriverDataSize != sizeof(allocation->Memory)) { ExFreePoolWithTag(allocation, TAG); return STATUS_INVALID_PARAMETER; }
            RtlCopyMemory(&allocation->Memory, info->pPrivateDriverData, sizeof(allocation->Memory));
            if (allocation->Memory.Owner != HandleToUlong(PsGetCurrentProcessId()) || !MemoryDevice(adapter, &allocation->Memory) || !NpuSchedulerMemoryReference(&allocation->Memory, TRUE)) {
                ExFreePoolWithTag(allocation, TAG); return STATUS_ACCESS_DENIED;
            }
            allocation->Registered = TRUE;
        }
        info->hAllocation = allocation; info->Size = allocation->Memory.Id ? allocation->Memory.Bytes : PAGE_SIZE; info->Alignment = PAGE_SIZE;
        info->PitchAlignedSize = 0; info->HintedBank.Value = 0; info->PhysicalAdapterIndex = 0;
        info->pAllocationUsageHint = NULL; info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        BOOLEAN existing = info->PrivateDriverDataSize != 0;
        info->SupportedReadSegmentSet = info->SupportedWriteSegmentSet = existing ? 4 : 1;
        info->EvictionSegmentSet = 0; info->PreferredSegment.Value = 0; info->PreferredSegment.SegmentId0 = existing ? 3 : 1;
        info->Flags.CpuVisible = info->FlagsWddm2.CpuVisible = 1;
        info->Flags.ExistingSysMem = existing;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY FreeAllocation(HANDLE adapter, const DXGKARG_DESTROYALLOCATION *p) {
    UNREFERENCED_PARAMETER(adapter); for (UINT i = 0; i < p->NumAllocations; i++) {
        ReleaseMemoryAllocation(p->pAllocationList[i]);
    } return STATUS_SUCCESS;
}
static NTSTATUS APIENTRY Paging(HANDLE adapter, DXGKARG_BUILDPAGINGBUFFER *p) {
    if (p->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE) {
        DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE *u = &p->UpdatePageTable;
        if (u->UpdateMode != DXGK_PAGETABLEUPDATE_CPU_VIRTUAL || !u->PageTableAddress.CpuVirtual || !u->pPageTableEntries ||
            u->PageTableLevel >= TABLE_LEVELS || u->StartIndex > TABLE_ENTRIES || u->NumPageTableEntries > TABLE_ENTRIES - u->StartIndex || u->Flags.Use64KBPages)
            return STATUS_INVALID_PARAMETER;
        DXGK_PTE *table = (DXGK_PTE*)u->PageTableAddress.CpuVirtual + u->StartIndex;
        for (UINT i = 0; i < u->NumPageTableEntries; i++) table[i] = u->pPageTableEntries[u->Flags.Repeat ? 0 : i];
        KeMemoryBarrier(); return STATUS_SUCCESS;
    }
    if (p->Operation == DXGK_OPERATION_FLUSH_TLB) { KeMemoryBarrier(); return STATUS_SUCCESS; }
    ADAPTER *a = adapter; if (p->DmaBufferPrivateDataSize < sizeof(DMA)) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    DMA *dma = p->pDmaBufferPrivateData; RtlZeroMemory(dma, sizeof(*dma));
    if (p->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT || p->Operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        ALLOCATION *allocation = p->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT ? p->MapApertureSegment.hAllocation : p->UnmapApertureSegment.hAllocation;
        if (!allocation || !allocation->Memory.Id) return STATUS_INVALID_PARAMETER;
        // Existing system pages already have a fenced FastRPC mapping. The
        // Windows aperture tracks their residency without duplicating storage.
        dma->Operation = 4;
    } else if (p->Operation == DXGK_OPERATION_FILL) {
        ULONGLONG offset = p->Fill.Destination.SegmentAddress.QuadPart, bytes = p->Fill.FillSize;
        if (offset > MEMORY_BYTES || bytes > MEMORY_BYTES - offset || bytes & 3) return STATUS_INVALID_PARAMETER;
        dma->Operation = 3; dma->Destination = a->Memory + offset; dma->Bytes = (ULONG)bytes; dma->Value = p->Fill.FillPattern;
    } else if (p->Operation == DXGK_OPERATION_TRANSFER) {
        SIZE_T bytes = p->Transfer.TransferSize, offset = (SIZE_T)p->Transfer.MdlOffset * PAGE_SIZE;
        if (!bytes || bytes > MEMORY_BYTES) return STATUS_INVALID_PARAMETER;
        if (p->Transfer.Source.SegmentId == 0) {
            PMDL mdl = p->Transfer.Source.pMdl;
            if (!mdl || offset > MmGetMdlByteCount(mdl) || bytes > MmGetMdlByteCount(mdl) - offset) return STATUS_INVALID_PARAMETER;
            PUCHAR map = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
            if (!map) return STATUS_INSUFFICIENT_RESOURCES;
            dma->Source = map + offset;
        } else if (p->Transfer.Source.SegmentId == 1) {
            ULONGLONG address = p->Transfer.Source.SegmentAddress.QuadPart;
            if (address > MEMORY_BYTES || bytes > MEMORY_BYTES - address) return STATUS_INVALID_PARAMETER;
            dma->Source = a->Memory + address;
        } else return STATUS_INVALID_PARAMETER;
        if (p->Transfer.Destination.SegmentId == 0) {
            PMDL mdl = p->Transfer.Destination.pMdl;
            if (!mdl || offset > MmGetMdlByteCount(mdl) || bytes > MmGetMdlByteCount(mdl) - offset) return STATUS_INVALID_PARAMETER;
            PUCHAR map = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
            if (!map) return STATUS_INSUFFICIENT_RESOURCES;
            dma->Destination = map + offset;
        } else if (p->Transfer.Destination.SegmentId == 1) {
            ULONGLONG address = p->Transfer.Destination.SegmentAddress.QuadPart;
            if (address > MEMORY_BYTES || bytes > MEMORY_BYTES - address) return STATUS_INVALID_PARAMETER;
            dma->Destination = a->Memory + address;
        } else return STATUS_INVALID_PARAMETER;
        dma->Operation = 2; dma->Bytes = (ULONG)bytes;
    } else if (p->Operation != DXGK_OPERATION_DISCARD_CONTENT) return STATUS_NOT_SUPPORTED;
    if (p->DmaSize < 4) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    *(ULONG*)p->pDmaBuffer = 0; p->pDmaBuffer = (PUCHAR)p->pDmaBuffer + 4; return STATUS_SUCCESS;
}
DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    DRIVER_INITIALIZATION_DATA d = {0}; d.Version = DXGKDDI_INTERFACE_VERSION;
    d.DxgkDdiAddDevice = Add; d.DxgkDdiStartDevice = Start; d.DxgkDdiStopDevice = Stop; d.DxgkDdiRemoveDevice = Remove;
    d.DxgkDdiUnload = Unload; d.DxgkDdiInterruptRoutine = Interrupt; d.DxgkDdiDpcRoutine = Dpc;
    d.DxgkDdiDispatchIoRequest = Dispatch;
    d.DxgkDdiQueryChildRelations = Children; d.DxgkDdiQueryChildStatus = ChildStatus; d.DxgkDdiQueryDeviceDescriptor = Descriptor;
    d.DxgkDdiSetPowerState = Power; d.DxgkDdiResetDevice = ResetDevice; d.DxgkDdiQueryAdapterInfo = Query;
    d.DxgkDdiCreateDevice = CreateDevice; d.DxgkDdiDestroyDevice = DestroyDevice; d.DxgkDdiCreateContext = CreateContext; d.DxgkDdiDestroyContext = DestroyContext;
    d.DxgkDdiCreateProcess = CreateProcess; d.DxgkDdiDestroyProcess = DestroyProcess;
    d.DxgkDdiGetRootPageTableSize = RootSize; d.DxgkDdiSetRootPageTable = SetRoot; d.DxgkDdiSubmitCommandVirtual = SubmitVirtual;
    d.DxgkDdiRender = Render; d.DxgkDdiPatch = Patch; d.DxgkDdiSubmitCommand = Submit; d.DxgkDdiPreemptCommand = Preempt;
    d.DxgkDdiGetNodeMetadata = Metadata; d.DxgkDdiCalibrateGpuClock = Clock; d.DxgkDdiFormatHistoryBuffer = History;
    d.DxgkDdiQueryEngineStatus = EngineStatus; d.DxgkDdiQueryDependentEngineGroup = Group; d.DxgkDdiResetEngine = ResetEngine;
    d.DxgkDdiResetFromTimeout = Reset; d.DxgkDdiRestartFromTimeout = Restart; d.DxgkDdiSetStablePowerState = Stable;
    d.DxgkDdiSetVirtualMachineData = VirtualMachine; d.DxgkDdiCollectDbgInfo = Debug;
    d.DxgkDdiOpenAllocation = Open; d.DxgkDdiCloseAllocation = Close; d.DxgkDdiDescribeAllocation = Describe; d.DxgkDdiGetStandardAllocationDriverData = Standard;
    d.DxgkDdiCreateAllocation = Allocate; d.DxgkDdiDestroyAllocation = FreeAllocation; d.DxgkDdiBuildPagingBuffer = Paging;
    return DxgkInitialize(driver, path, &d);
}
