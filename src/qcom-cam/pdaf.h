#pragma once

typedef struct {
    USHORT Left[221],Right[221];
    LONG Dcc[48];
    ULONG QFormat;
    BOOLEAN Valid;
} CAM_PDAF_CAL;

typedef struct { LONG Phase,Defocus; ULONG Confidence,Window; } CAM_PDAF_RESULT;

static inline ULONG CamPdafWord(const UCHAR *p) {return p[0]|((ULONG)p[1]<<8);}

static inline BOOLEAN CamPdafCalibration(CAM_PDAF_CAL *c,const UCHAR *data,ULONG bytes) {
    c->Valid=FALSE;
    if(!data||bytes<1024||CamPdafWord(data)!=10||CamPdafWord(data+4)!=17||CamPdafWord(data+6)!=13||
       CamPdafWord(data+8)>16||CamPdafWord(data+10)!=8||CamPdafWord(data+12)!=6)return FALSE;
    c->QFormat=CamPdafWord(data+8);
    for(ULONG i=0;i<221;i++) {
        c->Left[i]=(USHORT)CamPdafWord(data+32+i*2);c->Right[i]=(USHORT)CamPdafWord(data+474+i*2);
        if(c->Left[i]<128||c->Left[i]>4096||c->Right[i]<128||c->Right[i]>4096)return FALSE;
    }
    for(ULONG i=0;i<48;i++) {
        ULONG value=CamPdafWord(data+928+i*2);c->Dcc[i]=value>=32768?(LONG)value-65536:(LONG)value;
        if(c->Dcc[i]<=0||c->Dcc[i]>16384)return FALSE;
    }
    c->Valid=TRUE;return TRUE;
}

static inline ULONG CamPdafGain(const USHORT *map,ULONG x,ULONG y) {
    ULONG gx=x*16*256/2047,gy=y*12*256/767,ix=min(gx>>8,15u),iy=min(gy>>8,11u);
    ULONG fx=gx-ix*256,fy=gy-iy*256;
    ULONG a=map[iy*17+ix]*(256-fx)+map[iy*17+ix+1]*fx;
    ULONG b=map[(iy+1)*17+ix]*(256-fx)+map[(iy+1)*17+ix+1]*fx;
    return (a*(256-fy)+b*fy+32768)>>16;
}

static inline ULONG CamPdafPixel(const UCHAR *row,ULONG x) {
    ULONG offset=x/4*5,n=x&3;return ((ULONG)row[offset+n]<<2)|((row[offset+4]>>(n*2))&3);
}

static inline ULONGLONG CamPdafRoot(ULONGLONG n) {
    ULONGLONG result=0,bit=1ULL<<62;
    while(bit>n)bit>>=2;
    while(bit) {
        if(n>=result+bit){n-=result+bit;result=(result>>1)+bit;}
        else result>>=1;
        bit>>=2;
    }
    return result;
}

static inline VOID CamPdafRoiOrigin(const QCAM_ROI_OPTIONS *roi,ULONG zoom,ULONG *x,ULONG *y) {
    ULONG width=(ULONG)(2048ULL*65536/zoom),height=(ULONG)(768ULL*65536/zoom);
    LONG centerX=(LONG)((2048-width)/2+(ULONGLONG)(roi->Left+roi->Right)*width/131072);
    LONG centerY=(LONG)((768-height)/2+(ULONGLONG)(roi->Top+roi->Bottom)*height/131072);
    *x=(ULONG)max(0,min(centerX-128,1792));*y=(ULONG)max(0,min(centerY-80,615));
}

