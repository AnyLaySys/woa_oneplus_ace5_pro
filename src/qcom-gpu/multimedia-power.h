#pragma once

#define GPU_MULTIMEDIA_DISPLAY 1u
#define GPU_MULTIMEDIA_VIDEO 2u

typedef struct {
    unsigned Address,Original,Applied,Owners,ExternalFloor;
    int Saved;
} GPU_MULTIMEDIA_RAIL;
typedef int (*GPU_RAIL_TRANSFER)(void *context,unsigned address,int write,unsigned *value);

/* Apps DRV2 has one vote per resource, shared by display and video. The
   inherited vote is a floor, not an encoder-owned vote to switch off. */
static inline int GpuMultimediaRailChange(GPU_MULTIMEDIA_RAIL *rail,unsigned owner,int enable,
    unsigned minimum,void *context,GPU_RAIL_TRANSFER transfer) {
    unsigned owners=enable?rail->Owners|owner:rail->Owners&~owner;
    if(!enable && !(rail->Owners&owner))return 0;
    unsigned current=0;int status=transfer(context,rail->Address,0,&current);
    if(status<0)return status;
    if(!rail->Saved){rail->Original=rail->Applied=current;rail->Saved=1;}
    if(current>rail->Applied && current>rail->ExternalFloor)rail->ExternalFloor=current;
    unsigned target=owners?minimum:0;
    if(target<rail->Original)target=rail->Original;
    if(target<rail->ExternalFloor)target=rail->ExternalFloor;
    /* A larger vote written by another driver must survive our release. */
    if((current>rail->Applied || enable) && current>target)target=current;
    if(enable)rail->Owners=owners;
    if(current!=target) {
        unsigned value=target;status=transfer(context,rail->Address,1,&value);
        if(status<0)return status;
    }
    rail->Applied=target;rail->Owners=owners;return 0;
}
