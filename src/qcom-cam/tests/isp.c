#include <stdint.h>
#include <string.h>

typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t LONG;
typedef int BOOLEAN;
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(p,s,n) memcpy(p,s,n)

#include "../isp-mnds.h"
#include "../isp-color.h"
#include "../isp-demosaic.h"
#include "../isp-route.h"
#include "../isp-output.h"
#include "../isp-bus.h"
#include "../isp-input.h"
#include "../isp-gamma.h"
#include "../isp-bayer.h"
#include "../isp-crop.h"
#include "../isp-pixel.h"
#include "../isp-dmi.h"
#include "../isp-precision.h"
#include "../isp-gtm.h"

int compdecomp(ULONG *words,const CAM_ISP_COMPDECOMP *input) { return CamIspCompDecompBuild(words,input); }
int compdecomp_registers(ULONG *offsets,ULONG resource,ULONG *first) { return CamIspCompDecompRegisters(offsets,resource,first); }
int compdecomp_disabled(ULONG *registers,ULONG resource,ULONG context,ULONG mode,ULONG control) { return CamIspCompDecompDisabled(registers,resource,context,mode,control); }
int gtm(CAM_ISP_GTM *output,const CAM_ISP_GTM_INPUT *input) { return CamIspGtmBuild(output,input); }

int gamma_dmi(ULONG *buffer,ULONG capacity,ULONGLONG address,ULONG base,ULONG marker,const CAM_ISP_GAMMA *gamma) {
    return CamIspGammaDmiBuild(buffer,capacity,address,base,marker,gamma);
}

int gamma_registers(ULONG *buffer,ULONG capacity,ULONGLONG address,ULONG base,ULONG marker,const CAM_ISP_GAMMA *gamma) {
    return CamIspGammaCommandBuild(buffer,capacity,address,base,marker,gamma,FALSE);
}

int pixel_plan(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) { return CamIspPixelReference(plan,width,height); }
int fd_pixel_plan(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) { return CamIspPixelReferenceProfile(plan,width,height,TRUE); }
int fd_exact_plan(CAM_ISP_PIXEL_PLAN *plan) { return CamIspPixelReferenceOutput(plan,4096,3072,1024,768,FALSE); }
int pixel_packed(CAM_ISP_PIXEL_PLAN *plan,const CAM_ISP_PIXEL_PACKED *packed) { return CamIspPixelBuild(plan,packed); }
int common_plan(ULONG *words,const ULONG *throttles,const ULONG *flags,const ULONG *pdaf,int hdr) {
    return CamIspCommonBuild(words,throttles,flags,pdaf,hdr);
}

int fd_crop(ULONG *words,ULONG width,ULONG height,ULONG bits) { return CamIspFdCropBuild(words,width,height,bits); }
int fd_chroma_crop(ULONG *words) { return CamIspFdChromaCropBuild(words); }
int fd_cst_bias(CAM_ISP_PIXEL_PLAN *plan,ULONG width,ULONG height) { return CamIspPixelCstBiasDiagnostic(plan,width,height); }
int fd_geometry(ULONG *words,ULONG width,ULONG height) {
    if(!CamIspMndsFdBuild(words,width,height))return 0;
    return CamIspFdCenteredCropBuild(words+20);
}

int channel_gain(ULONG *words,const ULONG *data) { return CamIspChannelGainBuild(words,data); }
int cc_plan(ULONG *words,const LONG *matrix,const LONG *offsets,ULONG q) { return CamIspCcBuild(words,matrix,offsets,q); }
int demux_bayer(ULONG *words) { return CamIspDemuxBayerBuild(words); }

int gamma_plan(CAM_ISP_GAMMA *plan,const ULONG *values,ULONG bank) {
    return CamIspGammaBuild(plan,values,bank);
}

int raw10_input(CAM_ISP_RAW10_INPUT *plan,ULONG vc,ULONG cid,ULONG x,ULONG y,ULONG width,ULONG height) {
    return CamIspRaw10Input(plan,vc,cid,x,y,width,height);
}

int fd_plan(CAM_ISP_FD_PLAN *plan,ULONG width,ULONG height,ULONGLONG address,ULONG capacity) {
    return CamIspFdPlan(plan,width,height,address,capacity);
}

int fd_ready(ULONG bus,ULONG ipp,ULONGLONG sof,ULONGLONG eof,ULONGLONG previous,ULONG y,ULONG c,const CAM_ISP_NV12_OUTPUT *output) {
    return CamIspFdFrameReady(bus,ipp,sof,eof,previous,y,c,output);
}

int crop(ULONG *words,ULONG x,ULONG y,ULONG width,ULONG height,int measure) {
    return CamIspCsidCrop(words,x,y,width,height,measure);
}

int layout(CAM_ISP_NV12_OUTPUT *output,ULONG width,ULONG height,ULONGLONG address,ULONG capacity) {
    return CamIspNv12Output(output,width,height,address,capacity);
}

int demosaic(ULONG *words) {
    return CamIspDemosaicNative(words);
}

int mnds(ULONG *words,ULONG width,ULONG height,ULONG outWidth,ULONG outHeight) {
    return CamIspMndsBuild(words,width,height,outWidth,outHeight);
}

int wb(ULONG *words,ULONG red,ULONG green,ULONG blue,const LONG *offsets) {
    return CamIspWbBuild(words,red,green,blue,offsets);
}

int cst(ULONG *words,const LONG *matrix,const LONG *subtract,const LONG *add,const ULONG *low,const ULONG *high) {
    return CamIspCstBuild(words,matrix,subtract,add,low,high);
}

int compand_raw10_reference(ULONG *words) { return CamIspCompandRaw10Reference(words); }

int channel_gain_reference(ULONG *words,ULONG bayer) { return CamIspChannelGainReference(words,bayer); }

int bds2_plan(ULONG *words) { CAM_ISP_PIXEL_PLAN p;if(!words||!CamIspPixelBds2Reference(&p))return 0;words[0]=p.Count;for(ULONG i=0;i<p.Count;i++){words[1+i*2]=p.Registers[i].Offset;words[2+i*2]=p.Registers[i].Value;}return 1; }

#include "../isp-gtm-reference.h"
int gtm_reference(CAM_ISP_GTM *output,ULONG bank) {return CamIspGtmNativeReference(output,bank);}

int gtm_dmi(ULONG *buffer,ULONG capacity,ULONGLONG address,ULONG base,ULONG marker,const CAM_ISP_GTM *gtm) {return CamIspGtmDmiBuild(buffer,capacity,address,base,marker,gtm);}
int gtm_dmi_format(ULONG *buffer,ULONG capacity,ULONGLONG address,ULONG base,ULONG marker,const CAM_ISP_GTM *gtm,ULONG dmi64) {
    return dmi64<=1&&CamIspGtmDmiBuildFormat(buffer,capacity,address,base,marker,gtm,(BOOLEAN)dmi64);
}

#include "../isp-gamma-reference.h"
int gamma_reference(CAM_ISP_GAMMA *output,ULONG bank) {return CamIspGammaNativeReference(output,bank);}

int luma_diagnostic_plan(ULONG *words,ULONG mode) { CAM_ISP_PIXEL_PLAN p;if(!words||mode>6||!CamIspPixelLumaDiagnostic(&p,4096,3072,mode))return 0;words[0]=p.Count;for(ULONG i=0;i<p.Count;i++){words[1+i*2]=p.Registers[i].Offset;words[2+i*2]=p.Registers[i].Value;}return 1; }
