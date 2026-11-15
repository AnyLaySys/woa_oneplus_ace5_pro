#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef PVOID PEX_TIMER;
typedef struct { ULONGLONG Sequence,Time100ns;ULONG Frame,Delta; } GPU_DISPLAY_TE_SAMPLE;
#define GPU_DISPLAY_TE_SAMPLES 64
typedef struct { PVOID Dpu;struct { ULONG Width,Height,Pitch; } DisplayInfo;ULONGLONG ScanoutAddress;volatile LONG PresentCount,Failed; } A830_ADAPTER;
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY ((LONG)0xc00000a3)
#define MAXULONG 0xffffffffu
#define STATUS_IO_TIMEOUT ((LONG)0xc00000b5)
#define STATUS_IO_DEVICE_ERROR ((LONG)0xc0000185)
static struct { ULONG LastStatus,DpuFailStage; } Activation;
static ULONGLONG Now;
static ULONG Regs[0x90000/4],SourceWrites,FlushWrites,WakeCount,GateFailure,ClearFailure;
static ULONGLONG PreciseTime(void){return Now;}
#define KeMemoryBarrier MemoryBarrier
#define ReadAcquire64(p) InterlockedCompareExchange64((volatile LONG64 *)(p),0,0)
/* The generated include contains the production struct, Queue, Restore,
   Complete and AdvanceTransaction function bodies extracted verbatim. */
