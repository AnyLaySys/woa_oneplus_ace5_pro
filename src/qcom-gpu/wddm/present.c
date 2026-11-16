#include "adapter.h"

static NTSTATUS Surface(A830_ADAPTER *a,const DXGK_ALLOCATIONLIST *info,const RECT *rect,
    PVOID *map,ULONG *pitch,A830_ALLOCATION **owned,PULONGLONG sharedAddress,SIZE_T *bytes,BOOLEAN *sharedMap) {
    A830_ALLOCATION *allocation=info->hDeviceSpecificAllocation;
    if(!allocation || rect->left<0 || rect->top<0 || rect->right<=rect->left || rect->bottom<=rect->top ||
        (ULONG)rect->right>allocation->Resource.Width || (ULONG)rect->bottom>allocation->Resource.Height ||
        allocation->Resource.Pitch<allocation->Resource.Width*4 ||
        (SIZE_T)allocation->Resource.Pitch*allocation->Resource.Height>allocation->Resource.Size)return STATUS_INVALID_PARAMETER;
    ULONGLONG offset=info->PhysicalAddress.QuadPart;
    *owned=NULL;*sharedMap=FALSE;*bytes=allocation->Resource.Size;*pitch=allocation->Resource.Pitch;
    if(allocation->Buffer.Mdl){*map=allocation->Buffer.Cpu;*owned=allocation;return STATUS_SUCCESS;}
    if(info->SegmentId==1) {
        if(offset>=A830_SEGMENT_ADDRESS)offset-=A830_SEGMENT_ADDRESS;
        if(offset>A830_SEGMENT_BYTES || allocation->Resource.Size>A830_SEGMENT_BYTES-offset)return STATUS_INVALID_PARAMETER;
        *map=a->Memory.Segment.Cpu+(SIZE_T)offset;
    } else if(info->SegmentId==2) {
        /* A DMA packet can be resubmitted after preemption. It describes the
           shared range; each execution job owns a fresh temporary mapping. */
        *map=NULL;*sharedAddress=offset;*sharedMap=TRUE;return STATUS_SUCCESS;
    } else return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY A830Present(HANDLE context,DXGKARG_PRESENT *p) {
    A830_CONTEXT *c=context;A830_ADAPTER *a=c->Adapter;

    if(p->Flags.Flip) {
        p->pDmaBuffer=NULL;return STATUS_SUCCESS;
    }
    if(!p->Flags.Blt || !p->pAllocationList ||
        (p->SubRectCnt && (!p->pDstSubRects || p->MultipassOffset>=p->SubRectCnt)) ||
        (!p->SubRectCnt && p->MultipassOffset) ||
        p->SrcRect.right-p->SrcRect.left!=p->DstRect.right-p->DstRect.left ||
        p->SrcRect.bottom-p->SrcRect.top!=p->DstRect.bottom-p->DstRect.top)return STATUS_NOT_SUPPORTED;
    if(p->DmaSize<sizeof(ULONG) || p->DmaBufferPrivateDataSize<sizeof(A830_DMA))return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    A830_DMA *dma=p->pDmaBufferPrivateData;RtlZeroMemory(dma,sizeof(*dma));
    ULONG start=p->MultipassOffset;
    dma->Count=p->SubRectCnt?min(p->SubRectCnt-start,(ULONG)RTL_NUMBER_OF(dma->Payload.Rects)):1;
    for(ULONG i=0;i<dma->Count;i++) {
        RECT rect=p->SubRectCnt?p->pDstSubRects[start+i]:p->DstRect;
        if(rect.left<p->DstRect.left || rect.top<p->DstRect.top || rect.right>p->DstRect.right ||
            rect.bottom>p->DstRect.bottom || rect.right<=rect.left || rect.bottom<=rect.top)return STATUS_INVALID_PARAMETER;
        rect.left-=p->DstRect.left;rect.right-=p->DstRect.left;
        rect.top-=p->DstRect.top;rect.bottom-=p->DstRect.top;dma->Payload.Rects[i]=rect;
    }
    NTSTATUS status=Surface(a,p->pAllocationList+DXGK_PRESENT_SOURCE_INDEX,&p->SrcRect,&dma->Source,&dma->SourcePitch,&dma->SourceAllocation,&dma->SourceShared,&dma->SourceBytes,&dma->SourceSharedMap);
    if(!NT_SUCCESS(status))return status;
    status=Surface(a,p->pAllocationList+DXGK_PRESENT_DESTINATION_INDEX,&p->DstRect,&dma->Destination,&dma->DestinationPitch,&dma->DestinationAllocation,&dma->DestinationShared,&dma->DestinationBytes,&dma->DestinationSharedMap);
    if(!NT_SUCCESS(status))return status;
    dma->Operation=6;dma->Width=p->SrcRect.right-p->SrcRect.left;dma->Height=p->SrcRect.bottom-p->SrcRect.top;
    dma->Offset=(ULONG)((SIZE_T)p->SrcRect.top*dma->SourcePitch+p->SrcRect.left*4);
    dma->Value=(ULONG)((SIZE_T)p->DstRect.top*dma->DestinationPitch+p->DstRect.left*4);
    *(PULONG)p->pDmaBuffer=0;p->pDmaBuffer=(PUCHAR)p->pDmaBuffer+sizeof(ULONG);
    p->pDmaBufferPrivateData=(PUCHAR)p->pDmaBufferPrivateData+sizeof(*dma);
    if(p->SubRectCnt && start+dma->Count<p->SubRectCnt) {
        p->MultipassOffset=start+dma->Count;return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    p->MultipassOffset=0;return STATUS_SUCCESS;
}

NTSTATUS A830PinPresent(A830_DMA *dma) {
    if(dma->Operation!=6)return STATUS_SUCCESS;
    A830_ALLOCATION *allocations[]={dma->SourceAllocation,dma->DestinationAllocation};
    for(ULONG i=0;i<RTL_NUMBER_OF(allocations);i++)if(allocations[i]) {
        SIZE_T bytes=i==0?dma->SourceBytes:dma->DestinationBytes;
        if(!allocations[i]->Buffer.Mdl || !allocations[i]->Buffer.Cpu || bytes>allocations[i]->Buffer.Size)return STATUS_INVALID_HANDLE;
        InterlockedIncrement(&allocations[i]->ScanoutReferences);
        if(i==0){dma->SourcePinned=TRUE;dma->Source=allocations[i]->Buffer.Cpu;}
        else {dma->DestinationPinned=TRUE;dma->Destination=allocations[i]->Buffer.Cpu;}
    }
    return STATUS_SUCCESS;
}
VOID A830ReleasePresent(A830_DMA *dma) {
    if(dma->SourceMdl){MmUnmapLockedPages(dma->Source,dma->SourceMdl);IoFreeMdl(dma->SourceMdl);dma->SourceMdl=NULL;dma->Source=NULL;}
    if(dma->DestinationMdl){MmUnmapLockedPages(dma->Destination,dma->DestinationMdl);IoFreeMdl(dma->DestinationMdl);dma->DestinationMdl=NULL;dma->Destination=NULL;}
    if(dma->SourcePinned){InterlockedDecrement(&dma->SourceAllocation->ScanoutReferences);dma->SourcePinned=FALSE;}
    if(dma->DestinationPinned){InterlockedDecrement(&dma->DestinationAllocation->ScanoutReferences);dma->DestinationPinned=FALSE;}
}
NTSTATUS A830MapPresent(A830_ADAPTER *a,A830_DMA *dma) {
    if(dma->SourceMdl || dma->DestinationMdl)return STATUS_INVALID_DEVICE_STATE;
    NTSTATUS status=STATUS_SUCCESS;
    if(dma->SourceSharedMap)status=A830MapSharedCpu(a,dma->SourceShared,dma->SourceBytes,&dma->Source,&dma->SourceMdl);
    if(NT_SUCCESS(status) && dma->DestinationSharedMap)status=A830MapSharedCpu(a,dma->DestinationShared,dma->DestinationBytes,&dma->Destination,&dma->DestinationMdl);
    if(!NT_SUCCESS(status))A830ReleasePresent(dma);
    return status;
}
static NTSTATUS CopyPresent(const A830_DMA *dma,PUCHAR destination,ULONG pitch,SIZE_T offset) {
    if(!dma->Source || !dma->Destination || !dma->Count || dma->Count>RTL_NUMBER_OF(dma->Payload.Rects))return STATUS_INVALID_PARAMETER;
    for(ULONG i=0;i<dma->Count;i++) {
        const RECT *rect=dma->Payload.Rects+i;
        if(rect->left<0 || rect->top<0 || rect->right<=rect->left || rect->bottom<=rect->top ||
            (ULONG)rect->right>dma->Width || (ULONG)rect->bottom>dma->Height)return STATUS_INVALID_PARAMETER;
        SIZE_T rowBytes=(SIZE_T)(rect->right-rect->left)*4;
        SIZE_T sourceLast=(SIZE_T)dma->Offset+(SIZE_T)(rect->bottom-1)*dma->SourcePitch+(SIZE_T)rect->left*4;
        if(dma->SourceBytes && (sourceLast>dma->SourceBytes || rowBytes>dma->SourceBytes-sourceLast))return STATUS_INVALID_PARAMETER;
        if(destination==dma->Destination && dma->DestinationBytes) {
            SIZE_T last=offset+(SIZE_T)(rect->bottom-1)*pitch+(SIZE_T)rect->left*4;
            if(last>dma->DestinationBytes || rowBytes>dma->DestinationBytes-last)return STATUS_INVALID_PARAMETER;
        }
        for(LONG y=rect->top;y<rect->bottom;y++)
            RtlMoveMemory(destination+offset+(SIZE_T)y*pitch+(SIZE_T)rect->left*4,
                (PUCHAR)dma->Source+dma->Offset+(SIZE_T)y*dma->SourcePitch+(SIZE_T)rect->left*4,(SIZE_T)(rect->right-rect->left)*4);
    }
    KeMemoryBarrier();return STATUS_SUCCESS;
}
NTSTATUS A830CopyPresent(const A830_DMA *dma) {
    return CopyPresent(dma,dma->Destination,dma->DestinationPitch,dma->Value);
}
NTSTATUS A830CopyPresentToFrame(const A830_DMA *dma,PUCHAR destination,ULONG pitch,ULONG width,ULONG height) {
    if(!destination || !dma->DestinationPitch || pitch<width*4 || (dma->Value&3))return STATUS_INVALID_PARAMETER;
    ULONG x=(dma->Value%dma->DestinationPitch)/4,y=dma->Value/dma->DestinationPitch;
    if(x>width || y>height || dma->Width>width-x || dma->Height>height-y)return STATUS_INVALID_PARAMETER;
    return CopyPresent(dma,destination,pitch,(SIZE_T)y*pitch+x*4);
}
