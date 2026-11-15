#include "../wddm/adapter.h"
#include "../wddm/capture-layout.h"
#include <stdlib.h>
#include <stdio.h>
static ULONG allocations,frees,failAfter=MAXULONG;
static void Check(int value){if(!value)abort();}
static PVOID NTAPI AllocatePool(POOL_FLAGS flags,SIZE_T bytes,ULONG tag){UNREFERENCED_PARAMETER(flags);UNREFERENCED_PARAMETER(tag);return calloc(1,bytes);}
static VOID NTAPI FreePool(PVOID p,ULONG tag){UNREFERENCED_PARAMETER(tag);free(p);}
PVOID (NTAPI *__imp_ExAllocatePool2)(POOL_FLAGS,SIZE_T,ULONG)=AllocatePool;
VOID (NTAPI *__imp_ExFreePoolWithTag)(PVOID,ULONG)=FreePool;
static NTSTATUS NTAPI Open(PHANDLE key,ACCESS_MASK access,POBJECT_ATTRIBUTES attributes){UNREFERENCED_PARAMETER(access);UNREFERENCED_PARAMETER(attributes);*key=(HANDLE)(ULONG_PTR)1;return STATUS_SUCCESS;}
static NTSTATUS NTAPI Query(HANDLE key,PUNICODE_STRING name,KEY_VALUE_INFORMATION_CLASS type,PVOID buffer,ULONG bytes,PULONG used){UNREFERENCED_PARAMETER(key);UNREFERENCED_PARAMETER(name);UNREFERENCED_PARAMETER(type);Check(bytes>=sizeof(KEY_VALUE_PARTIAL_INFORMATION)+4);PKEY_VALUE_PARTIAL_INFORMATION value=buffer;value->Type=REG_DWORD;value->DataLength=4;*(PULONG)value->Data=1;*used=sizeof(KEY_VALUE_PARTIAL_INFORMATION)+4;return STATUS_SUCCESS;}
static NTSTATUS NTAPI Close(HANDLE key){UNREFERENCED_PARAMETER(key);return STATUS_SUCCESS;}
NTSTATUS (NTAPI *__imp_ZwOpenKey)(PHANDLE,ACCESS_MASK,POBJECT_ATTRIBUTES)=Open;
NTSTATUS (NTAPI *__imp_ZwQueryValueKey)(HANDLE,PUNICODE_STRING,KEY_VALUE_INFORMATION_CLASS,PVOID,ULONG,PULONG)=Query;
NTSTATUS (NTAPI *__imp_ZwClose)(HANDLE)=Close;
static VOID NTAPI Acquire(PFAST_MUTEX lock){UNREFERENCED_PARAMETER(lock);}
static VOID NTAPI Release(PFAST_MUTEX lock){UNREFERENCED_PARAMETER(lock);}
VOID (NTAPI *__imp_ExAcquireFastMutex)(PFAST_MUTEX)=Acquire;
VOID (NTAPI *__imp_ExReleaseFastMutex)(PFAST_MUTEX)=Release;
static VOID NTAPI Event(PRKEVENT event,EVENT_TYPE type,BOOLEAN state){UNREFERENCED_PARAMETER(event);UNREFERENCED_PARAMETER(type);UNREFERENCED_PARAMETER(state);}
VOID (NTAPI *__imp_KeInitializeEvent)(PRKEVENT,EVENT_TYPE,BOOLEAN)=Event;
static PVOID NTAPI Contiguous(SIZE_T bytes,PHYSICAL_ADDRESS low,PHYSICAL_ADDRESS high,PHYSICAL_ADDRESS boundary,MEMORY_CACHING_TYPE cache){UNREFERENCED_PARAMETER(low);UNREFERENCED_PARAMETER(boundary);Check(high.QuadPart==MAXULONG && cache==MmWriteCombined);if(allocations>=failAfter)return NULL;allocations++;return calloc(1,bytes);}
PVOID (NTAPI *__imp_MmAllocateContiguousMemorySpecifyCache)(SIZE_T,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,PHYSICAL_ADDRESS,MEMORY_CACHING_TYPE)=Contiguous;
static VOID NTAPI FreeContiguous(PVOID p){free(p);frees++;}
VOID (NTAPI *__imp_MmFreeContiguousMemory)(PVOID)=FreeContiguous;
static PMDL NTAPI Mdl(PVOID address,ULONG bytes,BOOLEAN secondary,BOOLEAN quota,PIRP irp){UNREFERENCED_PARAMETER(secondary);UNREFERENCED_PARAMETER(quota);UNREFERENCED_PARAMETER(irp);PMDL mdl=calloc(1,sizeof(*mdl));mdl->MappedSystemVa=address;mdl->ByteCount=bytes;return mdl;}
PMDL (NTAPI *__imp_IoAllocateMdl)(PVOID,ULONG,BOOLEAN,BOOLEAN,PIRP)=Mdl;
static VOID NTAPI Build(PMDL mdl){UNREFERENCED_PARAMETER(mdl);}
VOID (NTAPI *__imp_MmBuildMdlForNonPagedPool)(PMDL)=Build;
static PHYSICAL_ADDRESS NTAPI Physical(PVOID address){UNREFERENCED_PARAMETER(address);PHYSICAL_ADDRESS p;p.QuadPart=0x10000000ULL+(ULONGLONG)allocations*0x100000;return p;}
PHYSICAL_ADDRESS (NTAPI *__imp_MmGetPhysicalAddress)(PVOID)=Physical;
VOID GpuFree(GPU_BO *b){if(b->Mdl){FreeContiguous(b->Cpu);free(b->Mdl);}RtlZeroMemory(b,sizeof(*b));}
#include "../wddm/scanout.c"
int main(void) {
    A830_ADAPTER *a=calloc(1,sizeof(*a));Check(a!=NULL);a->DisplayInfo.Width=16;a->DisplayInfo.Height=16;
    Check(NT_SUCCESS(A830ScanoutStart(a)) && allocations==11 && A830ScanoutStagingReady(a));
    GPU_BO stage[2],buffers[10]={0};
    Check(NT_SUCCESS(A830ScanoutStaging(a,0,stage)) && NT_SUCCESS(A830ScanoutStaging(a,1,stage+1)));
    Check(stage[0].Physical!=stage[1].Physical && stage[0].Contiguous && stage[1].Physical<=MAXULONG);
    PUCHAR captured=NULL;
    Check(A830CaptureBo(stage,stage[0].Physical,256,16,16,&captured) && captured==stage[0].Cpu);
    Check(A830CaptureBo(stage,stage[0].Physical+64,256,16,16,&captured) && captured==stage[0].Cpu+64);
    Check(!A830CaptureBo(stage,stage[0].Physical+stage[0].Size-1,256,16,16,&captured));
    Check(!A830CaptureBo(stage,stage[0].Physical-1,256,16,16,&captured));
    Check(!A830CaptureBo(stage,stage[0].Physical,60,16,16,&captured));
    GPU_BO scattered=stage[0];scattered.Contiguous=FALSE;
    Check(!A830CaptureBo(&scattered,scattered.Physical,256,16,16,&captured));
    for(ULONG i=0;i<10;i++)Check(NT_SUCCESS(A830ScanoutAllocate(a,buffers+i,0x4000)));
    Check(allocations==12 && !A830ScanoutFree(a,buffers+9));GpuFree(buffers+9);
    for(ULONG i=0;i<9;i++)Check(A830ScanoutFree(a,buffers+i));
    A830ScanoutStop(a);Check(!a->ScanoutPool && frees==allocations);
    allocations=frees=0;failAfter=4;
    Check(NT_SUCCESS(A830ScanoutStart(a)) && A830ScanoutStagingReady(a));
    Check(NT_SUCCESS(A830ScanoutAllocate(a,buffers,0x4000)) && NT_SUCCESS(A830ScanoutAllocate(a,buffers+1,0x4000)));
    Check(A830ScanoutAllocate(a,buffers+2,0x4000)==STATUS_INSUFFICIENT_RESOURCES);
    A830ScanoutStop(a);Check(a->ScanoutPool!=NULL);
    Check(A830ScanoutFree(a,buffers) && A830ScanoutFree(a,buffers+1));Check(!a->ScanoutPool && frees==allocations);
    allocations=frees=0;failAfter=1;
    Check(NT_SUCCESS(A830ScanoutStart(a)) && !a->ScanoutPool && !A830ScanoutStagingReady(a) && frees==allocations);
    free(a);puts("Scanout: two low4GB staging frames, nine primary slots, overflow contiguous BO, partial pool OOM, staging OOM refusal, ordered free: PASS");return 0;
}
