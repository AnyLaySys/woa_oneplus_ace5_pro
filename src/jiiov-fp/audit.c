#include "fingerprint.h"
#include <ntstrsafe.h>

NTSTATUS JfpQteeTaLog(JFP_TEE *tee, const UCHAR *data, ULONG size) {
    if (!size || size > 0xf000 || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_PARAMETER;
    CHAR header[96];
    SIZE_T length = 0;
    NTSTATUS status = RtlStringCbPrintfA(header, sizeof(header), "\r\ncommand=%08lx bytes=%lu\r\n",
        tee->Info.Command, size);
    if (NT_SUCCESS(status)) status = RtlStringCbLengthA(header, sizeof(header), &length);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\SystemRoot\\Temp\\jiiov-fp-ta.log");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    IO_STATUS_BLOCK io = {0};
    HANDLE file = NULL;
    status = ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &attributes, &io, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(status)) return status;
    status = ZwWriteFile(file, NULL, NULL, NULL, &io, header, (ULONG)length, NULL, NULL);
    if (NT_SUCCESS(status) && io.Information != length) status = STATUS_DATA_ERROR;
    if (NT_SUCCESS(status)) {
        status = ZwWriteFile(file, NULL, NULL, NULL, &io, (PVOID)data, size, NULL, NULL);
        if (NT_SUCCESS(status) && io.Information != size) status = STATUS_DATA_ERROR;
    }
    ZwClose(file);
    return status;
}

NTSTATUS JfpQteeTrace(JFP_TEE *tee, ULONG phase, const ULONGLONG *registers, NTSTATUS result) {
    if (!tee->AuditEnabled) return STATUS_SUCCESS;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    ULONGLONG empty[8] = {0};
    if (!registers) registers = empty;
    CHAR line[512];
    SIZE_T length = 0;
    NTSTATUS status = RtlStringCbPrintfA(line, sizeof(line),
        "seq=%lu phase=%lu time=%I64u cpu=%lu result=%08lx stage=%lu command=%08lx pending=%u args=%I64x,%I64x,%I64x,%I64x,%I64x,%I64x,%I64x,%I64x\r\n",
        ++tee->AuditSequence, phase, KeQueryInterruptTime(), KeGetCurrentProcessorNumber(),
        (ULONG)result, tee->Info.Stage, tee->Info.Command, (ULONG)tee->Pending,
        registers[0], registers[1], registers[2], registers[3], registers[4], registers[5], registers[6], registers[7]);
    if (NT_SUCCESS(status)) status = RtlStringCbLengthA(line, sizeof(line), &length);
    if (!NT_SUCCESS(status)) return status;
    UNICODE_STRING path = RTL_CONSTANT_STRING(L"\\SystemRoot\\Temp\\jiiov-fp-tee.log");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    IO_STATUS_BLOCK io = {0};
    HANDLE file = NULL;
    status = ZwCreateFile(&file, FILE_APPEND_DATA | SYNCHRONIZE, &attributes, &io, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(status)) { tee->Info.AuditStatus = status; return status; }
    status = ZwWriteFile(file, NULL, NULL, NULL, &io, line, (ULONG)length, NULL, NULL);
    if (NT_SUCCESS(status) && io.Information != length) status = STATUS_DATA_ERROR;
    ZwClose(file);
    tee->Info.AuditStatus = status;
    return status;
}
