#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <algorithm>
using UCHAR=uint8_t;using CHAR=int8_t;using USHORT=uint16_t;using ULONG=uint32_t;using ULONGLONG=uint64_t;using LONG=int32_t;
#define VOID void
#define QCAM_WIDTH 4096u
#define QCAM_HEIGHT 3072u
#define QCAM_STRIDE 5120u
#define QCAM_VIDEO_WIDTH 1024u
#define QCAM_VIDEO_HEIGHT 768u
#define QCAM_FRAME_BYTES (QCAM_STRIDE*QCAM_HEIGHT)
#define QCAM_VIDEO_BYTES (QCAM_VIDEO_WIDTH*QCAM_VIDEO_HEIGHT*3/2)
#define MAXULONG UINT32_MAX
#define RtlCopyMemory std::memcpy
#define TRUE true
#define FALSE false
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
using BOOLEAN=bool;
#include "../video/raw-row.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../video/nv12.h"
#include "../video/yuy2.h"
#pragma GCC diagnostic pop

extern "C" void CamRgbRowMapped(const UCHAR *,ULONG,const UCHAR [3][1024],const CAM_RAW_BLOCK *,USHORT *,USHORT *,USHORT *);
extern "C" void CamRgbRowMappedWidth(const UCHAR *,ULONG,const UCHAR [3][1024],const CAM_RAW_BLOCK *,USHORT *,USHORT *,USHORT *,ULONG);

