#pragma once
#include <ntddk.h>

typedef struct {
    HANDLE Handle;
    PKEVENT Event;
    PVOID Owner;
    BOOLEAN Owned;
} PLATFORM_LOCK_GUARD;

static NTSTATUS PlatformLockAcquire(PLATFORM_LOCK_GUARD *guard,BOOLEAN smmu) {
    if(!guard || KeGetCurrentIrql()!=PASSIVE_LEVEL || guard->Handle || guard->Owned)return STATUS_INVALID_DEVICE_STATE;

    UNICODE_STRING name;RtlInitUnicodeString(&name,smmu?L"\\Device\\AnyLaySys.AppsSmmu.Serialization.v1":L"\\Device\\AnyLaySys.AppsRsc.Drv2.Serialization.v1");
    KeEnterCriticalRegion();
    guard->Event=IoCreateSynchronizationEvent(&name,&guard->Handle);
    if(!guard->Event){guard->Handle=NULL;KeLeaveCriticalRegion();return STATUS_INSUFFICIENT_RESOURCES;}
    LARGE_INTEGER timeout;timeout.QuadPart=-5000000;
    NTSTATUS status=KeWaitForSingleObject(guard->Event,Executive,KernelMode,FALSE,&timeout);
    if(status!=STATUS_SUCCESS) {
        ZwClose(guard->Handle);guard->Handle=NULL;guard->Event=NULL;
        KeLeaveCriticalRegion();return status==STATUS_TIMEOUT?STATUS_IO_TIMEOUT:status;
    }
    guard->Owner=KeGetCurrentThread();guard->Owned=TRUE;return STATUS_SUCCESS;
}

static NTSTATUS PlatformLockRelease(PLATFORM_LOCK_GUARD *guard) {
    if(!guard || !guard->Owned)return STATUS_SUCCESS;
    if(guard->Owner!=KeGetCurrentThread())return STATUS_RESOURCE_NOT_OWNED;
    if(KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_DEVICE_STATE;
    KeSetEvent(guard->Event,IO_NO_INCREMENT,FALSE);
    NTSTATUS status=ZwClose(guard->Handle);
    guard->Handle=NULL;guard->Event=NULL;guard->Owner=NULL;guard->Owned=FALSE;
    KeLeaveCriticalRegion();return status;
}
