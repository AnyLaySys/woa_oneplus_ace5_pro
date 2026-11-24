#pragma once

typedef struct { ULONGLONG SensorStart; LONGLONG PresentationStart,Last; BOOLEAN Ready; } CAM_FRAME_CLOCK;

static inline ULONGLONG CamFrameTicks(ULONGLONG ticks,ULONG hz) {
    return ticks/hz*10000000+ticks%hz*10000000/hz;
}

static inline BOOLEAN CamFrameClock(CAM_FRAME_CLOCK *clock,const QCAM_FRAME_TIME *frame,ULONGLONG started,LONGLONG reference,LONGLONG *time) {
    if(frame->Size!=sizeof(*frame)||frame->Abi!=QCAM_ABI||!frame->ClockHz||!frame->Sequence||frame->Sof>frame->Eof||frame->Captured<started)return FALSE;
    if(!clock->Ready) {
        clock->SensorStart=frame->Sof;
        clock->PresentationStart=reference+(LONGLONG)(frame->Captured-started)-(LONGLONG)CamFrameTicks(frame->Eof-frame->Sof,frame->ClockHz);
    } else if(frame->Sof<=clock->SensorStart)return FALSE;
    LONGLONG result=clock->PresentationStart+(LONGLONG)CamFrameTicks(frame->Sof-clock->SensorStart,frame->ClockHz);
    if(clock->Ready&&result<=clock->Last)return FALSE;
    clock->Last=result;clock->Ready=TRUE;*time=result;return TRUE;
}
