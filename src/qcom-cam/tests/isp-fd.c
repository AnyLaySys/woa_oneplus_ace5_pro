#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef uint32_t ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t LONG,NTSTATUS;
typedef uint16_t USHORT;
typedef uint8_t UCHAR;
typedef char CHAR;
typedef unsigned char *PUCHAR;
typedef int BOOLEAN;
typedef void VOID;
#define TRUE 1
#define FALSE 0
#define MAXULONG UINT32_MAX
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_REVISION_MISMATCH -2
#define STATUS_DEVICE_BUSY -3
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_IO_TIMEOUT -5
#define STATUS_NOT_SUPPORTED -6
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
enum {CamCsid=5,CamTfe=6};
#include "../uapi.h"
static ULONG csid[0x3000/4],tfe[0x10000/4],csidBefore[0x3000/4],tfeBefore[0x10000/4];
static ULONG writes,fault,faultOffset=0x1570;
static ULONGLONG ticks;
static ULONG CamRead(PUCHAR base,ULONG offset) { return ((ULONG *)base)[offset/4]; }
static ULONGLONG CamRead64(PUCHAR base,ULONG offset) { return CamRead(base,offset)|((ULONGLONG)CamRead(base,offset+4)<<32); }
static void CamWrite(PUCHAR base,ULONG offset,ULONG value) {
    if(base==(PUCHAR)tfe) {
        /* Standby and rollback must never enqueue an address, enable a WM,
           write an IRQ acknowledge register or touch a DMI data port. */
        assert(offset!=0x1404&&offset!=0x1504&&offset!=0x1400&&offset!=0x1500);
        assert(offset!=0x6b08&&offset!=0x6b10&&offset!=0x6b14);
        assert(offset!=0x820&&offset!=0x824&&offset!=0x830);
    } else assert(offset!=0x604&&(offset!=0x600||!(value&0x80000000u)));
    ((ULONG *)base)[offset/4]=value;writes++;
    if(fault&&base==(PUCHAR)tfe&&offset==faultOffset){((ULONG *)base)[offset/4]^=1;fault=0;}
}
static ULONGLONG KeQueryInterruptTime(void) { return ticks++; }
static void CamDelay(ULONG us) { ticks+=us*10; }
#include "../isp-fd-check.h"
#include "../isp-release.h"
int main(void) {
    static QCAM_ISP_FD_STATE state;
    state.Stage=0x7f;
    assert(CamIspFdReleased(&state,FALSE,FALSE,FALSE));
    assert(!CamIspFdReleased(NULL,FALSE,FALSE,FALSE));
    assert(!CamIspFdReleased(&state,TRUE,FALSE,FALSE));
    assert(!CamIspFdReleased(&state,FALSE,TRUE,FALSE));
    assert(!CamIspFdReleased(&state,FALSE,FALSE,TRUE));
    state.Stage=0x1f;assert(!CamIspFdReleased(&state,FALSE,FALSE,FALSE));
    state.Stage=0x5f;assert(!CamIspFdReleased(&state,FALSE,FALSE,FALSE));
    state.Stage=0x7f;state.DrainStatus=0x103;assert(!CamIspFdReleased(&state,FALSE,FALSE,FALSE));
    state.DrainStatus=0;state.RestoreStatus=0xc0000185;assert(!CamIspFdReleased(&state,FALSE,FALSE,FALSE));
    memset(&state,0,sizeof(state));
    csid[0]=0x30000006;tfe[0]=0x10000000;
    const ULONG widths[]={4096,3264,1600,2304},heights[]={3072,2448,1200,1728};
    for(ULONG camera=0;camera<4;camera++) {
        assert(CamIspFdBuild(&state,widths[camera],heights[camera],0xe0000000,QCAM_FRAME_BYTES));
        assert(state.Count==QCAM_ISP_FD_BASE_REG_COUNT&&state.Stride==1280&&state.ImageBytes==1382400);
        for(ULONG i=0;i<state.Count;i++)for(ULONG j=0;j<i;j++)
            assert(state.Region[i]!=state.Region[j]||state.Offset[i]!=state.Offset[j]);
        for(ULONG i=0;i<state.Count;i++) {
            ULONG offset=state.Offset[i];
            if(offset!=0x3f0&&offset!=0x600&&offset!=0x924&&offset!=0xdbf4&&offset!=0xe1f4&&
               offset!=0x78f4&&offset!=0x8ef4&&offset!=0xe3f4)
                ((ULONG *)CamIspFdBase((PUCHAR)csid,(PUCHAR)tfe,state.Region[i]))[offset/4]=i+7;
        }
        memcpy(csidBefore,csid,sizeof(csid));memcpy(tfeBefore,tfe,sizeof(tfe));writes=0;
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe&&
            (state.Offset[i]==0x6260||state.Offset[i]==0xe260||state.Offset[i]==0x8d60||state.Offset[i]==0x6660)) {
            assert(state.Configured[i]==(state.Before[i]&~1u));
            assert((state.Configured[i]&~1u)==(state.Before[i]&~1u));
        }
        for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe&&state.Offset[i]==0x858)
            assert(state.Configured[i]==(state.Before[i]|0x1036));
        assert(tfe[0x6658/4]==tfeBefore[0x6658/4]);
        ULONG frameWrites=writes;
        assert(!CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state));
        state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;
        csid[0x600/4]|=0x80000000u;
        assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state));
        assert(state.AtFrameCount==QCAM_ISP_FD_BASE_REG_COUNT&&!state.AtFrameMismatch&&writes==frameWrites);
        /* Simulate a CST control lost during startup, after the earlier
           configured snapshot passed. Capture must expose this divergence. */
        tfe[0x6d60/4]^=1;
        assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&state.AtFrameMismatch==1);
        assert(writes==frameWrites);
        tfe[0x6d60/4]^=1;csid[0x600/4]&=~0x80000000u;
        state.FrameEof=state.FrameSof;
        assert(!CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state));
        assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(writes==2*(QCAM_ISP_FD_BASE_REG_COUNT+3));
        csidBefore[0x18/4]=csidBefore[0x1c/4]=csidBefore[0x20/4]=1;
        assert(!memcmp(csidBefore,csid,sizeof(csid))&&!memcmp(tfeBefore,tfe,sizeof(tfe)));
        assert(!memcmp(state.Before,state.Restored,sizeof(state.Before)));
    }
    assert(CamIspFdBuild(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES));
    fault=1;assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_IO_DEVICE_ERROR);
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
    writes=0;
    for(ULONG i=0;i<28;i++) {
        tfe[(0xd00+i*0x100)/4]=1;
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_DEVICE_BUSY&&!writes);
        assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_DEVICE_BUSY&&!writes);
        tfe[(0xd00+i*0x100)/4]=0;
    }
    for(ULONG i=0;i<9;i++) {
        csid[(0x600+i*0x200)/4]=0x80000000u;
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_DEVICE_BUSY&&!writes);
        csid[(0x600+i*0x200)/4]=0;
    }
    tfe[0x924/4]=0x21;
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_DEVICE_BUSY&&!writes);
    tfe[0x924/4]=0;
    const ULONG contexts[]={0x8ef4,0xe3f4};
    for(ULONG i=0;i<2;i++)for(ULONG value=1;value<=0x400;value<<=1) {
        if(!(value&0x41f))continue;
        tfe[contexts[i]/4]=value;
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_DEVICE_BUSY&&!writes);
        tfe[contexts[i]/4]=0;
    }
    assert(!CamIspFdBuild(&state,1024,720,0xe0000000,QCAM_FRAME_BYTES));
    assert(!CamIspFdBuild(&state,4096,3072,0xe0000000,1382399));
    assert(!CamIspFdBuild(&state,4096,3072,0xfffffffffULL,1382400));
    assert(CamIspFdBuildProfile(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,TRUE));
    assert(state.ScalerWidth==2048&&state.ScalerHeight==1536);
    assert(!CamIspFdBuildProfile(&state,2304,1728,0xe0000000,QCAM_FRAME_BYTES,TRUE));
    assert(!CamIspFdBuildProfile(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,2));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,2));
    assert(state.Width==1024&&state.Height==768&&state.Stride==1024&&state.ImageBytes==1179648);
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,2));
    assert(!CamIspFdBuildMode(&state,4096,3072,0xe0000000,1179647,2));
    for(ULONG camera=0;camera<4;camera++) {
        assert(CamIspFdBuildMode(&state,widths[camera],heights[camera],0xe0000000,QCAM_FRAME_BYTES,3));
        assert(state.Count==QCAM_ISP_FD_BASE_REG_COUNT&&state.ImageBytes==1382400);
        ULONG packers=0;
        for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe&&(state.Offset[i]==0x1418||state.Offset[i]==0x1518)) {
            assert(state.Expected[i]==1);packers++;
        }
        assert(packers==2);
    }
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,4));
    ULONG constantOffsets=0;
    for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe) {
        ULONG offset=state.Offset[i];
        if(offset==0x6d7c){assert(state.Expected[i]==256);constantOffsets++;}
        if(offset==0x6d94){assert(state.Expected[i]==512);constantOffsets++;}
        if(offset==0x6dac){assert(state.Expected[i]==768);constantOffsets++;}
        if(offset==0x6d68||offset==0x6d6c||offset==0x6d80||offset==0x6d84||offset==0x6d98||offset==0x6d9c)
            assert(!state.Expected[i]);
    }
    assert(constantOffsets==3);
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,5));
    ULONG chromaControls=0;
    for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe) {
        ULONG offset=state.Offset[i];
        if(offset==0x7570||offset==0x7578){assert(state.Expected[i]==0x80);chromaControls++;}
        if(offset==0x7588||offset==0x758c){assert(state.Expected[i]==128);chromaControls++;}
    }
    assert(chromaControls==4);
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,6));
    ULONG biasControls=0;
    for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe) {
        ULONG offset=state.Offset[i];
        if(offset==0x6d78){assert(state.Expected[i]==256);biasControls++;}
        if(offset==0x6d90){assert(state.Expected[i]==512);biasControls++;}
        if(offset==0x6da8){assert(state.Expected[i]==768);biasControls++;}
        if(offset==0x6d7c||offset==0x6d94||offset==0x6dac)assert(!state.Expected[i]);
    }
    assert(biasControls==3);
    assert(!CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,33));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,7));
    assert(state.Profile==7&&state.Count==QCAM_ISP_FD_COMPAND_REG_COUNT);
    ULONG curve[49],curveOffsets[49],first;
    assert(CamIspCompandRaw10Reference(curve)&&CamIspCompDecompRegisters(curveOffsets,17,&first)&&first==1);
    for(ULONG i=1;i<49;i++) {
        ULONG matches=0;
        for(ULONG j=0;j<state.Count;j++)if(state.Region[j]==CamTfe&&state.Offset[j]==curveOffsets[i]) {
            assert(state.Expected[j]==curve[i]);matches++;
        }
        assert(matches==1);
    }
    memcpy(tfeBefore,tfe,sizeof(tfe));
    /* Use nonzero old curve/control values to prove complete restoration. */
    for(ULONG i=1;i<49;i++)tfe[curveOffsets[i]/4]=0xa5a50000u+i;
    memcpy(tfeBefore,tfe,sizeof(tfe));
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(tfe[0x6260/4]==9);
    state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
    ULONG curveFrameWrites=writes;
    assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&state.AtFrameCount==QCAM_ISP_FD_COMPAND_REG_COUNT&&!state.AtFrameMismatch);
    assert(writes==curveFrameWrites);csid[0x600/4]&=~0x80000000u;
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
    fault=1;faultOffset=0x6334;
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_IO_DEVICE_ERROR);
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));faultOffset=0x1570;
    state.Count--;writes=0;
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_INVALID_PARAMETER&&!writes);
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_INVALID_PARAMETER&&!writes);
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,7));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,8));
    assert(state.Profile==8&&state.Count==QCAM_ISP_FD_BASE_REG_COUNT);
    ULONG gammaIndices[3]={0};
    for(ULONG i=0;i<state.Count;i++)if(state.Region[i]==CamTfe&&state.Offset[i]>=0x6b58&&state.Offset[i]<=0x6b60)
        gammaIndices[(state.Offset[i]-0x6b58)/4]=i;
    for(ULONG original=0;original<2;original++) {
        tfe[0x6b58/4]=tfe[0x6b5c/4]=original;tfe[0x6b60/4]=1;
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        ULONG bank=0xa5;
        assert(CamIspFdGammaBank(&state,&bank)&&bank==(original^1u));
        assert(!CamIspFdGammaConfigured(&state));
        state.GammaBank=bank;state.GammaLoaded=1;state.GammaStatus=0;
        state.GammaConfig[0]=state.GammaConfig[1]=bank;state.GammaConfig[2]=1;
        state.GammaConfig[2]=0;assert(!CamIspFdGammaConfigured(&state));state.GammaConfig[2]=1;
        state.GammaStatus=0xc0000185;assert(!CamIspFdGammaConfigured(&state));state.GammaStatus=0;
        assert(CamIspFdGammaConfigured(&state));
        tfe[0x6b58/4]=tfe[0x6b5c/4]=bank;tfe[0x6b60/4]=1;
        assert(state.Before[gammaIndices[0]]==original&&state.Before[gammaIndices[1]]==original);
        state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
        assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&!state.AtFrameMismatch);
        csid[0x600/4]&=~0x80000000u;
        assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(tfe[0x6b58/4]==original&&tfe[0x6b5c/4]==original&&tfe[0x6b60/4]==1);
        state.GammaLoaded=0;
    }
    state.Before[gammaIndices[0]]=2;ULONG gammaBank=0xa5;
    assert(!CamIspFdGammaBank(&state,&gammaBank)&&gammaBank==0xa5);
    assert(!CamIspFdGammaBank(&state,NULL));
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,8));
    static QCAM_ISP_FD_STATE ordinary;
    assert(CamIspFdBuildMode(&ordinary,4096,3072,0xe0000000,QCAM_FRAME_BYTES,0));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,9));
    ULONG bypassChanged=0;
    for(ULONG i=0;i<state.Count;i++) {
        assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
        if(state.Expected[i]!=ordinary.Expected[i]) {
            assert(state.Region[i]==CamTfe&&state.Offset[i]==0x6460&&state.Expected[i]==0);
            bypassChanged++;
        }
    }
    assert(bypassChanged==1&&state.Count==QCAM_ISP_FD_BASE_REG_COUNT);
    tfe[0x6460/4]=0xa5a50001;
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(tfe[0x6460/4]==0xa5a50000);
    state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
    assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&!state.AtFrameMismatch);
    csid[0x600/4]&=~0x80000000u;
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(tfe[0x6460/4]==0xa5a50001);
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,9));
    assert(!CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,33));
    for(ULONG profile=10;profile<=11;profile++) {
        ULONG offset=profile==10?0xda60:0xe060,changed=0;
        assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,profile));
        for(ULONG i=0;i<state.Count;i++) {
            assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
            if(state.Expected[i]!=ordinary.Expected[i]) {
                assert(state.Region[i]==CamTfe&&state.Offset[i]==offset&&!state.Expected[i]);changed++;
            }
        }
        assert(changed==1);tfe[offset/4]=0xa5a50001;
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(tfe[offset/4]==0xa5a50000);
        state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
        assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&!state.AtFrameMismatch);
        csid[0x600/4]&=~0x80000000u;
        assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(tfe[offset/4]==0xa5a50001);
        assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,profile));
    }
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,12));
    ULONG lumaChanged=0;
    for(ULONG i=0;i<state.Count;i++) {
        assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
        if(state.Expected[i]!=ordinary.Expected[i]) {
            assert(state.Region[i]==CamTfe&&state.Offset[i]==0x6d78&&state.Expected[i]==512);lumaChanged++;
        }
    }
    assert(lumaChanged==1);
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,12));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,13));
    assert(state.ScalerWidth==2048&&state.ScalerHeight==1536&&state.Count==QCAM_ISP_FD_BASE_REG_COUNT);
    tfe[0x7760/4]=0x123;tfe[0x7794/4]=0x456;
    memcpy(tfeBefore,tfe,sizeof(tfe));
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(tfe[0x7760/4]==0x1001&&tfe[0x7794/4]==16&&tfe[0x7768/4]==2048&&tfe[0x776c/4]==1536);
    state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
    assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&!state.AtFrameMismatch);
    csid[0x600/4]&=~0x80000000u;
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,13));
    for(ULONG original=0;original<2;original++) {
        assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,14));
        assert(state.Count==QCAM_ISP_FD_GTM_REG_COUNT&&state.Profile==14);
        for(ULONG i=0;i<263;i++)tfe[(0x6658+i*4)/4]=i+0x12340000;
        tfe[0x6658/4]=tfe[0x665c/4]=original;
        memcpy(tfeBefore,tfe,sizeof(tfe));
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(state.GtmBank==(original^1u)&&tfe[0x6658/4]==state.GtmBank&&tfe[0x665c/4]==state.GtmBank);
        assert(tfe[0x6660/4]==1&&tfe[0x6664/4]==16&&tfe[0x6668/4]==32);
        state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
        assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&!state.AtFrameMismatch);
        csid[0x600/4]&=~0x80000000u;
        assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
    }
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,14));
    tfe[0x6658/4]=tfe[0x665c/4]=2;ULONG invalidBankWrites=writes;
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_NOT_SUPPORTED&&writes==invalidBankWrites);
    tfe[0x6658/4]=tfe[0x665c/4]=0;
    memcpy(tfeBefore,tfe,sizeof(tfe));fault=1;faultOffset=0x6a70;
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_IO_DEVICE_ERROR);
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
    assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,14));
    for(ULONG profile=15;profile<=32;profile++)for(ULONG original=0;original<2;original++) {
        assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,profile));
        assert(state.Count==QCAM_ISP_FD_PROFILE_REG_COUNT(profile)&&state.ScalerWidth==((profile==16||profile==23)?2048u:4096u));
        if(profile==22||profile==23)for(ULONG i=1;i<49;i++) {
            ULONG matches=0;
            for(ULONG j=0;j<state.Count;j++)if(state.Region[j]==CamTfe&&state.Offset[j]==curveOffsets[i]) {
                assert(state.Expected[j]==curve[i]);matches++;
            }
            assert(matches==1);
        }
        tfe[0x6658/4]=tfe[0x665c/4]=original^1u;
        tfe[0x6b58/4]=tfe[0x6b5c/4]=original;tfe[0x6b60/4]=1;
        memcpy(tfeBefore,tfe,sizeof(tfe));
        assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        ULONG gammaBank=MAXULONG;
        assert(CamIspFdGammaBank(&state,&gammaBank)&&gammaBank==(original^1u));
        assert(state.GtmBank==original);
        state.GammaBank=gammaBank;state.GammaLoaded=1;state.GammaStatus=0;
        state.GammaConfig[0]=state.GammaConfig[1]=gammaBank;state.GammaConfig[2]=1;
        assert(CamIspFdGammaConfigured(&state));
        tfe[0x6b58/4]=tfe[0x6b5c/4]=gammaBank;tfe[0x6b60/4]=1;
        state.Stage|=0x1c;state.FrameSof=100;state.FrameEof=120;csid[0x600/4]|=0x80000000u;
        assert(CamIspFdFrameRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)&&!state.AtFrameMismatch);
        csid[0x600/4]&=~0x80000000u;
        assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
        assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
        assert(!CamIspFdBuildMode(&state,3264,2448,0xe0000000,QCAM_FRAME_BYTES,profile));
    }
    assert(CamIspFdBuildMode(&ordinary,4096,3072,0xe0000000,QCAM_FRAME_BYTES,15));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,26));
    ULONG cstChanges=0;
    for(ULONG i=0;i<state.Count;i++) {
        assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
        if(state.Offset[i]==0x6d60&&state.Region[i]==CamTfe){assert(state.Expected[i]==0&&ordinary.Expected[i]==1);cstChanges++;}
        else assert(state.Expected[i]==ordinary.Expected[i]);
    }
    assert(cstChanges==1);
    tfe[0x6d60/4]=0xa5a50001;memcpy(tfeBefore,tfe,sizeof(tfe));
    assert(CamIspFdApply((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(tfe[0x6d60/4]==0xa5a50000);
    assert(CamIspFdRestoreRegisters((PUCHAR)csid,(PUCHAR)tfe,&state)==STATUS_SUCCESS);
    assert(!memcmp(tfeBefore,tfe,sizeof(tfe)));
    assert(CamIspFdBuildMode(&ordinary,4096,3072,0xe0000000,QCAM_FRAME_BYTES,15));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,30));
    assert(state.Count==ordinary.Count);
    for(ULONG i=0;i<state.Count;i++) {
        assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
        assert(state.Expected[i]==ordinary.Expected[i]);
    }
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,31));
    assert(state.Count==ordinary.Count);
    for(ULONG i=0;i<state.Count;i++) {
        assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
        assert(state.Expected[i]==ordinary.Expected[i]);
    }
    for(ULONG profile=27;profile<=29;profile++) {
        assert(CamIspFdBuildMode(&ordinary,4096,3072,0xe0000000,QCAM_FRAME_BYTES,profile==27?15:26));
        assert(CamIspFdBuildMode(&state,4096,3072,0xe0000000,QCAM_FRAME_BYTES,profile));
        ULONG changes=0;
        for(ULONG i=0;i<state.Count;i++) {
            assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
            if(profile!=27&&state.Region[i]==CamTfe&&(state.Offset[i]==0x7374||state.Offset[i]==0x7574||state.Offset[i]==0x757c)) {
                assert(state.Expected[i]==((profile==28?8u:6u)<<3)&&ordinary.Expected[i]==16);changes++;
            } else assert(state.Expected[i]==ordinary.Expected[i]);
        }
        assert(changes==(profile==27?0u:3u));
    }
    assert(CamIspFdBuildMode(&ordinary,4096,3072,0xe0000000,QCAM_FRAME_BYTES,30));
    assert(CamIspFdBuildMode(&state,4096,3072,0xe0000080,QCAM_FRAME_BYTES-128,32));
    ULONG offsetChanges=0;
    for(ULONG i=0;i<state.Count;i++) {
        assert(state.Region[i]==ordinary.Region[i]&&state.Offset[i]==ordinary.Offset[i]);
        if(state.Region[i]==CamTfe&&(state.Offset[i]==0x1470||state.Offset[i]==0x1570)) {
            assert(state.Expected[i]==128&&ordinary.Expected[i]==0);offsetChanges++;
        } else assert(state.Expected[i]==ordinary.Expected[i]);
    }
    assert(offsetChanges==2&&state.Address==0xe0000080&&state.ImageBytes==1382400);
    CAM_ISP_FD_PLAN packedGuard;memset(&packedGuard,0xa5,sizeof(packedGuard));
    assert(!CamIspFdPlanPacked(&packedGuard,1280,720,0xe0000000,QCAM_FRAME_BYTES,2));
    for(ULONG i=0;i<sizeof(packedGuard);i++)assert(((unsigned char *)&packedGuard)[i]==0xa5);
    assert(!CamIspFdSignalValid(27,0,0));
    assert(!CamIspFdSignalValid(27,255,255));
    assert(!CamIspFdSignalValid(15,128,128));
    assert(CamIspFdSignalValid(27,0,255));
    assert(CamIspFdSignalValid(15,5,128));
    assert(CamIspFdSignalValid(28,255,255));
    assert(CamIspFdSignalValid(6,64,64));
    assert(!CamIspFdSignalValid(30,0,0));
    assert(CamIspFdSignalValid(30,0,255));
    assert(!CamIspFdSignalValid(31,0,0));
    assert(CamIspFdSignalValid(31,0,255));
    assert(!CamIspFdSignalValid(32,0,0));
    assert(CamIspFdSignalValid(32,0,255));
    assert(!CamIspFdSignalValid(33,0,255));
    assert(!CamIspFdSignalValid(27,0,256));
    assert(!CamIspFdSignalValid(27,128,64));
    CAM_ISP_NV12_OUTPUT layout;
    CAM_ISP_NV12_COVERAGE coverage,coverageBefore;
    unsigned char diagnostic[32*16*3/2];
    memset(diagnostic,0xa5,sizeof(diagnostic));
    assert(CamIspNv12Coverage(&coverage,diagnostic,sizeof(diagnostic),32,16));
    assert(coverage.Minimum[0]==165&&coverage.Maximum[0]==165&&coverage.Minimum[1]==165&&coverage.Maximum[1]==165);
    assert(!coverage.Changed[0]&&!coverage.Changed[1]&&coverage.FirstRow[0]==MAXULONG&&coverage.LastRow[1]==MAXULONG);
    assert(coverage.MissingRows[0]==16&&coverage.MissingRows[1]==8);
    assert(!CamIspNv12CoverageValid(&coverage));
    memset(diagnostic+32*3,0,32*5);memset(diagnostic+32*16+32*2,0,32*3);
    assert(CamIspNv12Coverage(&coverage,diagnostic,sizeof(diagnostic),32,16));
    assert(coverage.Minimum[0]==0&&coverage.Maximum[0]==165&&coverage.Minimum[1]==0&&coverage.Maximum[1]==165);
    assert(coverage.Changed[0]==160&&coverage.FirstRow[0]==3&&coverage.LastRow[0]==7);
    assert(coverage.Changed[1]==96&&coverage.FirstRow[1]==2&&coverage.LastRow[1]==4);
    assert(coverage.Bands[0][1]==32&&coverage.Bands[0][2]==64&&coverage.Bands[0][3]==64);
    assert(coverage.Bands[1][2]==32&&coverage.Bands[1][3]==32&&coverage.Bands[1][4]==32);
    assert(coverage.BlockParity[0][0]==160&&!coverage.BlockParity[0][1]);
    assert(coverage.BlockParity[1][0]==96&&!coverage.BlockParity[1][1]);
    assert(coverage.WrittenMinimum[0]==0&&coverage.WrittenMaximum[0]==0);
    assert(coverage.WrittenMinimum[1]==0&&coverage.WrittenMaximum[1]==0);
    assert(!CamIspNv12CoverageValid(&coverage));
    coverageBefore=coverage;
    assert(!CamIspNv12Coverage(&coverage,diagnostic,sizeof(diagnostic)-1,32,16));
    assert(!CamIspNv12Coverage(&coverage,diagnostic,sizeof(diagnostic),32,15));
    assert(!CamIspNv12Coverage(&coverage,NULL,sizeof(diagnostic),32,16));
    assert(!memcmp(&coverage,&coverageBefore,sizeof(coverage)));
    /* Actual bit8 gaps with an offset128 start split the writes between
       both relative parity buckets; neither parity nor marker variance
       can certify a complete image. Exercise other alignments too. */
    unsigned char offsetFrame[1280*4*3/2];
    const ULONG frameOffsets[]={0,16,128,256};
    for(ULONG offsetIndex=0;offsetIndex<4;offsetIndex++) {
        ULONG offset=frameOffsets[offsetIndex];
        for(ULONG i=0;i<sizeof(offsetFrame);i++)
            offsetFrame[i]=((i+offset)&256)?0xa5:(i<1280*4?0:128);
        assert(CamIspNv12Coverage(&coverage,offsetFrame,sizeof(offsetFrame),1280,4));
        assert(coverage.Changed[0]==1280*2&&coverage.Changed[1]==1280);
        if(offset==128)for(ULONG plane=0;plane<2;plane++)
            assert(coverage.BlockParity[plane][0]&&coverage.BlockParity[plane][1]);
        assert(coverage.MarkerSpans[0]&&coverage.MarkerSpans[1]);
        assert(coverage.WrittenMinimum[0]==0&&coverage.WrittenMaximum[0]==0);
        assert(!CamIspNv12CoverageValid(&coverage));
    }
    memset(offsetFrame,128,sizeof(offsetFrame));
    offsetFrame[17]=0xa5;offsetFrame[1280*4+17]=0xa5;
    assert(CamIspNv12Coverage(&coverage,offsetFrame,sizeof(offsetFrame),1280,4));
    assert(CamIspNv12CoverageValid(&coverage));
    assert(!CamIspNv12CoverageValid(NULL));
    memset(offsetFrame+31,0xa5,64);
    assert(CamIspNv12Coverage(&coverage,offsetFrame,sizeof(offsetFrame),1280,4));
    assert(!CamIspNv12CoverageValid(&coverage));
    memset(diagnostic,0,sizeof(diagnostic));memset(diagnostic+32*5,0xa5,32);
    assert(CamIspNv12Coverage(&coverage,diagnostic,sizeof(diagnostic),32,16));
    assert(!coverage.MarkerSpans[0]&&coverage.MissingRows[0]==1);
    assert(!CamIspNv12CoverageValid(&coverage));
    assert(CamIspNv12Output(&layout,1280,720,0xe0000000,QCAM_FRAME_BYTES));
    CAM_ISP_NV12_OUTPUT shifted;
    assert(CamIspNv12Output(&shifted,1280,720,0xe0000100,QCAM_FRAME_BYTES-256));
    assert(shifted.ImageAddress[0]==layout.ImageAddress[0]+1&&shifted.ImageAddress[1]==layout.ImageAddress[1]+1);
    assert(!shifted.AddressOffset[0]&&!shifted.AddressOffset[1]&&shifted.Bytes==layout.Bytes);
    assert(CamIspFdFrameReady(8,0x18,100,120,0,layout.ImageAddress[0],layout.ImageAddress[1],&layout));
    assert(!CamIspFdFrameReady(0x30000,0x18,100,120,0,layout.ImageAddress[0],layout.ImageAddress[1],&layout));
    assert(!CamIspFdFrameReady(8,0x18,100,120,0,layout.ImageAddress[0],layout.ImageAddress[1]+1,&layout));
    assert(CamIspFdFrameSof(100,80,120,90,95)==100);
    assert(CamIspFdFrameSof(140,100,120,90,95)==100);
    assert(!CamIspFdFrameSof(140,80,120,90,95));
    assert(!CamIspFdFrameSof(140,100,120,90,120));
    assert(!CamIspFdFrameSof(120,100,120,90,95));
    assert(!CamIspFdFrameSof(140,130,120,90,95));
    ULONG empty[0x1000/4]={0};
    assert(CamDmaDrain((PUCHAR)empty,(PUCHAR)csid,(PUCHAR)tfe,(PUCHAR)empty,(PUCHAR)empty,10)==STATUS_SUCCESS);
    puts("FD transport: four sensors, both planes, late chroma fault rollback, active resource/context rejection and no address queue/DMA/DMI side effects passed");
    return 0;
}
