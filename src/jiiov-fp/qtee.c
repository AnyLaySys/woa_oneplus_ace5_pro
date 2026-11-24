#include "fingerprint.h"

#define JFP_TEE_PAGE 4096u
#define JFP_TEE_DATA_CAPACITY 65536u
#define JFP_TEE_STORAGE_CAPACITY 131072u
#define JFP_TEE_FILE_CAPACITY 32768u
#define JFP_TEE_INVOKE 0x72000602u
#define JFP_TEE_CALLBACK 0x72000601u
#define JFP_TEE_BRIDGE_CREATE 0x42000c1eu
#define JFP_TEE_BRIDGE_DELETE 0x42000c1du
#define JFP_TEE_UNAVAILABLE (-96)

typedef union {
    struct { ULONG Offset; ULONG Size; } Buffer;
    ULONG Object;
} JFP_TEE_ARG;

typedef struct {
    ULONG Context;
    ULONG Operation;
    ULONG Counts;
    JFP_TEE_ARG Args[10];
} JFP_TEE_MESSAGE;

typedef struct {
    LONG Result;
    ULONG Context;
    ULONG Operation;
    ULONG Counts;
    JFP_TEE_ARG Args[10];
} JFP_TEE_CALLBACK_MESSAGE;

static NTSTATUS ScmStatus(LONG status) {
    switch (status) {
    case 0: return STATUS_SUCCESS;
    case -2:
    case -3: return STATUS_INVALID_PARAMETER;
    case -4: return STATUS_NOT_SUPPORTED;
    case -5: return STATUS_INSUFFICIENT_RESOURCES;
    case -12: return STATUS_DEVICE_BUSY;
    case -1: return STATUS_IO_DEVICE_ERROR;
    default: return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS Call(JFP_TEE *tee, ULONG id, ULONG arginfo, ULONGLONG a,
    ULONGLONG b, ULONGLONG c, ULONGLONG d, ULONGLONG *result) {
    ULONGLONG registers[8] = {id, arginfo, a, b, c, d};
    if (id == 1) {
        ULONGLONG cookie = tee->ResumeReturn[6];
        RtlCopyMemory(registers, tee->ResumeArgs, sizeof(registers));
        registers[0] = 1;
        registers[6] = cookie;
        id = (ULONG)tee->ResumeArgs[0];
    } else RtlCopyMemory(tee->ResumeArgs, registers, sizeof(registers));
    ULONGLONG deadline = KeQueryInterruptTime() + 20000000;
    do {
        NTSTATUS written = JfpQteeTrace(tee, 1, registers, STATUS_SUCCESS);
        if (!NT_SUCCESS(written)) return written;
        KeMemoryBarrier();
        JfpScmCall(registers);
        KeMemoryBarrier();
        tee->Pending = registers[0] == 1 || registers[0] == 2 || registers[0] == 3 ||
            (!registers[0] && registers[1] == 3 && (id == JFP_TEE_INVOKE || id == JFP_TEE_CALLBACK));
        RtlCopyMemory(tee->ResumeReturn, registers, sizeof(registers));
        RtlCopyMemory(tee->Info.ScmRegisters, registers, sizeof(registers));
        JfpQteeTrace(tee, 2, registers, (NTSTATUS)registers[0]);
        if (registers[0] != 1) break;
        ULONGLONG cookie = registers[6];
        RtlCopyMemory(tee->ResumeReturn, registers, sizeof(registers));
        if (KeQueryInterruptTime() >= deadline) break;
        LARGE_INTEGER delay;
        delay.QuadPart = -10000;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        RtlCopyMemory(registers, tee->ResumeArgs, sizeof(registers));
        registers[0] = 1;
        registers[6] = cookie;
    } while (TRUE);
    RtlCopyMemory(tee->ResumeReturn, registers, sizeof(registers));
    RtlCopyMemory(tee->Info.ScmRegisters, registers, sizeof(registers));
    tee->Info.Calls++;
    tee->Info.CallId = id;
    tee->Info.ScmStatus = (LONG)registers[0];
    RtlCopyMemory(tee->Info.Return, registers, sizeof(tee->Info.Return));
    if (result) RtlCopyMemory(result, registers, 4 * sizeof(*result));
    if (registers[0] == 1 || registers[0] == 2 || registers[0] == 3) {
        tee->Pending = TRUE;
        return STATUS_IO_TIMEOUT;
    }
    return ScmStatus((LONG)registers[0]);
}

static BOOLEAN SecureObject(ULONG object) {
    return object > 1 && !(object & 0x80000000u);
}

static ULONG Size(JFP_TEE *tee) {
    return tee->MessageSize + JFP_TEE_PAGE;
}

static ULONG AddObject(JFP_TEE *tee, ULONG kind) {
    for (ULONG i = 0; i < RTL_NUMBER_OF(tee->Objects); i++) {
        if (!tee->Objects[i]) {
            if (tee->NextObject == 0x00ffffffu) return 0;
            tee->Objects[i] = 0x8a000000u | ++tee->NextObject;
            tee->ObjectKinds[i] = kind;
            tee->Info.LiveObjects++;
            return tee->Objects[i];
        }
    }
    return 0;
}

static ULONG FindObject(JFP_TEE *tee, ULONG object) {
    if (!object) return FALSE;
    for (ULONG i = 0; i < RTL_NUMBER_OF(tee->Objects); i++)
        if (tee->Objects[i] == object) return tee->ObjectKinds[i];
    return FALSE;
}

static VOID DropObject(JFP_TEE *tee, ULONG object) {
    for (ULONG i = 0; i < RTL_NUMBER_OF(tee->Objects); i++) {
        if (tee->Objects[i] == object && object) {
            tee->Objects[i] = 0;
            tee->ObjectKinds[i] = 0;
            tee->Info.LiveObjects--;
            tee->Info.Releases++;
            break;
        }
    }
}

static NTSTATUS Async(JFP_TEE *tee, ULONG offset) {
    PUCHAR buffer = tee->Memory + tee->MessageSize;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG start = offset;
    while (offset <= JFP_TEE_PAGE - 8) {
        ULONG header[3];
        RtlCopyMemory(header, buffer + offset, 8);
        if (!header[0]) break;
        if ((header[0] >> 16) != 1 || header[1] != 0xffff || offset > JFP_TEE_PAGE - 12) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
            break;
        }
        RtlCopyMemory(&header[2], buffer + offset + 8, 4);
        if (header[2] > (JFP_TEE_PAGE - offset - 12) / 4) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
            break;
        }
        for (ULONG i = 0; i < header[2]; i++) {
            ULONG object;
            RtlCopyMemory(&object, buffer + offset + 12 + i * 4, 4);
            DropObject(tee, object);
        }
        offset += (12 + header[2] * 4 + 7) & ~7u;
    }
    RtlZeroMemory(buffer + start, JFP_TEE_PAGE - start);
    return status;
}

