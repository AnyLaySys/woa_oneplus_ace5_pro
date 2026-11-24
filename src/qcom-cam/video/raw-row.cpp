#if defined(_WIN32)
#include <ntddk.h>
#include <arm64_neon.h>
#include "../uapi.h"
#else
#include <stdint.h>
#include <string.h>
typedef uint8_t UCHAR;typedef uint16_t USHORT;typedef uint32_t ULONG;
typedef int8_t CHAR;typedef uint64_t ULONGLONG;
typedef int32_t LONG;
#define VOID void
#define QCAM_STRIDE 5120u
#define QCAM_VIDEO_WIDTH 1024u
#define QCAM_VIDEO_HEIGHT 768u
#define QCAM_WIDTH 4096u
#define QCAM_HEIGHT 3072u
#include <arm_neon.h>
#endif
#include "raw-row.h"

static inline VOID ReadMapped(const UCHAR *row,ULONG offset,ULONG companion,UCHAR *data,ULONG bytes) {
    ULONG count=256-(offset&255);if(count>bytes)count=bytes;
    memcpy(data,row+offset+((offset&256)?companion:0),count);
    if(count<bytes){offset+=count;memcpy(data+count,row+offset+((offset&256)?companion:0),bytes-count);}
}

static inline uint8x16_t Mapped16(const UCHAR *row,ULONG offset,ULONG companion) {
    if((offset&255)<=240)return vld1q_u8(row+offset+((offset&256)?companion:0));
    UCHAR data[16];ReadMapped(row,offset,companion,data,16);return vld1q_u8(data);
}

static inline uint8x8_t Mapped8(const UCHAR *row,ULONG offset,ULONG companion) {
    if((offset&255)<=248)return vld1_u8(row+offset+((offset&256)?companion:0));
    UCHAR data[8];ReadMapped(row,offset,companion,data,8);return vld1_u8(data);
}

static inline uint8x8_t Lookup(uint8x16x3_t values,uint8x8_t index) {
#if defined(_WIN32)
    return neon_tbl3_q8(values,index);
#else
    return vqtbl3_u8(values,index);
#endif
}

static inline uint8x8_t Load8(const UCHAR *row,ULONG offset,ULONG companion) {
    return vld1_u8(row+offset+((offset&256)?companion:0));
}

static inline uint8x16_t Load16(const UCHAR *row,ULONG offset,ULONG companion) {
    if((offset&255)==248)return vcombine_u8(Load8(row,offset,companion),Load8(row,offset+8,companion));
    return vld1q_u8(row+offset+((offset&256)?companion:0));
}

extern "C" VOID CamRgbRow(const UCHAR *row,ULONG companion,const UCHAR colors[3][1024],USHORT *red,USHORT *green,USHORT *blue) {
    const UCHAR ridx[]={0,5,10,15,20,25,30,35},gidx[]={1,6,11,16,21,26,31,36},lidx[]={4,9,14,19,24,29,34,39};
    uint8x8_t ri=vld1_u8(ridx),gi=vld1_u8(gidx),li=vld1_u8(lidx),mask=vdup_n_u8(3);
    for(ULONG x=0;x<QCAM_VIDEO_WIDTH;x+=8) {
        ULONG offset=x*5;const UCHAR *top=row+offset,*bottom=top+QCAM_STRIDE;
        uint8x16x3_t a,b;
        if((offset&255)<=216) {
            ULONG extra=(offset&256)?companion:0;top+=extra;bottom+=extra;
            a={{vld1q_u8(top),vld1q_u8(top+16),vcombine_u8(vld1_u8(top+32),vdup_n_u8(0))}};
            b={{vld1q_u8(bottom),vld1q_u8(bottom+16),vcombine_u8(vld1_u8(bottom+32),vdup_n_u8(0))}};
        } else {
            a={{Load16(row,offset,companion),Load16(row,offset+16,companion),vcombine_u8(Load8(row,offset+32,companion),vdup_n_u8(0))}};
            b={{Load16(row+QCAM_STRIDE,offset,companion),Load16(row+QCAM_STRIDE,offset+16,companion),vcombine_u8(Load8(row+QCAM_STRIDE,offset+32,companion),vdup_n_u8(0))}};
        }
        uint8x8_t la=Lookup(a,li),lb=Lookup(b,li);
        uint16x8_t r=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(a,ri)),2),vmovl_u8(vand_u8(la,mask)));
        uint16x8_t gt=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(a,gi)),2),vmovl_u8(vand_u8(vshr_n_u8(la,2),mask)));
        uint16x8_t gb=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(b,ri)),2),vmovl_u8(vand_u8(lb,mask)));
        uint16x8_t bl=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(b,gi)),2),vmovl_u8(vand_u8(vshr_n_u8(lb,2),mask)));
        USHORT rv[8],gv[8],bv[8];vst1q_u16(rv,r);vst1q_u16(gv,vshrq_n_u16(vaddq_u16(gt,gb),1));vst1q_u16(bv,bl);
        for(ULONG i=0;i<8;i++){red[x+i]=colors[0][rv[i]];green[x+i]=colors[1][gv[i]];blue[x+i]=colors[2][bv[i]];}
    }
}

extern "C" VOID CamRgbRowMapped(const UCHAR *row,ULONG companion,const UCHAR colors[3][1024],const CAM_RAW_BLOCK *blocks,USHORT *red,USHORT *green,USHORT *blue) {
    uint8x8_t mask=vdup_n_u8(3);
    for(ULONG x=0;x<QCAM_VIDEO_WIDTH;x+=8) {
        const CAM_RAW_BLOCK *block=blocks+x/8;ULONG off=block->Offset;const UCHAR *bottom=row+QCAM_STRIDE;
        uint8x16x3_t a={{Mapped16(row,off,companion),Mapped16(row,off+16,companion),vcombine_u8(Mapped8(row,off+32,companion),vdup_n_u8(0))}};
        uint8x16x3_t b={{Mapped16(bottom,off,companion),Mapped16(bottom,off+16,companion),vcombine_u8(Mapped8(bottom,off+32,companion),vdup_n_u8(0))}};
        uint8x8_t ri=vld1_u8(block->Red),gi=vld1_u8(block->Green),li=vld1_u8(block->Low),la=Lookup(a,li),lb=Lookup(b,li);
        int8x8_t shift=vld1_s8(block->Shift),greenShift=vsub_s8(shift,vdup_n_s8(2));
        uint16x8_t r=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(a,ri)),2),vmovl_u8(vand_u8(vshl_u8(la,shift),mask)));
        uint16x8_t gt=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(a,gi)),2),vmovl_u8(vand_u8(vshl_u8(la,greenShift),mask)));
        uint16x8_t gb=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(b,ri)),2),vmovl_u8(vand_u8(vshl_u8(lb,shift),mask)));
        uint16x8_t bl=vorrq_u16(vshlq_n_u16(vmovl_u8(Lookup(b,gi)),2),vmovl_u8(vand_u8(vshl_u8(lb,greenShift),mask)));
        USHORT rv[8],gv[8],bv[8];vst1q_u16(rv,r);vst1q_u16(gv,vshrq_n_u16(vaddq_u16(gt,gb),1));vst1q_u16(bv,bl);
        for(ULONG i=0;i<8;i++){red[x+i]=colors[0][rv[i]];green[x+i]=colors[1][gv[i]];blue[x+i]=colors[2][bv[i]];}
    }
}
