#define RPC_CID 7u
#define RPC_SIZE 0x10000u
#define RPC_DATA (RPC_SIZE-PAGE_SIZE)
typedef struct {ULONG Pid,Tid;ULONGLONG Context;ULONG Handle,Scalar;ULONGLONG Address,Size;} RPC_MESSAGE;
typedef struct {ULONGLONG Pointer,Length;} RPC_ARGUMENT;
typedef struct {ULONG Count,Index;} RPC_BUFFER;
typedef struct {ULONGLONG Address,Size;} RPC_PAGE;
typedef struct {
    volatile LONG State;KEVENT Event;PFILE_OBJECT File;PUCHAR Data;PHYSICAL_ADDRESS Physical;RPC_MESSAGE Message;
    ULONG Offset[16],Result,Pid;BOOLEAN Sent,ReleaseSent;ULONGLONG Release;
} RPC_CALL;
static struct {
    FAST_MUTEX Lock;PSIA_LINK Link;PKEVENT Work;volatile LONG64 FastUntil;ULONG Rcid,Count,Iid[8],Size[8],LocalIid;
    BOOLEAN Opening,Acked,Requested,Free[8],Closing;ULONGLONG Sequence;
    RPC_CALL Call[2];
} Rpc;
VOID QcomRpcInit(VOID) {
    ExInitializeFastMutex(&Rpc.Lock);for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)KeInitializeEvent(&Rpc.Call[i].Event,NotificationEvent,FALSE);
}
BOOLEAN QcomRpcFast(VOID) {
    if(KeQueryInterruptTime()>=(ULONGLONG)InterlockedCompareExchange64(&Rpc.FastUntil,0,0))return FALSE;
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++){LONG state=InterlockedCompareExchange(&Rpc.Call[i].State,0,0);if(state==1 || state==2)return TRUE;}return FALSE;
}
static VOID RpcCancel(PDEVICE_OBJECT device,PIRP irp) {
    UNREFERENCED_PARAMETER(device);RPC_CALL *c=irp->Tail.Overlay.DriverContext[0];KeSetEvent(&c->Event,IO_NO_INCREMENT,FALSE);IoReleaseCancelSpinLock(irp->CancelIrql);
}
NTSTATUS QcomRpcInvoke(PIRP irp,PULONG bytes) {
    PIO_STACK_LOCATION p=IoGetCurrentIrpStackLocation(irp);QCOM_RPC_PACKET *q=irp->AssociatedIrp.SystemBuffer;ULONG base=FIELD_OFFSET(QCOM_RPC_PACKET,Data);
    ULONG input=p->Parameters.DeviceIoControl.InputBufferLength,output=p->Parameters.DeviceIoControl.OutputBufferLength;
    if(input<base || output<base || !q || q->Count>16 || q->Count!=((q->Scalar>>16)&255)+((q->Scalar>>8)&255) || (q->Scalar&255))return STATUS_INVALID_PARAMETER;
    ULONG in=(q->Scalar>>16)&255,used=(40*q->Count+388+127)&~127u;
    for(ULONG i=0;i<q->Count;i++) {
        ULONG limit=i<in?input:output;QCOM_RPC_ARGUMENT *a=&q->Arg[i];
        if(a->Offset<base || a->Offset>limit || a->Length>limit-a->Offset)return STATUS_INVALID_PARAMETER;
        used=(used+127)&~127u;if(a->Length>RPC_DATA-used)return STATUS_BUFFER_TOO_SMALL;used+=a->Length;
    }
    ExAcquireFastMutex(&Rpc.Lock);RPC_CALL *c=NULL;NTSTATUS status=STATUS_DEVICE_BUSY;
    if(!Rpc.Link || Rpc.Closing){status=STATUS_DEVICE_NOT_CONNECTED;goto done;}
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)if(Rpc.Call[i].Release)goto done;
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)if((!Rpc.Call[i].File || Rpc.Call[i].File==p->FileObject) && !InterlockedCompareExchange(&Rpc.Call[i].State,-1,0)){c=&Rpc.Call[i];break;}
    if(!c)goto done;
    if(!c->Data) {
        status=SiaDspMemoryAcquire();if(!NT_SUCCESS(status))goto reset;
        PHYSICAL_ADDRESS low={0},high,boundary={0};high.QuadPart=0xffffffffULL;
        c->Data=MmAllocateContiguousMemorySpecifyCache(RPC_SIZE,low,high,boundary,MmNonCached);
        if(!c->Data){SiaDspMemoryRelease(FALSE);status=STATUS_INSUFFICIENT_RESOURCES;goto reset;}
        c->Physical=MmGetPhysicalAddress(c->Data);
    }
    RtlZeroMemory(c->Data,used);RPC_ARGUMENT *args=(RPC_ARGUMENT*)c->Data;RPC_BUFFER *bufs=(RPC_BUFFER*)(args+q->Count);RPC_PAGE *pages=(RPC_PAGE*)(bufs+q->Count);
    ULONG at=(40*q->Count+388+127)&~127u;
    for(ULONG i=0;i<q->Count;i++) {
        at=(at+127)&~127u;c->Offset[i]=at;args[i].Pointer=(ULONGLONG)(ULONG_PTR)(c->Data+at);args[i].Length=q->Arg[i].Length;
        bufs[i].Count=q->Arg[i].Length?1:0;bufs[i].Index=i;pages[i].Address=((ULONGLONG)c->Physical.QuadPart+(at&~(PAGE_SIZE-1u)))|0x100000000ULL;pages[i].Size=(q->Arg[i].Length+(at&(PAGE_SIZE-1u))+PAGE_SIZE-1u)&~(PAGE_SIZE-1u);
        if(i<in)RtlCopyMemory(c->Data+at,(PUCHAR)q+q->Arg[i].Offset,q->Arg[i].Length);at+=q->Arg[i].Length;
    }
    c->Message=(RPC_MESSAGE){IoGetRequestorProcessId(irp),HandleToUlong(PsGetCurrentThreadId()),(++Rpc.Sequence<<16)|((ULONGLONG)(c-Rpc.Call+1)<<8)|2,q->Handle,q->Scalar,(ULONGLONG)c->Physical.QuadPart|0x100000000ULL,(used+PAGE_SIZE-1u)&~(PAGE_SIZE-1u)};
    c->File=p->FileObject;c->Pid=c->Message.Pid;if(q->Handle==1)c->Message.Pid=0;
    c->Result=0;c->Sent=FALSE;KeClearEvent(&c->Event);irp->Tail.Overlay.DriverContext[0]=c;
    KIRQL level;IoAcquireCancelSpinLock(&level);
    if(irp->Cancel){IoReleaseCancelSpinLock(level);status=STATUS_CANCELLED;goto reset;}
    IoSetCancelRoutine(irp,RpcCancel);InterlockedExchange(&c->State,1);IoReleaseCancelSpinLock(level);
    InterlockedExchange64(&Rpc.FastUntil,(LONG64)(KeQueryInterruptTime()+1000000));if(Rpc.Work)KeSetEvent(Rpc.Work,IO_NO_INCREMENT,FALSE);ExReleaseFastMutex(&Rpc.Lock);
    KeWaitForSingleObject(&c->Event,Executive,KernelMode,FALSE,NULL);
    IoAcquireCancelSpinLock(&level);IoSetCancelRoutine(irp,NULL);IoReleaseCancelSpinLock(level);ExAcquireFastMutex(&Rpc.Lock);
    status=Rpc.Closing || !Rpc.Link?STATUS_DEVICE_NOT_CONNECTED:irp->Cancel?STATUS_CANCELLED:STATUS_SUCCESS;
    if(NT_SUCCESS(status) && InterlockedCompareExchange(&c->State,0,0)==3) {
        q->Result=c->Result;*bytes=base;
        for(ULONG i=in;i<q->Count;i++){RtlCopyMemory((PUCHAR)q+q->Arg[i].Offset,c->Data+c->Offset[i],q->Arg[i].Length);*bytes=max(*bytes,q->Arg[i].Offset+q->Arg[i].Length);}
        InterlockedExchange(&c->State,0);
    } else {
        if(NT_SUCCESS(status))status=STATUS_DEVICE_NOT_READY;
        if(InterlockedCompareExchange(&c->State,0,1)!=1 && InterlockedCompareExchange(&c->State,0,3)!=3)InterlockedCompareExchange(&c->State,4,2);
    }
    goto done;
