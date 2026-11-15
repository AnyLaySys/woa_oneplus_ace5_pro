#include "../wddm/adapter.h"
#include <stdlib.h>
#include <stdio.h>
static ULONG maps,unmaps,freed;
static VOID NTAPI UnexpectedUnmap(PVOID address,PMDL mdl){if(mdl->MappedSystemVa!=address)abort();mdl->MappedSystemVa=NULL;free(address);unmaps++;}
static VOID NTAPI UnexpectedFree(PMDL mdl){if(mdl->MappedSystemVa)abort();free(mdl);freed++;}
VOID (NTAPI *__imp_MmUnmapLockedPages)(PVOID,PMDL)=UnexpectedUnmap;
VOID (NTAPI *__imp_IoFreeMdl)(PMDL)=UnexpectedFree;
VOID A830Record(PCWSTR name,ULONG value){UNREFERENCED_PARAMETER(name);UNREFERENCED_PARAMETER(value);}
NTSTATUS A830MapSharedCpu(A830_ADAPTER *a,ULONGLONG offset,SIZE_T size,PVOID *map,PMDL *mdl) {
    UNREFERENCED_PARAMETER(a);UNREFERENCED_PARAMETER(offset);
    *mdl=calloc(1,sizeof(**mdl));*map=calloc(1,size);if(!*mdl || !*map)abort();
    (*mdl)->MappedSystemVa=*map;(*mdl)->ByteCount=(ULONG)size;maps++;return STATUS_SUCCESS;
}
int main(void) {
    A830_ADAPTER *a=calloc(1,sizeof(*a));if(!a)return 1;
    a->Memory.Segment.Cpu=malloc(8192);if(!a->Memory.Segment.Cpu)return 1;
    A830_CONTEXT context={0};context.Adapter=a;
    A830_ALLOCATION source={{A830_ABI_VERSION,4096,16,16,64,D3DDDIFMT_A8R8G8B8,0}};
    DXGK_ALLOCATIONLIST list[3]={0};
    list[1].hDeviceSpecificAllocation=&source;list[1].SegmentId=1;list[1].PhysicalAddress.QuadPart=0;
    list[2].hDeviceSpecificAllocation=&source;list[2].SegmentId=1;list[2].PhysicalAddress.QuadPart=4096;
    A830_DMA dma;ULONG command;DXGKARG_PRESENT p={0};p.Flags.Blt=1;
    p.pAllocationList=list;p.SrcRect.right=p.DstRect.right=16;p.SrcRect.bottom=p.DstRect.bottom=16;
    p.pDmaBuffer=&command;p.DmaSize=sizeof(command);p.pDmaBufferPrivateData=&dma;p.DmaBufferPrivateDataSize=sizeof(dma);
    if(A830Present(&context,&p)!=STATUS_SUCCESS || dma.Source!=a->Memory.Segment.Cpu ||
        dma.Destination!=a->Memory.Segment.Cpu+4096 || dma.Width!=16 || dma.Height!=16 || dma.Operation!=6)return 1;
    memset(a->Memory.Segment.Cpu,0xCC,4096);memset(a->Memory.Segment.Cpu+4096,0xA5,4096);
    for(ULONG y=0;y<16;y++)for(ULONG x=0;x<16;x++)((PULONG)a->Memory.Segment.Cpu)[y*16+x]=0xFF000000|(x<<8)|y;
    RECT rects[]={{4,5,6,7},{10,8,12,11}};
    p.SrcRect=(RECT){2,3,10,9};p.DstRect=(RECT){4,5,12,11};p.SubRectCnt=2;p.pDstSubRects=rects;
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    if(A830Present(&context,&p)!=STATUS_SUCCESS || A830CopyPresent(&dma)!=STATUS_SUCCESS ||
       a->Telemetry.PresentBlt!=2 || dma.PresentBytes!=40)return 1;
    PUCHAR frame=malloc(80*16);if(!frame)return 1;memset(frame,0xA5,80*16);
    if(A830CopyPresentToFrame(&dma,frame,80,16,16)!=STATUS_SUCCESS ||
       A830CopyPresentToFrame(&dma,frame,80,10,16)!=STATUS_INVALID_PARAMETER)return 1;
    for(ULONG y=0;y<16;y++)for(ULONG x=0;x<16;x++) {
        BOOLEAN changed=(x>=4 && x<6 && y>=5 && y<7) || (x>=10 && x<12 && y>=8 && y<11);
        ULONG expected=changed?(0xFF000000|((x-2)<<8)|(y-2)):0xA5A5A5A5;
        if(((PULONG)(a->Memory.Segment.Cpu+4096))[y*16+x]!=expected)return 1;
        if(((PULONG)(frame+y*80))[x]!=expected)return 1;
    }
    for(ULONG y=0;y<16;y++)for(ULONG x=64;x<80;x++)if(frame[y*80+x]!=0xA5)return 1;
    free(frame);
    RECT many[257];for(ULONG i=0;i<RTL_NUMBER_OF(many);i++)many[i]=rects[i%2];
    p.SubRectCnt=RTL_NUMBER_OF(many);p.pDstSubRects=many;
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    if(A830Present(&context,&p)!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER || p.MultipassOffset!=256 || dma.Count!=256 ||
        A830CopyPresent(&dma)!=STATUS_SUCCESS)return 1;
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    if(A830Present(&context,&p)!=STATUS_SUCCESS || p.MultipassOffset || dma.Count!=1 || A830CopyPresent(&dma)!=STATUS_SUCCESS)return 1;
    many[0].right=17;p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    if(A830Present(&context,&p)!=STATUS_INVALID_PARAMETER)return 1;
    /* Replay one DMA packet twice, as VidSch may after preemption. Each job
       must create its own mapping; retiring one must not invalidate the other. */
    p.SrcRect=p.DstRect=(RECT){0,0,16,16};p.SubRectCnt=0;p.MultipassOffset=0;
    list[1].SegmentId=2;list[1].PhysicalAddress.QuadPart=0;
    p.pDmaBuffer=&command;p.pDmaBufferPrivateData=&dma;
    if(A830Present(&context,&p)!=STATUS_SUCCESS || maps || dma.SourceMdl || dma.Source || !dma.SourceSharedMap)return 1;
    A830_DMA first=dma,second=dma;
    if(A830MapPresent(a,&first)!=STATUS_SUCCESS || A830MapPresent(a,&second)!=STATUS_SUCCESS ||
       first.Source==second.Source || first.SourceMdl==second.SourceMdl || dma.SourceMdl)return 1;
    A830ReleasePresent(&first);
    memset(second.Source,0x5a,second.SourceBytes);
    if(A830CopyPresent(&second)!=STATUS_SUCCESS || a->Memory.Segment.Cpu[4096]!=0x5a)return 1;
    A830ReleasePresent(&second);if(maps!=2 || unmaps!=2 || freed!=2)return 1;
    /* Private BOs have an independent per-job reference, including when a
       source and destination are the same allocation. */
    A830_ALLOCATION owned={0};owned.Buffer.Mdl=(PMDL)(ULONG_PTR)1;owned.Buffer.Cpu=a->Memory.Segment.Cpu;
    first=(A830_DMA){0};first.Operation=6;first.SourceAllocation=first.DestinationAllocation=&owned;
    if(A830PinPresent(&first)!=STATUS_SUCCESS || owned.ScanoutReferences!=2)return 1;
    A830ReleasePresent(&first);A830ReleasePresent(&first);if(owned.ScanoutReferences)return 1;
    free(a->Memory.Segment.Cpu);free(a);
    puts("{\"present_allocation_list_layout\":\"PASS\",\"dirty_rect_preservation\":\"PASS\",\"multipass\":\"PASS\",\"invalid_rect\":\"PASS\",\"resubmit_independent_mapping\":\"PASS\",\"owned_present_pin\":\"PASS\"}");return 0;
}
