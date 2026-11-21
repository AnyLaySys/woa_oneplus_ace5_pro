#pragma once

#define CAM_CDM_COMMAND_BYTES 24u
#define CAM_CDM_FETCH_BYTES 8u

static VOID CamCdmEncodeFetch(ULONG *words,ULONG marker) {
    words[0]=0x06000001;
    words[1]=marker;
}

static VOID CamCdmEncode(ULONG *words,ULONG marker) {
    words[0]=0x08127000;
    words[1]=0x04000001;
    words[2]=0xe10;
    words[3]=0x8011;
    words[4]=0x06000001;
    words[5]=marker;
}

static BOOLEAN CamCdmFetched(PUCHAR base,ULONG marker) {
    return CamRead(base,0x80)==marker&&
           !(CamRead(base,0x6c)&0x1ff)&&
           (CamRead(base,0x44)&0x70002)==2;
}

static BOOLEAN CamCdmCompleted(PUCHAR base,ULONG marker) {
    return CamCdmFetched(base,marker)&&
           ((CamRead(base,0xd0)&0xffffff)==0x127e10||CamRead(base,0xd0)==0xad27e10)&&
           CamRead(base,0xd4)==0x8011;
}