static inline CAM_PDAF_RESULT CamPdafRegion(const UCHAR *raw,const CAM_PDAF_CAL *cal,ULONG x,ULONG y,ULONG window,LONG *workspace) {
    CAM_PDAF_RESULT result={0};LONG (*left)[63]=(LONG (*)[63])workspace,(*right)[63]=(LONG (*)[63])(workspace+1260);
    ULONG saturated=0,signal=0;
    for(ULONG row=0;row<20;row++) {
        LONG previousL=0,previousR=0;
        const UCHAR *p=raw+(y+row*8)*QCAM_STRIDE;
        for(ULONG col=0;col<64;col++) {
            LONG l=0,r=0;
            for(ULONG bin=0;bin<4;bin++) {
                ULONG pair=x+col*4+bin,lv=CamPdafPixel(p,pair*2),rv=CamPdafPixel(p,pair*2+1);
                saturated+=lv>=1000||rv>=1000;
                ULONG a=lv>64?lv-64:0,b=rv>64?rv-64:0;signal+=a+b;
                l+=(LONG)(a*CamPdafGain(cal->Left,pair,y+row*8)/256);
                r+=(LONG)(b*CamPdafGain(cal->Right,pair,y+row*8)/256);
            }
            if(col){left[row][col-1]=l-previousL;right[row][col-1]=r-previousR;}
            previousL=l;previousR=r;
        }
    }
    if(signal<20*64*4*8||saturated>20*64*2)return result;
    LONG correlations[17],best=-1001;ULONG index=0;ULONGLONG energyL=0;
    for(ULONG row=0;row<20;row++)for(ULONG col=8;col<55;col++)energyL+=(ULONGLONG)((long long)left[row][col]*left[row][col]);
    if(energyL<20000)return result;
    for(LONG shift=-8;shift<=8;shift++) {
        long long cross=0;ULONGLONG energyR=0;
        for(ULONG row=0;row<20;row++)for(ULONG col=8;col<55;col++) {
            LONG l=left[row][col],r=right[row][(LONG)col+shift];cross+=(long long)l*r;energyR+=(ULONGLONG)((long long)r*r);
        }
        ULONGLONG scale=CamPdafRoot(energyL)*CamPdafRoot(energyR);
        LONG correlation=scale?(LONG)(cross*1000/(long long)scale):-1000;correlation=min(correlation,1000);
        correlations[shift+8]=correlation;if(correlation>best){best=correlation;index=(ULONG)(shift+8);}
    }
    LONG second=-1000;
    for(ULONG i=0;i<17;i++)if(i+1<index||i>index+1)second=max(second,correlations[i]);
    if(!index||index==16||best<550||best-second<50)return result;
    LONG a=correlations[index-1],b=correlations[index],c=correlations[index+1],denom=2*(a-2*b+c);
    LONG fraction=denom?(a-c)*256/denom:0;fraction=max(-128,min(fraction,128));
    result.Phase=(((LONG)index-8)*256+fraction)*4;
    ULONG dcc=min((y+80)*6/768,5u)*8+min((x+128)*8/2048,7u);
    result.Defocus=(LONG)(-((long long)result.Phase*cal->Dcc[dcc])/(256LL*(1LL<<cal->QFormat)*2));
    result.Confidence=(ULONG)best;result.Window=window;return result;
}

static inline CAM_PDAF_RESULT CamPdafWindow(const UCHAR *raw,const CAM_PDAF_CAL *cal,ULONG window,LONG *workspace) {
    return CamPdafRegion(raw,cal,384+(window%3)*512,112+(window/3)*192,window,workspace);
}
static inline CAM_PDAF_RESULT CamPdafEstimate(const UCHAR *raw,ULONG bytes,const CAM_PDAF_CAL *cal,LONG *workspace) {
    CAM_PDAF_RESULT result={0};
    if(!raw||bytes<QCAM_PDAF_BYTES||!cal->Valid)return result;
    result=CamPdafWindow(raw,cal,4,workspace);
    if(result.Confidence)return result;
    const ULONG windows[]={1,3,5,7,0,2,6,8};
    for(ULONG i=0;i<8;i++) {
        CAM_PDAF_RESULT candidate=CamPdafWindow(raw,cal,windows[i],workspace);
        ULONG priority=i<4?64:128;
        if(candidate.Confidence>result.Confidence+priority)result=candidate;
    }
    return result;
}

static inline VOID CamPdafGatherRows(const UCHAR *source,UCHAR *destination,ULONG first) {
    for(ULONG i=0;i<20;i++) {
        ULONG row=(first+i*8)*QCAM_STRIDE;
        for(ULONG x=0;x<QCAM_STRIDE;x+=512) {
            RtlCopyMemory(destination+row+x,source+row+x,256);
            RtlCopyMemory(destination+row+x+256,source+row+QCAM_PDAF_BYTES*2+512+x,256);
        }
    }
}

static inline VOID CamPdafGather(const UCHAR *source,UCHAR *destination,ULONG group) {
    CamPdafGatherRows(source,destination,112+group*192);
}
