#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#define RPMH_LOCK_TEST
typedef LONG NTSTATUS;
typedef PVOID PKEVENT;
typedef struct { USHORT Length,MaximumLength;PWSTR Buffer; } UNICODE_STRING;
#define RTL_CONSTANT_STRING(s) {sizeof(s)-sizeof(WCHAR),sizeof(s),(PWSTR)(s)}
#define PASSIVE_LEVEL 0
#define Executive 0
#define KernelMode 0
#define IO_NO_INCREMENT 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xc00000b5)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xc0000001)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009a)
#define STATUS_RESOURCE_NOT_OWNED ((NTSTATUS)0xc0000264)
#define NT_SUCCESS(s) ((NTSTATUS)(s)>=0)
static WCHAR Name[128];
static volatile LONG Handles,Signals,FailCreate,Active,Counter;
static __declspec(thread) int TestIrql,CriticalDepth;
static int KeGetCurrentIrql(void){return TestIrql;}
static PVOID KeGetCurrentThread(void){return (PVOID)(uintptr_t)GetCurrentThreadId();}
static void KeEnterCriticalRegion(void){CriticalDepth++;}
static void KeLeaveCriticalRegion(void){assert(CriticalDepth>0);CriticalDepth--;}
static PKEVENT IoCreateSynchronizationEvent(UNICODE_STRING *name,PHANDLE handle) {
    assert(wcscmp(name->Buffer,L"\\Device\\AnyLaySys.AppsRsc.Drv2.Serialization.v1")==0);
    if(FailCreate)return NULL;
    *handle=CreateEventW(NULL,FALSE,TRUE,Name); /* existing state is preserved */
    if(*handle)InterlockedIncrement(&Handles);return *handle;
}
static NTSTATUS KeWaitForSingleObject(PVOID event,int why,int mode,BOOLEAN alert,LARGE_INTEGER *timeout) {
    (void)why;(void)mode;assert(!alert && timeout->QuadPart<0);
    DWORD status=WaitForSingleObject(event,(DWORD)(-timeout->QuadPart/10000));
    return status==WAIT_OBJECT_0?STATUS_SUCCESS:status==WAIT_TIMEOUT?STATUS_TIMEOUT:STATUS_UNSUCCESSFUL;
}
static LONG KeSetEvent(PKEVENT event,LONG increment,BOOLEAN wait) {
    assert(!increment && !wait);assert(SetEvent(event));InterlockedIncrement(&Signals);return 0;
}
static NTSTATUS ZwClose(HANDLE handle) {
    assert(CloseHandle(handle));InterlockedDecrement(&Handles);return STATUS_SUCCESS;
}
#include "../rpmh-lock.h"
static DWORD WINAPI TimeoutOwner(PVOID parameter) {
    RPMH_LOCK_GUARD *other=parameter;
    assert(RpmhLockRelease(other)==STATUS_RESOURCE_NOT_OWNED);
    RPMH_LOCK_GUARD mine={0};ULONGLONG start=GetTickCount64();LONG signals=Signals;
    assert(RpmhLockAcquire(&mine)==STATUS_IO_TIMEOUT);
    assert(GetTickCount64()-start>=400 && Signals==signals && !mine.Handle && !mine.Owned);
    assert(RpmhLockRelease(&mine)==STATUS_SUCCESS && Signals==signals);
    assert(!CriticalDepth);return 0;
}
static DWORD WINAPI CompetingOwner(PVOID unused) {
    (void)unused;
    for(unsigned i=0;i<200;i++) {
        RPMH_LOCK_GUARD guard={0};assert(RpmhLockAcquire(&guard)==STATUS_SUCCESS);
        assert(InterlockedIncrement(&Active)==1);
        LONG value=Counter;SwitchToThread();Counter=value+1;
        assert(InterlockedDecrement(&Active)==0);
        assert(RpmhLockRelease(&guard)==STATUS_SUCCESS);
        assert(!CriticalDepth && !guard.Handle);
    }
    return 0;
}
static int FailInsideOwner(void) {
    RPMH_LOCK_GUARD guard={0};assert(RpmhLockAcquire(&guard)==STATUS_SUCCESS);
    __try {RaiseException(0xe0000001,0,0,NULL);}
    __finally {assert(RpmhLockRelease(&guard)==STATUS_SUCCESS);}
    return 0;
}
int main(void) {
    swprintf_s(Name,128,L"Local\\WoA.RpmhLock.Test.%lu",GetCurrentProcessId());
    DWORD before,after;assert(GetProcessHandleCount(GetCurrentProcess(),&before));
    RPMH_LOCK_GUARD guard={0};TestIrql=2;
    assert(RpmhLockAcquire(&guard)==STATUS_INVALID_DEVICE_STATE && !Handles);TestIrql=0;
    FailCreate=1;assert(RpmhLockAcquire(&guard)==STATUS_INSUFFICIENT_RESOURCES && !Handles);FailCreate=0;
    assert(RpmhLockAcquire(&guard)==STATUS_SUCCESS);
    HANDLE t=CreateThread(NULL,0,TimeoutOwner,&guard,0,NULL);assert(t);
    assert(WaitForSingleObject(t,3000)==WAIT_OBJECT_0);CloseHandle(t);
    assert(guard.Owned && Handles==1 && CriticalDepth==1);
    assert(RpmhLockRelease(&guard)==STATUS_SUCCESS);
    __try {FailInsideOwner();assert(0);}
    __except(EXCEPTION_EXECUTE_HANDLER) {assert(GetExceptionCode()==0xe0000001);}
    assert(!Handles && !CriticalDepth);
    HANDLE threads[4];for(unsigned i=0;i<4;i++){threads[i]=CreateThread(NULL,0,CompetingOwner,NULL,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;i++)CloseHandle(threads[i]);
    assert(Counter==800 && !Active && !Handles);
    assert(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);
    puts("RPMh shared event: 800 competing owners, preserved existing state, bounded timeout, wrong-owner rejection, SEH cleanup, no handle leak PASS");
    return 0;
}
