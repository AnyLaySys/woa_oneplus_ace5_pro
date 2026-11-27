#pragma once

#define CAM_DMA_IOVA 0x10000000ULL

static ULONG CamDmaImageAddress(ULONGLONG address) {return (ULONG)(address>>8);}

/* Validate the whole DMA-visible arena before adding the guard-page offset.
   Framework logical addresses must not be silently replaced with CPU PAs. */
static inline BOOLEAN CamDmaArenaAddressReady(ULONGLONG address,ULONG bytes) {
    return address&&bytes>=8192&&!(address&4095)&&!(bytes&4095)&&
        address<=0x1000000000ULL-bytes;
}