static NTSTATUS Callback(JFP_TEE *tee) {
    JFP_TEE_CALLBACK_MESSAGE *message = (JFP_TEE_CALLBACK_MESSAGE *)(tee->Memory + tee->MessageSize);
    tee->Info.Callbacks++;
    tee->Info.CallbackContext = message->Context;
    tee->Info.CallbackOperation = message->Operation;
    tee->Info.CallbackCounts = message->Counts;
    LONG result = JFP_TEE_UNAVAILABLE;
    ULONG counts = message->Counts;
    ULONG buffers = (counts & 15u) + ((counts >> 4) & 15u);
    ULONG args = buffers + ((counts >> 8) & 15u) + ((counts >> 12) & 15u);
    ULONG tail = (16 + args * 8 + 7) & ~7u;
    BOOLEAN valid = !(counts & 0xffff0000u) && args <= 10;
    for (ULONG i = 0; valid && i < buffers; i++) {
        ULONG offset = message->Args[i].Buffer.Offset;
        ULONG size = message->Args[i].Buffer.Size;
        if (offset != tail || tail > JFP_TEE_PAGE || size > JFP_TEE_PAGE - tail) valid = FALSE;
        else tail += (size + 7) & ~7u;
    }
    NTSTATUS status = STATUS_SUCCESS;
    if (valid && tail <= JFP_TEE_PAGE) status = Async(tee, tail);
    else status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (NT_SUCCESS(status) && message->Context == 0x80000000u) {
        if (message->Operation == 0 && counts == 0x1110 &&
            message->Args[0].Buffer.Size >= 20 && tee->DataSize &&
            FindObject(tee, message->Args[1].Object) && FindObject(tee, message->Args[1].Object) != 4 &&
            FindObject(tee, message->Args[1].Object) != 6) {
            ULONG kind = FindObject(tee, message->Args[1].Object);
            ULONG object = AddObject(tee, kind);
            if (object) {
                PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(tee->Memory +
                    (kind == 2 ? tee->ImageOffset : kind == 3 ? tee->StorageOffset :
                        kind == 5 ? tee->FileOffset : tee->DataOffset));
                ULONGLONG size = kind == 2 ? tee->ImageCapacity : kind == 3 ? tee->StorageSize :
                    kind == 5 ? tee->FileSize : tee->DataSize;
                ULONG permissions = 6;
                PUCHAR mapping = (PUCHAR)message + message->Args[0].Buffer.Offset;
                RtlCopyMemory(mapping, &physical.QuadPart, 8);
                RtlCopyMemory(mapping + 8, &size, 8);
                RtlCopyMemory(mapping + 16, &permissions, 4);
                message->Args[0].Buffer.Size = 20;
                message->Args[2].Object = object;
                tee->Info.Maps++;
                result = 0;
            }
        } else if (message->Operation == 1 && !counts) {
            LARGE_INTEGER delay;
            delay.QuadPart = -10000;
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
            result = 0;
        } else if (message->Operation == 2 && counts == 1 && message->Args[0].Buffer.Size == 4) {
            ULONG milliseconds;
            RtlCopyMemory(&milliseconds, (PUCHAR)message + message->Args[0].Buffer.Offset, 4);
            if (milliseconds <= 100) {
                LARGE_INTEGER delay;
                delay.QuadPart = -(LONGLONG)milliseconds * 10000;
                KeDelayExecutionThread(KernelMode, FALSE, &delay);
                result = 0;
            }
        }
    } else if (NT_SUCCESS(status) && ((message->Context == tee->StorageObject && tee->StorageObject) ||
        (message->Context == tee->FileObject && tee->FileObject))) {
        if (message->Operation == 0 && counts == 0x4020 && message->Args[1].Buffer.Size == 4) {
            status = JfpQteeStorage(tee, message->Context == tee->FileObject);
            if (NT_SUCCESS(status)) {
                message->Args[0].Buffer.Size &= ~3u;
                RtlZeroMemory((PUCHAR)message + message->Args[1].Buffer.Offset, 4);
                for (ULONG i = 2; i < 6; i++) message->Args[i].Object = 0;
                result = 0;
            }
        } else if (message->Operation == 1 && !counts) result = 0;
    }
    message->Result = result;
    return status;
}

