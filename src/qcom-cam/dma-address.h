#pragma once

#define CAM_DMA_IOVA 0x10000000ULL

static ULONG CamDmaImageAddress(ULONGLONG address) {return (ULONG)(address>>8);}
