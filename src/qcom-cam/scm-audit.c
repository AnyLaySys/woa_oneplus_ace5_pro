#include "cam.h"
#include <ntstrsafe.h>

NTSTATUS CamScmAudit(CAM *c,ULONG phase,const ULONGLONG *registers,NTSTATUS result) {
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    ULONGLONG empty[8]={0};if(!registers)registers=empty;
    CHAR line[768];SIZE_T length=0;c->ScmAuditSequence++;
    NTSTATUS status=RtlStringCbPrintfA(line,sizeof(line),
        "seq=%lu phase=%lu time=%I64u cpu=%lu result=%08lx step=%lu pending=%u original=%I64x args=%I64x %I64x %I64x %I64x %I64x %I64x %I64x %I64x\r\n",
        c->ScmAuditSequence,phase,KeQueryInterruptTime(),KeGetCurrentProcessorNumber(),(ULONG)result,c->Diag.Step,
        (ULONG)c->ScmPending,c->ScmResumeArgs[0],registers[0],registers[1],registers[2],registers[3],
        registers[4],registers[5],registers[6],registers[7]);
    if(NT_SUCCESS(status))status=RtlStringCbLengthA(line,sizeof(line),&length);
    if(!NT_SUCCESS(status))return status;
    UNICODE_STRING path=RTL_CONSTANT_STRING(L"\\SystemRoot\\Temp\\qcom-cam-scm.log");
    OBJECT_ATTRIBUTES attributes;InitializeObjectAttributes(&attributes,&path,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    IO_STATUS_BLOCK io={0};HANDLE file=NULL;
    status=ZwCreateFile(&file,FILE_APPEND_DATA|SYNCHRONIZE,&attributes,&io,NULL,FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,FILE_OPEN_IF,FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE|FILE_WRITE_THROUGH,NULL,0);
    if(!NT_SUCCESS(status)){c->Trace.ScmAuditStatus=status;c->Trace.ScmAuditSequence=c->ScmAuditSequence;return status;}
    status=ZwWriteFile(file,NULL,NULL,NULL,&io,line,(ULONG)length,NULL,NULL);
    if(NT_SUCCESS(status)&&io.Information!=length)status=STATUS_DATA_ERROR;
    ZwClose(file);c->Trace.ScmAuditStatus=status;c->Trace.ScmAuditSequence=c->ScmAuditSequence;
    return status;
}
