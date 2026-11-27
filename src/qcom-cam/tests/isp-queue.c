#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define RtlZeroMemory(p,n) memset(p,0,n)
#include "../isp-queue.h"
#include "../isp-bus.h"

int main(void) {
    CAM_ISP_QUEUE queue;CAM_ISP_LEASE preview,video,photo,leases[4];ULONG slot;
    assert(CamIspQueueInit(&queue,1)&&CamIspQueueIdle(&queue));
    assert(CamIspQueueArm(&queue,&slot)&&!CamIspQueueIdle(&queue));
    assert(!CamIspQueueAcquire(&queue,1,0,&preview));
    assert(!CamIspQueueComplete(&queue,slot^1,100,120));
    assert(!CamIspQueueComplete(&queue,slot,120,100));
    assert(CamIspQueueComplete(&queue,slot,100,120));
    assert(CamIspQueueAcquire(&queue,1,0,&preview));
    assert(CamIspQueueAcquire(&queue,2,0,&video));
    assert(CamIspQueueAcquire(&queue,4,0,&photo));
    assert(preview.Slot==video.Slot&&photo.Sequence==video.Sequence);
    assert(!CamIspQueueAcquire(&queue,3,0,&leases[0]));
    assert(!CamIspQueueAcquire(&queue,1,0,&leases[0]));
    assert(CamIspQueueRelease(&queue,&preview)&&!CamIspQueueIdle(&queue));
    assert(!CamIspQueueRelease(&queue,&preview));
    assert(CamIspQueueArm(&queue,&slot)&&slot!=video.Slot);
    assert(!CamIspQueueComplete(&queue,slot,100,120));
    assert(CamIspQueueComplete(&queue,slot,200,220));
    assert(CamIspQueueRelease(&queue,&photo)&&CamIspQueueRelease(&queue,&video));
    assert(CamIspQueueIdle(&queue));
    assert(CamIspQueueInit(&queue,2));
    for(ULONG i=0;i<4;i++) {
        assert(CamIspQueueArm(&queue,&slot));
        assert(CamIspQueueComplete(&queue,slot,300+i*100,320+i*100));
        assert(CamIspQueueAcquire(&queue,1,i,&leases[i]));
        for(ULONG j=0;j<i;j++)assert(leases[j].Slot!=slot);
    }
    assert(!CamIspQueueArm(&queue,&slot));
    assert(CamIspQueueRelease(&queue,&leases[0]));
    assert(CamIspQueueArm(&queue,&slot)&&slot==leases[0].Slot);
    assert(CamIspQueueComplete(&queue,slot,700,720));
    assert(CamIspQueueAcquire(&queue,1,4,&preview));
    assert(!CamIspQueueRelease(&queue,&leases[0]));
    for(ULONG i=1;i<4;i++)assert(CamIspQueueRelease(&queue,&leases[i]));
    assert(CamIspQueueRelease(&queue,&preview)&&CamIspQueueIdle(&queue));
    assert(CamIspQueueInit(&queue,3));
    assert(!CamIspQueueRelease(&queue,&preview));
    for(ULONG i=0;i<4;i++) {
        assert(CamIspQueueArm(&queue,&slot));
        assert(CamIspQueueComplete(&queue,slot,100+i*100,120+i*100));
        if(i<3)assert(CamIspQueueAcquire(&queue,1,i,&leases[i]));
    }
    ULONG latest=queue.Latest;
    assert(CamIspQueueArm(&queue,&slot)&&slot==latest);
    assert(!CamIspQueueAcquire(&queue,2,0,&video));
    assert(CamIspQueueComplete(&queue,slot,500,520));
    assert(CamIspQueueAcquire(&queue,2,4,&video)&&video.Sequence==5);
    assert(CamIspQueueRelease(&queue,&video));
    for(ULONG i=0;i<3;i++)assert(CamIspQueueRelease(&queue,&leases[i]));
    assert(CamIspQueueIdle(&queue)&&CamIspQueueInit(&queue,4));
    for(ULONG i=1;i<=10000;i++) {
        assert(CamIspQueueArm(&queue,&slot));
        assert(CamIspQueueComplete(&queue,slot,(ULONGLONG)i*100,(ULONGLONG)i*100+20));
        assert(CamIspQueueAcquire(&queue,2,i-1,&video));
        if(i%3==0)assert(CamIspQueueAcquire(&queue,1,i-1,&preview));
        if(i%5==0)assert(CamIspQueueAcquire(&queue,4,i-1,&photo));
        assert(CamIspQueueRelease(&queue,&video));
        if(i%3==0)assert(CamIspQueueRelease(&queue,&preview));
        if(i%5==0)assert(CamIspQueueRelease(&queue,&photo));
        assert(CamIspQueueIdle(&queue));
    }
    CAM_ISP_FD_PLAN plan;
    assert(CamIspFdPlan(&plan,424,240,0x100080,155520));
    assert(CamIspFdFrameReady(8,0x18,100,120,80,4096,4501,&plan.Output));
    assert(!CamIspFdFrameReady(0x30000,0x18,100,120,80,4096,4501,&plan.Output));
    assert(!CamIspFdFrameReady(8,1,100,120,80,4096,4501,&plan.Output));
    assert(!CamIspFdFrameReady(8,0x18,100,120,120,4096,4501,&plan.Output));
    assert(!CamIspFdFrameReady(8,0x18,100,120,80,4096,4502,&plan.Output));
    puts("ISP buffer ownership: concurrent consumers, exhaustion, delayed/stale releases, 10000 frames and paired NV12 completion passed");
    return 0;
}