static NTSTATUS Execute(JFP_TEE *tee) {
    PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(tee->Memory);
    ULONGLONG registers[4];
    NTSTATUS status;
    if (tee->Pending) {
        if (tee->ResumeReturn[0] == 1) status = Call(tee, 1, 0, 0, 0, 0, 0, registers);
        else if (!tee->ResumeReturn[0] && tee->ResumeReturn[1] == 3) {
            RtlCopyMemory(registers, tee->ResumeReturn, sizeof(registers));
            status = STATUS_SUCCESS;
        } else return STATUS_NOT_SUPPORTED;
        if (NT_SUCCESS(status)) tee->Pending = registers[1] == 3;
    } else status = Call(tee, JFP_TEE_INVOKE, 0x224, physical.QuadPart, tee->MessageSize,
        physical.QuadPart + tee->MessageSize, JFP_TEE_PAGE, registers);
    for (ULONG i = 0; NT_SUCCESS(status) && registers[1] == 3; i++) {
        tee->Pending = TRUE;
        NTSTATUS callback = Callback(tee);
        if (!NT_SUCCESS(callback)) {
            LONG unavailable = JFP_TEE_UNAVAILABLE;
            RtlCopyMemory(tee->Memory + tee->MessageSize, &unavailable, 4);
        }
        status = Call(tee, JFP_TEE_CALLBACK, 0x22, physical.QuadPart + tee->MessageSize,
            JFP_TEE_PAGE, 0, 0, registers);
        if (NT_SUCCESS(status)) tee->Pending = registers[1] == 3;
        if (i >= 255 && tee->Pending) {
            status = STATUS_IO_TIMEOUT;
            break;
        }
    }
    if (!NT_SUCCESS(status)) return status;
    NTSTATUS asynchronous = Async(tee, 0);
    tee->Info.ResponseType = (ULONG)registers[1];
    tee->Info.ObjectStatus = (LONG)registers[2];
    if (registers[1] != 0 && registers[1] != 1) return STATUS_DEVICE_PROTOCOL_ERROR;
    if (!NT_SUCCESS(asynchronous)) return asynchronous;
    if (registers[2]) return STATUS_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

static NTSTATUS Invoke(JFP_TEE *tee, ULONG object, ULONG operation, ULONG counts,
    BOOLEAN input, ULONG value, ULONG *output) {
    if (!tee->Memory || !tee->Shared || tee->Pending ||
        (object != 1 && !SecureObject(object)) ||
        (counts != 0 && counts != 1 && counts != 0x1100 && counts != 0x1001)) return STATUS_INVALID_DEVICE_STATE;
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    RtlZeroMemory(tee->Memory, tee->DataSize ? tee->DataOffset : Size(tee));
    RtlZeroMemory(tee->Memory + tee->MessageSize, JFP_TEE_PAGE);
    message->Context = object;
    message->Operation = operation;
    message->Counts = counts;
    if (input) {
        message->Args[0].Buffer.Offset = 32;
        message->Args[0].Buffer.Size = sizeof(value);
        RtlCopyMemory(tee->Memory + 32, &value, sizeof(value));
    }
    NTSTATUS status = Execute(tee);
    if (!NT_SUCCESS(status)) return status;
    if (output) {
        ULONG handle = message->Args[1].Object;
        if (!SecureObject(handle) || handle == object || handle == tee->Client)
            return STATUS_DEVICE_PROTOCOL_ERROR;
        *output = handle;
    }
    if (message->Context != object || message->Operation != operation || message->Counts != counts ||
        (input && (message->Args[0].Buffer.Offset != 32 || message->Args[0].Buffer.Size != sizeof(value))))
        return STATUS_DEVICE_PROTOCOL_ERROR;
    return STATUS_SUCCESS;
}

NTSTATUS JfpQteeRelease(JFP_DEVICE *device) {
    JFP_TEE *tee = &device->Tee;
    NTSTATUS status = STATUS_SUCCESS;
    if (tee->Pending) status = STATUS_IO_TIMEOUT;
    if (NT_SUCCESS(status) && tee->App) {
        status = Invoke(tee, tee->App, 0xffff, 0, FALSE, 0, NULL);
        if (NT_SUCCESS(status)) tee->App = 0;
    }
    if (NT_SUCCESS(status) && tee->Region) {
        status = Invoke(tee, tee->Region, 0xffff, 0, FALSE, 0, NULL);
        if (NT_SUCCESS(status)) tee->Region = 0;
    }
    if (NT_SUCCESS(status) && tee->StorageService) {
        status = Invoke(tee, tee->StorageService, 0xffff, 0, FALSE, 0, NULL);
        if (NT_SUCCESS(status)) tee->StorageService = 0;
    }
    if (NT_SUCCESS(status) && tee->Service) {
        status = Invoke(tee, tee->Service, 0xffff, 0, FALSE, 0, NULL);
        if (NT_SUCCESS(status)) tee->Service = 0;
    }
    if (NT_SUCCESS(status) && tee->Client) {
        status = Invoke(tee, tee->Client, 0xffff, 0, FALSE, 0, NULL);
        if (NT_SUCCESS(status)) tee->Client = 0;
    }
    if (NT_SUCCESS(status) && tee->Info.LiveObjects) status = STATUS_DEVICE_BUSY;
    if (NT_SUCCESS(status) && tee->Shared) {
        status = Call(tee, JFP_TEE_BRIDGE_DELETE, 1, tee->Bridge, 0, 0, 0, NULL);
        if (NT_SUCCESS(status)) tee->Shared = FALSE;
    }
    if (NT_SUCCESS(status) && tee->Memory) {
        JfpQteeStorageClose(tee);
        RtlZeroMemory(tee->Memory, Size(tee));
        MmFreeContiguousMemory(tee->Memory);
        tee->Memory = NULL;
        tee->DataOffset = tee->DataSize = 0;
        tee->StorageOffset = tee->StorageSize = tee->StorageObject = tee->StorageMemory = 0;
        tee->FileOffset = tee->FileSize = tee->FileObject = tee->FileMemory = 0;
    }
    tee->Info.Pinned = tee->Memory != NULL;
    tee->Info.CleanupStatus = status;
    return status;
}

static NTSTATUS Connect(JFP_DEVICE *device, ULONG uid, ULONG messageSize) {
    if (uid != 16 && uid != 122 && uid != 2033) return STATUS_INVALID_PARAMETER;
    JFP_TEE *tee = &device->Tee;
    if (tee->Memory || tee->Pending) return STATUS_INVALID_DEVICE_STATE;
    tee->AuditEnabled = TRUE;
    tee->MessageSize = messageSize;
    RtlZeroMemory(&tee->Info, sizeof(tee->Info));
    tee->Info.Size = sizeof(tee->Info);
    tee->Info.Version = JFP_VERSION;
    tee->Info.Uid = uid;
    tee->Info.InitResult = MAXULONG;
    tee->Info.WakeResult = MAXULONG;
    tee->Info.CaptureResult = MAXULONG;
    tee->Info.AuthStartResult = MAXULONG;
    tee->Info.FeatureResult = MAXULONG;
    tee->Info.FeatureCommandResult = MAXULONG;
    tee->Info.FeatureValue = MAXULONG;
    tee->Info.CancelResult = MAXULONG;
    tee->Info.PreEnrollResult = MAXULONG;
    tee->Info.MissingAuthResult = MAXULONG;
    tee->Info.PostEnrollResult = MAXULONG;
    tee->Info.Stage = 1;
    ULONGLONG registers[4];
    NTSTATUS status = Call(tee, 0x42000601, 1, 0x32000602, 0, 0, 0, registers);
    if (!NT_SUCCESS(status)) goto finish;
    tee->Info.Available = (ULONG)registers[1];
    if (registers[1] != 1) { status = STATUS_NOT_SUPPORTED; goto finish; }
    tee->Info.Stage = 2;
    status = Call(tee, JFP_TEE_INVOKE, 0x224, 0, 0, 0, 0, registers);
    if (status != STATUS_INVALID_PARAMETER) {
        if (NT_SUCCESS(status)) status = STATUS_DEVICE_PROTOCOL_ERROR;
        goto finish;
    }
    PHYSICAL_ADDRESS low, high, boundary;
    low.QuadPart = 0;
    high.QuadPart = MAXLONGLONG;
    boundary.QuadPart = 0;
    tee->Memory = MmAllocateContiguousMemorySpecifyCache(Size(tee), low, high, boundary, MmNonCached);
    if (!tee->Memory) { status = STATUS_INSUFFICIENT_RESOURCES; goto finish; }
    RtlZeroMemory(tee->Memory, Size(tee));
    PHYSICAL_ADDRESS physical = MmGetPhysicalAddress(tee->Memory);
    if (physical.QuadPart & (JFP_TEE_PAGE - 1)) { status = STATUS_DATATYPE_MISALIGNMENT; goto finish; }
    tee->Info.Stage = 3;
    status = Call(tee, JFP_TEE_BRIDGE_CREATE, 4, physical.QuadPart | 6,
        physical.QuadPart | 6, Size(tee) | 0x200u, 3, registers);
    if (!NT_SUCCESS(status)) goto finish;
    if (registers[1]) { status = STATUS_NOT_SUPPORTED; goto finish; }
    tee->Bridge = registers[2];
    tee->Shared = TRUE;
    tee->Info.Stage = 4;
    status = Invoke(tee, 1, 5, 0x1100, FALSE, 0, &tee->Client);
    if (!NT_SUCCESS(status)) goto finish;
    tee->Info.Client = tee->Client;
    tee->Info.Stage = 5;
    status = Invoke(tee, tee->Client, 0, 0x1001, TRUE, uid, &tee->Service);
    if (NT_SUCCESS(status)) tee->Info.Service = tee->Service;
finish:
    return status;
}

static NTSTATUS Finish(JFP_DEVICE *device, NTSTATUS status) {
    JFP_TEE *tee = &device->Tee;
    JFP_TEE_DIAGNOSTICS diagnostic = tee->Info;
    NTSTATUS released = JfpQteeRelease(device);
    diagnostic.Pinned = tee->Info.Pinned;
    diagnostic.LiveObjects = tee->Info.LiveObjects;
    diagnostic.Releases = tee->Info.Releases;
    diagnostic.CleanupStatus = released;
    diagnostic.Status = NT_SUCCESS(status) && !NT_SUCCESS(released) ? released : status;
    tee->Info = diagnostic;
    return diagnostic.Status;
}

NTSTATUS JfpQteeSpiServiceProbe(JFP_DEVICE *device) {
    /* The stock qsee_spi ABI uses CSPI UID16: open(0), close(1),
       full_duplex(4). This bounded probe reads only JV0301 chip ID registers. */
    if (device->Tee.Memory || device->Tee.Pending) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = Connect(device, 16, JFP_TEE_PAGE);
    JFP_TEE *tee = &device->Tee;
    BOOLEAN opened = FALSE, powered = FALSE;
    LONG flow[3] = {status, STATUS_INVALID_DEVICE_STATE, STATUS_INVALID_DEVICE_STATE};
    if (NT_SUCCESS(status)) {
        status = JfpSensorOn(device);
        powered = NT_SUCCESS(status);
    }
    if (NT_SUCCESS(status)) status = JfpSensorClockOn(device);
    if (NT_SUCCESS(status)) {
        status = Invoke(tee, tee->Service, 0, 1, TRUE, 2, NULL);
        opened = NT_SUCCESS(status);
        flow[1] = status;
    }
    if (NT_SUCCESS(status)) {
        ULONG chip = 0;
        /* Stock qsee_spi conversion: clock polarity, loopback, shifting,
           CS mode, clock mode, bits, Hz, slave, two delays, endian. */
        static const ULONG config[11] = {1, 1, 1, 2, 1, 8, 25000000, 0, 0, 0, 0};
        for (ULONG reg = 2; reg <= 3 && NT_SUCCESS(status); reg++) {
            RtlZeroMemory(tee->Memory, JFP_TEE_PAGE);
            RtlZeroMemory(tee->Memory + tee->MessageSize, JFP_TEE_PAGE);
            JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
            message->Context = tee->Service;
            message->Operation = 4;
            message->Counts = 0x23;
            static const ULONG offsets[5] = {96, 104, 152, 160, 168};
            static const ULONG sizes[5] = {4, 44, 4, 8, 4};
            for (ULONG i = 0; i < 5; i++) {
                message->Args[i].Buffer.Offset = offsets[i];
                message->Args[i].Buffer.Size = sizes[i];
            }
            ULONG instance = 2;
            RtlCopyMemory(tee->Memory + 96, &instance, 4);
            RtlCopyMemory(tee->Memory + 104, config, sizeof(config));
            tee->Memory[152] = 0x15;
            tee->Memory[153] = (UCHAR)reg;
            status = Execute(tee);
            if (NT_SUCCESS(status)) {
                if (message->Context != tee->Service || message->Operation != 4 || message->Counts != 0x23)
                    status = STATUS_DEVICE_PROTOCOL_ERROR;
                for (ULONG i = 0; NT_SUCCESS(status) && i < 5; i++)
                    if (message->Args[i].Buffer.Offset != offsets[i] || message->Args[i].Buffer.Size != sizes[i])
                        status = STATUS_DEVICE_PROTOCOL_ERROR;
                ULONG transferred[2];
                RtlCopyMemory(transferred, tee->Memory + 160, sizeof(transferred));
                if (NT_SUCCESS(status) && (transferred[0] != 4 || transferred[1] != 4))
                    status = STATUS_DEVICE_DATA_ERROR;
                if (NT_SUCCESS(status)) chip |= (ULONG)tee->Memory[171] << ((reg - 2) * 8);
            }
        }
        tee->Info.Data[0] = chip;
        if (NT_SUCCESS(status) && chip != 0x5446) status = STATUS_DEVICE_PROTOCOL_ERROR;
        flow[2] = status;
    }
    if (opened && !tee->Pending) {
        NTSTATUS closed = Invoke(tee, tee->Service, 1, 1, TRUE, 2, NULL);
        if (NT_SUCCESS(status)) status = closed;
    }
    RtlCopyMemory(tee->Info.Flow, flow, sizeof(flow));
    status = Finish(device, status);
    if (powered) {
        NTSTATUS off = JfpSensorOff(device);
        tee->Info.PowerStatus = off;
        if (NT_SUCCESS(status)) status = off;
    }
    tee->Info.Status = status;
    return status;
}

NTSTATUS JfpQteeProbe(JFP_DEVICE *device, ULONG uid) {
    if (uid != 16 && uid != 122 && uid != 2033) return STATUS_INVALID_PARAMETER;
    if (device->Tee.Memory || device->Tee.Pending) return STATUS_INVALID_DEVICE_STATE;
    return Finish(device, Connect(device, uid, JFP_TEE_PAGE));
}

static NTSTATUS Validate(const UCHAR *image, ULONG size, ULONG limit) {
    if (!image || size < 64 || size > limit || image[0] != 0x7f ||
        image[1] != 'E' || image[2] != 'L' || image[3] != 'F' || image[4] != 2 ||
        image[5] != 1 || image[18] != 183 || image[19] != 0) return STATUS_INVALID_PARAMETER;
    ULONGLONG phoff;
    USHORT phsize, phcount;
    RtlCopyMemory(&phoff, image + 32, sizeof(phoff));
    RtlCopyMemory(&phsize, image + 54, sizeof(phsize));
    RtlCopyMemory(&phcount, image + 56, sizeof(phcount));
    if (phoff < 64 || phoff > size || phsize != 56 || !phcount || phcount > 32 ||
        (ULONG)phcount * phsize > size - phoff) return STATUS_INVALID_PARAMETER;
    for (ULONG i = 0; i < phcount; i++) {
        ULONGLONG offset, length;
        RtlCopyMemory(&offset, image + phoff + i * phsize + 8, sizeof(offset));
        RtlCopyMemory(&length, image + phoff + i * phsize + 32, sizeof(length));
        if (offset > size || length > size - offset) return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS LoadNamed(JFP_DEVICE *device, const UCHAR *image, ULONG size, ULONG dataSize,
    const UCHAR *name, ULONG nameSize, ULONG maximum) {
    NTSTATUS valid = Validate(image, size, maximum);
    if (!NT_SUCCESS(valid)) return valid;
    JFP_TEE *tee = &device->Tee;
    if (tee->Memory || tee->Pending) return STATUS_INVALID_DEVICE_STATE;
    ULONG messageSize = (size + dataSize + 256 + JFP_TEE_PAGE - 1) & ~(JFP_TEE_PAGE - 1);
    NTSTATUS status = Connect(device, 122, messageSize);
    if (!NT_SUCCESS(status)) return Finish(device, status);
    tee->Info.Stage = 6;
    tee->Info.ImageSize = size;
    RtlZeroMemory(tee->Memory, Size(tee));
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    ULONG nameOffset = (48 + size + 7) & ~7u;
    ULONG outputOffset = (nameOffset + nameSize + 7) & ~7u;
    message->Context = tee->Service;
    message->Operation = 1;
    message->Counts = 0x1012;
    message->Args[0].Buffer.Offset = 48;
    message->Args[0].Buffer.Size = size;
    message->Args[1].Buffer.Offset = nameOffset;
    message->Args[1].Buffer.Size = nameSize;
    message->Args[2].Buffer.Offset = outputOffset;
    message->Args[2].Buffer.Size = 64;
    RtlCopyMemory(tee->Memory + 48, image, size);
    RtlCopyMemory(tee->Memory + nameOffset, name, nameSize);
    status = Execute(tee);
    if (NT_SUCCESS(status)) {
        ULONG app = message->Args[3].Object;
        if (!SecureObject(app) || app == tee->Client || app == tee->Service) status = STATUS_DEVICE_PROTOCOL_ERROR;
        else {
            tee->App = app;
            tee->Info.App = app;
            if (message->Context != tee->Service || message->Operation != 1 || message->Counts != 0x1012 ||
                message->Args[0].Buffer.Offset != 48 || message->Args[0].Buffer.Size != size ||
                message->Args[1].Buffer.Offset != nameOffset || message->Args[1].Buffer.Size != nameSize ||
                message->Args[2].Buffer.Offset != outputOffset || message->Args[2].Buffer.Size > 64)
                status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    }
    if (NT_SUCCESS(status)) {
        tee->DataSize = dataSize;
        tee->DataOffset = tee->MessageSize - dataSize;
    }
    return status;
}

static NTSTATUS Load(JFP_DEVICE *device, const UCHAR *image, ULONG size, ULONG dataSize) {
    static const UCHAR name[] = "uff_spi";
    return LoadNamed(device, image, size, dataSize, name, sizeof(name) - 1, JFP_TA_MAX_IMAGE);
}

NTSTATUS JfpQteeLoadProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size) {
    return Finish(device, Load(device, image, size, 0));
}

static NTSTATUS KeymasterRequest(JFP_TEE *tee, const UCHAR *request, ULONG requestSize,
    UCHAR *response) {
    if (!tee->App || tee->Pending || requestSize < 4 || requestSize > 128)
        return STATUS_INVALID_DEVICE_STATE;
    RtlZeroMemory(tee->Memory, Size(tee));
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    const ULONG sizes[6] = {requestSize, 256, 0, 4, requestSize, 256};
    ULONG offsets[6], offset = 96;
    message->Context = tee->App;
    message->Operation = 0;
    message->Counts = 0x424;
    for (ULONG i = 0; i < 6; i++) {
        offsets[i] = offset;
        message->Args[i].Buffer.Offset = offset;
        message->Args[i].Buffer.Size = sizes[i];
        offset += (sizes[i] + 7) & ~7u;
    }
    ULONG is64 = 1;
    RtlCopyMemory(tee->Memory + 96, request, requestSize);
    RtlCopyMemory(tee->Memory + offsets[3], &is64, sizeof(is64));
    LONG sentinel = (LONG)MAXULONG;
    RtlCopyMemory(tee->Memory + offsets[1], &sentinel, sizeof(sentinel));
    RtlCopyMemory(tee->Memory + offsets[5], &sentinel, sizeof(sentinel));
    tee->Info.Stage = 7;
    RtlCopyMemory(&tee->Info.Command, request, sizeof(ULONG));
    tee->Info.CommandStatus = sentinel;
    tee->Info.ResponseSize = 0;
    NTSTATUS status = Execute(tee);
    if (NT_SUCCESS(status)) {
        if (message->Context != tee->App || message->Operation != 0 || message->Counts != 0x424)
            return STATUS_DEVICE_PROTOCOL_ERROR;
        for (ULONG i = 0; i < 6; i++) {
            if (message->Args[i].Buffer.Offset != offsets[i] ||
                message->Args[i].Buffer.Size > sizes[i] ||
                (i < 4 && message->Args[i].Buffer.Size != sizes[i]))
                return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        if (message->Args[5].Buffer.Size < 4) return STATUS_DEVICE_PROTOCOL_ERROR;
        tee->Info.ResponseSize = message->Args[5].Buffer.Size;
        RtlCopyMemory(response, tee->Memory + offsets[5], tee->Info.ResponseSize);
        RtlCopyMemory(&tee->Info.CommandStatus, response, sizeof(LONG));
        if (tee->Info.CommandStatus) status = STATUS_IO_DEVICE_ERROR;
    }
    return status;
}

NTSTATUS JfpQteeKeymasterProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size, ULONG operation) {
    if (operation < 7 || operation > 9) return STATUS_INVALID_PARAMETER;
    /* Standard ephemeral sharing only; no account, HAT or provisioning writes. */
    static const UCHAR name[] = "keymaster64";
    NTSTATUS status = LoadNamed(device, image, size, 0, name, sizeof(name) - 1, JFP_KM_MAX_IMAGE);
    JFP_TEE *tee = &device->Tee;
    UCHAR response[256] = {0};
    const ULONG request[2] = {operation == 7 ? 0x205u : 0x220eu, operation == 7 ? 2u : 0u};
    if (NT_SUCCESS(status)) status = KeymasterRequest(tee, (const UCHAR *)request, sizeof(request), response);
    if (operation == 8 && NT_SUCCESS(status)) {
        /* Public seed/nonce values; preserve their wire encoding for diagnosis. */
        RtlCopyMemory(tee->Info.Data, response, tee->Info.ResponseSize);
    }
    if (operation == 9 && NT_SUCCESS(status)) {
        tee->Info.Flow[0] = tee->Info.CommandStatus;
        /* Original TA emits {41: bstr(32), 42: bstr(32)}. Reject any
           different encoding before issuing COMPUTE with tag 43. */
        const UCHAR first[] = {0xa2, 0x18, 0x29, 0x58, 0x20};
        const UCHAR second[] = {0x18, 0x2a, 0x58, 0x20};
        ULONG length = 0;
        RtlCopyMemory(&length, response + 4, sizeof(length));
        BOOLEAN canonical = TRUE;
        for (ULONG i = 0; i < sizeof(first); i++) if (response[8 + i] != first[i]) canonical = FALSE;
        for (ULONG i = 0; i < sizeof(second); i++) if (response[45 + i] != second[i]) canonical = FALSE;
        if (tee->Info.ResponseSize < 81 || length != 73 || !canonical)
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        else {
            UCHAR compute[77] = {0};
            const ULONG header[2] = {0x220f, 69};
            const UCHAR map[] = {0xa1, 0x18, 0x2b, 0x58, 0x40};
            RtlCopyMemory(compute, header, sizeof(header));
            RtlCopyMemory(compute + 8, map, sizeof(map));
            RtlCopyMemory(compute + 13, response + 13, 32);
            RtlCopyMemory(compute + 45, response + 49, 32);
            RtlZeroMemory(response, sizeof(response));
            status = KeymasterRequest(tee, compute, sizeof(compute), response);
            tee->Info.Flow[1] = tee->Info.CommandStatus;
            RtlZeroMemory(compute, sizeof(compute));
            if (NT_SUCCESS(status)) {
                const ULONG auth[2] = {0x205, 2};
                RtlZeroMemory(response, sizeof(response));
                status = KeymasterRequest(tee, (const UCHAR *)auth, sizeof(auth), response);
                tee->Info.Flow[2] = tee->Info.CommandStatus;
            }
        }
    }
    if (NT_SUCCESS(status) && operation != 8) {
        /* Export only the envelope header; keep payload in the erased allocation. */
        if (tee->Info.ResponseSize < 12) status = STATUS_DEVICE_PROTOCOL_ERROR;
        else {
            RtlCopyMemory(tee->Info.Response, response, 12);
            ULONG start = tee->Info.Response[1], length = tee->Info.Response[2];
            if (start < 12 || start > tee->Info.ResponseSize || !length ||
                length > tee->Info.ResponseSize - start) status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
    }
    RtlZeroMemory(response, sizeof(response));
    return Finish(device, status);
}

static NTSTATUS Command(JFP_TEE *tee, ULONG type, ULONG command, ULONG length,
    ULONG qseeLength, const UCHAR *payload, ULONG payloadSize) {
    if (!tee->App || tee->Pending || !tee->DataSize || length < 48 || length > tee->DataSize ||
        qseeLength < length || qseeLength > tee->DataSize ||
        payloadSize > length - 48 || (payloadSize && !payload)) return STATUS_INVALID_DEVICE_STATE;
    for (ULONG i = 0; i < RTL_NUMBER_OF(tee->Objects); i++)
        if (tee->Objects[i] && tee->ObjectKinds[i] == 1) return STATUS_DEVICE_BUSY;
    ULONG object = AddObject(tee, 1);
    if (!object) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(tee->Memory, tee->DataOffset);
    RtlZeroMemory(tee->Memory + tee->MessageSize, JFP_TEE_PAGE);
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    message->Context = tee->App;
    message->Operation = 0;
    message->Counts = 0x424;
    static const ULONG sizes[6] = {64, 1024, 16, 4, 64, 1024};
    ULONG offset = 96;
    for (ULONG i = 0; i < 6; i++) {
        message->Args[i].Buffer.Offset = offset;
        message->Args[i].Buffer.Size = sizes[i];
        offset += (sizes[i] + 7) & ~7u;
    }
    ULONG embedded = 4, is64 = 1;
    RtlCopyMemory(tee->Memory + 96, &qseeLength, 4);
    RtlCopyMemory(tee->Memory + message->Args[2].Buffer.Offset, &embedded, 4);
    RtlCopyMemory(tee->Memory + message->Args[3].Buffer.Offset, &is64, 4);
    message->Args[6].Object = object;
    PUCHAR data = tee->Memory + tee->DataOffset;
    RtlZeroMemory(data, tee->DataSize);
    RtlCopyMemory(data, &type, 4);
    RtlCopyMemory(data + 4, &command, 4);
    RtlCopyMemory(data + 0x18, &length, 4);
    ULONG extra = type ? qseeLength - length : 0;
    RtlCopyMemory(data + 0x1c, &extra, 4);
    ULONG invalid = MAXULONG;
    if (!type && length == 408) RtlCopyMemory(data + 0x194, &invalid, 4);
    RtlCopyMemory(data + 8, &invalid, 4);
    ULONGLONG timestamp = KeQueryInterruptTime() * 100;
    if (type) RtlCopyMemory(data + 16, &timestamp, 8);
    if (payloadSize) RtlCopyMemory(data + 48, payload, payloadSize);
    if (command == 0x7d4 && type == 2) RtlCopyMemory(data + 0x6c, &invalid, 4);
    /* GET_FEATURE's result_type/value at +0x58 are optional acquired-event
       outputs. The stock ANC 300 success path leaves their zero input intact.
       The required command result at +8 remains the completion sentinel. */
    tee->Info.Stage = 7;
    tee->Info.Command = command;
    NTSTATUS status = Execute(tee);
    tee->Info.TaLogSize = 0;
    tee->Info.TaLogStatus = STATUS_SUCCESS;
    if (!tee->Pending && extra) {
        ULONG used = 0;
        while (used < extra && data[length + used]) used++;
        tee->Info.TaLogSize = used;
        if (used) tee->Info.TaLogStatus = JfpQteeTaLog(tee, data + length, used);
    }
    tee->Info.CommandStatus = *(LONG *)(data + 8);
    RtlCopyMemory(tee->Info.Data, data, sizeof(tee->Info.Data));
    ULONG responseOffset = message->Args[5].Buffer.Offset;
    ULONG responseSize = message->Args[5].Buffer.Size;
    if (responseOffset == 1272 && responseSize <= 1024) {
        tee->Info.ResponseSize = responseSize;
        RtlCopyMemory(tee->Info.Response, tee->Memory + responseOffset, min(responseSize, (ULONG)sizeof(tee->Info.Response)));
    } else status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (!tee->Pending && !NT_SUCCESS(status) && !tee->Info.Maps) DropObject(tee, object);
    if (NT_SUCCESS(status) && tee->Info.CommandStatus) status = STATUS_IO_DEVICE_ERROR;
    return status;
}

static NTSTATUS Send(JFP_TEE *tee, ULONG command) {
    return Command(tee, 0, command, 408, 408, NULL, 0);
}

NTSTATUS JfpQteeSpiProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size, ULONG operation) {
    if (operation != 1 && operation != 2 && operation != 3 && operation != 5) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = Load(device, image, size, JFP_TEE_PAGE);
    if (NT_SUCCESS(status)) status = Send(&device->Tee, MAXULONG);
    if (NT_SUCCESS(status) && operation >= 2) {
        device->Tee.Info.Stage = 8;
        status = JfpSensorOn(device);
        device->Tee.Info.PowerAddress = device->Info.PowerAddress;
        RtlCopyMemory(device->Tee.Info.SavedPower, device->SavedPower, sizeof(device->SavedPower));
        device->Tee.Info.PowerStatus = status;
        if (NT_SUCCESS(status) && (operation == 2 || operation == 5)) status = JfpSensorClockOn(device);
        if (NT_SUCCESS(status) && operation == 2) status = Send(&device->Tee, 2);
    }
    status = Finish(device, status);
    if (operation >= 2) {
        NTSTATUS off = JfpSensorOff(device);
        device->Tee.Info.PowerStatus = off;
        if (NT_SUCCESS(status) && !NT_SUCCESS(off)) status = off;
        device->Tee.Info.Status = status;
    }
    return status;
}

NTSTATUS JfpQteeContinue(JFP_DEVICE *device) {
    JFP_TEE *tee = &device->Tee;
    if (!tee->Pending || !tee->Memory || !tee->Shared) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = Execute(tee);
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    if (NT_SUCCESS(status)) {
        if (message->Context == 1 && message->Operation == 5 && message->Counts == 0x1100) {
            if (!SecureObject(message->Args[1].Object)) status = STATUS_DEVICE_PROTOCOL_ERROR;
            else tee->Info.Client = tee->Client = message->Args[1].Object;
        } else if (message->Context == tee->Client && message->Operation == 0 && message->Counts == 0x1001) {
            if (!SecureObject(message->Args[1].Object)) status = STATUS_DEVICE_PROTOCOL_ERROR;
            else tee->Info.Service = tee->Service = message->Args[1].Object;
        } else if (message->Context == tee->Service && message->Operation == 1 && message->Counts == 0x1012) {
            if (!SecureObject(message->Args[3].Object)) status = STATUS_DEVICE_PROTOCOL_ERROR;
            else tee->Info.App = tee->App = message->Args[3].Object;
        } else if (message->Context == tee->App && message->Operation == 0 && message->Counts == 0x424 && tee->DataSize) {
            RtlCopyMemory(tee->Info.Data, tee->Memory + tee->DataOffset, sizeof(tee->Info.Data));
            tee->Info.CommandStatus = (LONG)tee->Info.Data[2];
            if (tee->Info.CommandStatus) status = STATUS_IO_DEVICE_ERROR;
        } else if (message->Operation != 0xffff) status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    status = Finish(device, status);
    NTSTATUS off = JfpSensorOff(device);
    if (NT_SUCCESS(status) && !NT_SUCCESS(off)) status = off;
    tee->Info.Status = status;
    return status;
}

static NTSTATUS LoadRegion(JFP_DEVICE *device, const UCHAR *image, ULONG size, BOOLEAN spi) {
    NTSTATUS status = Validate(image, size, spi ? JFP_TA_MAX_IMAGE : JFP_JV_MAX_IMAGE);
    if (!NT_SUCCESS(status)) return status;
    JFP_TEE *tee = &device->Tee;
    ULONG capacity = (size + JFP_TEE_PAGE - 1) & ~(JFP_TEE_PAGE - 1);
    status = Connect(device, 122, capacity + JFP_TEE_PAGE + JFP_TEE_DATA_CAPACITY +
        (spi ? 0 : JFP_TEE_STORAGE_CAPACITY + JFP_TEE_FILE_CAPACITY));
    if (!NT_SUCCESS(status)) return status;
    tee->DataOffset = JFP_TEE_PAGE;
    /* Normal fpcore requests include a 0xf000-byte trailing workspace. */
    tee->DataSize = JFP_TEE_DATA_CAPACITY;
    tee->ImageOffset = JFP_TEE_PAGE + tee->DataSize;
    tee->ImageCapacity = capacity;
    if (!spi) {
        tee->StorageOffset = tee->ImageOffset + capacity;
        tee->StorageSize = JFP_TEE_STORAGE_CAPACITY;
        tee->FileOffset = tee->StorageOffset + tee->StorageSize;
        tee->FileSize = JFP_TEE_FILE_CAPACITY;
    }
    RtlCopyMemory(tee->Memory + tee->ImageOffset, image, size);
    tee->Info.ImageSize = size;
    tee->Info.Stage = 9;
    RtlZeroMemory(tee->Memory, JFP_TEE_PAGE);
    static const UCHAR spiPath[] = "/odm/vendor/firmware/uff_spi";
    static const UCHAR jvPath[] = "/odm/vendor/firmware/uff_jv";
    const UCHAR *path = spi ? spiPath : jvPath;
    ULONG pathSize = spi ? sizeof(spiPath) - 1 : sizeof(jvPath) - 1;
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    message->Context = tee->Service;
    message->Operation = 0;
    message->Counts = 0x1101;
    message->Args[0].Buffer.Offset = 40;
    message->Args[0].Buffer.Size = pathSize;
    message->Args[1].Object = AddObject(tee, 2);
    if (!message->Args[1].Object) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(tee->Memory + 40, path, pathSize);
    status = Execute(tee);
    if (!NT_SUCCESS(status)) return status;
    if (!SecureObject(message->Args[2].Object)) return STATUS_DEVICE_PROTOCOL_ERROR;
    tee->Region = tee->Info.Region = message->Args[2].Object;
    tee->Info.Stage = 10;
    RtlZeroMemory(tee->Memory, JFP_TEE_PAGE);
    message->Context = tee->Service;
    message->Operation = 2;
    message->Counts = 0x1011;
    message->Args[0].Buffer.Offset = 40;
    message->Args[0].Buffer.Size = pathSize;
    message->Args[1].Buffer.Offset = 72;
    message->Args[1].Buffer.Size = 4;
    RtlCopyMemory(tee->Memory + 40, path, pathSize);
    status = Execute(tee);
    if (!NT_SUCCESS(status)) return status;
    if (!SecureObject(message->Args[2].Object)) return STATUS_DEVICE_PROTOCOL_ERROR;
    tee->App = tee->Info.App = message->Args[2].Object;
    return STATUS_SUCCESS;
}

NTSTATUS JfpQteeJvProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size) {
    return Finish(device, LoadRegion(device, image, size, FALSE));
}

static NTSTATUS Configure(JFP_TEE *tee) {
    UCHAR configuration[128] = {0};
    RtlCopyMemory(configuration, "24811", 6);
    RtlCopyMemory(configuration + 32, "BOE", 4);
    RtlCopyMemory(configuration + 64, "G_OPTICAL_JV0301", 16);
    RtlCopyMemory(configuration + 96, "NULL", 5);
    return Command(tee, 2, 0x834, 176, 176, configuration, sizeof(configuration));
}

static NTSTATUS RegisterStorage(JFP_TEE *tee, BOOLEAN normal) {
    ULONG *callback = normal ? &tee->FileObject : &tee->StorageObject;
    ULONG *memory = normal ? &tee->FileMemory : &tee->StorageMemory;
    *callback = AddObject(tee, normal ? 6 : 4);
    *memory = AddObject(tee, normal ? 5 : 3);
    if (!*callback || !*memory) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(tee->Memory, JFP_TEE_PAGE);
    RtlZeroMemory(tee->Memory + tee->MessageSize, JFP_TEE_PAGE);
    JFP_TEE_MESSAGE *message = (JFP_TEE_MESSAGE *)tee->Memory;
    message->Context = tee->StorageService;
    message->Operation = 0;
    message->Counts = 0x201;
    message->Args[0].Buffer.Offset = 40;
    message->Args[0].Buffer.Size = 4;
    message->Args[1].Object = *callback;
    message->Args[2].Object = *memory;
    /* qseecomd: FILE_SERVICE = 10; 0x4000 belongs to Secure UI. */
    ULONG listener = normal ? 10 : 0x7000;
    RtlCopyMemory(tee->Memory + 40, &listener, 4);
    NTSTATUS status = Execute(tee);
    if (!NT_SUCCESS(status) && !tee->Pending) {
        DropObject(tee, *callback);
        DropObject(tee, *memory);
        *callback = *memory = 0;
    }
    return status;
}

static NTSTATUS StorageConnect(JFP_TEE *tee) {
    NTSTATUS status = JfpQteeStoragePrepare();
    if (NT_SUCCESS(status)) status = Invoke(tee, tee->Client, 0, 0x1001, TRUE, 87, &tee->StorageService);
    if (NT_SUCCESS(status)) status = RegisterStorage(tee, FALSE);
    if (NT_SUCCESS(status)) status = RegisterStorage(tee, TRUE);
    return status;
}

static NTSTATUS ReadConfiguration(JFP_TEE *tee) {
    /* GET_TA_CONFIG uses the same fp_get_ta_config_by_id as vendor INIT.
       The requested size is at command+0x238; data begins at +0x38. */
    static const ULONG ids[JFP_CONFIG_COUNT] = {0, 0xa, 0xb, 0xc, 0x10, 0x3eb, 0x3ec, 0x3f1, 0x3f3, 0x3f4, 0x3f9};
    static const ULONG sizes[JFP_CONFIG_COUNT] = {128, 32, 32, 32, 32, 4, 4, 4, 4, 16, 4};
    NTSTATUS first = STATUS_SUCCESS;
    for (ULONG i = 0; i < JFP_CONFIG_COUNT; i++) {
        UCHAR payload[528] = {0};
        RtlCopyMemory(payload, &ids[i], 4);
        RtlCopyMemory(payload + 0x208, &sizes[i], 4);
        NTSTATUS status = Command(tee, 2, 0x835, 576, 576, payload, sizeof(payload));
        JFP_CONFIG_DIAGNOSTICS *info = &tee->Info.Config[i];
        info->Id = ids[i];
        info->Size = sizes[i];
        info->Status = status;
        info->Result = tee->Info.CommandStatus;
        RtlCopyMemory(info->Data, tee->Memory + tee->DataOffset + 0x38,
            min(sizes[i], (ULONG)sizeof(info->Data)));
        if (!NT_SUCCESS(status) && NT_SUCCESS(first)) first = status;
        if (tee->Pending || tee->Info.LiveObjects) break;
    }
    return first;
}

NTSTATUS JfpQteeConfigProbe(JFP_DEVICE *device, const UCHAR *image, ULONG size) {
    NTSTATUS status = LoadRegion(device, image, size, FALSE);
    if (NT_SUCCESS(status)) status = Configure(&device->Tee);
    if (NT_SUCCESS(status)) status = ReadConfiguration(&device->Tee);
    return Finish(device, status);
}

static NTSTATUS FingerprintStart(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize) {
    if (device->Tee.Memory || device->Tee.Pending) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = Validate(spi, spiSize, JFP_TA_MAX_IMAGE);
    if (NT_SUCCESS(status)) status = Validate(jv, jvSize, JFP_JV_MAX_IMAGE);
    if (!NT_SUCCESS(status)) return status;
    LONG flow[6] = {STATUS_INVALID_DEVICE_STATE, STATUS_INVALID_DEVICE_STATE, STATUS_INVALID_DEVICE_STATE,
        STATUS_INVALID_DEVICE_STATE, STATUS_INVALID_DEVICE_STATE, STATUS_INVALID_DEVICE_STATE};
    status = LoadRegion(device, spi, spiSize, TRUE);
    if (NT_SUCCESS(status)) status = Send(&device->Tee, MAXULONG);
    flow[0] = status;
    if (NT_SUCCESS(status)) status = JfpSensorOn(device);
    if (NT_SUCCESS(status)) status = JfpSensorClockOn(device);
    flow[1] = status;
    if (NT_SUCCESS(status)) status = Invoke(&device->Tee, device->Tee.App, 2, 0, FALSE, 0, NULL);
    status = Finish(device, status);
    if (NT_SUCCESS(status)) {
        LARGE_INTEGER delay;
        delay.QuadPart = -5000000;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
        status = LoadRegion(device, jv, jvSize, FALSE);
    }
    flow[2] = status;
    if (NT_SUCCESS(status)) status = StorageConnect(&device->Tee);
    device->Tee.Info.StorageStatus = status;
    if (NT_SUCCESS(status)) status = Configure(&device->Tee);
    flow[3] = status;
    if (NT_SUCCESS(status)) {
        status = Command(&device->Tee, 2, 0x7d0, 448, 0xf1c0, NULL, 0);
        device->Tee.Info.InitResult = device->Tee.Info.CommandStatus;
    }
    flow[4] = status;
    RtlCopyMemory(device->Tee.Info.Flow, flow, sizeof(flow));
    device->Tee.Info.PowerAddress = device->Info.PowerAddress;
    RtlCopyMemory(device->Tee.Info.SavedPower, device->SavedPower, sizeof(device->SavedPower));
    return status;
}

static NTSTATUS StartVerify(JFP_TEE *tee) {
    /* Initializes extraction only; no compare, enrollment or token is issued. */
    NTSTATUS status = Command(tee, 2, 0x7dc, 136, 0xf088, NULL, 0);
    tee->Info.AuthStartResult = tee->Info.CommandStatus;
    tee->VerifyStarted = NT_SUCCESS(status);
    return status;
}

static NTSTATUS FingerprintSample(JFP_DEVICE *device, BOOLEAN feature) {
    JFP_TEE *tee = &device->Tee;
    tee->Info.WakeResult = tee->Info.CaptureResult = MAXULONG;
    tee->Info.FeatureResult = tee->Info.FeatureCommandResult = tee->Info.FeatureValue = MAXULONG;
    /* Follow the stock verification power sequence, rather than enrollment
       mode 8. AUTH_DOWN's touch/config payload is 112 bytes, initially zero. */
    NTSTATUS status = feature ? Command(tee, 2, 0x7dd, 160, 0xf0a0, NULL, 0) :
        Command(tee, 2, 0x7d7, 144, 0xf090, NULL, 0);
    tee->Info.WakeResult = tee->Info.CommandStatus;
    if (NT_SUCCESS(status)) {
        UCHAR payload[136] = {0};
        ULONG sensorMode = 3, enabled = 1, uptime = (ULONG)(KeQueryInterruptTime() / 10000);
        RtlCopyMemory(payload + 4, &sensorMode, 4);
        RtlCopyMemory(payload + 8, &enabled, 4);
        RtlCopyMemory(payload + 28, &uptime, 4);
        status = Command(tee, 2, 0x7d4, 184, 0xf0b8, payload, sizeof(payload));
        tee->Info.CaptureResult = tee->Info.Data[27];
        if (NT_SUCCESS(status)) {
            if (tee->Info.CaptureResult) status = STATUS_DEVICE_DATA_ERROR;
            else tee->Info.Stage = 11;
        }
    }
    if (feature && NT_SUCCESS(status)) {
        UCHAR payload[112] = {0};
        ULONG enabled = 1, sensorMode = 3;
        RtlCopyMemory(payload, &enabled, 4);
        RtlCopyMemory(payload + 4, &sensorMode, 4);
        status = Command(tee, 2, 0x7d3, 160, 0xf0a0, payload, sizeof(payload));
        tee->Info.FeatureCommandResult = tee->Info.CommandStatus;
        tee->Info.FeatureResult = tee->Info.Data[22];
        tee->Info.FeatureValue = tee->Info.Data[23];
        if (NT_SUCCESS(status) && tee->Info.FeatureResult) status = STATUS_DEVICE_DATA_ERROR;
    }
    tee->Info.SampleCount++;
    tee->Info.Flow[5] = tee->Info.Status = status;
    return status;
}

NTSTATUS JfpQteeSessionEnd(JFP_DEVICE *device) {
    JFP_TEE *tee = &device->Tee;
    NTSTATUS status = tee->Pending ? STATUS_IO_TIMEOUT : STATUS_SUCCESS;
    tee->Info.SessionActive = FALSE;
    if (tee->VerifyStarted && !tee->Pending) {
        status = Command(tee, 2, 0x7e3, 48, 0xf030, NULL, 0);
        tee->Info.CancelResult = tee->Info.CommandStatus;
    }
    tee->VerifyStarted = FALSE;
    status = Finish(device, status);
    NTSTATUS off = JfpSensorOff(device);
    tee->Info.PowerStatus = off;
    if (NT_SUCCESS(status) && !NT_SUCCESS(off)) status = off;
    tee->Info.Status = status;
    return status;
}

NTSTATUS JfpQteeSessionBegin(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize) {
    if (device->Tee.Memory || device->Tee.Pending) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = FingerprintStart(device, spi, spiSize, jv, jvSize);
    if (NT_SUCCESS(status)) status = StartVerify(&device->Tee);
    if (!NT_SUCCESS(status)) {
        JfpQteeSessionEnd(device);
        device->Tee.Info.Status = status;
        return status;
    }
    device->Tee.Info.SessionActive = TRUE;
    device->Tee.Info.Pinned = TRUE;
    device->Tee.Info.Status = STATUS_SUCCESS;
    return STATUS_SUCCESS;
}

NTSTATUS JfpQteeSessionSample(JFP_DEVICE *device) {
    JFP_TEE *tee = &device->Tee;
    if (!tee->Info.SessionActive || !tee->VerifyStarted || !tee->App || !tee->Memory || tee->Pending)
        return STATUS_INVALID_DEVICE_STATE;
    return FingerprintSample(device, TRUE);
}

static NTSTATUS FingerprintProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize, ULONG mode) {
    if (device->Tee.Memory || device->Tee.Pending) return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status = FingerprintStart(device, spi, spiSize, jv, jvSize);
    if (mode == 2 && NT_SUCCESS(status)) status = StartVerify(&device->Tee);
    if (mode && NT_SUCCESS(status)) status = FingerprintSample(device, mode == 2);
    NTSTATUS ended = JfpQteeSessionEnd(device);
    if (NT_SUCCESS(status)) status = ended;
    device->Tee.Info.Status = status;
    return status;
}

NTSTATUS JfpQteeInitProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize) {
    return FingerprintProbe(device, spi, spiSize, jv, jvSize, 0);
}

NTSTATUS JfpQteeCaptureProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize) {
    return FingerprintProbe(device, spi, spiSize, jv, jvSize, 1);
}