reset:
    InterlockedExchange(&c->State,0);
done:
    ExReleaseFastMutex(&Rpc.Lock);return status;
}
static VOID RpcRelease(RPC_CALL *c) {
    if(!c->Data || !c->Pid || c->Release)return;
    PUCHAR data=c->Data+RPC_DATA;RtlZeroMemory(data,516);RPC_ARGUMENT *a=(RPC_ARGUMENT*)data;RPC_BUFFER *b=(RPC_BUFFER*)(a+1);RPC_PAGE *p=(RPC_PAGE*)(b+1);
    a->Pointer=(ULONGLONG)(ULONG_PTR)(data+512);a->Length=4;b->Count=1;p->Address=((ULONGLONG)c->Physical.QuadPart+RPC_DATA)|0x100000000ULL;p->Size=PAGE_SIZE;RtlCopyMemory(data+512,&c->Pid,4);
    c->ReleaseSent=FALSE;KeMemoryBarrier();c->Release=(++Rpc.Sequence<<16)|((ULONGLONG)(c-Rpc.Call+3)<<8)|2;
}
VOID QcomRpcDetach(PFILE_OBJECT file) {
    ExAcquireFastMutex(&Rpc.Lock);
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)if(Rpc.Call[i].File==file){RpcRelease(&Rpc.Call[i]);KeSetEvent(&Rpc.Call[i].Event,IO_NO_INCREMENT,FALSE);}
    ExReleaseFastMutex(&Rpc.Lock);
}
static BOOLEAN RpcSend(PSIA_LINK l,const RPC_MESSAGE *message) {
    if(!Rpc.Acked || !Rpc.Rcid)return FALSE;ULONG i;
    for(i=0;i<Rpc.Count && !(Rpc.Free[i] && Rpc.Size[i]>=sizeof(*message));i++);
    if(i==Rpc.Count){if(!Rpc.Requested)Rpc.Requested=SiaGlinkCmd(l,GLINK_CMD_RX_INTENT_REQ,RPC_CID,sizeof(*message));return FALSE;}
    UCHAR packet[16+sizeof(*message)];ULONG h[]={GLINK_CMD_TX_DATA|(RPC_CID<<16),Rpc.Iid[i],sizeof(*message),0};
    RtlCopyMemory(packet,h,sizeof(h));RtlCopyMemory(packet+16,message,sizeof(*message));
    if(!SiaGlinkSend(l,packet,sizeof(packet)))return FALSE;Rpc.Free[i]=FALSE;return TRUE;
}
static VOID RpcPoll(PSIA_LINK l) {
    if(Rpc.Link!=l || !l->VersionReady)return;
    BOOLEAN pending=FALSE;for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)pending|=InterlockedCompareExchange(&Rpc.Call[i].State,0,0)==1;
    if(!Rpc.Opening && !Rpc.Closing && pending) {
        static const CHAR name[]="fastrpcglink-apps-dsp";UCHAR open[32]={0};ULONG h[]={GLINK_CMD_OPEN|(RPC_CID<<16),sizeof(name)};
        RtlCopyMemory(open,h,8);RtlCopyMemory(open+8,name,sizeof(name));Rpc.Opening=SiaGlinkSend(l,open,sizeof(open));
    }
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++) {
        RPC_CALL *c=&Rpc.Call[i];if(!c->Release || c->ReleaseSent)continue;
        RPC_MESSAGE m={0,0,c->Release,1,0x01010000,((ULONGLONG)c->Physical.QuadPart+RPC_DATA)|0x100000000ULL,PAGE_SIZE};c->ReleaseSent=RpcSend(l,&m);
    }
    if(Rpc.Closing)return;
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++) {
        RPC_CALL *c=&Rpc.Call[i];if(InterlockedCompareExchange(&c->State,2,1)!=1)continue;
        c->Sent=TRUE;KeMemoryBarrier();
        if(!RpcSend(l,&c->Message)){c->Sent=FALSE;if(InterlockedCompareExchange(&c->State,1,2)!=2)InterlockedCompareExchange(&c->State,0,4);}
    }
}
static BOOLEAN RpcHandle(PSIA_LINK l,USHORT cmd,USHORT p1,ULONG p2,ULONG tail,const ULONG *frame) {
    if(cmd==GLINK_CMD_OPEN) {
        CHAR name[32]={0};SiaFifoRead(l,tail+8,(PUCHAR)name,min(p2&0xffff,31u));if(strcmp(name,"fastrpcglink-apps-dsp"))return FALSE;
        Rpc.Rcid=p1;Rpc.LocalIid=1;SiaGlinkCmd(l,GLINK_CMD_OPEN_ACK,p1,0);ULONG intent[]={GLINK_CMD_INTENT|(RPC_CID<<16),1,64,Rpc.LocalIid};SiaGlinkSend(l,(PUCHAR)intent,sizeof(intent));return TRUE;
    }
    if(cmd==GLINK_CMD_OPEN_ACK && p1==RPC_CID){Rpc.Acked=TRUE;return TRUE;}
    if(cmd==GLINK_CMD_CLOSE_ACK && p1==RPC_CID){Rpc.Acked=FALSE;return TRUE;}
    if(!Rpc.Rcid || p1!=Rpc.Rcid)return FALSE;
    if(cmd==GLINK_CMD_INTENT) {
        Rpc.Requested=FALSE;for(ULONG i=0;i<p2;i++) {
            ULONG pair[2];SiaFifoRead(l,tail+8+i*8,(PUCHAR)pair,8);ULONG j;for(j=0;j<Rpc.Count && Rpc.Iid[j]!=pair[1];j++);
            if(j<8){Rpc.Iid[j]=pair[1];Rpc.Size[j]=pair[0];Rpc.Free[j]=TRUE;Rpc.Count=max(Rpc.Count,j+1);}
        }return TRUE;
    }
    if(cmd==GLINK_CMD_INTENT_REQ_ACK){if(!p2)Rpc.Requested=FALSE;return TRUE;}
    if(cmd==GLINK_CMD_RX_DONE || cmd==GLINK_CMD_RX_DONE_REUSE) {
        for(ULONG i=0;i<Rpc.Count;i++)if(Rpc.Iid[i]==p2) {
            if(cmd==GLINK_CMD_RX_DONE_REUSE)Rpc.Free[i]=TRUE;
            else {--Rpc.Count;Rpc.Iid[i]=Rpc.Iid[Rpc.Count];Rpc.Size[i]=Rpc.Size[Rpc.Count];Rpc.Free[i]=Rpc.Free[Rpc.Count];}break;
        }return TRUE;
    }
    if(cmd==GLINK_CMD_RX_INTENT_REQ) {
        ULONG intent[]={GLINK_CMD_INTENT|(RPC_CID<<16),1,64,Rpc.LocalIid+1};BOOLEAN granted=p2<=64 && Rpc.LocalIid<MAXULONG && SiaGlinkSend(l,(PUCHAR)intent,sizeof(intent));
        if(granted)Rpc.LocalIid++;SiaGlinkCmd(l,GLINK_CMD_INTENT_REQ_ACK,RPC_CID,granted);return TRUE;
    }
    if(cmd==GLINK_CMD_TX_DATA || cmd==GLINK_CMD_TX_DATA_CONT) {
        if(cmd==GLINK_CMD_TX_DATA && !frame[1] && frame[0]>=12 && frame[0]<=64) {
            UCHAR response[64];SiaFifoRead(l,tail+16,response,frame[0]);ULONGLONG context;ULONG result;RtlCopyMemory(&context,response,8);RtlCopyMemory(&result,response+8,4);
            for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++) {
                RPC_CALL *c=&Rpc.Call[i];if(c->Release && (c->Release>>8)==(context>>8)){c->Release=0;c->File=NULL;continue;}if((c->Message.Context>>8)!=(context>>8) || !c->Sent)continue;
                c->Result=result;c->Sent=FALSE;InterlockedExchange64(&Rpc.FastUntil,(LONG64)(KeQueryInterruptTime()+1000000));KeMemoryBarrier();
                if(InterlockedCompareExchange(&c->State,3,2)==2)KeSetEvent(&c->Event,IO_NO_INCREMENT,FALSE);else InterlockedCompareExchange(&c->State,0,4);
            }
        }
        return TRUE;
    }
    if(cmd==GLINK_CMD_CLOSE){SiaGlinkCmd(l,GLINK_CMD_CLOSE_ACK,p1,0);Rpc.Rcid=0;Rpc.Acked=FALSE;Rpc.Closing=TRUE;for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)KeSetEvent(&Rpc.Call[i].Event,IO_NO_INCREMENT,FALSE);return TRUE;}return FALSE;
}
static VOID RpcStart(PSIA_DEVICE d) {
    ExAcquireFastMutex(&Rpc.Lock);Rpc.Link=d->Link;Rpc.Work=&d->WorkEvent;Rpc.FastUntil=0;Rpc.Closing=FALSE;Rpc.Opening=FALSE;Rpc.Acked=FALSE;Rpc.Rcid=Rpc.Count=0;Rpc.Requested=FALSE;ExReleaseFastMutex(&Rpc.Lock);
}
static VOID RpcStop(PSIA_DEVICE d) {
    PSIA_LINK l=d->Link;ExAcquireFastMutex(&Rpc.Lock);Rpc.Closing=TRUE;
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++) {
        RPC_CALL *c=&Rpc.Call[i];KeSetEvent(&c->Event,IO_NO_INCREMENT,FALSE);if(!c->Data || !c->Pid)continue;
        RpcRelease(c);
    }
    ExReleaseFastMutex(&Rpc.Lock);
    for(ULONG tick=0;tick<1000;tick++) {
        BOOLEAN busy=FALSE;for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++)busy|=Rpc.Call[i].Sent || Rpc.Call[i].Release!=0;
        if(!busy)break;
        RpcPoll(l);SiaLinkPoll(d);SiaSleep(1);
    }
    if(Rpc.Acked)SiaGlinkCmd(l,GLINK_CMD_CLOSE,RPC_CID,0);
    ExAcquireFastMutex(&Rpc.Lock);Rpc.Link=NULL;Rpc.Work=NULL;
    for(ULONG i=0;i<RTL_NUMBER_OF(Rpc.Call);i++) {
        RPC_CALL *c=&Rpc.Call[i];if(!c->Data)continue;
        if(!c->Sent && !c->Release){c->File=NULL;MmFreeContiguousMemorySpecifyCache(c->Data,RPC_SIZE,MmNonCached);c->Data=NULL;SiaDspMemoryRelease(FALSE);}
        else d->RetainedDma=TRUE;
    }
    ExReleaseFastMutex(&Rpc.Lock);
}
