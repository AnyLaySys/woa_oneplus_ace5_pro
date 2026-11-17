#pragma once
typedef struct { ULONGLONG Started;ULONG X,Y; } TAP_POINT;
typedef struct { TAP_POINT Points[16];ULONG Active,Pending; } TAP;
static ULONG TapUpdate(TAP *tap,ULONGLONG now,UCHAR count,const UCHAR points[][6]) {
    ULONG active=0;
    for(UCHAR i=0;i<count;i++) {
        UCHAR id=points[i][1]&15;ULONG bit=1u<<id,x=points[i][2]|((ULONG)points[i][3]<<8),y=points[i][4]|((ULONG)points[i][5]<<8);
        TAP_POINT *p=&tap->Points[id];active|=bit;
        if(!(tap->Active&bit)){*p=(TAP_POINT){now,x,y};tap->Pending|=bit;}
        else if((x>p->X?x-p->X:p->X-x)>512 || (y>p->Y?y-p->Y:p->Y-y)>512)tap->Pending&=~bit;
    }
    ULONG pressed=active&~tap->Active;tap->Active=active;tap->Pending&=active;return pressed;
}
static ULONG TapHold(TAP *tap,ULONGLONG now) {
    ULONG held=0;
    for(UCHAR i=0;i<16;i++) {
        TAP_POINT *p=&tap->Points[i];
        if((tap->Pending&(1u<<i)) && now>=p->Started && now-p->Started>4950000)held|=1u<<i;
    }
    tap->Pending&=~held;return held;
}
