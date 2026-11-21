#include <ntddk.h>
#include <wdf.h>
#define IR_ERRORS 0x3300003eu
typedef struct {
    PUCHAR Base, Pin, Data;
    ULONG Depth, Count, Sent;
    NTSTATUS Status;
    WDFINTERRUPT Interrupt;
    WDFDPC Dpc;
    KEVENT Done;
} IR;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(IR, Context);
static ULONG Read(IR *c, ULONG offset) { return READ_REGISTER_ULONG((PULONG)(c->Base + offset)); }
static VOID Write(IR *c, ULONG offset, ULONG value) {
    WRITE_REGISTER_ULONG((PULONG)(c->Base + offset), value);
}
static VOID Fill(IR *c) {
    ULONG used = Read(c, 0x800) & 0x0fffffffu;
    while (used++ < c->Depth && c->Sent < c->Count) {
        ULONG word = 0;
        for (ULONG i = 0; i < 4 && c->Sent < c->Count; i++)
            word |= (ULONG)c->Data[c->Sent++] << (i * 8);
        Write(c, 0x700, word);
    }
    if (c->Sent == c->Count) Write(c, 0x80c, 0);
}
static EVT_WDF_DPC Finish;
static VOID Finish(WDFDPC dpc) {
    KeSetEvent(&Context(WdfDpcGetParentObject(dpc))->Done, IO_NO_INCREMENT, FALSE);
}
static EVT_WDF_INTERRUPT_ISR Receive;
static BOOLEAN Receive(WDFINTERRUPT interrupt, ULONG message) {
    UNREFERENCED_PARAMETER(message);
    IR *c = Context(WdfInterruptGetDevice(interrupt));
    ULONG irq = Read(c, 0x610);
    if (!irq) return FALSE;
    if (c->Data) {
        if (irq & IR_ERRORS) c->Status = STATUS_IO_DEVICE_ERROR;
        else if (irq & (1u << 30)) Fill(c);
        if ((irq & 1u) && !(irq & IR_ERRORS))
            c->Status = c->Sent == c->Count ? STATUS_SUCCESS : STATUS_DEVICE_PROTOCOL_ERROR;
        if ((irq & (IR_ERRORS | 1u)) != 0) {
            Write(c, 0x614, 0);
            Write(c, 0x80c, 0);
            WdfDpcEnqueue(c->Dpc);
        }
    }
    Write(c, 0x618, irq);
    return TRUE;
}
static EVT_WDF_IO_QUEUE_IO_WRITE Send;
static VOID Send(WDFQUEUE queue, WDFREQUEST request, size_t length) {
    IR *c = Context(WdfIoQueueGetDevice(queue));
    PUCHAR data = NULL;
    NTSTATUS status = length && length <= 2048
                          ? WdfRequestRetrieveInputBuffer(request, length, (PVOID *)&data, NULL)
                          : STATUS_INVALID_BUFFER_SIZE;
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }
    WdfInterruptAcquireLock(c->Interrupt);
    if (Read(c, 0x40) & 1u) {
        WdfInterruptReleaseLock(c->Interrupt);
        WdfRequestComplete(request, STATUS_DEVICE_BUSY);
        return;
    }
    KeClearEvent(&c->Done);
    c->Data = data;
    c->Count = (ULONG)length;
    c->Sent = 0;
    c->Status = STATUS_IO_TIMEOUT;
    Write(c, 0x618, 0xffffffffu);
    Write(c, 0x26c, c->Count);
    Write(c, 0x614, IR_ERRORS | (1u << 30) | 1u);
    Write(c, 0x80c, 0);
    Write(c, 0x600, 1u << 27);
    Fill(c);
    if (c->Sent < c->Count) Write(c, 0x80c, 1);
    WdfInterruptReleaseLock(c->Interrupt);
    LARGE_INTEGER timeout;
    timeout.QuadPart = -2500000;
    status = KeWaitForSingleObject(&c->Done, Executive, KernelMode, FALSE, &timeout);
    WdfInterruptAcquireLock(c->Interrupt);
    status = status == STATUS_SUCCESS ? c->Status : STATUS_IO_TIMEOUT;
    Write(c, 0x614, 0);
    Write(c, 0x80c, 0);
    c->Data = NULL;
    if (!NT_SUCCESS(status)) Write(c, 0x604, 2);
    WdfInterruptReleaseLock(c->Interrupt);
    WdfDpcCancel(c->Dpc, TRUE);
    if (!NT_SUCCESS(status)) {
        ULONGLONG end = KeQueryInterruptTime() + 200000;
        while ((Read(c, 0x40) & 1u) && KeQueryInterruptTime() < end) KeStallExecutionProcessor(10);
        Write(c, 0x618, 0xffffffffu);
        Write(c, 0x20, 1);
    }
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? length : 0);
}
static NTSTATUS Release(WDFDEVICE device, WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(resources);
    IR *c = Context(device);
    if (c->Base) {
        MmUnmapIoSpace(c->Base, 0x4000);
        c->Base = NULL;
    }
    if (c->Pin) {
        MmUnmapIoSpace(c->Pin, 0x1000);
        c->Pin = NULL;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS Prepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST resources) {
    UNREFERENCED_PARAMETER(raw);
    IR *c = Context(device);
    for (ULONG i = 0; i < WdfCmResourceListGetCount(resources); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(resources, i);
        if (r->Type != CmResourceTypeMemory) continue;
        PUCHAR *base = NULL;
        if (r->u.Memory.Start.QuadPart == 0x00a94000 && r->u.Memory.Length == 0x4000) base = &c->Base;
        else if (r->u.Memory.Start.QuadPart == 0x0f135000 && r->u.Memory.Length == 0x1000) base = &c->Pin;
        if (!base || *base) return STATUS_DEVICE_CONFIGURATION_ERROR;
        *base = MmMapIoSpaceEx(r->u.Memory.Start, r->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        if (!*base) return STATUS_INSUFFICIENT_RESOURCES;
    }
    return c->Base && c->Pin ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}
static NTSTATUS Enter(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    IR *c = Context(device);
    if (((Read(c, 0x68) >> 8) & 255u) != 1u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    Write(c, 0x2008, Read(c, 0x2008) & ~1u);
    if (Read(c, 0x64) & 1u) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG parameters = Read(c, 0xe24);
    c->Depth = (parameters >> 16) & 255u;
    if (c->Depth < 4 || ((parameters >> 24) & 63u) != 32 || (Read(c, 0x40) & 1u))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Write(c, 0x28, Read(c, 0x28) | 0x7fu);
    Write(c, 0xe30, Read(c, 0xe30) | 15u);
    Write(c, 0x24, 0x7f);
    Write(c, 0x20, 1);
    Write(c, 0x258, 0);
    Write(c, 0xe18, 0);
    Write(c, 0xe1c, 4);
    Write(c, 0x614, 0);
    Write(c, 0x644, 0);
    Write(c, 0x618, 0xffffffffu);
    Write(c, 0x648, 0xffffffffu);
    Write(c, 0x260, 0x7f8fe);
    Write(c, 0x264, 0xffefe);
    Write(c, 0x254, 0);
    Write(c, 0x224, 0);
    Write(c, 0x22c, 0);
    Write(c, 0x230, 0);
    Write(c, 0x24c, 0);
    Write(c, 0x250, 0);
    Write(c, 0x25c, Read(c, 0x25c) & ~2u);
    Write(c, 0x268, 4);
    Write(c, 0x274, 0);
    Write(c, 0x278, 0);
    Write(c, 0x80c, 0);
    Write(c, 0x810, c->Depth - 3);
    Write(c, 0x814, c->Depth - 2);
    Write(c, 0x7c, 0);
    Write(c, 0x48, (168u << 4) | 1u);
    WRITE_REGISTER_ULONG((PULONG)c->Pin, (READ_REGISTER_ULONG((PULONG)c->Pin) & ~0x3ffu) | 0x84u);
    return STATUS_SUCCESS;
}
static NTSTATUS Exit(WDFDEVICE device, WDF_POWER_DEVICE_STATE next) {
    UNREFERENCED_PARAMETER(next);
    IR *c = Context(device);
    Write(c, 0x614, 0);
    Write(c, 0xe1c, 0);
    Write(c, 0x20, 1);
    WRITE_REGISTER_ULONG((PULONG)(c->Pin + 4), 0);
    WRITE_REGISTER_ULONG((PULONG)c->Pin, (READ_REGISTER_ULONG((PULONG)c->Pin) & ~0x3ffu) | 0x200u);
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
    p.EvtDevicePrepareHardware = Prepare;
    p.EvtDeviceReleaseHardware = Release;
    p.EvtDeviceD0Entry = Enter;
    p.EvtDeviceD0Exit = Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &p);
    WDF_OBJECT_ATTRIBUTES a;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, IR);
    a.ExecutionLevel = WdfExecutionLevelPassive;
    a.SynchronizationScope = WdfSynchronizationScopeNone;
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &a, &device);
    if (!NT_SUCCESS(status)) return status;
    IR *c = Context(device);
    KeInitializeEvent(&c->Done, NotificationEvent, FALSE);
    WDF_INTERRUPT_CONFIG interrupt;
    WDF_INTERRUPT_CONFIG_INIT(&interrupt, Receive, NULL);
    interrupt.AutomaticSerialization = FALSE;
    status = WdfInterruptCreate(device, &interrupt, WDF_NO_OBJECT_ATTRIBUTES, &c->Interrupt);
    if (!NT_SUCCESS(status)) return status;
    WDF_DPC_CONFIG dpc;
    WDF_DPC_CONFIG_INIT(&dpc, Finish);
    dpc.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&a);
    a.ParentObject = device;
    status = WdfDpcCreate(&dpc, &a, &c->Dpc);
    if (!NT_SUCCESS(status)) return status;
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG q;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&q, WdfIoQueueDispatchSequential);
    q.EvtIoWrite = Send;
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
