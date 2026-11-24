#pragma once
#include "raw-row.h"
#if defined(_M_ARM64)
#include <arm64_neon.h>
#define CAM_NV12_NEON 1
#elif defined(__aarch64__)
#include <arm_neon.h>
#define CAM_NV12_NEON 1
#endif

static UCHAR CamRawByte(const UCHAR *row,ULONG offset,ULONG companion) {
    return row[offset+((offset&256)?companion:0)];
}

static ULONG CamRawValueDual(const UCHAR *row,ULONG x,ULONG companion) {
    ULONG offset=(x/4)*5,n=x&3;
    return ((ULONG)CamRawByte(row,offset+n,companion)<<2)|((CamRawByte(row,offset+4,companion)>>(2*n))&3);
}


static LONG CamColor(ULONG value) {
    static const UCHAR gamma[]={0,71,99,120,137,152,165,177,188,198,208,216,225,233,241,248,255};
    ULONG linear=value<=64?0:value>=1023?255:(value-64)*255/959,index=linear>>4;
    return gamma[index]+((gamma[index+1]-gamma[index])*(linear&15)+8)/16;
}

static VOID CamWhiteBalance(const UCHAR *raw,ULONG companion,ULONG gains[3]) {
    ULONG sums[3]={0},count=0;
    for(ULONG y=0;y<QCAM_HEIGHT;y+=64)for(ULONG x=0;x<QCAM_WIDTH;x+=64) {
        const UCHAR *row=raw+y*QCAM_STRIDE;
        ULONG r=CamRawValueDual(row,x,companion),g=(CamRawValueDual(row,x+1,companion)+CamRawValueDual(row+QCAM_STRIDE,x,companion))/2,b=CamRawValueDual(row+QCAM_STRIDE,x+1,companion);
        if(r<=64||g<=64||b<=64||r>=960||g>=960||b>=960)continue;
        sums[0]+=r-64;sums[1]+=g-64;sums[2]+=b-64;count++;
    }
    if(count<16||sums[0]<count*4||sums[1]<count*4||sums[2]<count*4)return;
    for(ULONG i=0;i<3;i+=2){ULONG gain=(sums[1]*256+sums[i]/2)/sums[i];gains[i]=gain<128?128:gain>1024?1024:gain;}
}

static ULONGLONG CamExposureTarget(const UCHAR *raw,ULONG companion,const ULONG gains[3],ULONG zoom,ULONGLONG current) {
    ULONG width=(ULONG)(((ULONGLONG)QCAM_WIDTH<<16)/zoom),height=(ULONG)(((ULONGLONG)QCAM_HEIGHT<<16)/zoom);
    ULONG left=(QCAM_WIDTH-width)/2,top=(QCAM_HEIGHT-height)/2,histogram[128]={0},sum=0,count=0;
    for(ULONG y=0;y<QCAM_HEIGHT;y+=64)if(y>=top&&y<top+height)
        for(ULONG x=0;x<QCAM_WIDTH;x+=64)if(x>=left&&x<left+width) {
            const UCHAR *row=raw+y*QCAM_STRIDE;
            ULONG values[3]={CamRawValueDual(row,x,companion),
                (CamRawValueDual(row,x+1,companion)+CamRawValueDual(row+QCAM_STRIDE,x,companion))/2,
                CamRawValueDual(row+QCAM_STRIDE,x+1,companion)};
            ULONG peak=0;
            for(ULONG i=0;i<3;i++) {
                ULONG signal=values[i]>64?values[i]-64:0;
                peak=max(peak,(signal*gains[i]+128)/256);
            }
            sum+=values[1]>64?values[1]-64:0;histogram[min(peak>>5,127u)]++;count++;
        }
    if(!count)return current;
    ULONG light=max(sum/count,1u),rank=(count*9+9)/10,seen=0,highlight=0;
    for(ULONG i=0;i<128;i++)if((seen+=histogram[i])>=rank){highlight=i*32+16;break;}
    ULONGLONG target=current*256/light;
    if(highlight)target=min(target,current*704/highlight);
    target=min(max(target,max(current/2,32ULL)),max(current*2,32ULL));
    return target>=current*97/100&&target<=current*103/100?current:target;
}

