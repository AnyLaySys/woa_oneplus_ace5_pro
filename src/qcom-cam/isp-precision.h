#pragma once

/* Exact CompDecomp20 register packet from Apollo980 packer0xce220.
   Its caller selects compression/decompression and calibration; this helper
   does not invent a bit-depth conversion curve or start a pixel path. */
typedef struct {
    ULONG Enabled,Mode,Knees[15],Slopes[16],Bases[16],Shifts[16];
} CAM_ISP_COMPDECOMP;

static inline BOOLEAN CamIspCompDecompBuild(ULONG words[49],const CAM_ISP_COMPDECOMP *input) {
    if(!words||!input||input->Enabled>1||input->Mode>3)return FALSE;
    for(ULONG i=0;i<16;i++)if(input->Bases[i]>0x3fff||input->Shifts[i]>63)return FALSE;
    ULONG result[49]={0};
    result[1]=input->Enabled|(input->Mode<<2);
    for(ULONG i=0;i<15;i++)result[2+i]=input->Knees[i];
    for(ULONG i=0;i<16;i++)result[17+i]=input->Slopes[i];
    for(ULONG i=0;i<8;i++) {
        result[33+i]=input->Bases[i*2]|(input->Bases[i*2+1]<<16);
        result[41+i]=input->Shifts[i*2]|(input->Shifts[i*2+1]<<16);
    }
    RtlCopyMemory(words,result,sizeof(result));return TRUE;
}

/* Native constructor0xcdaa0 selects these destinations. FD resource17
   omits the shared context word, as does original emitter0xcdeb0. */
static inline BOOLEAN CamIspCompDecompRegisters(ULONG offsets[49],ULONG resource,ULONG *first) {
    if(!offsets||!first||(resource!=1&&resource!=2&&resource!=17))return FALSE;
    ULONG start=resource==1?0x8d00:resource==2?0xe200:0x6200;
    offsets[0]=start+0x1f4;offsets[1]=start+0x60;
    for(ULONG i=0;i<15;i++)offsets[2+i]=start+0x68+i*4;
    for(ULONG i=0;i<16;i++)offsets[17+i]=start+0xb4+i*4;
    for(ULONG i=0;i<8;i++){offsets[33+i]=start+0x100+i*4;offsets[41+i]=start+0x134+i*4;}
    *first=resource==17?1:0;return TRUE;
}

/* Original disable emitter0xce7b0 writes the context before the control.
   The two destinations are independently verified through constructor0xcdaa0.
   mode is the native object+0x10 selector, not a compression mode or proof
   that a pipeline branch is disconnected. FD resource17 has no such context. */
static inline BOOLEAN CamIspCompDecompDisabled(ULONG registers[4],ULONG resource,ULONG context,ULONG mode,ULONG control) {
    if(!registers||(resource!=1&&resource!=2)||context>31||mode>1)return FALSE;
    ULONG start=resource==1?0x8d00:0xe200;
    registers[0]=start+0x1f4;registers[1]=mode?context:0x400;
    registers[2]=start+0x60;registers[3]=control&~1u;
    return TRUE;
}

/* Diagnostic-only RAW10 mode5 reserve shared by all four verified hummer
   tuning files. Original IQ f1790 output SHA256
   0668e6e400999fc8f0f31f27a3252c6bfdf19c01444a9f3db9506951d311ab80.
   Full input and native packer/emitter are oracle checked. This does not
   select production tuning or implement a software pixel conversion. */
static inline BOOLEAN CamIspCompandRaw10Reference(ULONG words[49]) {
    static const CAM_ISP_COMPDECOMP input={
        1,2,
        {0,256,378,524,690,883,1099,1339,1598,1885,2195,2529,2887,3262,3667},
        {0,0,1024,1244,1464,1680,1900,2120,2340,2556,2776,2996,3216,3436,3652,3872},
        {0,8192,14772,12344,10659,9338,8344,15019,13664,12559,11627,10792,10068,9437,8900,8537},
        {14,11,13,13,13,13,13,14,14,14,14,14,14,14,14,14},
    };
    return CamIspCompDecompBuild(words,&input);
}
