#pragma once

#define CAM_DMA_IOVA 0x10000000ULL

static ULONG CamDmaImageAddress(ULONGLONG address) {return (ULONG)(address>>8);}

static inline BOOLEAN CamDmaPageRangeReady(ULONGLONG address,ULONG bytes,ULONG width) {
    if((width!=32&&width!=36)||!address||!bytes||((address|bytes)&4095))return FALSE;
    ULONGLONG limit=1ULL<<width;
    return bytes<=limit&&address<=limit-bytes;
}

/* Validate the whole DMA-visible arena before adding the guard-page offset.
   Framework logical addresses must not be silently replaced with CPU PAs. */
static inline BOOLEAN CamDmaArenaAddressReady(ULONGLONG address,ULONG bytes) {
    return bytes>=8192&&CamDmaPageRangeReady(address,bytes,36);
}
