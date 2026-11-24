#pragma once

typedef struct { ULONG Offset,Span; UCHAR Red[8],Green[8],Low[8]; CHAR Shift[8]; } CAM_RAW_BLOCK;

static inline VOID CamRawColumns(const CAM_RAW_BLOCK *blocks,ULONG width,ULONG *columns) {
    for(ULONG x=0;x<width;x++) {
        const CAM_RAW_BLOCK *b=blocks+x/8;ULONG at=b->Offset+b->Red[x&7];
        columns[x]=(at/5*4+at%5)/2;
    }
}

static inline VOID CamRawMapGeometry(ULONG zoom,ULONG sourceWidth,ULONG sourceHeight,ULONG outputWidth,ULONG outputHeight,CAM_RAW_BLOCK *blocks,ULONG *rows) {
    ULONG cropWidth=sourceWidth,cropHeight=sourceHeight;
    if((ULONGLONG)cropWidth*outputHeight>(ULONGLONG)cropHeight*outputWidth)cropWidth=(ULONG)((ULONGLONG)cropHeight*outputWidth/outputHeight);
    else cropHeight=(ULONG)((ULONGLONG)cropWidth*outputHeight/outputWidth);
    ULONG width=(ULONG)(((ULONGLONG)cropWidth<<16)/zoom),height=(ULONG)(((ULONGLONG)cropHeight<<16)/zoom);
    ULONG left=((sourceWidth-width)/2)&~1u,top=((sourceHeight-height)/2)&~1u;
    for(ULONG x=0;x<outputWidth;x+=8) {
        CAM_RAW_BLOCK *b=blocks+x/8;b->Span=0;
        ULONG first=left+((x*width/outputWidth)&~1u);b->Offset=first/4*5;
        for(ULONG i=0;i<8;i++) {
            ULONG pixel=left+(((x+i)*width/outputWidth)&~1u);if(pixel+1>=sourceWidth)pixel=sourceWidth-2;
            ULONG offset=pixel/4*5-b->Offset;
            b->Red[i]=(UCHAR)(offset+(pixel&3));b->Green[i]=b->Red[i]+1;b->Low[i]=(UCHAR)(offset+4);b->Shift[i]=(CHAR)-(LONG)((pixel&3)*2);
            if(b->Span<offset+5)b->Span=offset+5;
        }
    }
    for(ULONG y=0;y<outputHeight;y++)rows[y]=(top+((y*height/outputHeight)&~1u))*QCAM_STRIDE;
}

static inline VOID CamRawZoomGeometry(ULONG zoom,ULONG sourceWidth,ULONG sourceHeight,CAM_RAW_BLOCK blocks[128],ULONG rows[768]) {
    CamRawMapGeometry(zoom,sourceWidth,sourceHeight,QCAM_VIDEO_WIDTH,QCAM_VIDEO_HEIGHT,blocks,rows);
}

static inline VOID CamRawZoom(ULONG zoom,CAM_RAW_BLOCK blocks[128],ULONG rows[768]) {CamRawZoomGeometry(zoom,QCAM_WIDTH,QCAM_HEIGHT,blocks,rows);}
