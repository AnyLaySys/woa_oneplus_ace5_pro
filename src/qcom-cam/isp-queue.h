#pragma once

#define CAM_ISP_SLOTS 4u
#define CAM_ISP_CONSUMERS 7u

typedef struct {
    ULONG Armed,Latest,Held[CAM_ISP_SLOTS],Leased[CAM_ISP_SLOTS];
    ULONGLONG Epoch,Sequence[CAM_ISP_SLOTS],Sof[CAM_ISP_SLOTS],Eof[CAM_ISP_SLOTS];
} CAM_ISP_QUEUE;

typedef struct { ULONG Slot,Consumer; ULONGLONG Epoch,Sequence; } CAM_ISP_LEASE;

static BOOLEAN CamIspQueueInit(CAM_ISP_QUEUE *queue,ULONGLONG epoch) {
    if(!queue||!epoch)return FALSE;
    RtlZeroMemory(queue,sizeof(*queue));queue->Armed=queue->Latest=MAXULONG;queue->Epoch=epoch;
    return TRUE;
}

static BOOLEAN CamIspQueueArm(CAM_ISP_QUEUE *queue,ULONG *slot) {
    if(!queue||!slot||queue->Armed!=MAXULONG)return FALSE;
    for(ULONG i=0;i<CAM_ISP_SLOTS;i++)if(!queue->Held[i]&&i!=queue->Latest) {
        queue->Armed=i;*slot=i;return TRUE;
    }
    if(queue->Latest<CAM_ISP_SLOTS&&!queue->Held[queue->Latest]) {
        queue->Armed=queue->Latest;*slot=queue->Latest;return TRUE;
    }
    return FALSE;
}

static BOOLEAN CamIspQueueComplete(CAM_ISP_QUEUE *queue,ULONG slot,ULONGLONG sof,ULONGLONG eof) {
    if(!queue||slot>=CAM_ISP_SLOTS||queue->Armed!=slot||queue->Held[slot]||!sof||eof<=sof)return FALSE;
    ULONGLONG sequence=1;
    if(queue->Latest<CAM_ISP_SLOTS) {
        if(sof<=queue->Sof[queue->Latest]||eof<=queue->Eof[queue->Latest]||queue->Sequence[queue->Latest]==~0ULL)return FALSE;
        sequence=queue->Sequence[queue->Latest]+1;
    }
    queue->Sequence[slot]=sequence;queue->Sof[slot]=sof;queue->Eof[slot]=eof;
    queue->Leased[slot]=0;queue->Latest=slot;queue->Armed=MAXULONG;return TRUE;
}

static BOOLEAN CamIspQueueAcquire(CAM_ISP_QUEUE *queue,ULONG consumer,ULONGLONG after,CAM_ISP_LEASE *lease) {
    if(!queue||!lease||!consumer||(consumer&(consumer-1))||(consumer&~CAM_ISP_CONSUMERS)||
       queue->Latest>=CAM_ISP_SLOTS)return FALSE;
    ULONG slot=queue->Latest;
    if(queue->Armed==slot||queue->Sequence[slot]<=after||(queue->Leased[slot]&consumer))return FALSE;
    queue->Held[slot]|=consumer;queue->Leased[slot]|=consumer;
    lease->Slot=slot;lease->Consumer=consumer;lease->Epoch=queue->Epoch;lease->Sequence=queue->Sequence[slot];return TRUE;
}

static BOOLEAN CamIspQueueRelease(CAM_ISP_QUEUE *queue,const CAM_ISP_LEASE *lease) {
    if(!queue||!lease||lease->Slot>=CAM_ISP_SLOTS||lease->Epoch!=queue->Epoch||
       lease->Sequence!=queue->Sequence[lease->Slot]||!lease->Consumer||
       (lease->Consumer&(lease->Consumer-1))||!(queue->Held[lease->Slot]&lease->Consumer))return FALSE;
    queue->Held[lease->Slot]&=~lease->Consumer;return TRUE;
}

static BOOLEAN CamIspQueueIdle(const CAM_ISP_QUEUE *queue) {
    if(!queue||queue->Armed!=MAXULONG)return FALSE;
    for(ULONG i=0;i<CAM_ISP_SLOTS;i++)if(queue->Held[i])return FALSE;
    return TRUE;
}
