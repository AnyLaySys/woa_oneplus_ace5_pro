#include "fingerprint.h"
#include <ntstrsafe.h>

/* GPFILE_SERVICE (0x7000), as used by the stock SM8750 cmnlib.
   The TA supplies encrypted file contents. Keep those bytes unchanged. */
static ULONG Error(NTSTATUS status) {
    if (NT_SUCCESS(status)) return 0;
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_OBJECT_PATH_NOT_FOUND) return 2;
    if (status == STATUS_ACCESS_DENIED) return 13;
    if (status == STATUS_INVALID_PARAMETER) return 22;
    return 5;
}

NTSTATUS JfpQteeStoragePrepare(VOID) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    static const WCHAR *paths[] = {L"\\SystemRoot\\System32\\jiiov-fp",
        L"\\SystemRoot\\System32\\jiiov-fp\\persist"};
    for (ULONG i = 0; i < RTL_NUMBER_OF(paths); i++) {
        UNICODE_STRING path;
        RtlInitUnicodeString(&path, paths[i]);
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        IO_STATUS_BLOCK io = {0};
        HANDLE directory = NULL;
        NTSTATUS status = ZwCreateFile(&directory, SYNCHRONIZE, &attributes, &io, NULL,
            FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
            FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
        if (!NT_SUCCESS(status)) return status;
        ZwClose(directory);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Path(const UCHAR *name, UNICODE_STRING *path, WCHAR *buffer, SIZE_T capacity) {
    static const CHAR prefix[] = "/mnt/vendor/persist/data/";
    ULONG length = 0;
    while (length < 256 && name[length]) length++;
    if (length == 256 || length <= sizeof(prefix) - 1 ||
        RtlCompareMemory(name, prefix, sizeof(prefix) - 1) != sizeof(prefix) - 1)
        return STATUS_ACCESS_DENIED;
    const UCHAR *base = name + sizeof(prefix) - 1;
    ULONG count = length - (ULONG)sizeof(prefix) + 1;
    if (count > 128) return STATUS_INVALID_PARAMETER;
    WCHAR file[129];
    for (ULONG i = 0; i < count; i++) {
        UCHAR c = base[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') ||
            (c == '.' && (!i || base[i - 1] == '.'))) return STATUS_ACCESS_DENIED;
        file[i] = c;
    }
    file[count] = 0;
    NTSTATUS status = RtlStringCbPrintfW(buffer, capacity,
        L"\\SystemRoot\\System32\\jiiov-fp\\persist\\%s", file);
    if (NT_SUCCESS(status)) RtlInitUnicodeString(path, buffer);
    return status;
}

static NTSTATUS Normal(JFP_TEE *tee) {
    if (tee->FileSize < 0x4e48) return STATUS_INVALID_DEVICE_STATE;
    PUCHAR data = tee->Memory + tee->FileOffset;
    ULONG command, descriptor, flags = 0, count = 0;
    RtlCopyMemory(&command, data, 4);
    RtlCopyMemory(&descriptor, data + 4, 4);
    tee->Info.StorageRequests++;
    tee->Info.StorageCommand = command;
    NTSTATUS status = STATUS_SUCCESS;
    LONG result = -1;
    IO_STATUS_BLOCK io = {0};
    HANDLE file = descriptor && descriptor <= RTL_NUMBER_OF(tee->Files) ? tee->Files[descriptor - 1] : NULL;
    if (command == 0x202 || command == 0x20f || command == 0x211) {
        WCHAR buffer[256];
        UNICODE_STRING path;
        status = Path(data + 4, &path, buffer, sizeof(buffer));
        if (command == 0x202) RtlCopyMemory(&flags, data + 0x104, 4);
        if (NT_SUCCESS(status)) {
            OBJECT_ATTRIBUTES attributes;
            InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
            ACCESS_MASK access = SYNCHRONIZE | FILE_READ_ATTRIBUTES;
            if (command == 0x202) access |= (flags & 3) == 0 ? FILE_READ_DATA :
                (flags & 3) == 1 ? FILE_WRITE_DATA : FILE_READ_DATA | FILE_WRITE_DATA;
            ULONG disposition = command == 0x202 && (flags & 0x40) ?
                (flags & 0x200 ? FILE_OVERWRITE_IF : FILE_OPEN_IF) : FILE_OPEN;
            status = ZwCreateFile(&file, access, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
                FILE_SHARE_READ | FILE_SHARE_WRITE, disposition,
                FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
        }
        if (NT_SUCCESS(status)) {
            if (command == 0x202) {
                for (ULONG i = 0; i < RTL_NUMBER_OF(tee->Files); i++) {
                    if (!tee->Files[i]) { tee->Files[i] = file; result = (LONG)i + 1; file = NULL; break; }
                }
                if (file) status = STATUS_INSUFFICIENT_RESOURCES;
            } else if (command == 0x211) result = 0;
            else {
                FILE_STANDARD_INFORMATION info;
                status = ZwQueryInformationFile(file, &io, &info, sizeof(info), FileStandardInformation);
                RtlZeroMemory(data + 4, 100);
                if (NT_SUCCESS(status)) {
                    ULONG mode = 0100666, links = info.NumberOfLinks, block = 4096;
                    ULONGLONG blocks = (info.AllocationSize.QuadPart + 511) / 512;
                    RtlCopyMemory(data + 20, &mode, 4);
                    RtlCopyMemory(data + 24, &links, 4);
                    RtlCopyMemory(data + 44, &block, 4);
                    RtlCopyMemory(data + 48, &info.EndOfFile.QuadPart, 8);
                    RtlCopyMemory(data + 56, &blocks, 8);
                    result = 0;
                }
            }
        }
        if (file) ZwClose(file);
    } else if (command == 0x207 || command == 0x208) {
        RtlCopyMemory(&count, data + (command == 0x207 ? 8 : 0x4e28), 4);
        if (!file || count > 0x4e20) status = STATUS_INVALID_PARAMETER;
        else if (command == 0x207) {
            status = ZwReadFile(file, NULL, NULL, NULL, &io, data + 4, count, NULL, NULL);
            if (status == STATUS_END_OF_FILE) { status = STATUS_SUCCESS; result = 0; }
            else if (NT_SUCCESS(status)) result = (LONG)io.Information;
        } else {
            status = ZwWriteFile(file, NULL, NULL, NULL, &io, data + 8, count, NULL, NULL);
            if (NT_SUCCESS(status)) result = (LONG)io.Information;
        }
    } else if (command == 0x209) {
        if (!file) status = STATUS_INVALID_PARAMETER;
        else { ZwClose(file); tee->Files[descriptor - 1] = NULL; result = 0; }
    } else if (command == 0x21c) result = (LONG)tee->FileError;
    else status = STATUS_NOT_SUPPORTED;
    if (!NT_SUCCESS(status)) { tee->FileError = Error(status); result = -1; }
    RtlCopyMemory(data + (command == 0x207 ? 0x4e24 : command == 0x20f ? 100 : 4), &result, 4);
    tee->Info.StorageIoStatus = status;
    return STATUS_SUCCESS;
}

VOID JfpQteeStorageClose(JFP_TEE *tee) {
    for (ULONG i = 0; i < RTL_NUMBER_OF(tee->Files); i++) {
        if (tee->Files[i]) { ZwClose(tee->Files[i]); tee->Files[i] = NULL; }
    }
}

NTSTATUS JfpQteeStorage(JFP_TEE *tee, BOOLEAN normal) {
    if (!tee->Memory || tee->StorageSize < 0x110 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_DEVICE_STATE;
    if (normal) return Normal(tee);
    PUCHAR data = tee->Memory + tee->StorageOffset;
    ULONG command, offset = 0, count = 0, flags = 0, transferred = 0;
    RtlCopyMemory(&command, data, 4);
    tee->Info.StorageRequests++;
    tee->Info.StorageCommand = command;
    NTSTATUS status = STATUS_SUCCESS;
    if (command == 12) {
        /* Version 2.0; this response is exactly 12 bytes. */
        ULONG version = 2, minor = 0;
        RtlCopyMemory(data + 4, &version, 4);
        RtlCopyMemory(data + 8, &minor, 4);
        tee->Info.StorageIoStatus = STATUS_SUCCESS;
        return STATUS_SUCCESS;
    }
    if (command > 11) status = STATUS_INVALID_PARAMETER;
    ULONG operation = command & 3;
    if (NT_SUCCESS(status) && operation <= 1) {
        RtlCopyMemory(&offset, data + 0x104, 4);
        RtlCopyMemory(&count, data + 0x108, 4);
        if (operation == 1) RtlCopyMemory(&flags, data + 0x10c, 4);
        if (count > tee->StorageSize - (operation ? 0x110u : 12u)) status = STATUS_INVALID_PARAMETER;
    }
    WCHAR pathBuffer[256];
    UNICODE_STRING path;
    if (NT_SUCCESS(status)) status = Path(data + 4, &path, pathBuffer, sizeof(pathBuffer));
    HANDLE file = NULL;
    IO_STATUS_BLOCK io = {0};
    if (NT_SUCCESS(status)) {
        OBJECT_ATTRIBUTES attributes;
        InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        ACCESS_MASK access = SYNCHRONIZE | (operation == 0 ? FILE_READ_DATA :
            operation == 1 ? FILE_WRITE_DATA : DELETE);
        status = ZwCreateFile(&file, access, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ, operation == 1 ? FILE_OPEN_IF : FILE_OPEN,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    }
    if (NT_SUCCESS(status)) {
        LARGE_INTEGER position;
        position.QuadPart = offset;
        if (operation == 0) {
            status = ZwReadFile(file, NULL, NULL, NULL, &io, data + 12, count, &position, NULL);
            if (status == STATUS_END_OF_FILE) status = STATUS_SUCCESS;
            else if (NT_SUCCESS(status)) transferred = (ULONG)io.Information;
        } else if (operation == 1) {
            /* The first chunk carries the original O_TRUNC flag; later chunks use zero. */
            if (flags & 0x200) {
                FILE_END_OF_FILE_INFORMATION end = {0};
                status = ZwSetInformationFile(file, &io, &end, sizeof(end), FileEndOfFileInformation);
            }
            if (NT_SUCCESS(status)) {
                status = ZwWriteFile(file, NULL, NULL, NULL, &io, data + 0x110, count, &position, NULL);
                if (NT_SUCCESS(status)) transferred = (ULONG)io.Information;
                if (NT_SUCCESS(status) && transferred != count) status = STATUS_DATA_ERROR;
            }
        } else if (operation == 2) {
            FILE_DISPOSITION_INFORMATION disposition = {TRUE};
            status = ZwSetInformationFile(file, &io, &disposition, sizeof(disposition), FileDispositionInformation);
        } else {
            /* Reject unsupported rename until its request has been validated. */
            status = STATUS_NOT_SUPPORTED;
        }
    }
    if (file) ZwClose(file);
    ULONG error = Error(status);
    RtlCopyMemory(data + 4, &error, 4);
    RtlCopyMemory(data + 8, &transferred, 4);
    tee->Info.StorageIoStatus = status;
    /* Filesystem failures belong in the GP response, not the transport result. */
    return STATUS_SUCCESS;
}
