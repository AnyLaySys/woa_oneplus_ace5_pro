#pragma once
typedef struct { ULONGLONG PowerAt,VolumeAt,RepeatDue;UCHAR State,Volume,Limited;BOOLEAN PowerDone; } KEY_FEEDBACK;
static ULONG KeyFeedback(KEY_FEEDBACK *f,UCHAR state,ULONGLONG now,ULONG limits) {
    ULONG action=0;UCHAR pressed=state&~f->State,volume=(state==2 || state==4)?state:0;
    if(pressed&1){f->PowerAt=now;f->PowerDone=FALSE;}
    if(pressed&6)action=1;
    if((state&1) && (state&6)){f->PowerDone=TRUE;if(pressed)action=1;}
    if((f->State&1) && !(state&1) && !f->PowerDone)action=1;
    if(state==1 && !f->PowerDone && now>=f->PowerAt && now-f->PowerAt>=24000000){f->PowerDone=TRUE;action=1;}
    if(volume!=f->Volume){f->VolumeAt=now;f->RepeatDue=now+2700000;f->Limited=FALSE;}
    BOOLEAN boundary=(limits&QH_VOLUME_READY) && (limits&(volume==2?QH_VOLUME_MAX:QH_VOLUME_MIN));
    if(volume && (limits&QH_VOLUME_READY) && !boundary)f->Limited=FALSE;
    if(volume && f->Limited!=2 && now>=f->VolumeAt && now-f->VolumeAt>=2700000) {
        if(boundary || f->Limited==1){f->Limited=1;action=2;}
        else if(now>=f->RepeatDue){f->RepeatDue=now+270000;action=5;}
    }
    f->Volume=volume;f->State=state;return action;
}
