#pragma once

#include "dma-address.h"

typedef struct {
    ULONG Stride,Bytes,ImageAddress[2],AddressOffset[2],ImageSize[2];
} CAM_ISP_NV12_OUTPUT;

typedef struct {
    ULONG Changed[2],FirstRow[2],LastRow[2],Bands[2][8],BlockParity[2][2],Minimum[2],Maximum[2];
    ULONG MarkerSpans[2],MissingRows[2],WrittenMinimum[2],WrittenMaximum[2];
} CAM_ISP_NV12_COVERAGE;

/* Stopped, cache-synchronized diagnostic memory only. No pixel conversion,
   and byte coverage never establishes frame completion or valid image data. */
static inline BOOLEAN CamIspNv12Coverage(CAM_ISP_NV12_COVERAGE *coverage,
    const unsigned char *image,ULONG capacity,ULONG stride,ULONG height) {
    if(!coverage||!image||!stride||!height||(height&1)||stride>4096||height>3072||
       stride*height+stride*height/2>capacity)return FALSE;
    CAM_ISP_NV12_COVERAGE result={0};
    result.Minimum[0]=result.Minimum[1]=255;
    result.WrittenMinimum[0]=result.WrittenMinimum[1]=255;
    result.FirstRow[0]=result.FirstRow[1]=result.LastRow[0]=result.LastRow[1]=MAXULONG;
    for(ULONG plane=0;plane<2;plane++) {
        ULONG rows=height>>plane,start=plane?stride*height:0;
        ULONG markerRun=0;
        for(ULONG row=0;row<rows;row++) {
            ULONG changed=0;
            for(ULONG column=0;column<stride;column++) {
                ULONG value=image[start+row*stride+column];
                if(value<result.Minimum[plane])result.Minimum[plane]=value;
                if(value>result.Maximum[plane])result.Maximum[plane]=value;
                if(value!=0xa5) {
                    changed++;result.BlockParity[plane][((start+row*stride+column)>>8)&1]++;
                    markerRun=0;
                    if(value<result.WrittenMinimum[plane])result.WrittenMinimum[plane]=value;
                    if(value>result.WrittenMaximum[plane])result.WrittenMaximum[plane]=value;
                } else if(++markerRun==64) {
                    result.MarkerSpans[plane]++;markerRun=0;
                }
            }
            if(!changed){result.MissingRows[plane]++;continue;}
            if(result.FirstRow[plane]==MAXULONG)result.FirstRow[plane]=row;
            result.LastRow[plane]=row;result.Changed[plane]+=changed;
            result.Bands[plane][row*8/rows]+=changed;
        }
    }
    *coverage=result;return TRUE;
}

/* Buffer-relative block parity can contain writes in both buckets while
   absolute bit8 gaps persist (observed with a 128-byte address offset).
   Reject retained marker spans independently of the buffer's alignment.
   A uniform marker-valued region is ambiguous, so this diagnostic fails
   conservatively; passing coverage still does not prove a real scene. */
static inline BOOLEAN CamIspNv12CoverageValid(const CAM_ISP_NV12_COVERAGE *coverage) {
    if(!coverage)return FALSE;
    for(ULONG plane=0;plane<2;plane++)
        if(!coverage->Changed[plane]||coverage->MissingRows[plane]||coverage->MarkerSpans[plane])return FALSE;
    return TRUE;
}

static BOOLEAN CamIspNv12Output(CAM_ISP_NV12_OUTPUT *layout,ULONG width,ULONG height,
    ULONGLONG address,ULONG capacity) {
    if(!layout||width<160||height<120||(width&7)||(height&1)||width>4096||height>3072||
       !address||address>0xfffffffffULL)return FALSE;
    ULONG stride=(width+15)&~15u,luma=stride*height,bytes=luma+luma/2;
    if(capacity<bytes||address>0xfffffffffULL-bytes+1)return FALSE;
    layout->Stride=stride;layout->Bytes=bytes;
    layout->ImageAddress[0]=CamDmaImageAddress(address);layout->ImageAddress[1]=CamDmaImageAddress(address+luma);
    layout->AddressOffset[0]=(ULONG)address&255;layout->AddressOffset[1]=(ULONG)(address+luma)&255;
    layout->ImageSize[0]=(height<<16)|width;layout->ImageSize[1]=((height/2)<<16)|width;
    return TRUE;
}

/* Only labelled arithmetic/constant/range probes may intentionally produce
   flat luma. Real-input image diagnostics cannot pass on a constant plane.
   Variance alone still does not establish a recognizable scene. */
static inline BOOLEAN CamIspFdSignalValid(ULONG profile,ULONG minimum,ULONG maximum) {
    if(profile>33||minimum>maximum||maximum>255)return FALSE;
    if(profile==4||profile==5||profile==6||profile==12||
       (profile>=17&&profile<=21)||profile==24||profile==25||profile==26||profile==28||profile==29)return TRUE;
    return minimum<maximum;
}
