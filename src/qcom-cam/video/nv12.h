#pragma once

#define QCAM_VIDEO_WIDTH 1024u
#define QCAM_VIDEO_HEIGHT 768u
#define QCAM_VIDEO_BYTES (QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT*3/2)

static UCHAR CamClip(LONG value) {return (UCHAR)(value<0?0:value>255?255:value);}

static ULONG CamRawPixel(const UCHAR *row,ULONG x) {
    const UCHAR *p=row+(x/4)*5;ULONG n=x&3;
    return ((ULONG)p[n]<<2)|((p[4]>>(2*n))&3);
}

static LONG CamColor(ULONG value) {return value<=64?0:(LONG)((value-64)*255/959);}

static VOID CamRgb(const UCHAR *raw,ULONG x,ULONG y,LONG *red,LONG *green,LONG *blue) {
    const UCHAR *row=raw+y*QCAM_STRIDE;
    *red=CamColor(CamRawPixel(row,x));
    *green=CamColor((CamRawPixel(row,x+1)+CamRawPixel(row+QCAM_STRIDE,x))/2);
    *blue=CamColor(CamRawPixel(row+QCAM_STRIDE,x+1));
}

static BOOLEAN CamNv12(const UCHAR *raw,ULONG rawBytes,UCHAR *output,ULONG outputBytes) {
    if(!raw||!output||rawBytes<QCAM_FRAME_BYTES||outputBytes<QCAM_VIDEO_BYTES)return FALSE;
    UCHAR *uv=output+QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;
    for(ULONG y=0;y<QCAM_VIDEO_HEIGHT;y+=2)for(ULONG x=0;x<QCAM_VIDEO_WIDTH;x+=2) {
        LONG sumR=0,sumG=0,sumB=0;
        for(ULONG dy=0;dy<2;dy++)for(ULONG dx=0;dx<2;dx++) {
            LONG r,g,b;CamRgb(raw,(x+dx)*4,(y+dy)*4,&r,&g,&b);
            output[(y+dy)*QCAM_VIDEO_WIDTH+x+dx]=CamClip(16+((66*r+129*g+25*b+128)>>8));
            sumR+=r;sumG+=g;sumB+=b;
        }
        LONG r=sumR/4,g=sumG/4,b=sumB/4;
        ULONG off=(y/2)*QCAM_VIDEO_WIDTH+x;
        uv[off]=CamClip(128+((-38*r-74*g+112*b+128)>>8));
        uv[off+1]=CamClip(128+((112*r-94*g-18*b+128)>>8));
    }
    return TRUE;
}
