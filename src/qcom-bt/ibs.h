#pragma once
#define QBT_IBS_IDLE_TICKS 2000000ull
#define QBT_IBS_RETRY_TICKS 1000000ull
#define QBT_IBS_TIMEOUT_TICKS 20000000ull
typedef struct {
 unsigned State;
 unsigned long long ChangedAt, Deadline;
} QBT_IBS;
/* State 0 = awake, 1 = asleep, 2 = waking. Actions are wire bytes. */
static unsigned char QbtIbsAction(QBT_IBS*s,unsigned long long now,int pending){
 if(s->State==0){if(!pending&&now-s->ChangedAt>=QBT_IBS_IDLE_TICKS){s->State=1;return 0xfe;}return 0;}
 if(s->State==1){if(!pending)return 0;s->State=2;s->Deadline=now+QBT_IBS_TIMEOUT_TICKS;s->ChangedAt=now;return 0xfd;}
 if(now>=s->Deadline)return 0xff;
 if(now-s->ChangedAt>=QBT_IBS_RETRY_TICKS){s->ChangedAt=now;return 0xfd;}
 return 0;
}
static void QbtIbsReceived(QBT_IBS*s,unsigned char byte,unsigned long long now){
 if(byte==0xfc&&s->State==2){s->State=0;s->ChangedAt=now;}
}