static inline VOID CamRgb(const UCHAR *row,ULONG x,ULONG companion,const UCHAR colors[3][1024],LONG *red,LONG *green,LONG *blue) {
    ULONG offset=x*5;const UCHAR *q=row+QCAM_STRIDE;
    if((offset&255)<=251) {
        const UCHAR *p=row+offset+((offset&256)?companion:0),*other=p+QCAM_STRIDE;USHORT top,bottom;
        RtlCopyMemory(&top,p,sizeof(top));RtlCopyMemory(&bottom,other,sizeof(bottom));UCHAR low=p[4],lowQ=other[4];
        *red=colors[0][((top&255)<<2)|(low&3)];
        *green=colors[1][(((top>>8)<<2|((low>>2)&3))+((bottom&255)<<2|(lowQ&3)))/2];
        *blue=colors[2][((bottom>>8)<<2)|((lowQ>>2)&3)];
        return;
    }
    UCHAR low=CamRawByte(row,offset+4,companion),lowQ=CamRawByte(q,offset+4,companion);
    *red=colors[0][((ULONG)CamRawByte(row,offset,companion)<<2)|(low&3)];
    *green=colors[1][((((ULONG)CamRawByte(row,offset+1,companion)<<2)|((low>>2)&3))+(((ULONG)CamRawByte(q,offset,companion)<<2)|(lowQ&3)))/2];
    *blue=colors[2][((ULONG)CamRawByte(q,offset+1,companion)<<2)|((lowQ>>2)&3)];
}

#if defined(CAM_NV12_NEON)
#if defined(__cplusplus)
extern "C"
#endif
VOID CamRgbRow(const UCHAR *row,ULONG companion,const UCHAR colors[3][1024],USHORT *red,USHORT *green,USHORT *blue);
#if defined(__cplusplus)
extern "C"
#endif
VOID CamRgbRowMapped(const UCHAR *row,ULONG companion,const UCHAR colors[3][1024],const CAM_RAW_BLOCK *blocks,USHORT *red,USHORT *green,USHORT *blue);
#endif

static VOID CamNv12Colors(const UCHAR *raw,ULONG companion,UCHAR colors[3][1024]) {
    ULONG gains[3]={256,256,256};CamWhiteBalance(raw,companion,gains);
    for(ULONG i=0;i<3;i++)for(ULONG value=0;value<1024;value++)colors[i][value]=(UCHAR)CamColor(value<=64?value:64+((value-64)*gains[i]+128)/256);
}

static VOID CamNv12RowsSampled(const UCHAR *raw,ULONG companion,UCHAR *output,const UCHAR colors[3][1024],USHORT *workspace,ULONG first,ULONG end,const CAM_RAW_BLOCK *blocks,const ULONG *rows) {
    UCHAR *uv=output+QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT;
#if defined(CAM_NV12_NEON)
    for(ULONG y=first;y<end;y+=2) {
        if(blocks) {
            CamRgbRowMapped(raw+rows[y],companion,colors,blocks,workspace,workspace+1024,workspace+2048);
            CamRgbRowMapped(raw+rows[y+1],companion,colors,blocks,workspace+3072,workspace+4096,workspace+5120);
        } else {
            CamRgbRow(raw+y*4*QCAM_STRIDE,companion,colors,workspace,workspace+1024,workspace+2048);
            CamRgbRow(raw+(y+1)*4*QCAM_STRIDE,companion,colors,workspace+3072,workspace+4096,workspace+5120);
        }
        for(ULONG x=0;x<QCAM_VIDEO_WIDTH;x+=8) {
        uint16x8_t r0=vld1q_u16(workspace+x),g0=vld1q_u16(workspace+1024+x),b0=vld1q_u16(workspace+2048+x);
        uint16x8_t r1=vld1q_u16(workspace+3072+x),g1=vld1q_u16(workspace+4096+x),b1=vld1q_u16(workspace+5120+x);
        uint16x8_t y0=vmlaq_n_u16(vmlaq_n_u16(vmulq_n_u16(r0,66),g0,129),b0,25);
        uint16x8_t y1=vmlaq_n_u16(vmlaq_n_u16(vmulq_n_u16(r1,66),g1,129),b1,25);
        uint8x8_t luma0=vadd_u8(vshrn_n_u16(vaddq_u16(y0,vdupq_n_u16(128)),8),vdup_n_u8(16));
        uint8x8_t luma1=vadd_u8(vshrn_n_u16(vaddq_u16(y1,vdupq_n_u16(128)),8),vdup_n_u8(16));
        vst1_u8(output+y*QCAM_VIDEO_WIDTH+x,luma0);vst1_u8(output+(y+1)*QCAM_VIDEO_WIDTH+x,luma1);
        int32x4_t r=vreinterpretq_s32_u32(vshrq_n_u32(vpaddlq_u16(vaddq_u16(r0,r1)),2));
        int32x4_t g=vreinterpretq_s32_u32(vshrq_n_u32(vpaddlq_u16(vaddq_u16(g0,g1)),2));
        int32x4_t b=vreinterpretq_s32_u32(vshrq_n_u32(vpaddlq_u16(vaddq_u16(b0,b1)),2));
        int32x4_t u=vmlaq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vdupq_n_s32(32896),r,-38),g,-74),b,112);
        int32x4_t v=vmlaq_n_s32(vmlaq_n_s32(vmlaq_n_s32(vdupq_n_s32(32896),r,112),g,-94),b,-18);
        uint16x4_t u16=vmovn_u32(vreinterpretq_u32_s32(vshrq_n_s32(u,8))),v16=vmovn_u32(vreinterpretq_u32_s32(vshrq_n_s32(v,8)));
        uint16x4_t chroma=vorr_u16(u16,vshl_n_u16(v16,8));
        vst1_u16((USHORT *)(uv+(y/2)*QCAM_VIDEO_WIDTH+x),chroma);
        }
    }
