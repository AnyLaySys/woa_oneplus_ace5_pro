#pragma once

#define CAM_SCM_SMART_BYTES 16u
#define CAM_SCM_RDI_BYTES 40u
#define CAM_SCM_CDM_BYTES 8u

static inline VOID CamScmCdmEncode(ULONG words[2]) {
    words[0]=0x10004c38;words[1]=3;
}

static inline VOID CamScmSmartEncode(ULONG words[4]) {
    words[0]=0x10005230;words[1]=0x65555544;
    words[2]=0x10005234;words[3]=0x66666666;
}

static inline VOID CamScmRdiEncode(ULONG words[10]) {
    CamScmSmartEncode(words);
    words[4]=0x10005238;words[5]=0x00001e40;
    words[6]=0x10005240;words[7]=0xffffff00;
    words[8]=0x10005248;words[9]=0x0000000f;
}
