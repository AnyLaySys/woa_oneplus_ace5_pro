#include <ntifs.h>
#include "npu.h"

BOOLEAN NpuBusy(NPU *n) {
    if (n->Pending || n->Running) return TRUE;
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        if (s->Attached || s->Heap || NpuAsyncPending(s)) return TRUE;
    }
    return FALSE;
}
BOOLEAN NpuGpuBusy(NPU_CLIENT *s) { return !s->Device->Preparing && s->Device->Running && s->Device->Running != s; }
NPU_CLIENT *NpuClientGet(NPU *n, WDFREQUEST request, PNTSTATUS status) {
    WDFFILEOBJECT file = WdfRequestGetFileObject(request);
    NPU_FILE *context = NpuFileContext(file);
    ULONG pid = IoGetRequestorProcessId(WdfRequestWdmGetIrp(request));
    *status = STATUS_ACCESS_DENIED;
    if (context->Closing || !pid) return NULL;
    if (context->Client) {
        if (context->Client->Pid != pid) return NULL;
        *status = STATUS_SUCCESS; return context->Client;
    }
    for (PLIST_ENTRY p = n->Clients.Flink; p != &n->Clients; p = p->Flink) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry);
        if (s->Pid == pid) { *status = STATUS_SHARING_VIOLATION; return NULL; }
    }
    if (n->ClientCount == NPU_CLIENTS) { *status = STATUS_INSUFFICIENT_RESOURCES; return NULL; }
    NPU_CLIENT *s = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*s), NPU_TAG);
    if (!s) { *status = STATUS_INSUFFICIENT_RESOURCES; return NULL; }
    s->Device = n; s->Pid = pid; s->File = file;
    PIRP irp = WdfRequestWdmGetIrp(request);
    s->Thread = irp->Tail.Overlay.Thread ? HandleToUlong(PsGetThreadId(irp->Tail.Overlay.Thread)) : 0;
    WdfObjectReference(file); InsertTailList(&n->Clients, &s->Entry); n->ClientCount++;
    context->Client = s; *status = STATUS_SUCCESS; return s;
}
VOID NpuClientRetire(NPU_CLIENT *s) {
    if (!s->Closing || s->Attached || s->RunRefs || NpuAsyncPending(s)) return;
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) if (s->Allocations[i].Buffer) return;
    for (ULONG i = 0; i < NPU_ASYNC_SLOTS; i++) if (s->Scheduled[i].State || s->Calls[i].Rpc) return;
    RemoveEntryList(&s->Entry); s->Device->ClientCount--;
    WdfObjectDereference(s->File); ExFreePoolWithTag(s, NPU_TAG);
}
VOID NpuClientClose(NPU_CLIENT *s) {
    s->Closing = TRUE; NpuCancelScheduled(s);
    if (!s->Attached && !NpuGpuBusy(s)) NpuFreeAllocations(s);
    else NpuDetach(s);
    NpuClientRetire(s);
}
VOID NpuClientSweep(NPU *n) {
    PLIST_ENTRY p = n->Clients.Flink;
    while (p != &n->Clients) {
        NPU_CLIENT *s = CONTAINING_RECORD(p, NPU_CLIENT, Entry); p = p->Flink;
        if (s->Closing) NpuClientClose(s);
    }
}
NPU_ALLOCATION *NpuAllocation(NPU_CLIENT *s, ULONG id) {
    for (ULONG i = 0; i < NPU_ALLOCATIONS; i++) {
        NPU_ALLOCATION *a = &s->Allocations[i];
        if (a->Id == id && a->Buffer && !a->Detached) return a;
    }
    return NULL;
}
