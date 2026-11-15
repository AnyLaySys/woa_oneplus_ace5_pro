#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "../uapi.h"

#define INTERVAL_CAPACITY 16384u
static double intervals[INTERVAL_CAPACITY];
static unsigned intervalCount;
static GPU_U64 lastSequence,lastTime,missingSamples,edgeGaps;
static double vsyncIntervals[INTERVAL_CAPACITY];
static unsigned vsyncIntervalCount;
static GPU_U64 lastVsyncSequence,lastVsyncTime,missingVsyncSamples,vsyncEdgeGaps;
static int Compare(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;return x<y?-1:x>y?1:0;
}
static int SampleCompare(const void *a,const void *b) {
    GPU_U64 x=((const GPU_DISPLAY_TE_SAMPLE *)a)->Sequence,y=((const GPU_DISPLAY_TE_SAMPLE *)b)->Sequence;
    return x<y?-1:x>y?1:0;
}
static int ReadState(HANDLE gpu,GPU_DISPLAY_ACTIVATION *state) {
    /* Phase zero is strictly a RAM snapshot, never an activation step. */
    GPU_DISPLAY_ACTIVATION_REQUEST request={1,0,0,0,0,0};DWORD bytes=0;
    ZeroMemory(state,sizeof(*state));
    if(!DeviceIoControl(gpu,GPU_IOCTL_DISPLAY_ACTIVATION,&request,sizeof(request),state,sizeof(*state),&bytes,NULL))return 0;
    if(bytes!=sizeof(*state) || state->Version!=1 || state->Size!=sizeof(*state)){SetLastError(ERROR_REVISION_MISMATCH);return 0;}
    return 1;
}
static int ReadTelemetry(HANDLE gpu,GPU_TELEMETRY *t) {
    DWORD bytes=0;ZeroMemory(t,sizeof(*t));
    if(!DeviceIoControl(gpu,GPU_IOCTL_TELEMETRY,NULL,0,t,sizeof(*t),&bytes,NULL))return 0;
    if(bytes!=sizeof(*t) || t->Version!=GPU_TELEMETRY_VERSION || t->Size!=sizeof(*t)){SetLastError(ERROR_REVISION_MISMATCH);return 0;}
    return 1;
}
static void Collect(const GPU_DISPLAY_ACTIVATION *state) {
    GPU_DISPLAY_TE_SAMPLE samples[GPU_DISPLAY_TE_SAMPLES];memcpy(samples,state->TeSamples,sizeof(samples));
    qsort(samples,GPU_DISPLAY_TE_SAMPLES,sizeof(*samples),SampleCompare);
    for(unsigned i=0;i<GPU_DISPLAY_TE_SAMPLES;i++) {
        const GPU_DISPLAY_TE_SAMPLE *sample=samples+i;
        if(!sample->Sequence || sample->Sequence<=lastSequence)continue;
        if(lastSequence && sample->Sequence>lastSequence+1)missingSamples+=sample->Sequence-lastSequence-1;
        if(sample->Delta>1)edgeGaps+=sample->Delta-1;
        if(lastSequence && lastTime && sample->Delta==1 && sample->Sequence==lastSequence+1 && sample->Time100ns>lastTime && intervalCount<INTERVAL_CAPACITY)
            intervals[intervalCount++]=(double)(sample->Time100ns-lastTime)/10000.0;
        lastSequence=sample->Sequence;lastTime=sample->Time100ns;
    }
}
static void CollectVsync(const GPU_DISPLAY_ACTIVATION *state) {
    GPU_DISPLAY_TE_SAMPLE samples[GPU_DISPLAY_TE_SAMPLES];memcpy(samples,state->VsyncSamples,sizeof(samples));
    qsort(samples,GPU_DISPLAY_TE_SAMPLES,sizeof(*samples),SampleCompare);
    for(unsigned i=0;i<GPU_DISPLAY_TE_SAMPLES;i++) {
        const GPU_DISPLAY_TE_SAMPLE *sample=samples+i;
        if(!sample->Sequence || sample->Sequence<=lastVsyncSequence)continue;
        if(lastVsyncSequence && sample->Sequence>lastVsyncSequence+1)missingVsyncSamples+=sample->Sequence-lastVsyncSequence-1;
        unsigned divisor=state->BackendFlags&64u?2u:1u;
        if(sample->Delta>divisor)vsyncEdgeGaps+=sample->Delta-divisor;
        /* For actual notifications a missed TE still makes one long VSync
         * interval. Keep it in the distribution rather than hiding jitter. */
        if(lastVsyncSequence && lastVsyncTime && sample->Sequence==lastVsyncSequence+1 && sample->Time100ns>lastVsyncTime && vsyncIntervalCount<INTERVAL_CAPACITY)
            vsyncIntervals[vsyncIntervalCount++]=(double)(sample->Time100ns-lastVsyncTime)/10000.0;
        lastVsyncSequence=sample->Sequence;lastVsyncTime=sample->Time100ns;
    }
}
static void PrintDistribution(double *values,unsigned count) {
    if(!count){printf("null");return;}
    qsort(values,count,sizeof(*values),Compare);
    printf("{\"samples\":%u,\"min\":%.5f,\"median\":%.5f,\"p95\":%.5f,\"max\":%.5f}",count,values[0],values[count/2],values[(count-1)*95/100],values[count-1]);
}
static void PrintState(const char *name,const GPU_DISPLAY_ACTIVATION *s) {
    printf("\"%s\":{\"phase\":%u,\"step\":%u,\"status\":\"%08x\",\"proof_mask\":%u,\"backend_flags\":%u,\"te_edges\":%llu,\"te_observed\":%llu,\"te_skipped\":%llu,\"period_100ns\":%llu,\"last_te_100ns\":%llu,\"active_address\":\"%llx\",\"pending_address\":\"%llx\",\"queued\":%llu,\"applied\":%llu}",
        name,s->Phase,s->Step,s->LastStatus,s->ProofMask,s->BackendFlags,s->TeEdges,s->TeObserved,s->TeSkipped,s->TePeriod100ns,s->TeLast100ns,s->ActiveAddress,s->PendingAddress,s->Queued,s->Applied);
}
int main(int argc,char **argv) {
    unsigned seconds=argc>1?(unsigned)strtoul(argv[1],NULL,10):10;
    if(argc>2 || seconds<1 || seconds>60){puts("{\"error\":\"duration must be 1..60 seconds\"}");return 2;}
    setvbuf(stdout,NULL,_IONBF,0);
    HANDLE gpu=CreateFileW(L"\\\\.\\A830Gpu",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
    if(gpu==INVALID_HANDLE_VALUE){printf("{\"open_error\":%lu}\n",GetLastError());return 1;}
    GPU_DISPLAY_ACTIVATION first,last;GPU_TELEMETRY before,after;
    LARGE_INTEGER frequency,start,now;QueryPerformanceFrequency(&frequency);
    if(!ReadState(gpu,&first) || !ReadTelemetry(gpu,&before)){printf("{\"query_error\":%lu}\n",GetLastError());CloseHandle(gpu);return 1;}
    last=first;lastSequence=first.TeObserved;lastTime=first.TeLast100ns;
    lastVsyncSequence=first.VsyncObserved;lastVsyncTime=0;
    for(unsigned i=0;i<GPU_DISPLAY_TE_SAMPLES;i++)
        if(first.VsyncSamples[i].Sequence==lastVsyncSequence)lastVsyncTime=first.VsyncSamples[i].Time100ns;
    QueryPerformanceCounter(&start);unsigned samples=0;int changed=0;
    do {
        Sleep(100);
        if(!ReadState(gpu,&last)){printf("{\"query_error\":%lu,\"during_sampling\":true}\n",GetLastError());CloseHandle(gpu);return 1;}
        if(last.TeObserved<first.TeObserved || last.TeEdges<first.TeEdges || last.Applied<first.Applied || last.LatchedCount<first.LatchedCount || last.VsyncObserved<first.VsyncObserved || last.GpuCopyCount<first.GpuCopyCount || last.GpuCopyBytes<first.GpuCopyBytes || ((last.BackendFlags^first.BackendFlags)&15u))changed=1;
        if(!changed){Collect(&last);CollectVsync(&last);}samples++;
        QueryPerformanceCounter(&now);
    } while(now.QuadPart-start.QuadPart<(LONGLONG)seconds*frequency.QuadPart);
    if(!ReadTelemetry(gpu,&after)){printf("{\"telemetry_error\":%lu}\n",GetLastError());CloseHandle(gpu);return 1;}
    CloseHandle(gpu);
    double elapsed=(double)(now.QuadPart-start.QuadPart)/(double)frequency.QuadPart;
    if(after.CpuScanoutBytes<before.CpuScanoutBytes || after.PresentFlip<before.PresentFlip || after.LastVsyncTime<before.LastVsyncTime)changed=1;
    printf("{\"seconds\":%.6f,\"samples\":%u,\"sampling_interval_ms\":100,\"state_changed_or_counters_reset\":%s,",elapsed,samples,changed?"true":"false");
    PrintState("before",&first);printf(",");PrintState("after",&last);
    int hardware=!changed && (last.BackendFlags&1u) && last.TeEdges>first.TeEdges;
    int proxy=(last.BackendFlags&32u)!=0;
    const char *source=proxy?"tx_frame_external_te_autorefresh_proxy":last.BackendFlags&16u?"rd_ptr_frame_counter":"unavailable";
    printf(",\"counter_source\":\"%s\",\"te_irq_measured\":false,\"hardware_counter_available\":%s,\"hardware_te_available\":%s,\"external_te_source\":%s,\"dpu_vsync_enabled\":%s,\"vsync_hardware_frame_divisor\":%u,\"hardware_counter_hz\":",source,hardware?"true":"false",hardware && !proxy?"true":"false",last.BackendFlags&2u?"true":"false",last.BackendFlags&8u?"true":"false",last.BackendFlags&64u?2u:1u);
    if(hardware)printf("%.4f",(double)(last.TeEdges-first.TeEdges)/elapsed);else printf("null");
    printf(",\"vsync_notifications_hz\":");if(!changed && last.VsyncObserved>first.VsyncObserved)printf("%.4f",(double)(last.VsyncObserved-first.VsyncObserved)/elapsed);else printf("null");
    printf(",\"hardware_te_hz\":");if(hardware && !proxy)printf("%.4f",(double)(last.TeEdges-first.TeEdges)/elapsed);else printf("null");
    printf(",\"te_counter_delta\":");if(!changed)printf("%llu",last.TeEdges-first.TeEdges);else printf("null");
    printf(",\"te_observation_interval_ms\":");
    if(hardware)PrintDistribution(intervals,intervalCount);else printf("null");
    printf(",\"vsync_notification_interval_ms\":");if(hardware && !changed)PrintDistribution(vsyncIntervals,vsyncIntervalCount);else printf("null");
    printf(",\"missed_vsync_ring_samples\":%llu,\"unexpected_hardware_frame_gaps_between_vsync\":%llu,\"latched_presents\":",missingVsyncSamples,vsyncEdgeGaps);
    if(!changed)printf("%llu",last.LatchedCount-first.LatchedCount);else printf("null");
    printf(",\"interval_time_source\":\"kernel observation of hardware counter changes\",\"missed_ring_samples\":%llu,\"skipped_te_edges_in_samples\":%llu,\"present_latched_sequence_delta\":",missingSamples,edgeGaps);
    if(!changed)printf("%llu",last.Applied-first.Applied);else printf("null");
    printf(",\"wddm_flip_requests_delta\":");if(!changed)printf("%llu",after.PresentFlip-before.PresentFlip);else printf("null");
    printf(",\"cpu_scanout_bytes_delta\":");if(!changed)printf("%llu",after.CpuScanoutBytes-before.CpuScanoutBytes);else printf("null");
    printf(",\"vsync_interval_bucket_bounds_ms\":[1,2,4,8,16,20,33,67,null],\"vsync_interval_bucket_counts\":[");
    for(unsigned i=0;i<GPU_TIMING_BUCKETS;i++){if(i)printf(",");if(after.VsyncInterval[i]>=before.VsyncInterval[i])printf("%llu",after.VsyncInterval[i]-before.VsyncInterval[i]);else printf("null");}
    printf("],\"gpu_copy_fallback_enabled\":%s,\"gpu_copy_pending\":%llu,\"gpu_copy_count_delta\":",last.BackendFlags&4u?"true":"false",last.GpuCopyPending);
    if(!changed)printf("%llu",last.GpuCopyCount-first.GpuCopyCount);else printf("null");
    printf(",\"gpu_copy_bytes_delta\":");if(!changed)printf("%llu",last.GpuCopyBytes-first.GpuCopyBytes);else printf("null");
    printf(",\"panel_content_fps\":null,\"s4_acceptance\":\"%s\"}\n",changed?"COUNTERS_CHANGED":hardware?"REQUIRES_VISUAL_VALIDATION":"HARDWARE_TE_UNAVAILABLE");
    return changed?3:0;
}
