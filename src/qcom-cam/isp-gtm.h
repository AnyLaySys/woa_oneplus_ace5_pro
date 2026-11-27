#pragma once

/* Exact GTM133 Apollo980 packet: 263 register words at0x6658 and
   256 packed49-bit LUT entries atDMI0x6608/selector1. Gamma is separate.
   Curves/segment geometry must come from the selected native IQ profile. */
typedef struct {
    ULONG Enabled,Bank,Config[4],Base[256],Slope[256],Shift[256],Segments[256];
} CAM_ISP_GTM_INPUT;
typedef struct { ULONG Registers[263]; ULONGLONG Table[256]; } CAM_ISP_GTM;

static inline BOOLEAN CamIspGtmBuild(CAM_ISP_GTM *output,const CAM_ISP_GTM_INPUT *input) {
    if(!output||!input||input->Enabled>1||input->Bank>1||input->Config[0]>63||
       input->Config[1]>63||input->Config[2]>63||input->Config[3]>127)return FALSE;
    for(ULONG i=0;i<256;i++)if(input->Base[i]>0xfffff||input->Slope[i]>0xffffff||input->Shift[i]>31)return FALSE;
    output->Registers[0]=output->Registers[1]=input->Bank;
    output->Registers[2]=input->Enabled;
    for(ULONG i=0;i<4;i++)output->Registers[3+i]=input->Config[i];
    for(ULONG i=0;i<256;i++) {
        output->Registers[7+i]=input->Segments[i];
        output->Table[i]=(ULONGLONG)input->Base[i]|((ULONGLONG)input->Slope[i]<<20)|((ULONGLONG)input->Shift[i]<<44);
    }
    return TRUE;
}
