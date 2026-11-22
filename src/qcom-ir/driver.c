#include <ntddk.h>
#include <wdf.h>
#include "../platform/lock.h"
typedef struct {
    PUCHAR Base, Pin, Gpi, Data, Allocation;
    ULONG Address, Tx, Event;
    BOOLEAN Owned[3], Ready, Pinned, Failed;
    PDMA_ADAPTER Dma;
    PHYSICAL_ADDRESS Logical;
    WDFINTERRUPT Interrupt;
    KEVENT Done;
} IR;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IR, Context);
static ULONG Read(PUCHAR base, ULONG offset) { return READ_REGISTER_ULONG((PULONG)(base + offset)); }
static VOID Write(PUCHAR base, ULONG offset, ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(base + offset), value);
}
static NTSTATUS Clock(VOID) {
    PHYSICAL_ADDRESS address;
    address.QuadPart = 0x00118518;
    PUCHAR base = MmMapIoSpaceEx(address, 0xb0, PAGE_READWRITE | PAGE_NOCACHE);
    if (!base) return STATUS_INSUFFICIENT_RESOURCES;
    NTSTATUS status = STATUS_DEVICE_CONFIGURATION_ERROR;
    if ((Read(base, 0x2c) & 0x3fffu) == 0x2601u && (Read(base, 0x6c) & 0xffffu) == 4u &&
        (Read(base, 0xac) & 0xffffu) == 0xfff4u) {
        Write(base, 0x14, Read(base, 0x14) | 1u);
        if (Read(base, 0x14) & 1u) status = STATUS_SUCCESS;
    }
    MmUnmapIoSpace(base, 0xb0);
    return status;
}
static ULONG State(IR *c, ULONG index) { return (Read(c->Gpi, index == 2 ? 0x1000 : index * 0x80) >> 20) & 15u; }
static NTSTATUS Command(IR *c, ULONG index, ULONG opcode, ULONG state) {
    ULONG source = index == 2 ? 0x3094 : 0x3090, bit = index == 2 ? 1 : 1u << index;
    Write(c->Gpi, source + 0x10, bit);
    Write(c->Gpi, index == 2 ? 0x3010 : 0x3008, (opcode << 24) | index);
    LARGE_INTEGER delay;
    delay.QuadPart = -10000;
    for (ULONG i = 0; i < 100; i++) {
        if ((Read(c->Gpi, source) & bit) && (opcode == 10 || State(c, index) == state)) return STATUS_SUCCESS;
        if (State(c, index) == 15) return STATUS_IO_DEVICE_ERROR;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return STATUS_IO_TIMEOUT;
}
static NTSTATUS Stop(IR *c) {
    c->Ready = FALSE;
    Write(c->Gpi, 0x3088, 0);
    LARGE_INTEGER delay;
    delay.QuadPart = -10000;
    for (ULONG command = 4; command >= 2 && (Read(c->Base, 0x40) & 1u); command >>= 1) {
        Write(c->Base, 0x604, command);
        for (ULONG i = 0; i < 100 && (Read(c->Base, 0x40) & 1u); i++)
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    for (ULONG n = 2; n; n--) {
        ULONG i = n - 1;
        if (c->Owned[i] && (State(c, i) == 2 || State(c, i) == 4)) Command(c, i, 2, 3);
    }
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG i = 0; i < 3; i++) {
        if (!c->Owned[i]) continue;
        status = Command(c, i, 9, 1);
        if (NT_SUCCESS(status)) status = Command(c, i, 10, 0);
        if (!NT_SUCCESS(status)) goto pin;
        c->Owned[i] = FALSE;
        ULONG offset = i == 2 ? 0x1000 : i * 0x80;
        Write(c->Gpi, offset + 4, 0);
        Write(c->Gpi, offset + 8, 0);
        Write(c->Gpi, offset + 12, 0);
    }
    Write(c->Gpi, 0x30c0, 1);
    Write(c->Gpi, 0x3120, 0);
    Write(c->Base, 0xe18, 0);
    if ((Read(c->Base, 0x40) & 1u) || (Read(c->Base, 0xe40) & 1u)) {
        status = STATUS_IO_TIMEOUT;
        goto pin;
    }
    c->Pinned = FALSE;
    return STATUS_SUCCESS;
pin:
    c->Pinned = TRUE;
    return status;
}
static BOOLEAN Match(ULONG smr, ULONG stream) {
    return (smr & 0x80000000u) && (((smr ^ stream) & ~(smr >> 16) & 0x7fffu) == 0);
}
static NTSTATUS DmaMapping(VOID) {
    PLATFORM_LOCK_GUARD guard = {0};
    NTSTATUS status = PlatformLockAcquire(&guard, TRUE);
    if (!NT_SUCCESS(status)) return status;
    PHYSICAL_ADDRESS address;
    address.QuadPart = 0x15000000;
    PUCHAR base = MmMapIoSpaceEx(address, 0x1000, PAGE_READONLY | PAGE_NOCACHE);
    if (base) {
        ULONG count = Read(base, 0x20) & 255u, ufs = MAXULONG, gpi = MAXULONG;
        for (ULONG i = 0; i < count; i++) {
            ULONG smr = Read(base, 0x800 + i * 4);
            if (Match(smr, 0x60)) ufs = Read(base, 0xc00 + i * 4);
            if (Match(smr, 0xb6)) gpi = Read(base, 0xc00 + i * 4);
        }
        status = count && ufs != MAXULONG && ufs == gpi && !(gpi & 3u)
                     ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
        MmUnmapIoSpace(base, 0x1000);
    } else status = STATUS_INSUFFICIENT_RESOURCES;
    PlatformLockRelease(&guard);
    return status;
}
static VOID Ring(IR *c, ULONG offset, ULONG address, ULONG config) {
    Write(c->Gpi, offset, config | (Read(c->Gpi, offset) & 0x00f00000u));
    Write(c->Gpi, offset + 4, 256);
    Write(c->Gpi, offset + 8, address);
    Write(c->Gpi, offset + 12, 0);
}
static NTSTATUS Enter(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    IR *c = Context(device);
    if (c->Pinned || c->Owned[0] || c->Owned[1] || c->Owned[2]) return STATUS_DEVICE_BUSY;
    NTSTATUS status = Clock();
    if (!NT_SUCCESS(status)) return status;
    if (((Read(c->Base, 0x68) >> 8) & 255u) != 1u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    Write(c->Base, 0x614, (Read(c->Base, 0x614) | 0x33c0007eu) & ~0x4c000001u);
    status = DmaMapping();
    if (!NT_SUCCESS(status)) return status;
    ULONG enabled = Read(c->Gpi, 0x3000);
    if (enabled == MAXULONG || !(enabled & 1u)) return STATUS_DEVICE_BUSY;
    if ((State(c, 0) && Read(c->Gpi, 8)) || (State(c, 1) && Read(c->Gpi, 0x88)) ||
        (State(c, 2) && Read(c->Gpi, 0x1008))) {
        if ((Read(c->Gpi, 0x60) & 255u) != 0x15 || (Read(c->Gpi, 0xe0) & 255u) != 0x15)
            return STATUS_DEVICE_BUSY;
        for (ULONG i = 0; i < 3; i++) c->Owned[i] = State(c, i) != 0;
        status = Stop(c);
        if (!NT_SUCCESS(status)) return status;
    }
    if (Read(c->Base, 0x40) & 1u) {
        status = Stop(c);
        if (!NT_SUCCESS(status)) return status;
    }
    Write(c->Base, 0x28, Read(c->Base, 0x28) | 0x7fu);
    Write(c->Base, 0xe30, Read(c->Base, 0xe30) | 15u);
    Write(c->Base, 0x24, 0x7f);
    Write(c->Base, 0x20, 1);
    Write(c->Base, 0x644, 0);
    Write(c->Base, 0x618, MAXULONG);
    Write(c->Base, 0x648, MAXULONG);
    Write(c->Base, 0xc44, MAXULONG);
    Write(c->Base, 0xd44, MAXULONG);
    Write(c->Base, 0xe1c, 0);
    Write(c->Base, 0x258, 1);
    Write(c->Base, 0xe18, 15);
    Write(c->Gpi, 0x3088, 0);
    Write(c->Gpi, 0x3098, 3);
    Write(c->Gpi, 0x309c, 1);
    Write(c->Gpi, 0x30b8, 1);
    Write(c->Gpi, 0x3110, Read(c->Gpi, 0x3100));
    Write(c->Gpi, 0x3108, 1);
    Write(c->Gpi, 0x3200, 0);
    Write(c->Gpi, 0x3120, 15);
    Write(c->Gpi, 0x3180, 1);
    Write(c->Gpi, 0x3188, 0);
    Write(c->Gpi, 0x318c, 0);
    c->Owned[2] = TRUE;
    status = Command(c, 2, 0, 1);
    if (!NT_SUCCESS(status)) goto fail;
    Ring(c, 0x1000, c->Address, 0x10010002);
    for (ULONG offset = 0x1020; offset <= 0x1034; offset += 4) Write(c->Gpi, offset, 0);
    Write(c->Gpi, 0x2104, 0);
    Write(c->Gpi, 0x2100, c->Address + 240);
    for (ULONG i = 0; i < 2; i++) {
        c->Owned[i] = TRUE;
        status = Command(c, i, 0, 1);
        if (!NT_SUCCESS(status)) goto fail;
        Ring(c, i * 0x80, c->Address + 256 + i * 256, 0x10000002 | (i == 0 ? 8u : 0));
        Write(c->Gpi, 0x2004 + i * 8, 0);
        Write(c->Gpi, 0x60 + i * 0x80, ((i == 0 ? 1u : 0) << 16) | 0x15u);
        for (ULONG offset = 0x64; offset <= 0x6c; offset += 4) Write(c->Gpi, offset + i * 0x80, 0);
        Write(c->Gpi, 0x5c + i * 0x80, 1);
    }
    for (ULONG i = 0; i < 2; i++) {
        status = Command(c, i, 1, 2);
        if (!NT_SUCCESS(status)) goto fail;
    }
    c->Tx = c->Event = 0;
    c->Ready = TRUE;
    Write(c->Pin, 0, (Read(c->Pin, 0) & ~0x3ffu) | 0x84u);
    return STATUS_SUCCESS;
fail:
    Stop(c);
    return status;
}
static EVT_WDF_INTERRUPT_DPC Finish;
static VOID Finish(WDFINTERRUPT interrupt, WDFOBJECT object) {
    UNREFERENCED_PARAMETER(object);
    KeSetEvent(&Context(WdfInterruptGetDevice(interrupt))->Done, IO_NO_INCREMENT, FALSE);
}
static EVT_WDF_INTERRUPT_ISR Receive;
static BOOLEAN Receive(WDFINTERRUPT interrupt, ULONG message) {
    UNREFERENCED_PARAMETER(message);
    IR *c = Context(WdfInterruptGetDevice(interrupt));
    ULONG irq = Read(c->Gpi, 0x3080);
    if (!(irq & 12u)) return FALSE;
    Write(c->Gpi, 0x3088, 0);
    Write(c->Gpi, 0x30c0, 1);
    if (irq & 4u) {
        c->Failed = TRUE;
        Write(c->Gpi, 0x3110, Read(c->Gpi, 0x3100));
    }
    WdfInterruptQueueDpcForIsr(c->Interrupt);
    return TRUE;
}
static VOID Tre(IR *c, ULONG index, ULONG word0, ULONG word2, ULONG flags) {
    PULONG words = (PULONG)(c->Data + 256 + index * 16);
    words[0] = word0; words[1] = 0; words[2] = word2; words[3] = flags;
}
static NTSTATUS Completion(IR *c, ULONG count, ULONG expected) {
    ULONG pointer = Read(c->Gpi, 0x1010), delta = pointer - c->Address;
    if (delta >= 256 || delta % 16) return STATUS_DEVICE_DATA_ERROR;
    ULONG last = delta / 16;
    NTSTATUS status = STATUS_PENDING;
    while (c->Event != last) {
        KeMemoryBarrier();
        PULONG event = (PULONG)(c->Data + c->Event * 16);
        ULONG code = event[2] >> 24;
        ULONG type = (event[3] >> 16) & 255u;
        if (type != 0x31) {
            status = (event[3] >> 16) == 0x22 && event[0] == expected && event[1] == 0 &&
                (code == 1 || code == 2 || code == 4) && (event[2] & 0xffffffu) == count
                ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
        }
        c->Event = (c->Event + 1) % 16;
        Write(c->Gpi, 0x2100, c->Address + ((c->Event + 15) % 16) * 16);
        if (!NT_SUCCESS(status)) break;
    }
    return status;
}
static EVT_WDF_IO_QUEUE_IO_WRITE Send;
static VOID Send(WDFQUEUE queue, WDFREQUEST request, size_t length) {
    IR *c = Context(WdfIoQueueGetDevice(queue));
    PUCHAR data = NULL;
    NTSTATUS status = length && length <= 2048
        ? WdfRequestRetrieveInputBuffer(request, length, (PVOID *)&data, NULL) : STATUS_INVALID_BUFFER_SIZE;
    if (NT_SUCCESS(status) && !c->Ready) status = STATUS_DEVICE_NOT_READY;
    if (!NT_SUCCESS(status)) { WdfRequestComplete(request, status); return; }
    RtlCopyMemory(c->Data + 2048, data, length);
    ULONG end = (c->Tx + 2) % 16, expected = c->Address + 256 + end * 16;
    Tre(c, c->Tx, 0x03000004, 0x000402bc, 0x00220001);
    Tre(c, (c->Tx + 1) % 16, 1, 0, 0x00200001);
    Tre(c, end, c->Address + 2048, (ULONG)length, 0x00100200);
    c->Failed = FALSE;
    Write(c->Base, 0x26c, (ULONG)length);
    c->Tx = (c->Tx + 3) % 16;
    KeMemoryBarrier();
    WdfInterruptAcquireLock(c->Interrupt);
    Write(c->Gpi, 0x30c0, 1);
    Write(c->Gpi, 0x3088, 12);
    Write(c->Gpi, 0x2000, c->Address + 256 + c->Tx * 16);
    WdfInterruptReleaseLock(c->Interrupt);
    LARGE_INTEGER timeout;
    ULONGLONG deadline = KeQueryInterruptTime() + 2500000;
    for (;;) {
        WdfInterruptAcquireLock(c->Interrupt);
        KeClearEvent(&c->Done);
        status = c->Failed ? STATUS_IO_DEVICE_ERROR : Completion(c, (ULONG)length, expected);
        if (status == STATUS_PENDING) Write(c->Gpi, 0x3088, 12);
        WdfInterruptReleaseLock(c->Interrupt);
        if (status != STATUS_PENDING) break;
        ULONGLONG now = KeQueryInterruptTime();
        if (now >= deadline) { status = STATUS_IO_TIMEOUT; break; }
        timeout.QuadPart = -(LONGLONG)(deadline - now);
        if (KeWaitForSingleObject(&c->Done, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS) {
            status = STATUS_IO_TIMEOUT;
            break;
        }
    }
    WdfInterruptAcquireLock(c->Interrupt);
    Write(c->Gpi, 0x3088, 0);
    WdfInterruptReleaseLock(c->Interrupt);
    if (!NT_SUCCESS(status)) Stop(c);
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? length : 0);
}
static NTSTATUS Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE next) {
    UNREFERENCED_PARAMETER(next);
    IR *c = Context(device);
    NTSTATUS status = Stop(c);
    Write(c->Pin, 4, 0);
    Write(c->Pin, 0, (Read(c->Pin, 0) & ~0x3ffu) | 0x200u);
    return status;
}
static NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);
    IR *c = Context(device);
    PUCHAR *bases[] = {&c->Base, &c->Pin, &c->Gpi};
    ULONG sizes[] = {0x4000, 0x1000, 0x4000};
    for (ULONG i = 0; i < 3; i++) if (*bases[i]) { MmUnmapIoSpace(*bases[i], sizes[i]); *bases[i] = NULL; }
    if (c->Dma && !c->Pinned) {
        if (c->Allocation)
            c->Dma->DmaOperations->FreeCommonBuffer(c->Dma, 8191, c->Logical, c->Allocation, FALSE);
        c->Dma->DmaOperations->PutDmaAdapter(c->Dma);
        c->Dma = NULL;
        c->Allocation = c->Data = NULL;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(raw);
    IR *c = Context(device);
    if (c->Pinned) return STATUS_DEVICE_BUSY;
    PUCHAR *bases[] = {&c->Base, &c->Pin, &c->Gpi};
    ULONG addresses[] = {0x00a94000, 0x0f135000, 0x00a18000}, sizes[] = {0x4000, 0x1000, 0x4000};
    ULONG count = 0;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(resources); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(resources, i);
        if (r->Type != CmResourceTypeMemory) continue;
        if (count >= 3 || r->u.Memory.Start.QuadPart != addresses[count] || r->u.Memory.Length != sizes[count])
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        *bases[count] = MmMapIoSpaceEx(r->u.Memory.Start, sizes[count], PAGE_READWRITE | PAGE_NOCACHE);
        if (!*bases[count++]) return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (count != 3) return STATUS_DEVICE_CONFIGURATION_ERROR;
    DEVICE_DESCRIPTION dma = {0};
    dma.Version = DEVICE_DESCRIPTION_VERSION3;
    dma.Master = dma.ScatterGather = dma.Dma64BitAddresses = TRUE;
    dma.DmaAddressWidth = 32;
    dma.InterfaceType = Internal;
    dma.MaximumLength = 2048;
    ULONG registers;
    c->Dma = IoGetDmaAdapter(WdfDeviceWdmGetPhysicalDevice(device), &dma, &registers);
    if (!c->Dma) return STATUS_INSUFFICIENT_RESOURCES;
    c->Allocation = c->Dma->DmaOperations->AllocateCommonBuffer(c->Dma, 8191, &c->Logical, FALSE);
    if (!c->Allocation) return STATUS_INSUFFICIENT_RESOURCES;
    ULONGLONG address = ((ULONGLONG)c->Logical.QuadPart + 4095) & ~4095ull;
    if (!address || address > MAXULONG - 4095u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    c->Address = (ULONG)address;
    c->Data = c->Allocation + (SIZE_T)(address - c->Logical.QuadPart);
    RtlZeroMemory(c->Data, 4096);
    return STATUS_SUCCESS;
}
static NTSTATUS Add(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    DECLARE_CONST_UNICODE_STRING(name, L"\\Device\\QcomIr");
    DECLARE_CONST_UNICODE_STRING(link, L"\\DosDevices\\QcomIr");
    DECLARE_CONST_UNICODE_STRING(security, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    NTSTATUS status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceInitAssignSDDLString(init, &security);
    if (!NT_SUCCESS(status)) return status;
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    WDF_PNPPOWER_EVENT_CALLBACKS p;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&p);
    p.EvtDevicePrepareHardware = Prepare; p.EvtDeviceReleaseHardware = Release;
    p.EvtDeviceD0Entry = Enter; p.EvtDeviceD0Exit = Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &p);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, IR);
    a.ExecutionLevel = WdfExecutionLevelPassive; a.SynchronizationScope = WdfSynchronizationScopeNone;
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    IR *c = Context(device);
    KeInitializeEvent(&c->Done, NotificationEvent, FALSE);
    WDF_INTERRUPT_CONFIG interrupt;
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, Receive, Finish);
    interrupt.AutomaticSerialization = FALSE;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG q;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q, WdfIoQueueDispatchSequential); q.EvtIoWrite = Send;
    status = WdfIoQueueCreate(device, &q, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) return status;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS idle;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&idle, IdleCannotWakeFromS0);
    idle.IdleTimeout = 1000;
    return WdfDeviceAssignS0IdleSettings(device, &idle);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, Add);
    return WdfDriverCreate(driver, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
