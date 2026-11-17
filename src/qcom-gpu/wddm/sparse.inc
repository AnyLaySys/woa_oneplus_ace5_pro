static A830_SPARSE *FindSparse(A830_CLIENT *c,ULONG handle) {
    for(PLIST_ENTRY e=c->Sparse.Flink;e!=&c->Sparse;e=e->Flink) {
        A830_SPARSE *s=CONTAINING_RECORD(e,A830_SPARSE,Entry);
        if(s->Handle==handle && !s->Released)return s;
    }
    return NULL;
}
static VOID FreeSparse(A830_BRIDGE *g,A830_CLIENT *c,A830_SPARSE *s) {
    if(g->Hardware && g->Hardware->Online && !NT_SUCCESS(GpuMapRange(g->Hardware,0,s->Address,s->Size,NULL,0,FALSE,TRUE)))GpuStop(g->Hardware);
    for(ULONG i=0;i<(ULONG)(s->Size>>PAGE_SHIFT);i++)if(s->Pages[i])c->SparseRefs[(ULONG)(s->Pages[i]>>32)-1]--;
    FreeAddress(g,s->Address,s->Size);RemoveEntryList(&s->Entry);ExFreePoolWithTag(s,GPU_TAG);
}
static VOID CollectSparse(A830_BRIDGE *g,A830_CLIENT *c) {
    PLIST_ENTRY e=c->Sparse.Flink;
    while(e!=&c->Sparse) {
        A830_SPARSE *s=CONTAINING_RECORD(e,A830_SPARSE,Entry);e=e->Flink;
        if(s->Released)FreeSparse(g,c,s);
    }
    if(IsListEmpty(&c->Sparse) && c->SparseRefs){ExFreePoolWithTag(c->SparseRefs,GPU_TAG);c->SparseRefs=NULL;}
}
static VOID CleanupSparse(A830_BRIDGE *g,A830_CLIENT *c) {
    while(!IsListEmpty(&c->Sparse))FreeSparse(g,c,CONTAINING_RECORD(c->Sparse.Flink,A830_SPARSE,Entry));
    if(c->SparseRefs){ExFreePoolWithTag(c->SparseRefs,GPU_TAG);c->SparseRefs=NULL;}
}
static NTSTATUS ResetSparse(A830_BRIDGE *g,A830_CLIENT *c) {
    for(PLIST_ENTRY e=c->Sparse.Flink;e!=&c->Sparse;e=e->Flink) {
        A830_SPARSE *s=CONTAINING_RECORD(e,A830_SPARSE,Entry);
        NTSTATUS status=GpuMapRange(g->Hardware,0,s->Address,s->Size,NULL,0,s->Zero,TRUE);
        if(!NT_SUCCESS(status))return status;
        ULONG count=(ULONG)(s->Size>>PAGE_SHIFT);
        for(ULONG at=0;at<count;) {
            ULONGLONG page=s->Pages[at];if(!page){at++;continue;}
            ULONG end=at+1;while(end<count && s->Pages[end]==page+end-at)end++;
            status=GpuMapRange(g->Hardware,0,s->Address+((ULONGLONG)at<<PAGE_SHIFT),(ULONGLONG)(end-at)<<PAGE_SHIFT,
                c->Buffers+(ULONG)(page>>32)-1,(ULONGLONG)(ULONG)page<<PAGE_SHIFT,FALSE,TRUE);
            if(!NT_SUCCESS(status))return status;
            at=end;
        }
    }
    return STATUS_SUCCESS;
}
static NTSTATUS SparseIo(A830_BRIDGE *g,A830_CLIENT *c,PIRP request,ULONG code,ULONG inLength,ULONG outLength) {
    PVOID data=request->AssociatedIrp.SystemBuffer;
    if(code==GPU_IOCTL_SPARSE_CAPS && !inLength && outLength>=sizeof(ULONG)) {
        *(PULONG)data=1;request->IoStatus.Information=sizeof(ULONG);return STATUS_SUCCESS;
    }
    if(!g->Hardware || !g->Hardware->Online)return STATUS_DEVICE_NOT_READY;
    if(code==GPU_IOCTL_VA_RESERVE && inLength==sizeof(GPU_VA_RESERVE) && outLength>=sizeof(GPU_VA_ALLOCATION)) {
        GPU_VA_RESERVE v=*(GPU_VA_RESERVE *)data;
        if(!v.Size || v.Size>GPU_CLIENT_VA_SIZE || ((v.Address|v.Size)&(PAGE_SIZE-1)) || v.Reserved || (v.Flags&~1u) || c->NextSparse==0x3fffffff)return STATUS_INVALID_PARAMETER;
        SIZE_T bytes=FIELD_OFFSET(A830_SPARSE,Pages)+(SIZE_T)(v.Size>>PAGE_SHIFT)*sizeof(ULONGLONG);
        A830_SPARSE *s=ExAllocatePool2(POOL_FLAG_NON_PAGED,bytes,GPU_TAG);if(!s)return STATUS_INSUFFICIENT_RESOURCES;
        if(!c->SparseRefs)c->SparseRefs=ExAllocatePool2(POOL_FLAG_NON_PAGED,GPU_MAX_BUFFERS*sizeof(ULONG),GPU_TAG);
        if(!c->SparseRefs){ExFreePoolWithTag(s,GPU_TAG);return STATUS_INSUFFICIENT_RESOURCES;}
        s->Address=Address(g,v.Size,v.Address);s->Size=v.Size;s->Zero=(v.Flags&1)!=0;
        if(!s->Address){ExFreePoolWithTag(s,GPU_TAG);return STATUS_INSUFFICIENT_RESOURCES;}
        NTSTATUS status=GpuMapRange(g->Hardware,0,s->Address,s->Size,NULL,0,s->Zero,TRUE);
        if(!NT_SUCCESS(status)){FreeAddress(g,s->Address,s->Size);ExFreePoolWithTag(s,GPU_TAG);GpuStop(g->Hardware);return status;}
        s->Handle=0x40000000|++c->NextSparse;InsertTailList(&c->Sparse,&s->Entry);
        GPU_VA_ALLOCATION allocation={s->Address,s->Size,s->Handle,0};RtlCopyMemory(data,&allocation,sizeof(allocation));
        request->IoStatus.Information=sizeof(allocation);return STATUS_SUCCESS;
    }
    if(code==GPU_IOCTL_VA_RELEASE && inLength==sizeof(ULONG) && !outLength) {
        A830_SPARSE *s=FindSparse(c,*(PULONG)data);if(!s)return STATUS_INVALID_HANDLE;
        s->Released=TRUE;if(c->Deferred.Fence<g->Hardware->Submitted)c->Deferred.Fence=g->Hardware->Submitted;
        CollectClientDeferred(g,c);return STATUS_SUCCESS;
    }
    if(code!=GPU_IOCTL_VA_BIND || !inLength || inLength%sizeof(GPU_VA_BIND) || outLength)return STATUS_INVALID_PARAMETER;
    ULONG count=inLength/sizeof(GPU_VA_BIND);GPU_VA_BIND *binds=data;
    for(ULONG i=0;i<count;i++) {
        GPU_VA_BIND *b=binds+i;A830_SPARSE *s=FindSparse(c,b->Handle);
        if(!s || !b->Size || ((b->Offset|b->BufferOffset|b->Size)&(PAGE_SIZE-1)) || b->Offset>s->Size || b->Size>s->Size-b->Offset || b->Buffer>GPU_MAX_BUFFERS)return STATUS_INVALID_PARAMETER;
        if(b->Buffer) {
            GPU_BO *bo=c->Buffers+b->Buffer-1;
            if(!bo->Mdl || c->Deferred.Buffers[b->Buffer-1] || b->BufferOffset>bo->Size || b->Size>bo->Size-b->BufferOffset)return STATUS_INVALID_PARAMETER;
        } else if(b->BufferOffset)return STATUS_INVALID_PARAMETER;
    }
    NTSTATUS status=GpuWait(g->Hardware,g->Hardware->Submitted,10000000);
    if(!NT_SUCCESS(status))return status;
    for(ULONG i=0;i<count;i++) {
        GPU_VA_BIND *b=binds+i;A830_SPARSE *s=FindSparse(c,b->Handle);
        status=GpuMapRange(g->Hardware,0,s->Address+b->Offset,b->Size,b->Buffer?c->Buffers+b->Buffer-1:NULL,b->BufferOffset,s->Zero,FALSE);
        if(!NT_SUCCESS(status)){GpuStop(g->Hardware);return status;}
        ULONG at=(ULONG)(b->Offset>>PAGE_SHIFT),pages=(ULONG)(b->Size>>PAGE_SHIFT);
        ULONGLONG first=((ULONGLONG)b->Buffer<<32)|(b->BufferOffset>>PAGE_SHIFT);
        for(ULONG p=0;p<pages;p++) {
            ULONGLONG old=s->Pages[at+p];if(old)c->SparseRefs[(ULONG)(old>>32)-1]--;
            s->Pages[at+p]=b->Buffer?first+p:0;
            if(b->Buffer)c->SparseRefs[b->Buffer-1]++;
        }
    }
    status=GpuFlushMappings(g->Hardware,0);
    if(!NT_SUCCESS(status)){GpuStop(g->Hardware);return status;}
    CollectClientDeferred(g,c);return STATUS_SUCCESS;
}
