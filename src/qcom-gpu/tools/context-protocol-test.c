#include "../gpu.h"
#include "../regs.h"
#include <stdio.h>
#include <stdlib.h>
static ULONG spinDepth,waitCalls;
static void Check(int value){if(!value)abort();}
static KIRQL NTAPI Acquire(PKSPIN_LOCK lock){UNREFERENCED_PARAMETER(lock);Check(!spinDepth);spinDepth++;return 0;}
static VOID NTAPI Release(PKSPIN_LOCK lock,KIRQL old){UNREFERENCED_PARAMETER(lock);UNREFERENCED_PARAMETER(old);Check(spinDepth==1);spinDepth--;}
KIRQL (NTAPI *__imp_KeAcquireSpinLockRaiseToDpc)(PKSPIN_LOCK)=Acquire;
VOID (NTAPI *__imp_KeReleaseSpinLock)(PKSPIN_LOCK,KIRQL)=Release;
static NTSTATUS NTAPI Wait(PVOID object,KWAIT_REASON reason,KPROCESSOR_MODE mode,BOOLEAN alert,PLARGE_INTEGER timeout){UNREFERENCED_PARAMETER(object);UNREFERENCED_PARAMETER(reason);UNREFERENCED_PARAMETER(mode);UNREFERENCED_PARAMETER(alert);UNREFERENCED_PARAMETER(timeout);waitCalls++;abort();}
NTSTATUS (NTAPI *__imp_KeWaitForSingleObject)(PVOID,KWAIT_REASON,KPROCESSOR_MODE,BOOLEAN,PLARGE_INTEGER)=Wait;
static LONG NTAPI Mutex(PRKMUTEX mutex,BOOLEAN wait){UNREFERENCED_PARAMETER(mutex);UNREFERENCED_PARAMETER(wait);abort();}
LONG (NTAPI *__imp_KeReleaseMutex)(PRKMUTEX,BOOLEAN)=Mutex;
static VOID NTAPI Free(PVOID object,ULONG tag){UNREFERENCED_PARAMETER(object);UNREFERENCED_PARAMETER(tag);abort();}
VOID (NTAPI *__imp_ExFreePoolWithTag)(PVOID,ULONG)=Free;
ULONG GpuRead(PUCHAR base,ULONG offset){UNREFERENCED_PARAMETER(base);Check(spinDepth==1 && offset==REG_A6XX_CP_RB_RPTR*4);return 0;}
VOID GpuWrite(PUCHAR base,ULONG offset,ULONG value){UNREFERENCED_PARAMETER(base);UNREFERENCED_PARAMETER(value);Check(spinDepth==1 && offset==REG_A6XX_CP_RB_WPTR*4);}
NTSTATUS GpuPoll(PUCHAR base,ULONG offset,ULONG mask,ULONG value,ULONG timeout){UNREFERENCED_PARAMETER(base);UNREFERENCED_PARAMETER(offset);UNREFERENCED_PARAMETER(mask);UNREFERENCED_PARAMETER(value);UNREFERENCED_PARAMETER(timeout);abort();}
NTSTATUS GpuPower(GPU *g,BOOLEAN enabled){UNREFERENCED_PARAMETER(g);UNREFERENCED_PARAMETER(enabled);abort();}
NTSTATUS GpuMmuStart(GPU *g){UNREFERENCED_PARAMETER(g);abort();}
VOID GpuMmuStop(GPU *g){UNREFERENCED_PARAMETER(g);abort();}
NTSTATUS GpuGmuStart(GPU *g){UNREFERENCED_PARAMETER(g);abort();}
VOID GpuGmuStop(GPU *g){UNREFERENCED_PARAMETER(g);abort();}
NTSTATUS GpuAllocate(GPU_BO *b,ULONG size){UNREFERENCED_PARAMETER(b);UNREFERENCED_PARAMETER(size);abort();}
VOID GpuFree(GPU_BO *b){UNREFERENCED_PARAMETER(b);abort();}
NTSTATUS GpuMap(GPU *g,ULONG vm,GPU_BO *b,ULONGLONG address){UNREFERENCED_PARAMETER(g);UNREFERENCED_PARAMETER(vm);UNREFERENCED_PARAMETER(b);UNREFERENCED_PARAMETER(address);abort();}
NTSTATUS GpuFirmware(PCWSTR name,PUCHAR *data,PULONG size){UNREFERENCED_PARAMETER(name);UNREFERENCED_PARAMETER(data);UNREFERENCED_PARAMETER(size);abort();}
static ULONGLONG TestTime(VOID){return 0;}
#define KeQueryInterruptTime TestTime
#ifndef MAXULONGLONG
#define MAXULONGLONG ((ULONGLONG)-1)
#endif
#pragma warning(push)
#pragma warning(disable:4702)
#include "../gpu.c"
#pragma warning(pop)
static ULONG Count(const ULONG *words,ULONG begin,ULONG end,ULONG op) {
    ULONG found=0;
    for(ULONG i=begin;i<end;) {ULONG header=words[i++],n=header&0x3fff;Check(i+n<=end);if(((header>>16)&0x7f)==op)found++;i+=n;}
    return found;
}
int main(void) {
    GPU g={0};ULONG words[8192]={0};ULONGLONG completed=0;GPU_BO bo={0};GPU_CONTEXT_PROTOCOL state,policy={1,sizeof(policy),1,0,0,0};
    g.Online=TRUE;g.Ring.Cpu=(PUCHAR)words;g.Ring.Mdl=(PMDL)(ULONG_PTR)1;g.Memory.Cpu=(PUCHAR)&completed;g.Memory.Mdl=(PMDL)(ULONG_PTR)1;g.Memory.Address=0x40000000;
    bo.Mdl=(PMDL)(ULONG_PTR)1;bo.Size=4096;bo.Address=0x100001000ULL;GPU_COMMAND command={&bo,0,4};
    Check(NT_SUCCESS(GpuContextProtocol(&g,NULL,&state)) && !state.Enabled && !state.Switches);
    Check(NT_SUCCESS(GpuSubmitContextBatch(&g,&command,1,9)));ULONG first=g.WriteIndex;
    Check(!Count(words,0,first,0x1f));
    Check(NT_SUCCESS(GpuContextProtocol(&g,&policy,&state)) && state.Enabled && !state.CookieValid);
    Check(NT_SUCCESS(GpuSubmitContextBatch(&g,&command,1,9)));ULONG second=g.WriteIndex;
    Check(Count(words,first,second,0x1f)==1 && words[first+2]==Packet(0x14,4) && words[first+6]==1);
    Check(words[first+7]==Packet(0x17,1) && words[first+8]==0x80000003u && words[first+10]==0xf && words[first+12]==0x80000003u);
    Check(NT_SUCCESS(GpuSubmitContextBatch(&g,&command,1,9)));ULONG third=g.WriteIndex;
    Check(!Count(words,second,third,0x1f) && g.ContextSwitchCount==1);
    Check(NT_SUCCESS(GpuSubmitContextBatch(&g,&command,1,10)));Check(g.ContextSwitchCount==2);
    Check(NT_SUCCESS(GpuSubmitBatch(&g,&command,1)));Check(g.ContextSwitchCount==3 && g.LastContextCookie==0);
    policy.Enabled=0;Check(NT_SUCCESS(GpuContextProtocol(&g,&policy,&state)) && !state.Enabled && !state.CookieValid);
    ULONG begin=g.WriteIndex;Check(NT_SUCCESS(GpuSubmitContextBatch(&g,&command,1,12)));
    Check(!Count(words,begin,g.WriteIndex,0x1f) && g.ContextSwitchCount==3 && !waitCalls);
    policy.Enabled=2;Check(GpuContextProtocol(&g,&policy,&state)==STATUS_INVALID_PARAMETER);
    policy.Enabled=1;Check(NT_SUCCESS(GpuContextProtocol(&g,&policy,&state)));
    begin=g.WriteIndex;g.WriteIndex=8191-60; /* Base submission fits, switched submission does not. */
    Check(GpuSubmitContextBatch(&g,&command,1,12)==STATUS_DEVICE_BUSY && g.ContextSwitchCount==3);
    g.WriteIndex=begin;GPU_U32 packet[11];Check(!GpuBuildContextPreamble(packet,10,g.Memory.Address,1));
    Check(GpuBuildContextPreamble(packet,11,g.Memory.Address,1)==11 && GpuBuildContextPreamble(packet,6,0,0)==6);
    puts("Actual GPU submission: default off, live toggle, switch-only WAIT/SYNC/RESET, same-cookie async, internal cookie0, disabled no packet, bounded ring, zero CPU waits: PASS");return 0;
}
