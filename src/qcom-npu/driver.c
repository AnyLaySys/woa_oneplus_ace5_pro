#include "npu.h"
#include <acpiioct.h>

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD NpuAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE NpuPrepare;
EVT_WDF_DEVICE_RELEASE_HARDWARE NpuRelease;
EVT_WDF_DEVICE_D0_ENTRY NpuEnter;
EVT_WDF_DEVICE_D0_EXIT NpuExit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL NpuControl;
EVT_WDF_FILE_CLEANUP NpuFileCleanup;
EVT_WDF_TIMER NpuTimer;

static const GUID NpuInterface = { 0x3287d383, 0x3425, 0x4b2e, { 0x8b, 0x25, 0x9e, 0x41, 0x70, 0xd8, 0x75, 0x00 } };

static NTSTATUS AcpiValue(WDFDEVICE device, const CHAR method[4], PULONG value) {
    ACPI_EVAL_INPUT_BUFFER input = { 0 };
    UCHAR buffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 16] = { 0 };
    ACPI_EVAL_OUTPUT_BUFFER *output = (ACPI_EVAL_OUTPUT_BUFFER *)buffer;
    input.Signature = ACPI_EVAL_INPUT_BUFFER_SIGNATURE;
    RtlCopyMemory(input.MethodName, method, 4);
    WDF_MEMORY_DESCRIPTOR in, out;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&in, &input, sizeof(input));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&out, buffer, sizeof(buffer));
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_SEC(2));
    NTSTATUS status = WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(device), NULL,
        IOCTL_ACPI_EVAL_METHOD, &in, &out, &options, NULL);
    if (!NT_SUCCESS(status)) return status;
    if (output->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || output->Count != 1 ||
        output->Argument[0].Type != ACPI_METHOD_ARGUMENT_INTEGER) return STATUS_DEVICE_PROTOCOL_ERROR;
    *value = output->Argument[0].Argument;
    return STATUS_SUCCESS;
}
static BOOLEAN FirmwareReady(WDFDEVICE device) {
    ULONG value = 0;
    NPU *n = NpuContext(device);
    if (NT_SUCCESS(AcpiValue(device, "QSTA", &value)) && value == 1) {
        n->LastResult = 0;
        return TRUE;
    }
    n->LastResult = NT_SUCCESS(AcpiValue(device, "QERR", &value)) ? value : MAXULONG;
    return FALSE;
}
NTSTATUS NpuPrepare(WDFDEVICE device, WDFCMRESLIST raw, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(raw);
    NPU *n = NpuContext(device);
    NpuMemoryInitialize(n);
    const ULONGLONG addresses[] = { 0x81d00000, 0x01f43000, 0x00406000, 0x15000000 };
    const ULONG sizes[] = { NPU_SMEM_SIZE, PAGE_SIZE, PAGE_SIZE, PAGE_SIZE };
    PUCHAR *maps[] = { &n->Smem, &n->Mutex, &n->Ipcc, &n->Smmu };
    ULONG found = 0;
    for (ULONG i = 0; i < WdfCmResourceListGetCount(translated); i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR r = WdfCmResourceListGetDescriptor(translated, i);
        if (!r || r->Type != CmResourceTypeMemory) continue;
        for (ULONG j = 0; j < RTL_NUMBER_OF(addresses); j++) {
            if ((ULONGLONG)r->u.Memory.Start.QuadPart != addresses[j]) continue;
            if (j == 3 && r->u.Memory.Length == 0x100000) n->SmmuBytes = 0x100000;
            else if (r->u.Memory.Length == sizes[j]) { if (j == 3) n->SmmuBytes = sizes[j]; }
            else return STATUS_DEVICE_CONFIGURATION_ERROR;
            if (*maps[j]) return STATUS_DEVICE_CONFIGURATION_ERROR;
            *maps[j] = MmMapIoSpaceEx(r->u.Memory.Start, j == 3 ? n->SmmuBytes : sizes[j], PAGE_READWRITE | PAGE_NOCACHE);
            if (!*maps[j]) return STATUS_INSUFFICIENT_RESOURCES;
            found++;
        }
    }
    if (found == RTL_NUMBER_OF(addresses)) { NpuSchedulerBind(device); return STATUS_SUCCESS; }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}