#else
    (void)workspace;
    for(ULONG y=first;y<end;y+=2)for(ULONG x=0;x<QCAM_VIDEO_WIDTH;x+=2) {
        LONG sumR=0,sumG=0,sumB=0;
        for(ULONG dy=0;dy<2;dy++)for(ULONG dx=0;dx<2;dx++) {
            LONG r,g,b;
            if(blocks) {
                ULONG col=x+dx,i=col&7;const CAM_RAW_BLOCK *sample=blocks+col/8;const UCHAR *row=raw+rows[y+dy];
                ULONG off=sample->Offset,shift=(ULONG)-(LONG)sample->Shift[i];
                ULONG low=CamRawByte(row,off+sample->Low[i],companion),lowQ=CamRawByte(row+QCAM_STRIDE,off+sample->Low[i],companion);
                r=colors[0][((ULONG)CamRawByte(row,off+sample->Red[i],companion)<<2)|((low>>shift)&3)];
                ULONG a=((ULONG)CamRawByte(row,off+sample->Green[i],companion)<<2)|((low>>(shift+2))&3),b0=((ULONG)CamRawByte(row+QCAM_STRIDE,off+sample->Red[i],companion)<<2)|((lowQ>>shift)&3);
                g=colors[1][(a+b0)/2];b=colors[2][((ULONG)CamRawByte(row+QCAM_STRIDE,off+sample->Green[i],companion)<<2)|((lowQ>>(shift+2))&3)];
            } else CamRgb(raw+(y+dy)*4*QCAM_STRIDE,x+dx,companion,colors,&r,&g,&b);
            output[(y+dy)*QCAM_VIDEO_WIDTH+x+dx]=(UCHAR)(16+((66*r+129*g+25*b+128)>>8));
            sumR+=r;sumG+=g;sumB+=b;
        }
        LONG r=sumR/4,g=sumG/4,b=sumB/4;
        ULONG off=(y/2)*QCAM_VIDEO_WIDTH+x;
        uv[off]=(UCHAR)(128+((-38*r-74*g+112*b+128)>>8));
        uv[off+1]=(UCHAR)(128+((112*r-94*g-18*b+128)>>8));
    }
#endif
}

static VOID CamNv12Rows(const UCHAR *raw,ULONG companion,UCHAR *output,const UCHAR colors[3][1024],USHORT *workspace,ULONG first,ULONG end) {
    CamNv12RowsSampled(raw,companion,output,colors,workspace,first,end,NULL,NULL);
}

static BOOLEAN CamNv12Work(const UCHAR *raw,ULONG rawBytes,ULONG companion,UCHAR *output,ULONG outputBytes,USHORT *workspace) {
    if(!raw||!output||rawBytes<QCAM_FRAME_BYTES||outputBytes<QCAM_VIDEO_BYTES)return FALSE;
    UCHAR colors[3][1024];CamNv12Colors(raw,companion,colors);
    CamNv12Rows(raw,companion,output,colors,workspace,0,QCAM_VIDEO_HEIGHT);
    return TRUE;
}

static BOOLEAN CamNv12Dual(const UCHAR *raw,ULONG rawBytes,ULONG companion,UCHAR *output,ULONG outputBytes) {
#if defined(CAM_NV12_NEON)
    USHORT workspace[6144];return CamNv12Work(raw,rawBytes,companion,output,outputBytes,workspace);
#else
    return CamNv12Work(raw,rawBytes,companion,output,outputBytes,NULL);
#endif
}

static BOOLEAN CamNv12(const UCHAR *raw,ULONG rawBytes,UCHAR *output,ULONG outputBytes) {
    return CamNv12Dual(raw,rawBytes,0,output,outputBytes);
}
