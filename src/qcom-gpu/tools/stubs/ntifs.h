#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef void VOID,*PVOID,*PMDL;
typedef unsigned char BOOLEAN,*PUCHAR;
typedef uint32_t ULONG,*PULONG;
typedef int32_t LONG,NTSTATUS;
typedef uint64_t ULONGLONG,*PULONGLONG;
typedef const char *PCSTR;
typedef const wchar_t *PCWSTR;
typedef struct { unsigned Value; } KEVENT;
typedef struct { int64_t QuadPart; } PHYSICAL_ADDRESS;
#define __forceinline inline
#define TRUE 1
#define FALSE 0
#define SynchronizationEvent 0
#define IO_NO_INCREMENT 0
#define PAGE_READWRITE 4
#define PAGE_NOCACHE 0x200
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3)
#define STATUS_RETRY ((NTSTATUS)0xC000022D)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009A)
#define STATUS_DEVICE_HARDWARE_ERROR ((NTSTATUS)0xC0000185)
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
ULONG TestRead(PULONG address);
VOID TestWrite(PULONG address,ULONG value);
#define READ_REGISTER_ULONG(p) TestRead(p)
#define WRITE_REGISTER_ULONG(p,v) TestWrite(p,v)
static inline VOID KeInitializeEvent(KEVENT *event,int type,BOOLEAN state){(void)type;event->Value=state;}
static inline ULONG KeSetEvent(KEVENT *event,int increment,BOOLEAN wait){(void)increment;(void)wait;event->Value=1;return 0;}
static inline LONG InterlockedIncrement(volatile LONG *value){return ++*value;}
static inline ULONGLONG KeQueryInterruptTime(void){static ULONGLONG time;return ++time;}
static inline VOID KeStallExecutionProcessor(ULONG duration){(void)duration;}
static inline VOID MmUnmapIoSpace(PVOID base,ULONG length){(void)base;(void)length;}
static inline PVOID MmMapIoSpaceEx(PHYSICAL_ADDRESS address,ULONG length,ULONG flags){(void)address;(void)length;(void)flags;return NULL;}