NTSTATUS NpuRelease(WDFDEVICE device, WDFCMRESLIST translated) {
    UNREFERENCED_PARAMETER(translated);
    NPU *n = NpuContext(device);
    NpuSchedulerUnbind(device);
    NpuMappingRelease(n);
    // Unconfirmed DSP memory stays pinned across removal; no dangling DMA target.
    if (n->Rpc && !NpuBusy(n)) {
        MmFreeContiguousMemorySpecifyCache(n->Rpc, n->RpcBytes, MmNonCached);
        n->Rpc = NULL;
        n->RpcBytes = 0;
    }
    PUCHAR *maps[] = { &n->Smem, &n->Mutex, &n->Ipcc, &n->Smmu };
    ULONG sizes[] = { NPU_SMEM_SIZE, PAGE_SIZE, PAGE_SIZE, PAGE_SIZE };
    for (ULONG i = 0; i < RTL_NUMBER_OF(maps); i++) {
        if (*maps[i]) { MmUnmapIoSpace(*maps[i], i == 3 ? n->SmmuBytes : sizes[i]); *maps[i] = NULL; }
    }
    return STATUS_SUCCESS;
}
NTSTATUS NpuEnter(WDFDEVICE device, WDF_POWER_DEVICE_STATE previous) {
    UNREFERENCED_PARAMETER(previous);
    NPU *n = NpuContext(device);
    if (NpuBusy(n)) return STATUS_DEVICE_BUSY;
    n->Firmware = FirmwareReady(device);
    n->VersionReady = n->Opening = n->Acked = n->Advertised = n->IntentRequested = FALSE;
    n->Rcid = n->IntentCount = n->PendingCommand = n->State = 0;
    n->In = n->Out = n->Desc = n->Tx = n->Rx = NULL;
    n->Partition = n->PartitionSize = 0;
    n->Closed = FALSE;
    n->Poisoned = FALSE;
    n->LastStatus = n->Firmware ? NpuLinkStart(n) : STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS((NTSTATUS)n->LastStatus) && n->SmmuBytes >= 0x100000) n->LastStatus = NpuMappingAcquire(n);
    if (NT_SUCCESS((NTSTATUS)n->LastStatus)) WdfTimerStart(n->Timer, WDF_REL_TIMEOUT_IN_MS(1));
    // The ACPI node remains installable before its first firmware-enabled boot.
    // Readiness is reported explicitly; unavailable hardware never accepts work.
    return STATUS_SUCCESS;
}
NTSTATUS NpuExit(WDFDEVICE device, WDF_POWER_DEVICE_STATE target) {
    UNREFERENCED_PARAMETER(target);
    NPU *n = NpuContext(device);
    WdfTimerStop(n->Timer, TRUE);
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) NpuDetach(CONTAINING_RECORD(p, NPU_CLIENT, Entry));
    NpuLinkClose(n);
    NpuMappingRelease(n);
    return STATUS_SUCCESS;
}
VOID NpuTimer(WDFTIMER timer) {
    NPU *n = NpuContext((WDFDEVICE)WdfTimerGetParentObject(timer));
    NpuLinkPoll(n);
    NpuClientSweep(n);
    if (n->Online) WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(20));
}
VOID NpuFileCleanup(WDFFILEOBJECT file) {
    NPU_FILE *context = NpuFileContext(file);
    context->Closing = TRUE;
    NPU_CLIENT *client = context->Client; context->Client = NULL;
    if (client) NpuClientClose(client);
}
VOID NpuControl(WDFQUEUE queue, WDFREQUEST request, SIZE_T output, SIZE_T input, ULONG code) {
    NPU *n = NpuContext(WdfIoQueueGetDevice(queue));
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG bytes = 0;
    NPU_CLIENT *session = NULL;
    if (code != QCOM_NPU_QUERY && code != QCOM_NPU_QUERY_MEMORY) {
        session = NpuClientGet(n, request, &status);
        if (!session) { WdfRequestComplete(request, status); return; }
    }
    if (code == QCOM_NPU_QUERY) {
        if (input) status = STATUS_INVALID_PARAMETER;
        else {
            QCOM_NPU_STATUS *s;
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*s), (PVOID *)&s, NULL);
            if (NT_SUCCESS(status)) {
                NpuLinkPoll(n);
                BOOLEAN attached = FALSE, user = FALSE;
                for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
                    NPU_CLIENT *client = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
                    attached |= client->Attached; user |= client->Attached && client->Pd == 1;
                }
                ULONG flags = (n->Firmware ? QCOM_NPU_FIRMWARE : 0) |
                    ((n->State & 2u) && !(n->State & 1u) ? QCOM_NPU_DSP_READY : 0) |
                    (n->Online && n->Acked && n->Rcid && !n->Closed ? QCOM_NPU_LINK_READY : 0) |
                    (n->MappingReady ? QCOM_NPU_DMA_READY : 0) |
                    (attached ? QCOM_NPU_ATTACHED : 0) | (NpuBusy(n) && !attached ? QCOM_NPU_DMA_PENDING : (n->Running || n->Pending ? QCOM_NPU_DMA_PENDING : 0)) |
                    (user ? QCOM_NPU_USER_PROCESS : 0) |
                    (n->PagedMemory ? QCOM_NPU_PAGED_MEMORY : 0);
                *s = (QCOM_NPU_STATUS){ QCOM_NPU_ABI, QCOM_NPU_VERSION, flags, n->State,
                    (ULONG)n->LastStatus, n->LastResult, n->Requests, n->Responses, NpuSocId(n) };
                bytes = sizeof(*s);
            }
        }
    } else if (code == QCOM_NPU_QUERY_MEMORY) {
        QCOM_NPU_MEMORY_STATUS *s = NULL;
        if (input) status = STATUS_INVALID_PARAMETER;
        else {
            status = WdfRequestRetrieveOutputBuffer(request, sizeof(*s), (PVOID *)&s, NULL);
            if (NT_SUCCESS(status)) { NpuMemoryStatus(n, s); bytes = sizeof(*s); }
        }
    } else if (code == QCOM_NPU_ATTACH) {
        status = input || output ? STATUS_INVALID_PARAMETER : NpuAttach(session, request);
    } else if (code == QCOM_NPU_CREATE) {
        if (output || input > QCOM_NPU_MAX_DATA + FIELD_OFFSET(QCOM_NPU_PROCESS, Shell)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            QCOM_NPU_PROCESS *p;
            status = WdfRequestRetrieveInputBuffer(request, FIELD_OFFSET(QCOM_NPU_PROCESS, Shell), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) status = NpuCreateProcess(session, p, (ULONG)input, request);
        }
    } else if (code == QCOM_NPU_INVOKE) {
        if (input > QCOM_NPU_MAX_DATA + sizeof(QCOM_NPU_PACKET) || output > QCOM_NPU_MAX_DATA + sizeof(QCOM_NPU_PACKET))
            status = STATUS_INVALID_BUFFER_SIZE;
        else {
            QCOM_NPU_PACKET *p;
            status = WdfRequestRetrieveInputBuffer(request, FIELD_OFFSET(QCOM_NPU_PACKET, Data), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) status = NpuInvoke(session, p, (ULONG)input, (ULONG)output, request, &bytes, FALSE);
        }
    } else if (code == QCOM_NPU_SUBMIT) {
        QCOM_NPU_PACKET *p;
        QCOM_NPU_TICKET *ticket;
        if (input > QCOM_NPU_MAX_DATA + FIELD_OFFSET(QCOM_NPU_PACKET, Data)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, FIELD_OFFSET(QCOM_NPU_PACKET, Data), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) {
                status = WdfRequestRetrieveOutputBuffer(request, sizeof(*ticket), (PVOID *)&ticket, NULL);
                if (NT_SUCCESS(status)) {
                    // METHOD_BUFFERED shares input/output storage; submission consumes
                    // the input before writing the ticket over its header.
                    QCOM_NPU_TICKET value;
                    status = NpuSubmit(session, p, (ULONG)input, request, &value);
                    if (NT_SUCCESS(status)) { *ticket = value; bytes = sizeof(value); }
                }
            }
        }
    } else if (code == QCOM_NPU_COLLECT) {
        QCOM_NPU_TICKET *ticket;
        QCOM_NPU_PACKET *p;
        if (input != sizeof(*ticket) || output > QCOM_NPU_MAX_DATA + FIELD_OFFSET(QCOM_NPU_PACKET, Data)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*ticket), (PVOID *)&ticket, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_TICKET value = *ticket;
                status = WdfRequestRetrieveOutputBuffer(request, FIELD_OFFSET(QCOM_NPU_PACKET, Data), (PVOID *)&p, NULL);
                if (NT_SUCCESS(status)) status = NpuCollect(session, &value, p, (ULONG)output, &bytes);
            }
        }
    } else if (code == QCOM_NPU_SHARED_ALLOCATE) {
        QCOM_NPU_SHARED_ALLOCATION *p;
        if (input != sizeof(*p) || output != sizeof(*p)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*p), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_SHARED_ALLOCATION value = *p;
                status = WdfRequestRetrieveOutputBuffer(request, sizeof(*p), (PVOID *)&p, NULL);
                if (NT_SUCCESS(status)) {
                    status = NpuAllocateShared(session, &value, request);
                    if (NT_SUCCESS(status)) { *p = value; bytes = sizeof(value); }
                }
            }
        }
    } else if (code == QCOM_NPU_MAP_SHARED) {
        QCOM_NPU_SHARED_MAPPING *p;
        if (input != sizeof(*p) || output) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*p), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) status = NpuMapShared(session, p, request);
        }
    } else if (code == QCOM_NPU_DISPATCH_QUEUE) {
        QCOM_NPU_QUEUE_DISPATCH *p; QCOM_NPU_TICKET *ticket;
        if (input != sizeof(*p) || output != sizeof(*ticket)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, input, (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_TICKET value;
                status = NpuQueueDispatch(session, p, request, &value);
                if (NT_SUCCESS(status)) {
                    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*ticket), (PVOID *)&ticket, NULL);
                    if (NT_SUCCESS(status)) { *ticket = value; bytes = sizeof(value); }
                }
            }
        }
    } else if (code == QCOM_NPU_DISPATCH_RESULT) {
        QCOM_NPU_TICKET *ticket;
        if (input != sizeof(*ticket) || output) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*ticket), (PVOID *)&ticket, NULL);
            if (NT_SUCCESS(status)) status = NpuDispatchResult(session, ticket);
        }
    } else if (code == QCOM_NPU_ALLOCATE || code == QCOM_NPU_FREE) {
        QCOM_NPU_ALLOCATION *a;
        if (input != sizeof(*a) || (code == QCOM_NPU_FREE && output)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*a), (PVOID *)&a, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_ALLOCATION value = *a;
                if (code == QCOM_NPU_FREE) status = NpuFree(session, &value, request);
                else {
                    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*a), (PVOID *)&a, NULL);
                    if (NT_SUCCESS(status)) {
                        status = NpuAllocate(session, &value, request);
                        if (NT_SUCCESS(status)) { *a = value; bytes = sizeof(value); }
                    }
                }
            }
        }
    } else if (code == QCOM_NPU_WRITE_MEMORY) {
        QCOM_NPU_MEMORY_WRITE *p;
        if (output || input > 0x10000 + FIELD_OFFSET(QCOM_NPU_MEMORY_WRITE, Data)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, FIELD_OFFSET(QCOM_NPU_MEMORY_WRITE, Data), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) status = NpuWriteMemory(session, p, (ULONG)input);
        }
    }
    if (code == QCOM_NPU_QUEUE_EXECUTION) {
        QCOM_NPU_PACKET *p; QCOM_NPU_TICKET *ticket;
        if (input != 236 || output != sizeof(*ticket)) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, input, (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_TICKET value;
                status = NpuQueueExecution(session, p, (ULONG)input, request, &value);
                if (NT_SUCCESS(status)) {
                    status = WdfRequestRetrieveOutputBuffer(request, sizeof(*ticket), (PVOID *)&ticket, NULL);
                    if (NT_SUCCESS(status)) { *ticket = value; bytes = sizeof(value); }
                }
            }
        }
    } else if (code == QCOM_NPU_EXECUTION_RESULT) {
        QCOM_NPU_TICKET *ticket; QCOM_NPU_PACKET *p;
        if (input != sizeof(*ticket) || output != 236) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, input, (PVOID *)&ticket, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_TICKET value = *ticket;
                status = WdfRequestRetrieveOutputBuffer(request, output, (PVOID *)&p, NULL);
                if (NT_SUCCESS(status)) status = NpuExecutionResult(session, &value, p, (ULONG)output, &bytes);
            }
        }
    } else if (code == QCOM_NPU_READ_MEMORY) {
        QCOM_NPU_MEMORY_READ *p;
        if (input != sizeof(*p) || !output || output > 0x10000) status = STATUS_INVALID_BUFFER_SIZE;
        else {
            status = WdfRequestRetrieveInputBuffer(request, sizeof(*p), (PVOID *)&p, NULL);
            if (NT_SUCCESS(status)) {
                QCOM_NPU_MEMORY_READ value = *p; PUCHAR target;
                status = WdfRequestRetrieveOutputBuffer(request, output, (PVOID *)&target, NULL);
                if (NT_SUCCESS(status)) {
                    status = NpuReadMemory(session, &value, target, (ULONG)output);
                    if (NT_SUCCESS(status)) bytes = value.Bytes;
                }
            }
        }
    }
    if (code != QCOM_NPU_QUERY && code != QCOM_NPU_QUERY_MEMORY && !(code == QCOM_NPU_COLLECT && status == STATUS_DEVICE_BUSY)) n->LastStatus = status;
    WdfRequestCompleteWithInformation(request, status, NT_SUCCESS(status) ? bytes : 0);
}
NTSTATUS NpuAdd(WDFDRIVER driver, PWDFDEVICE_INIT init) {
    UNREFERENCED_PARAMETER(driver);
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = NpuPrepare;
    power.EvtDeviceReleaseHardware = NpuRelease;
    power.EvtDeviceD0Entry = NpuEnter;
    power.EvtDeviceD0Exit = NpuExit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);
    WdfDeviceInitSetExclusive(init, FALSE);
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\QcomNpu");
    NTSTATUS status = WdfDeviceInitAssignName(init, &name);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING security = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");
    status = WdfDeviceInitAssignSDDLString(init, &security);
    if (!NT_SUCCESS(status)) return status;
    WDF_FILEOBJECT_CONFIG file;
    WDF_FILEOBJECT_CONFIG_INIT(&file, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, NpuFileCleanup);
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, NPU_FILE);
    WdfDeviceInitSetFileObjectConfig(init, &file, &fileAttributes);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, NPU);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    attributes.SynchronizationScope = WdfSynchronizationScopeDevice;
    WDFDEVICE device;
    status = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(status)) return status;
    NPU *n = NpuContext(device);
    n->Sequence = KeQueryInterruptTime();
    InitializeListHead(&n->Clients);
    status = WdfDeviceCreateDeviceInterface(device, &NpuInterface, NULL);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\QcomNpu");
    status = WdfDeviceCreateSymbolicLink(device, &link);
    if (!NT_SUCCESS(status)) return status;
    WDF_IO_QUEUE_CONFIG config;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&config, WdfIoQueueDispatchSequential);
    config.EvtIoDeviceControl = NpuControl;
    status = WdfIoQueueCreate(device, &config, WDF_NO_OBJECT_ATTRIBUTES, NULL);
    if (!NT_SUCCESS(status)) return status;
    WDF_TIMER_CONFIG timer;
    WDF_TIMER_CONFIG_INIT(&timer, NpuTimer);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfTimerCreate(&timer, &attributes, &n->Timer);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING path) {
    NpuSchedulerInitialize();
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, NpuAdd);
    return WdfDriverCreate(object, path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
