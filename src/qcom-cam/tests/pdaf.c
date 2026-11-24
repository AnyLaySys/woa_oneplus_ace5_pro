#include "cam.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static VOID PutWord(UCHAR *p,ULONG n) {p[0]=(UCHAR)n;p[1]=(UCHAR)(n>>8);}
static VOID Pack(UCHAR *row,ULONG x,ULONG n) {
    ULONG off=x/4*5,shift=(x&3)*2;row[off+(x&3)]=(UCHAR)(n>>2);
    row[off+4]=(UCHAR)((row[off+4]&~(3u<<shift))|((n&3)<<shift));
}
static ULONG Texture(ULONG x,ULONG y) {
    ULONG sum=0;
    for(ULONG i=0;i<5;i++){ULONG v=(x+i+y*1777)*747796405u+2891336453u;v=((v>>((v>>28)+4))^v)*277803737u;sum+=(v^(v>>22))&255;}
    return 100+sum/8;
}
int main(void) {
    UCHAR data[1024]={0};CAM_PDAF_CAL calibration={0};LONG workspace[2520];
    PutWord(data,10);PutWord(data+4,17);PutWord(data+6,13);PutWord(data+8,4);PutWord(data+10,8);PutWord(data+12,6);
    for(ULONG i=0;i<221;i++){PutWord(data+32+i*2,512);PutWord(data+474+i*2,256);}
    for(ULONG i=0;i<48;i++)PutWord(data+928+i*2,2880);
    assert(CamPdafCalibration(&calibration,data,sizeof(data)));
    assert(CamPdafGain(calibration.Left,0,0)==512&&CamPdafGain(calibration.Right,2047,767)==256);
    UCHAR *raw=calloc(1,QCAM_PDAF_BYTES);assert(raw);
    for(LONG shift=-8;shift<=8;shift+=4) {
        for(ULONG y=0;y<QCAM_PDAF_HEIGHT;y++)for(ULONG x=0;x<2048;x++) {
            Pack(raw+y*QCAM_STRIDE,x*2,64+Texture(x,y)/2);
            Pack(raw+y*QCAM_STRIDE,x*2+1,64+Texture((ULONG)((LONG)x-shift),y));
        }
        CAM_PDAF_RESULT result=CamPdafEstimate(raw,QCAM_PDAF_BYTES,&calibration,workspace);
        assert(result.Confidence>900&&abs(result.Phase-shift*256)<128);
        assert(abs(result.Defocus+shift*90)<45);
    }
    UCHAR *dual=malloc(QCAM_PDAF_BYTES*4+256),*gathered=calloc(1,QCAM_PDAF_BYTES);assert(dual&&gathered);
    memset(dual,0xa5,QCAM_PDAF_BYTES*4+256);
    for(ULONG i=0;i<QCAM_PDAF_BYTES;i++)dual[i+((i&256)?QCAM_PDAF_BYTES*2+256:0)]=raw[i];
    for(ULONG group=0;group<3;group++)CamPdafGather(dual,gathered,group);
    CAM_PDAF_RESULT expected=CamPdafEstimate(raw,QCAM_PDAF_BYTES,&calibration,workspace);
    CAM_PDAF_RESULT actual=CamPdafEstimate(gathered,QCAM_PDAF_BYTES,&calibration,workspace);
    assert(!memcmp(&expected,&actual,sizeof(actual)));
    for(ULONG window=0;window<9;window++) {
        expected=CamPdafWindow(raw,&calibration,window,workspace);actual=CamPdafWindow(gathered,&calibration,window,workspace);
        assert(!memcmp(&expected,&actual,sizeof(actual)));
    }
    QCAM_ROI_OPTIONS roi={sizeof(roi),QCAM_ABI,QCAM_ROI_FOCUS,0,0,65536,65536};ULONG x,y;
    CamPdafRoiOrigin(&roi,QCAM_ZOOM_MIN,&x,&y);assert(x==896&&y==304);
    roi.Right=roi.Bottom=1024;CamPdafRoiOrigin(&roi,QCAM_ZOOM_MIN,&x,&y);assert(!x&&!y);
    roi.Left=roi.Top=64512;roi.Right=roi.Bottom=65536;CamPdafRoiOrigin(&roi,QCAM_ZOOM_MIN,&x,&y);assert(x==1792&&y==615);
    CamPdafRoiOrigin(&roi,QCAM_ZOOM_MAX,&x,&y);assert(x<1792&&y<615);
    for(ULONG first=0;first<=615;first+=41) {
        memset(gathered,0,QCAM_PDAF_BYTES);CamPdafGatherRows(dual,gathered,first);
        expected=CamPdafRegion(raw,&calibration,1792,first,9,workspace);actual=CamPdafRegion(gathered,&calibration,1792,first,9,workspace);
        assert(!memcmp(&expected,&actual,sizeof(actual))&&actual.Confidence>900);
    }
    free(dual);free(gathered);
    memset(raw,0,QCAM_PDAF_BYTES);assert(!CamPdafEstimate(raw,QCAM_PDAF_BYTES,&calibration,workspace).Confidence);
    assert(!CamPdafEstimate(raw,QCAM_PDAF_BYTES-1,&calibration,workspace).Confidence);
    PutWord(data+32,0);assert(!CamPdafCalibration(&calibration,data,sizeof(data)));
    assert(!calibration.Valid);assert(!CamPdafCalibration(&calibration,data,1023));
    free(raw);puts("PDAF calibration, gain correction, positive and negative phase, conversion, low signal and invalid data passed");return 0;
}