int main() {
    constexpr ULONG companion=QCAM_STRIDE*3072*2+256;
    auto dma=static_cast<UCHAR *>(std::malloc(companion+QCAM_STRIDE*4));assert(dma);
    UCHAR colors[3][1024];USHORT pixels[4][4096],output[3][4098];
    USHORT scratch[4096*6+2];UCHAR nv12[4096*6+2],yuy2[4096*8+2];
    for(ULONG c=0;c<3;c++)for(ULONG value=0;value<1024;value++)colors[c][value]=static_cast<UCHAR>((value*(c*5+3)+19)%251);
    const ULONG sizes[][2]={{4096,3072},{3264,2448},{2304,1728},{1600,1200}};
    const ULONG outputs[][2]={{160,120},{320,240},{640,360},{640,480},{800,600},{960,540},{1024,768},{1280,720},{1280,960},
        {1600,900},{1600,1200},{1920,1080},{1920,1440},{2048,1536},{2304,1728},{2560,1440},{2560,1920},{3264,2448},{3840,2160},{4096,2304},{4096,3072}};
    const ULONG ratios[]={65536,65537,72090,87381,98304,131072,196608,262144};
    for(const auto &size:sizes) {
        std::memset(dma,0xa5,companion+QCAM_STRIDE*4);
        for(ULONG y=0;y<4;y++)for(ULONG x=0;x<size[0];x++)pixels[y][x]=static_cast<USHORT>((x*13+y*127+x*x*3)&1023);
        for(ULONG y=0;y<4;y++)for(ULONG x=0;x<size[0];x+=4) {
            UCHAR bytes[5]={0};
            for(ULONG i=0;i<4;i++){bytes[i]=static_cast<UCHAR>(pixels[y][x+i]>>2);bytes[4]|=static_cast<UCHAR>((pixels[y][x+i]&3)<<(i*2));}
            for(ULONG i=0;i<5;i++){ULONG off=x/4*5+i;dma[y*QCAM_STRIDE+off+((off&256)?companion:0)]=bytes[i];}
        }
        for(const auto &target:outputs)for(ULONG ratio:ratios) {
            ULONG ow=target[0],oh=target[1];CAM_RAW_BLOCK blocks[512];ULONG rows[3072];CamRawMapGeometry(ratio,size[0],size[1],ow,oh,blocks,rows);
            for(auto &channel:output){channel[0]=0xfeed;channel[ow+1]=0xbeef;}
            CamRgbRowMappedWidth(dma,companion,colors,blocks,output[0]+1,output[1]+1,output[2]+1,ow);
            ULONG fit=static_cast<ULONGLONG>(size[0])*oh>static_cast<ULONGLONG>(size[1])*ow?static_cast<ULONG>(static_cast<ULONGLONG>(size[1])*ow/oh):size[0];
            ULONG crop=static_cast<ULONG>((static_cast<ULONGLONG>(fit)*65536)/ratio),left=((size[0]-crop)/2)&~1u;
            for(ULONG x=0;x<ow;x++) {
                ULONG actual=left+((x*crop/ow)&~1u);assert(actual+1<size[0]);
                assert(output[0][x+1]==colors[0][pixels[0][actual]]);
                assert(output[1][x+1]==colors[1][(pixels[0][actual+1]+pixels[1][actual])/2]);
                assert(output[2][x+1]==colors[2][pixels[1][actual+1]]);
            }
            for(const auto &channel:output)assert(channel[0]==0xfeed&&channel[ow+1]==0xbeef);
            for(ULONG y=0;y<oh;y++)assert(rows[y]/QCAM_STRIDE+1<size[1]);
            CAM_RAW_BLOCK dense[256];ULONG denseRows[2],columns[4096];
            CamRawMapGeometry(65536,size[0],2,size[0]/2,1,dense,denseRows);CamRawColumns(blocks,ow,columns);
            for(ULONG x=0;x<ow;x++)assert(columns[x]*2==left+((x*crop/ow)&~1u));
            const ULONG patterns[][4]={{0,0,2,2},{0,2,0,2},{0,0,0,0},{2,0,2,2}};
            for(const auto &pattern:patterns)for(bool swap:{false,true}) {
                ULONG selectedRows[4];for(ULONG y=0;y<4;y++)selectedRows[y]=pattern[y]*QCAM_STRIDE;
                scratch[0]=0xfeed;scratch[4096*6+1]=0xbeef;nv12[0]=0xfe;nv12[ow*6+1]=0xef;
                CamNv12RowsCached(dma,companion,nv12+1,colors,scratch+1,0,4,dense,columns,selectedRows,swap,ow,4,size[0]/2);
                for(ULONG y=0;y<4;y+=2)for(ULONG x=0;x<ow;x+=2) {
                    LONG sums[3]={0};
                    for(ULONG dy=0;dy<2;dy++)for(ULONG dx=0;dx<2;dx++) {
                        ULONG p=left+(((x+dx)*crop/ow)&~1u),row=pattern[y+dy];
                        LONG rgb[3]={colors[0][pixels[row][p]],colors[1][(pixels[row][p+1]+pixels[row+1][p])/2],colors[2][pixels[row+1][p+1]]};
                        if(swap)std::swap(rgb[0],rgb[2]);
                        assert(nv12[1+(y+dy)*ow+x+dx]==16+((66*rgb[0]+129*rgb[1]+25*rgb[2]+128)>>8));
                        for(ULONG c=0;c<3;c++)sums[c]+=rgb[c];
                    }
                    LONG r=sums[0]/4,g=sums[1]/4,b=sums[2]/4;ULONG off=1+ow*4+y/2*ow+x;
                    assert(nv12[off]==128+((-38*r-74*g+112*b+128)>>8));
                    assert(nv12[off+1]==128+((112*r-94*g-18*b+128)>>8));
                }
                assert(scratch[0]==0xfeed&&scratch[4096*6+1]==0xbeef&&nv12[0]==0xfe&&nv12[ow*6+1]==0xef);
                yuy2[0]=0xad;yuy2[ow*8+1]=0xbe;CamNv12Yuy2(nv12+1,yuy2+1,ow,4);
                for(ULONG y=0;y<4;y++)for(ULONG x=0;x<ow;x++){assert(yuy2[1+(y*ow+x)*2]==nv12[1+y*ow+x]);assert(yuy2[2+(y*ow+x)*2]==nv12[1+ow*4+(y/2)*ow+x]);}
                assert(yuy2[0]==0xad&&yuy2[ow*8+1]==0xbe);
            }
        }
    }
    CAM_RAW_BLOCK dense[256];ULONG denseRows[2];CamRawMapGeometry(65536,4096,2,2048,1,dense,denseRows);
    for(ULONG y=0;y<2;y++)for(ULONG x=0;x<4096;x+=4) {
        UCHAR bytes[5]={0};
        for(ULONG i=0;i<4;i++){ULONG value=((x+i)/2)&1023;bytes[i]=(UCHAR)(value>>2);bytes[4]|=(UCHAR)((value&3)<<(i*2));}
        for(ULONG i=0;i<5;i++){ULONG off=x/4*5+i;dma[y*QCAM_STRIDE+off+((off&256)?companion:0)]=bytes[i];}
    }
    for(ULONG gain=128;gain<=1024;gain++) {
        ULONG gains[3]={gain,gain,gain};
        for(ULONG c=0;c<3;c++)for(ULONG value=0;value<1024;value++)colors[c][value]=(UCHAR)CamColor(value<=64?value:64+((value-64)*gain+128)/256);
        CamRgbRowMappedGainsWidth(dma,companion,colors,gains,dense,output[0],output[1],output[2],2048);
        for(ULONG x=0;x<2048;x++)for(ULONG c=0;c<3;c++)assert(output[c][x]==colors[c][x&1023]);
        ULONG columns[2048],rows[4]={0,0,0,0};CamRawColumns(dense,2048,columns);
        UCHAR reference[2048*6];
        CamNv12RowsGeometry(dma,companion,reference,colors,scratch,0,4,dense,rows,FALSE,2048,4);
        CamNv12RowsGeometryGains(dma,companion,nv12,colors,gains,scratch,0,4,dense,rows,FALSE,2048,4);
        assert(!std::memcmp(reference,nv12,sizeof(reference)));
        CamNv12RowsCachedGains(dma,companion,nv12,colors,gains,scratch,0,4,dense,columns,rows,FALSE,2048,4,2048);
        assert(!std::memcmp(reference,nv12,sizeof(reference)));
    }
    std::free(dma);std::puts("ARM64 RAW/cached NV12 pixel references passed: four sensors, 21 resolutions, eight zooms, both Bayer orders; exact SIMD gamma and NV12 for all 1024 RAW values and every gain 128 through 1024");
}
