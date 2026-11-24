#pragma once

typedef struct { ULONG Offset; UCHAR Red[8],Green[8],Low[8]; CHAR Shift[8]; } CAM_RAW_BLOCK;

static inline VOID CamRawZoom(ULONG zoom,CAM_RAW_BLOCK blocks[128],ULONG rows[768]) {
    ULONG width=(ULONG)(((ULONGLONG)QCAM_WIDTH<<16)/zoom),height=(ULONG)(((ULONGLONG)QCAM_HEIGHT<<16)/zoom);
    ULONG left=((QCAM_WIDTH-width)/2)&~1u,top=((QCAM_HEIGHT-height)/2)&~1u;
    for(ULONG x=0;x<QCAM_VIDEO_WIDTH;x+=8) {
        CAM_RAW_BLOCK *b=blocks+x/8;
        ULONG first=left+((x*width/QCAM_VIDEO_WIDTH)&~1u);b->Offset=first/4*5;
        for(ULONG i=0;i<8;i++) {
            ULONG pixel=left+(((x+i)*width/QCAM_VIDEO_WIDTH)&~1u),offset=pixel/4*5-b->Offset;
            b->Red[i]=(UCHAR)(offset+(pixel&3));b->Green[i]=b->Red[i]+1;b->Low[i]=(UCHAR)(offset+4);b->Shift[i]=(CHAR)-(LONG)((pixel&3)*2);
        }
    }
    for(ULONG y=0;y<QCAM_VIDEO_HEIGHT;y++)rows[y]=(top+((y*height/QCAM_VIDEO_HEIGHT)&~1u))*QCAM_STRIDE;
}
