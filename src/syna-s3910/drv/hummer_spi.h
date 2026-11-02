/*
 * GENI SPI4 transport for the Synaptics S3910 on hummer (SM8750).
 *
 * There is no Qualcomm GENI SPI driver for Windows on this platform, so the
 * controller is driven from inside this driver: clocks through GCC, pin mux
 * through TLMM, data through the serial engine's GPI/GSI DMA path (this
 * engine reports GENI_IF_DISABLE_RO bit0 = 1, so its PIO FIFO does not exist
 * and the DMA interface block is not host-accessible).
 *
 * Register maps, the ring/TRE layout and the init sequence follow
 * drivers/dma/qcom/gpi.c and drivers/soc/qcom/qcom-geni-se.c in mainline.
 */
#ifndef HUMMER_SPI_H
#define HUMMER_SPI_H

#include <ntddk.h>
#include <wdf.h>

#define HSP_TRE_BYTES   16u
#define HSP_CH_ELEMS    64u
#define HSP_EV_ELEMS    128u

#define HSP_EV_BYTES    (HSP_EV_ELEMS * HSP_TRE_BYTES)
#define HSP_TX_BYTES    (HSP_CH_ELEMS * HSP_TRE_BYTES)
#define HSP_RX_BYTES    (HSP_CH_ELEMS * HSP_TRE_BYTES)
#define HSP_RING_BYTES  (HSP_EV_BYTES + HSP_TX_BYTES + HSP_RX_BYTES)
#define HSP_DATA_BYTES  256u
#define HSP_SMMU_MAX_SMR 256u

struct hsp_cfg
{
    ULONG word_len;
    ULONG cpol;
    ULONG cpha;
    ULONG clk_div;
    ULONG clk_src;
    ULONG cs;
};

typedef struct _HSP_CTX
{
    PUCHAR Se;
    PUCHAR Gpi;
    PUCHAR Tlmm;
    PUCHAR Gcc;
    PUCHAR Smmu;

    ULONG  Gpii;
    ULONG  Ee;
    BOOLEAN Ready;

    ULONG  TxWp;
    ULONG  RxWp;
    ULONG  EvRp;
    ULONG  EvWp;

    PHYSICAL_ADDRESS EvPhys;
    PVOID  EvVirt;
    PHYSICAL_ADDRESS TxPhys;
    PVOID  TxVirt;
    PHYSICAL_ADDRESS RxPhys;
    PVOID  RxVirt;
    WDFDMAENABLER DmaEnabler;
    WDFCOMMONBUFFER Buffers[5];
    PVOID TxData;
    PVOID RxData;
    PHYSICAL_ADDRESS TxDataAddress;
    PHYSICAL_ADDRESS RxDataAddress;
    BOOLEAN EventOwned;
    BOOLEAN ChannelOwned[2];
    BOOLEAN Faulted;
    BOOLEAN CommandUncertain;
    BOOLEAN MasksSaved;
    ULONG SavedMasks[6];
    ULONG SmmuSlot[2];
    ULONG SmmuSavedSmr[2];
    ULONG SmmuSavedS2cr[2];
    BOOLEAN SmmuReady;
    BOOLEAN SmmuChanged[2];
} HSP_CTX, *PHSP_CTX;

NTSTATUS
HspInit (
    _Out_ PHSP_CTX Ctx,
    _In_  WDFDEVICE Device,
    _In_  PUCHAR   Se,
    _In_  PUCHAR   Gpi,
    _In_  PUCHAR   Tlmm,
    _In_  PUCHAR   Gcc,
    _In_  PUCHAR   Smmu
    );

NTSTATUS
HspDeinit (
    _In_ PHSP_CTX Ctx
    );

NTSTATUS
HspStartDma (
    _Inout_ PHSP_CTX Ctx
    );

NTSTATUS
HspResetAndConfigurePins (_In_ PUCHAR Tlmm);

NTSTATUS
HspConfigureSmmu (_Inout_ PHSP_CTX Ctx);

NTSTATUS
HspXfer (
    _In_  PHSP_CTX   Ctx,
    _In_  const struct hsp_cfg *Cfg,
    _In_  const VOID *Tx,
    _Out_ VOID       *Rx,
    _In_  ULONG      Len
    );

NTSTATUS
HspClocksOn (
    _In_ PHSP_CTX Ctx
    );

#endif
