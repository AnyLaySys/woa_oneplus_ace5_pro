#include "../gpu.h"
#include <stdio.h>
#include <stdlib.h>
#define WINAPI __stdcall
typedef int BOOL;
__declspec(dllimport) HANDLE WINAPI CreateMutexW(PVOID,BOOL,PCWSTR);
__declspec(dllimport) HANDLE WINAPI CreateEventW(PVOID,BOOL,BOOL,PCWSTR);
__declspec(dllimport) ULONG WINAPI WaitForSingleObject(HANDLE,ULONG);
__declspec(dllimport) BOOL WINAPI SetEvent(HANDLE);
__declspec(dllimport) BOOL WINAPI CloseHandle(HANDLE);
__declspec(dllimport) BOOL WINAPI ReleaseMutex(HANDLE);
__declspec(dllimport) HANDLE WINAPI CreateThread(PVOID,SIZE_T,ULONG (WINAPI *)(PVOID),PVOID,ULONG,PULONG);
static HANDLE mutex,inside,proceed,stopStarted,stopDone;
static BOOLEAN pauseMap;static NTSTATUS mapStatus;static GPU *expected;
static void Check(int value){if(!value)abort();}
static KIRQL TestIrql(VOID){return PASSIVE_LEVEL;}
#define KeGetCurrentIrql TestIrql
static NTSTATUS NTAPI KernelWait(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alertable,PLARGE_INTEGER timeout){UNREFERENCED_PARAMETER(reason);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(alertable);Check(object==&expected->MappingLock && !timeout);Check(WaitForSingleObject(mutex,5000)==0);return STATUS_SUCCESS;}
static LONG NTAPI KernelRelease(PRKMUTEX object,BOOLEAN wait){UNREFERENCED_PARAMETER(wait);Check(object==&expected->MappingLock);Check(ReleaseMutex(mutex));return 0;}
NTSTATUS (NTAPI *__imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=KernelWait;
LONG (NTAPI *__imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=KernelRelease;
static VOID NTAPI FreeContiguous(PVOID p){free(p);}
VOID (NTAPI *__imp_MmFreeContiguousMemory)(PVOID)=FreeContiguous;
static VOID NTAPI FreePool(PVOID p,ULONG tag){UNREFERENCED_PARAMETER(tag);free(p);}
VOID (NTAPI *__imp_ExFreePoolWithTag)(PVOID,ULONG)=FreePool;
static VOID NTAPI FreePlain(PVOID p){free(p);}
VOID (NTAPI *__imp_ExFreePool)(PVOID)=FreePlain;
static VOID NTAPI Unmap(PVOID cpu,PMDL mdl){UNREFERENCED_PARAMETER(cpu);UNREFERENCED_PARAMETER(mdl);abort();}
VOID (NTAPI *__imp_MmUnmapLockedPages)(PVOID,PMDL)=Unmap;
static VOID NTAPI FreePages(PMDL mdl){UNREFERENCED_PARAMETER(mdl);abort();}
VOID (NTAPI *__imp_MmFreePagesFromMdl)(PMDL)=FreePages;
static PMDL NTAPI Pages(PHYSICAL_ADDRESS low,PHYSICAL_ADDRESS high,PHYSICAL_ADDRESS skip,SIZE_T bytes,MEMORY_CACHING_TYPE cache,ULONG flags){UNREFERENCED_PARAMETER(low);UNREFERENCED_PARAMETER(high);UNREFERENCED_PARAMETER(skip);UNREFERENCED_PARAMETER(bytes);UNREFERENCED_PARAMETER(cache);UNREFERENCED_PARAMETER(flags);abort();}
PMDL (NTAPI *__imp_MmAllocatePagesForMdlEx)(PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,SIZE_T,MEMORY_CACHING_TYPE,ULONG)=Pages;
static PVOID NTAPI Map(PMDL mdl,KPROCESSOR_MODE mode,MEMORY_CACHING_TYPE cache,PVOID address,ULONG bugcheck,ULONG priority){UNREFERENCED_PARAMETER(mdl);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(cache);UNREFERENCED_PARAMETER(address);UNREFERENCED_PARAMETER(bugcheck);UNREFERENCED_PARAMETER(priority);abort();}
PVOID (NTAPI *__imp_MmMapLockedPagesSpecifyCache)(PMDL,KPROCESSOR_MODE,MEMORY_CACHING_TYPE,PVOID,ULONG,ULONG)=Map;
static VOID NTAPI FlushBuffers(PMDL mdl,BOOLEAN read,BOOLEAN dma){UNREFERENCED_PARAMETER(mdl);UNREFERENCED_PARAMETER(read);UNREFERENCED_PARAMETER(dma);abort();}
VOID NTAPI KeFlushIoBuffers(PMDL mdl,BOOLEAN read,BOOLEAN dma){FlushBuffers(mdl,read,dma);}
VOID (NTAPI *__imp_KeFlushIoBuffers)(PMDL,BOOLEAN,BOOLEAN)=FlushBuffers;
static PVOID NTAPI Contiguous(SIZE_T bytes,PHYSICAL_ADDRESS low,PHYSICAL_ADDRESS high,PHYSICAL_ADDRESS skip,MEMORY_CACHING_TYPE cache){UNREFERENCED_PARAMETER(bytes);UNREFERENCED_PARAMETER(low);UNREFERENCED_PARAMETER(high);UNREFERENCED_PARAMETER(skip);UNREFERENCED_PARAMETER(cache);abort();}
PVOID (NTAPI *__imp_MmAllocateContiguousMemorySpecifyCache)(SIZE_T,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,MEMORY_CACHING_TYPE)=Contiguous;
static PMDL NTAPI AllocateMdl(PVOID cpu,ULONG size,BOOLEAN secondary,BOOLEAN quota,PIRP irp){UNREFERENCED_PARAMETER(cpu);UNREFERENCED_PARAMETER(size);UNREFERENCED_PARAMETER(secondary);UNREFERENCED_PARAMETER(quota);UNREFERENCED_PARAMETER(irp);abort();}
PMDL (NTAPI *__imp_IoAllocateMdl)(PVOID,ULONG,BOOLEAN,BOOLEAN,PIRP)=AllocateMdl;
static VOID NTAPI BuildMdl(PMDL mdl){UNREFERENCED_PARAMETER(mdl);abort();}
VOID (NTAPI *__imp_MmBuildMdlForNonPagedPool)(PMDL)=BuildMdl;
static PHYSICAL_ADDRESS NTAPI Physical(PVOID p){UNREFERENCED_PARAMETER(p);abort();}
PHYSICAL_ADDRESS (NTAPI *__imp_MmGetPhysicalAddress)(PVOID)=Physical;
VOID (NTAPI *__imp_IoFreeMdl)(PMDL)=BuildMdl;
ULONG GpuRead(PUCHAR base,ULONG offset){UNREFERENCED_PARAMETER(base);UNREFERENCED_PARAMETER(offset);return 0;}
VOID GpuWrite(PUCHAR base,ULONG offset,ULONG value){UNREFERENCED_PARAMETER(base);UNREFERENCED_PARAMETER(offset);UNREFERENCED_PARAMETER(value);}
NTSTATUS GpuPoll(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG timeout){UNREFERENCED_PARAMETER(base);UNREFERENCED_PARAMETER(offset);UNREFERENCED_PARAMETER(mask);UNREFERENCED_PARAMETER(value);UNREFERENCED_PARAMETER(timeout);if(pauseMap){SetEvent(inside);Check(WaitForSingleObject(proceed,5000)==0);}return STATUS_SUCCESS;}
VOID GpuStop(GPU *g){GpuMmuStop(g);}
#pragma warning(push)
#pragma warning(disable:4702)
#include "../mmu.c"
#pragma warning(pop)
typedef struct {GPU *Gpu;GPU_BO *Buffer;} MAP_ARGS;
static ULONG WINAPI Mapper(PVOID p){MAP_ARGS *args=p;mapStatus=GpuMap(args->Gpu,0,args->Buffer,0x301900000ULL);return 0;}
static ULONG WINAPI Stopper(PVOID p){SetEvent(stopStarted);GpuMmuStop(p);SetEvent(stopDone);return 0;}
int main(void) {
    GPU g={0};GPU_BO bo={0};expected=&g;
    mutex=CreateMutexW(NULL,FALSE,NULL);inside=CreateEventW(NULL,TRUE,FALSE,NULL);proceed=CreateEventW(NULL,TRUE,FALSE,NULL);
    stopStarted=CreateEventW(NULL,TRUE,FALSE,NULL);stopDone=CreateEventW(NULL,TRUE,FALSE,NULL);Check(mutex && inside && proceed && stopStarted && stopDone);
    g.Registers=calloc(1,GPU_WINDOW);Check(g.Registers!=NULL);
    for(ULONG i=0;i<2;i++){g.Vm[i].Tables.Cpu=calloc(1,GPU_TABLE_BYTES);g.Vm[i].Tables.Mdl=calloc(1,sizeof(MDL));g.Vm[i].Tables.Contiguous=TRUE;g.Vm[i].Tables.Size=GPU_TABLE_BYTES;g.Vm[i].Tables.Physical=0x10000000ULL+i*0x04000000ULL;g.Vm[i].Used=1;}
    bo.Mdl=calloc(1,sizeof(MDL)+sizeof(PFN_NUMBER));bo.Size=4096;MmGetMdlPfnArray(bo.Mdl)[0]=0x12345;
    /* Initialization mappings remain valid while Online=false. */
    Check(NT_SUCCESS(GpuMap(&g,0,&bo,0x301900000ULL)));
    pauseMap=TRUE;MAP_ARGS args={&g,&bo};HANDLE mapping=CreateThread(NULL,0,Mapper,&args,0,NULL);Check(mapping!=NULL && WaitForSingleObject(inside,5000)==0);
    HANDLE stopping=CreateThread(NULL,0,Stopper,&g,0,NULL);Check(stopping!=NULL && WaitForSingleObject(stopStarted,5000)==0);
    /* Stop is genuinely competing from another thread, not a mirrored state
       machine: actual GpuMmuStop cannot free pages while GpuMap holds them. */
    Check(WaitForSingleObject(stopDone,30)==258 && g.Vm[0].Tables.Cpu!=NULL);
    SetEvent(proceed);Check(WaitForSingleObject(mapping,5000)==0 && WaitForSingleObject(stopping,5000)==0 && NT_SUCCESS(mapStatus));
    Check(!g.Vm[0].Tables.Cpu && !g.Vm[0].Tables.Mdl);
    Check(GpuMap(&g,0,&bo,0x301900000ULL)==STATUS_DEVICE_NOT_READY);
    GpuUnmap(&g,0,&bo);Check(!bo.Address);
    free(bo.Mdl);free(g.Registers);CloseHandle(mapping);CloseHandle(stopping);CloseHandle(mutex);CloseHandle(inside);CloseHandle(proceed);CloseHandle(stopStarted);CloseHandle(stopDone);
    puts("Actual GpuMap vs GpuMmuStop threads: table lifetime serialized, initialization Online=false allowed, map/unmap after stop rejected safely: PASS");return 0;
}