NTSTATUS JfpQteeFeatureProbe(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize) {
    return FingerprintProbe(device, spi, spiSize, jv, jvSize, 2);
}

NTSTATUS JfpQteeEnrollPreflight(JFP_DEVICE *device, const UCHAR *spi, ULONG spiSize,
    const UCHAR *jv, ULONG jvSize) {
    if (device->Tee.Memory || device->Tee.Pending) return STATUS_INVALID_DEVICE_STATE;
    JFP_TEE *tee = &device->Tee;
    NTSTATUS status = FingerprintStart(device, spi, spiSize, jv, jvSize);
    BOOLEAN initialized = NT_SUCCESS(status);
    if (initialized) {
        status = Command(tee, 2, 0x7d5, 128, 0xf080, NULL, 0);
        tee->Info.PreEnrollResult = tee->Info.CommandStatus;
        if (NT_SUCCESS(status)) {
            ULONGLONG challenge = 0;
            /* Stock PRE_ENROLL stores its challenge at command+0x78. */
            RtlCopyMemory(&challenge, tee->Memory + tee->DataOffset + 0x78, sizeof(challenge));
            tee->Info.ChallengePresent = challenge != 0;
            if (!challenge) status = STATUS_DEVICE_PROTOCOL_ERROR;
        }
        if (NT_SUCCESS(status)) {
            /* Negative authorization test: no token, no replay and no keys.
               Never advance to sampling or template creation, even if accepted. */
            status = Command(tee, 2, 0x7d6, 200, 0xf0c8, NULL, 0);
            tee->Info.MissingAuthResult = tee->Info.CommandStatus;
        }
    }
    if (initialized && !tee->Pending) {
        NTSTATUS cancelled = Command(tee, 2, 0x7e3, 48, 0xf030, NULL, 0);
        tee->Info.CancelResult = tee->Info.CommandStatus;
        if (NT_SUCCESS(status)) status = cancelled;
        if (!tee->Pending) {
            NTSTATUS posted = Command(tee, 2, 0x7da, 120, 0xf078, NULL, 0);
            tee->Info.PostEnrollResult = tee->Info.CommandStatus;
            if (NT_SUCCESS(status)) status = posted;
        }
    }
    NTSTATUS ended = JfpQteeSessionEnd(device);
    if (NT_SUCCESS(status)) status = ended;
    tee->Info.Status = status;
    return status;
}
