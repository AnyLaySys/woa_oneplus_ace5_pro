#pragma once
#include "base.h"
#include <wchar.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
typedef char CHAR;
typedef wchar_t WCHAR;
typedef size_t SIZE_T;
typedef ULONG ACCESS_MASK;
typedef PVOID HANDLE;
typedef struct { const WCHAR *Buffer; } UNICODE_STRING;
typedef struct { UNICODE_STRING *ObjectName; } OBJECT_ATTRIBUTES;
typedef struct { ULONG Information; } IO_STATUS_BLOCK;
typedef struct { LARGE_INTEGER EndOfFile; } FILE_END_OF_FILE_INFORMATION;
typedef struct { BOOLEAN DeleteFile; } FILE_DISPOSITION_INFORMATION;
typedef struct { LARGE_INTEGER AllocationSize, EndOfFile; ULONG NumberOfLinks; } FILE_STANDARD_INFORMATION;
#define PASSIVE_LEVEL 0
#define KeGetCurrentIrql() 0
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xc0000034)
#define STATUS_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xc000003a)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003e)
#define STATUS_END_OF_FILE ((NTSTATUS)0xc0000011)
#define SYNCHRONIZE 1u
#define FILE_READ_DATA 2u
#define FILE_WRITE_DATA 4u
#define DELETE 8u
#define FILE_READ_ATTRIBUTES 16u
#define OBJ_CASE_INSENSITIVE 0
#define OBJ_KERNEL_HANDLE 0
#define FILE_ATTRIBUTE_NORMAL 0
#define FILE_SHARE_READ 0
#define FILE_SHARE_WRITE 0
#define FILE_OPEN 1u
#define FILE_OPEN_IF 2u
#define FILE_OVERWRITE_IF 3u
#define FILE_SYNCHRONOUS_IO_NONALERT 0
#define FILE_NON_DIRECTORY_FILE 0
#define FILE_DIRECTORY_FILE 1u
#define FILE_WRITE_THROUGH 0
#define FileEndOfFileInformation 1
#define FileDispositionInformation 2
#define FileStandardInformation 3
#define InitializeObjectAttributes(a,p,f,r,s) ((a)->ObjectName=(p))
static inline SIZE_T RtlCompareMemory(const void *a,const void *b,SIZE_T n) { SIZE_T i=0;while(i<n && ((const UCHAR *)a)[i]==((const UCHAR *)b)[i])i++;return i; }
static inline void RtlInitUnicodeString(UNICODE_STRING *s,const WCHAR *p) {s->Buffer=p;}
static inline NTSTATUS RtlStringCbPrintfW(WCHAR *b,SIZE_T n,const WCHAR *format,...) {
    (void)format;va_list ap;va_start(ap,format);const WCHAR *file=va_arg(ap,const WCHAR *);int r=swprintf(b,n/sizeof(*b),L"\\SystemRoot\\System32\\jiiov-fp\\persist\\%ls",file);va_end(ap);return r<0?STATUS_INVALID_PARAMETER:STATUS_SUCCESS;
}
NTSTATUS ZwCreateFile(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *,IO_STATUS_BLOCK *,PVOID,ULONG,ULONG,ULONG,ULONG,PVOID,ULONG);
NTSTATUS ZwReadFile(HANDLE,PVOID,PVOID,PVOID,IO_STATUS_BLOCK *,PVOID,ULONG,LARGE_INTEGER *,PVOID);
NTSTATUS ZwWriteFile(HANDLE,PVOID,PVOID,PVOID,IO_STATUS_BLOCK *,PVOID,ULONG,LARGE_INTEGER *,PVOID);
NTSTATUS ZwSetInformationFile(HANDLE,IO_STATUS_BLOCK *,PVOID,ULONG,ULONG);
NTSTATUS ZwQueryInformationFile(HANDLE,IO_STATUS_BLOCK *,PVOID,ULONG,ULONG);
VOID ZwClose(HANDLE);