#include "dpu-transaction-under-test.inc"
static ULONG Read(A830_DPU *d,ULONG offset){(void)d;assert(offset<sizeof(Regs));return Regs[offset/4];}
static void Write(A830_DPU *d,ULONG offset,ULONG value) {
    assert(offset<sizeof(Regs));
    if(offset==0x14+0x4000 || offset==0x14+0x6000 || offset==0x24+0x4000 || offset==0x24+0x6000) {
        assert(!(Regs[(d->Intf+0x288)/4]&(1u<<20)));
        assert(!(Regs[(d->Intf+0x2b4)/4]&(1u<<31)));
        ULONG line=Regs[(d->Intf+0x2b0)/4]&0xffffu;assert(line==0 || line>=2780);SourceWrites++;
    }
    if(offset==d->Ctls[0].Base+0x18)FlushWrites++;
    if(offset==d->Intf+0x808){if(!ClearFailure)Regs[(d->Intf+0x804)/4]&=~value;return;}
    if(offset==0x18){if(!ClearFailure)Regs[0x14/4]&=~value;return;}
    if(GateFailure && offset==d->Intf+0x2b4 && !(value&(1u<<31)))return;
    Regs[offset/4]=value;
}
static ULONG Frame(A830_DPU *d){return Read(d,d->Intf+0xac);}
static ULONG TxFrame(A830_DPU *d){return Frame(d);}
static BOOLEAN FlushComplete(A830_DPU *d){return !(Read(d,d->Ctls[0].Base+0x18)&3);}
static void TraceFlip(A830_DPU *d,ULONG kind,ULONGLONG seq,ULONGLONG physical,ULONGLONG addr,ULONGLONG old,ULONG frame) {
    (void)d;(void)kind;(void)seq;(void)physical;(void)addr;(void)old;(void)frame;
}
static void A830WakeDisplay(A830_ADAPTER *a){(void)a;WakeCount++;}
static void Setup(A830_ADAPTER *a,A830_DPU *d) {
    memset(a,0,sizeof(*a));memset(d,0,sizeof(*d));memset(Regs,0,sizeof(Regs));memset(&Activation,0,sizeof(Activation));
    Now=100000;SourceWrites=FlushWrites=WakeCount=GateFailure=ClearFailure=0;
    a->Dpu=d;a->DisplayInfo.Width=1264;a->DisplayInfo.Height=2780;a->DisplayInfo.Pitch=5056;
    d->Adapter=a;d->Enabled=1;d->Intf=0x35000;d->CtlCount=1;d->Ctls[0].Base=0x15000;d->Ctls[0].Mask=3;
    d->PipeCount=2;d->Pipes[0].Base=0x4000;d->Pipes[1].Base=0x6000;
    d->Pipes[0].OriginalStride=d->Pipes[1].OriginalStride=5056;d->PpMask=0x100;
    d->ActivePhysical=0xf0000000;d->TxFrame=100;
    Regs[(d->Intf+0x288)/4]=0x180037;Regs[(d->Intf+0x2b4)/4]=0x80000001;
    Regs[(d->Intf+0xac)/4]=100;Regs[(d->Intf+0x2b0)/4]=1300;
    Regs[(d->Intf+0x804)/4]=0x4f;Regs[0x14/4]=0x100;
}
static void CommitAndFinish(A830_DPU *d) {
    Regs[(d->Intf+0x2b0)/4]=2780;AdvanceTransaction(d);
    assert(SourceWrites==4 && FlushWrites==1 && d->TransactionState==2);
    assert(Regs[(d->Intf+0x288)/4]==0x180037 && Regs[(d->Intf+0x2b4)/4]==0x80000001);
    assert(!(Regs[(d->Intf+0x804)/4]&1) && !Regs[0x14/4]);
    /* Stale/irrelevant events and frame increments alone cannot retire. */
    Regs[(d->Intf+0xac)/4]=101;AdvanceTransaction(d);assert(d->Applied==0);
    Regs[(d->Intf+0x804)/4]|=1;AdvanceTransaction(d);assert(d->Applied==0);
    Regs[(d->Intf+0x2b0)/4]=110;AdvanceTransaction(d);assert(d->TransactionState==2 && d->Applied==0);
    Regs[(d->Ctls[0].Base+0x18)/4]=0;AdvanceTransaction(d);assert(d->TransactionState==3 && d->Applied==0);
    Regs[0x14/4]=0x200;Regs[(d->Intf+0x2b0)/4]=2780;
    Regs[(d->Ctls[0].Base+0x18)/4]=0;AdvanceTransaction(d);assert(d->Applied==0); /* PP1 is not PP0 */
    Regs[0x14/4]=0x100;AdvanceTransaction(d);assert(d->Applied==2 && d->LatchedCount==1);
}
int main(void) {
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
    A830_ADAPTER a;A830_DPU d;Setup(&a,&d);
    assert(A830DpuQueue(&a,0xf1000000,5120,0x70000000)==STATUS_SUCCESS);
    assert(SourceWrites==0 && FlushWrites==0 && WakeCount==1);AdvanceTransaction(&d);
    assert(d.TransactionState==1 && SourceWrites==0 && d.TransactionPhysical==0xf1000000);
    for(unsigned i=0;i<30;i++){Now+=2000;AdvanceTransaction(&d);assert(SourceWrites==0 && d.Applied==0);}
    /* A newer pending buffer cannot overwrite the in-progress transaction. */
    assert(A830DpuQueue(&a,0xf2000000,5120,0x71000000)==STATUS_SUCCESS);
    assert(d.TransactionPhysical==0xf1000000 && d.PendingPhysical==0xf2000000);
    CommitAndFinish(&d);assert(d.ActivePhysical==0xf1000000 && d.PendingPhysical==0xf2000000);
    Setup(&a,&d);A830DpuQueue(&a,0xf1000000,5120,0x70000000);AdvanceTransaction(&d);
    Now=d.TransactionDeadline;AdvanceTransaction(&d);
    assert(d.TransactionState==4 && Activation.DpuFailStage==11 && SourceWrites==0);
    assert(d.ActivePhysical==0xf0000000 && d.TransactionPhysical==0xf1000000 && d.PendingPhysical==0xf1000000);
    assert(Regs[(d.Intf+0x288)/4]==0x180037 && Regs[(d.Intf+0x2b4)/4]==0x80000001);
    Setup(&a,&d);GateFailure=1;A830DpuQueue(&a,0xf1000000,5120,0x70000000);AdvanceTransaction(&d);
    assert(d.TransactionState==4 && Activation.DpuFailStage==13 && SourceWrites==0);
    Setup(&a,&d);ClearFailure=1;Regs[(d.Intf+0x2b0)/4]=2780;A830DpuQueue(&a,0xf1000000,5120,0x70000000);AdvanceTransaction(&d);
    assert(d.TransactionState==4 && Activation.DpuFailStage==12 && SourceWrites==0);
    Setup(&a,&d);A830DpuQueue(&a,0xf1000000,5120,0x70000000);AdvanceTransaction(&d);RestoreTransaction(&d);
    assert(!d.TransactionGated && Regs[(d.Intf+0x288)/4]==0x180037 && Regs[(d.Intf+0x2b4)/4]==0x80000001);
    puts("DPU production transaction: IRQ publication only, active-TX drain, fresh AF/PP0 completion, pending replacement, timeout/failed gate/failed W1C/stop restore PASS");return 0;
}
