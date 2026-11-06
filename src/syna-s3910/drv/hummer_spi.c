/*
 * GENI SPI4 transport for the Synaptics S3910 on hummer (SM8750).
 */
#include "hummer_spi.h"

#define HSP_C_CONFIG            0x00u
#define HSP_C_R_LENGTH          0x04u
#define HSP_C_RING_BASE_LSB     0x08u
#define HSP_C_RING_BASE_MSB     0x0Cu
#define HSP_C_RING_RP_LSB       0x10u
#define HSP_C_RING_RP_MSB       0x14u
#define HSP_C_RING_INT_MOD      0x20u
#define HSP_C_RING_MSI_LSB      0x28u
#define HSP_C_RING_MSI_MSB      0x2Cu
#define HSP_C_RING_RP_UPD_LSB   0x30u
#define HSP_C_RING_RP_UPD_MSB   0x34u

#define HSP_C0_EL_SIZE(sz)      ((ULONG)(sz) << 24)
#define HSP_C0_CHSTATE(v)       (((v) >> 20) & 0xFu)
#define HSP_C0_CHSTATE_MASK     0x00F00000u
#define HSP_C0_DIR_OUT          0x8u
#define HSP_C0_PROTO_GPI        0x2u
#define HSP_C0_SE_PROTO_SPI     0x1u
#define HSP_EV_C0_INTTYPE_IRQ   (1u << 16)
#define HSP_EV_C0_CHTYPE_GPI_EV 0x2u

#define HSP_ST_ERROR            0xFu

#define HSP_CH_ALLOCATE         0u
#define HSP_CH_START            1u
#define HSP_CH_STOP             2u
#define HSP_CH_RESET            9u
#define HSP_CH_DE_ALLOC         10u
#define HSP_EV_CHID             2u
#define HSP_EV_RESET            9u
#define HSP_EV_DE_ALLOC         10u

#define HSP_TRE_TYPE_DMA        0x10u
#define HSP_TRE_TYPE_GO         0x20u
#define HSP_TRE_TYPE_CONFIG0    0x22u
#define HSP_TRE_CHAIN           0x1u
#define HSP_TRE_IEOB            (1u << 8)
#define HSP_TRE_IEOT            (1u << 9)
#define HSP_TRE_LINK            (1u << 11)
#define HSP_TRE_TYPE_SHIFT      16u

#define HSP_SPI_CMD_DUPLEX      3u

#define HSP_EV_XFER_COMPLETE    0x22u
#define HSP_TCE_SUCCESS         1u
#define HSP_TCE_EOT             2u
#define HSP_TCE_EOB             4u
#define HSP_TCE_UNEXP_ERR       16u

#define HSP_EE_OFF              0x10000u

#define HSP_CH_CNTXT(n, k)      (0x20000u + 0x4000u * (n) + 0x80u * (k))
#define HSP_CH_QOS(n, k)        (0x2005Cu + 0x4000u * (n) + 0x80u * (k))
#define HSP_CH_SCRATCH(n, k)    (0x20060u + 0x4000u * (n) + 0x80u * (k))
#define HSP_CH_DB(n, k)         (0x22000u + 0x4000u * (n) + 0x8u * (k))
#define HSP_CH_CMD(n)           (0x23008u + 0x4000u * (n))
#define HSP_EE_STATUS(n)        (0x23000u + 0x4000u * (n))
#define HSP_EE_STATUS_ENABLED   (1u << 0)
#define HSP_EV_CNTXT(n)         (0x21000u + 0x4000u * (n))
#define HSP_EV_DB(n)            (0x22100u + 0x4000u * (n))
#define HSP_EV_CMD(n)           (0x23010u + 0x4000u * (n))
#define HSP_TYPE_IRQ(n)         (0x23080u + 0x4000u * (n))
#define HSP_TYPE_IRQ_MSK(n)     (0x23088u + 0x4000u * (n))
#define HSP_SRC_CH_IRQ(n)       (0x23090u + 0x4000u * (n))
#define HSP_SRC_EV_CH_IRQ(n)    (0x23094u + 0x4000u * (n))
#define HSP_SRC_CH_IRQ_CLR(n)   (0x230A0u + 0x4000u * (n))
#define HSP_SRC_EV_CH_IRQ_CLR(n)(0x230A4u + 0x4000u * (n))
#define HSP_GLOB_IRQ_STTS(n)    (0x23100u + 0x4000u * (n))
#define HSP_GLOB_IRQ_EN(n)      (0x23108u + 0x4000u * (n))
#define HSP_GPII_IRQ_EN(n)      (0x23120u + 0x4000u * (n))
#define HSP_ERRL(n)             (0x23200u + 0x4000u * (n))
#define HSP_SRC_CH_IRQ_MSK(n)   (0x23098u + 0x4000u * (n))
#define HSP_SRC_EV_IRQ_MSK(n)   (0x2309Cu + 0x4000u * (n))
#define HSP_SRC_IEOB_MSK(n)     (0x230B8u + 0x4000u * (n))

#define HSP_SE_M_IRQ_EN         0x614u
#define HSP_SE_M_IRQ_CLEAR      0x618u
#define HSP_SE_S_IRQ_CLEAR      0x648u
#define HSP_SE_FORCE_DEFAULT    0x020u
#define HSP_SE_OUTPUT_CTRL      0x024u
#define HSP_SE_CGC_CTRL         0x028u
#define HSP_SE_M_CFG_0644       0x644u
#define HSP_SE_WRAP_0E30        0xE30u
#define HSP_SE_DMA_TX_IRQ_CLR   0xC44u
#define HSP_SE_DMA_RX_IRQ_CLR   0xD44u
#define HSP_SE_GENI_DMA_MODE_EN 0x258u
#define HSP_SE_GSI_EVENT_EN     0xE18u
#define HSP_SE_IRQ_EN           0xE1Cu
#define HSP_SE_M_CMD_DONE       (1u << 0)
#define HSP_SE_M_TX_FIFO_WM     (1u << 30)
#define HSP_SE_M_RX_FIFO_WM     (1u << 26)
#define HSP_SE_M_RX_FIFO_LAST   (1u << 27)

#define HSP_POLL_TRIES          1000u
#define HSP_XFER_TRIES          150u    // x 1 ms stall = 150 ms ceiling per transfer

#define HSP_TLMM_STRIDE         0x1000u
#define HSP_OE_BIT              9u
#define HSP_GCC_VOTE_ENABLE     0x52008u
#define HSP_GCC_GPLL0_ENABLE    0x52020u
#define HSP_VOTE_CORE_2X        (1u << 18)
#define HSP_VOTE_CORE           (1u << 19)
#define HSP_VOTE_M_AHB          (1u << 20)
#define HSP_VOTE_S_AHB          (1u << 21)
#define HSP_VOTE_S4             (1u << 26)
#define HSP_VOTE_ALL            (HSP_VOTE_CORE_2X | HSP_VOTE_CORE | HSP_VOTE_M_AHB | HSP_VOTE_S_AHB | HSP_VOTE_S4)

#define HSP_HALT_CORE_2X        0x2315Cu
#define HSP_HALT_CORE           0x23148u
#define HSP_HALT_M_AHB          0x23140u
#define HSP_HALT_S_AHB          0x23144u
#define HSP_HALT_S4             0x183CCu
#define HSP_CBCR_CLK_OFF        (1u << 31)

#define HSP_S4_RCG              0x183DCu
#define HSP_RCG_CFG             0x4u
#define HSP_RCG_DFSR_CMD        0x14u
#define HSP_RCG_PERF_CFG(l)     (0x1Cu + 4u * (l))
#define HSP_RCG_PERF_M(l)       (0x5Cu + 4u * (l))
#define HSP_RCG_PERF_N(l)       (0x9Cu + 4u * (l))
#define HSP_RCG_DFS_EN          (1u << 0)
#define HSP_SMMU_IDR0           0x020u
#define HSP_SMMU_SMR(i)         (0x800u + 4u * (i))
#define HSP_SMMU_S2CR(i)        (0xC00u + 4u * (i))
#define HSP_SMMU_SMR_VALID      0x80000000u
#define HSP_SMMU_TYPE_MASK      0x3u
#define HSP_SMMU_ALLOC_LIMIT    128u
#define HSP_GPI_STREAM_ID       0xB6u
#define HSP_QUP_STREAM_ID       0xA3u
#define HSP_UFS_STREAM_ID       0x60u

static
__declspec(noinline)
ULONG
HspRd (
    _In_ PUCHAR Base,
    _In_ ULONG  Offset
    )
{
    return READ_REGISTER_ULONG ((PULONG)(Base + Offset));
}

static
__declspec(noinline)
VOID
HspWr (
    _In_ PUCHAR Base,
    _In_ ULONG  Offset,
    _In_ ULONG  Value
    )
{
    WRITE_REGISTER_ULONG ((PULONG)(Base + Offset), Value);
    KeMemoryBarrier ();
}

static
PUCHAR
HspGpiAt (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Offset
    )
{
    return Ctx->Gpi + (Offset - Ctx->Ee);
}

static
ULONG
HspGpiRd (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Offset
    )
{
    return HspRd (HspGpiAt (Ctx, Offset), 0);
}

static
VOID
HspGpiWr (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Offset,
    _In_ ULONG    Value
    )
{
    HspWr (HspGpiAt (Ctx, Offset), 0, Value);
}

static
ULONG
HspSeRd (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Offset
    )
{
    return HspRd (Ctx->Se, Offset);
}

static
VOID
HspSeWr (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Offset,
    _In_ ULONG    Value
    )
{
    HspWr (Ctx->Se, Offset, Value);
}

static
VOID
HspWaitUs (
    _In_ ULONG Microseconds
    )
{
    //
    // KeStallExecutionProcessor is exact at these sizes; a thread sleep would
    // round every call up to the timer resolution and turn the poll budgets
    // below into minutes. Callers only use this for 1..2000 us slices.
    //
    if (Microseconds > 2000) {
        Microseconds = 2000;
    }
    KeStallExecutionProcessor (Microseconds);
}

static
NTSTATUS
HspCmdWait (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Cntxt,
    _In_ ULONG    Want
    )
{
    ULONG Index;
    ULONG State;

    for (Index = 0; Index < HSP_POLL_TRIES; Index++) {
        ULONG Value = HspGpiRd (Ctx, Cntxt + HSP_C_CONFIG);
        if (Value == MAXULONG) {
            Ctx->CommandUncertain = TRUE;
            return STATUS_IO_DEVICE_ERROR;
        }
        State = HSP_C0_CHSTATE(Value);
        if (State == Want) {
            return STATUS_SUCCESS;
        }
        if (State == HSP_ST_ERROR) {
            return STATUS_IO_DEVICE_ERROR;
        }
        HspWaitUs (100);
    }
    Ctx->CommandUncertain = TRUE;
    return STATUS_IO_TIMEOUT;
}

static
VOID
HspChanCmd (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Channel,
    _In_ ULONG    Opcode
    )
{
    HspGpiWr (Ctx, HSP_CH_CMD (Ctx->Gpii), (Opcode << 24) | (Channel & 0xFFu));
}

static
VOID
HspProgramChan (
    _In_ PHSP_CTX Ctx,
    _In_ ULONG    Channel,
    _In_ BOOLEAN  DirOut,
    _In_ ULONGLONG RingPhys,
    _In_ ULONG    RingLen
    )
{
    ULONG Base = HSP_CH_CNTXT (Ctx->Gpii, Channel);
    ULONG Keep = HspGpiRd (Ctx, Base + HSP_C_CONFIG) & HSP_C0_CHSTATE_MASK;

    HspGpiWr (Ctx, Base + HSP_C_CONFIG,
              Keep | HSP_C0_EL_SIZE (HSP_TRE_BYTES) | (DirOut ? HSP_C0_DIR_OUT : 0u) | HSP_C0_PROTO_GPI);
    HspGpiWr (Ctx, Base + HSP_C_R_LENGTH, RingLen);
    HspGpiWr (Ctx, Base + HSP_C_RING_BASE_LSB, (ULONG)RingPhys);
    HspGpiWr (Ctx, Base + HSP_C_RING_BASE_MSB, (ULONG)(RingPhys >> 32));
    HspGpiWr (Ctx, HSP_CH_DB (Ctx->Gpii, Channel) + (HSP_C_RING_RP_MSB - HSP_C_RING_RP_LSB), (ULONG)(RingPhys >> 32));
    HspGpiWr (Ctx, HSP_CH_SCRATCH (Ctx->Gpii, Channel),
              ((ULONG)(Channel ? 0u : 1u) << 16) | (HSP_C0_SE_PROTO_SPI << 4) | 4u);
    HspGpiWr (Ctx, HSP_CH_SCRATCH (Ctx->Gpii, Channel) + 0x4u, 0);
    HspGpiWr (Ctx, HSP_CH_SCRATCH (Ctx->Gpii, Channel) + 0x8u, 0);
    HspGpiWr (Ctx, HSP_CH_SCRATCH (Ctx->Gpii, Channel) + 0xCu, 0);
    HspGpiWr (Ctx, HSP_CH_QOS (Ctx->Gpii, Channel), 1);
}

static
VOID
HspProgramEv (
    _In_ PHSP_CTX Ctx
    )
{
    ULONG Base = HSP_EV_CNTXT (Ctx->Gpii);

    HspGpiWr (Ctx, Base + HSP_C_CONFIG,
              HSP_C0_EL_SIZE (HSP_TRE_BYTES) | HSP_EV_C0_INTTYPE_IRQ | HSP_EV_C0_CHTYPE_GPI_EV);
    HspGpiWr (Ctx, Base + HSP_C_R_LENGTH, HSP_EV_BYTES);
    HspGpiWr (Ctx, Base + HSP_C_RING_BASE_LSB, (ULONG)Ctx->EvPhys.QuadPart);
    HspGpiWr (Ctx, Base + HSP_C_RING_BASE_MSB, (ULONG)((ULONGLONG)Ctx->EvPhys.QuadPart >> 32));
    HspGpiWr (Ctx, HSP_EV_DB (Ctx->Gpii) + (HSP_C_RING_RP_MSB - HSP_C_RING_RP_LSB), (ULONG)((ULONGLONG)Ctx->EvPhys.QuadPart >> 32));
    HspGpiWr (Ctx, Base + HSP_C_RING_INT_MOD, 0);
    HspGpiWr (Ctx, Base + HSP_C_RING_MSI_LSB, 0);
    HspGpiWr (Ctx, Base + HSP_C_RING_MSI_MSB, 0);
    HspGpiWr (Ctx, Base + HSP_C_RING_RP_UPD_LSB, 0);
    HspGpiWr (Ctx, Base + HSP_C_RING_RP_UPD_MSB, 0);
}

static
VOID
HspSeGpiMode (
    _In_ PHSP_CTX Ctx
    )
{
    ULONG Value;

    HspSeWr (Ctx, HSP_SE_GSI_EVENT_EN, 0);
    HspSeWr (Ctx, HSP_SE_M_IRQ_CLEAR, 0xFFFFFFFFu);
    HspSeWr (Ctx, HSP_SE_S_IRQ_CLEAR, 0xFFFFFFFFu);
    HspSeWr (Ctx, HSP_SE_DMA_TX_IRQ_CLR, 0xFFFFFFFFu);
    HspSeWr (Ctx, HSP_SE_DMA_RX_IRQ_CLR, 0xFFFFFFFFu);
    HspSeWr (Ctx, HSP_SE_CGC_CTRL, HspSeRd (Ctx, HSP_SE_CGC_CTRL) | 0x7Fu);
    HspSeWr (Ctx, HSP_SE_WRAP_0E30, HspSeRd (Ctx, HSP_SE_WRAP_0E30) | 0xFu);
    HspSeWr (Ctx, HSP_SE_OUTPUT_CTRL, 0x7Fu);
    HspSeWr (Ctx, HSP_SE_FORCE_DEFAULT, 1u);
    HspSeWr (Ctx, HSP_SE_IRQ_EN, 0);
    Value = HspSeRd (Ctx, HSP_SE_M_IRQ_EN);
    Value &= ~(HSP_SE_M_CMD_DONE | HSP_SE_M_TX_FIFO_WM | HSP_SE_M_RX_FIFO_WM | HSP_SE_M_RX_FIFO_LAST);
    HspSeWr (Ctx, HSP_SE_M_IRQ_EN, Value);
    HspSeWr (Ctx, HSP_SE_M_CFG_0644, HspSeRd (Ctx, HSP_SE_M_CFG_0644) & ~1u);
    HspSeWr (Ctx, HSP_SE_GENI_DMA_MODE_EN, HspSeRd (Ctx, HSP_SE_GENI_DMA_MODE_EN) | 1u);
    HspSeWr (Ctx, HSP_SE_GSI_EVENT_EN, HspSeRd (Ctx, HSP_SE_GSI_EVENT_EN) | 0xFu);

}

NTSTATUS
HspClocksOn (
    _In_ PHSP_CTX Ctx
    )
{
    ULONG Index;
    ULONG Value;

    if (Ctx->Gcc == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Value = HspRd (Ctx->Gcc, HSP_GCC_GPLL0_ENABLE);
    HspWr (Ctx->Gcc, HSP_GCC_GPLL0_ENABLE, Value | 1u);

    Value = HspRd (Ctx->Gcc, HSP_GCC_VOTE_ENABLE);
    HspWr (Ctx->Gcc, HSP_GCC_VOTE_ENABLE, Value | HSP_VOTE_ALL);

    for (Index = 0; Index < 200; Index++) {
        Value = HspRd (Ctx->Gcc, HSP_HALT_CORE_2X);
        if ((Value & HSP_CBCR_CLK_OFF) == 0) {
            break;
        }
        HspWaitUs (100);
    }
    if (Index == 200) {
        return STATUS_IO_TIMEOUT;
    }
    for (Index = 0; Index < 200; Index++) {
        Value = HspRd (Ctx->Gcc, HSP_HALT_S4);
        if ((Value & HSP_CBCR_CLK_OFF) == 0) {
            break;
        }
        HspWaitUs (100);
    }
    return Index < 200 ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

NTSTATUS
HspResetAndConfigurePins (_In_ PUCHAR Tlmm)
{
    ULONG Pin;
    ULONG Config;
    ULONG ResetLow;
    ULONG ResetHigh;
    LARGE_INTEGER Delay;

    if (Tlmm == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    for (Pin = 48; Pin <= 51; Pin++) {
        HspWr(Tlmm, Pin * HSP_TLMM_STRIDE, 0x84u);
        Config = HspRd(Tlmm, Pin * HSP_TLMM_STRIDE);
        if ((Config & 0x3FFu) != 0x84u) {
            return STATUS_IO_DEVICE_ERROR;
        }
    }
    HspWr(Tlmm, 162 * HSP_TLMM_STRIDE, 0);
    HspWr(Tlmm, 161 * HSP_TLMM_STRIDE + 4, 2);
    HspWr(Tlmm, 161 * HSP_TLMM_STRIDE, 1u << HSP_OE_BIT);
    Config = HspRd(Tlmm, 161 * HSP_TLMM_STRIDE);
    if ((Config & 0x3FFu) != 0x200u || (HspRd(Tlmm, 162 * HSP_TLMM_STRIDE) & 0x3FFu) != 0) {
        return STATUS_IO_DEVICE_ERROR;
    }
    if ((HspRd(Tlmm, 161 * HSP_TLMM_STRIDE) & 0x3FFu) != 0x200u) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    HspWr(Tlmm, 161 * HSP_TLMM_STRIDE + 4, 0);
    Delay.QuadPart = -100000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    ResetLow = HspRd(Tlmm, 161 * HSP_TLMM_STRIDE + 4);
    HspWr(Tlmm, 161 * HSP_TLMM_STRIDE + 4, 2);
    ResetHigh = HspRd(Tlmm, 161 * HSP_TLMM_STRIDE + 4);
    Delay.QuadPart = -2000000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    return (ResetLow & 2u) == 0 && (ResetHigh & 2u) != 0 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR;
}

static
BOOLEAN
HspSmmuMatch(ULONG Smr, ULONG StreamId)
{
    return (Smr & HSP_SMMU_SMR_VALID) != 0 &&
        (((Smr ^ StreamId) & ~(Smr >> 16) & 0x7FFFu) == 0);
}

NTSTATUS
HspConfigureSmmu (_Inout_ PHSP_CTX Ctx)
{
    ULONG Count, Slot, Index, Stream[2] = { HSP_GPI_STREAM_ID, HSP_QUP_STREAM_ID };
    ULONG FoundUfs = MAXULONG, FreeSlot[2] = { MAXULONG, MAXULONG };
    ULONG FreeCount = 0, Needed = 0;
    ULONG UfsS2cr = 0;
    ULONG ExistingSlot[2] = { MAXULONG, MAXULONG };
    ULONG ExistingS2cr[2] = { 0, 0 };
    ULONG Id;

    if (Ctx == NULL || Ctx->Smmu == NULL) return STATUS_INVALID_PARAMETER;
    Count = HspRd(Ctx->Smmu, HSP_SMMU_IDR0) & 0xFFu;
    Id = HspRd(Ctx->Smmu, HSP_SMMU_IDR0);
    if (Id == MAXULONG || Count == 0 || Count > HSP_SMMU_MAX_SMR) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (Slot = 0; Slot < Count; Slot++) {
        ULONG Smr = HspRd(Ctx->Smmu, HSP_SMMU_SMR(Slot));
        if (Smr == MAXULONG) return STATUS_IO_DEVICE_ERROR;
        if (HspSmmuMatch(Smr, HSP_UFS_STREAM_ID)) {
            FoundUfs = Slot;
            UfsS2cr = HspRd(Ctx->Smmu, HSP_SMMU_S2CR(Slot));
            if (UfsS2cr == MAXULONG || (UfsS2cr & HSP_SMMU_TYPE_MASK) != 0) return STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        if ((Smr & HSP_SMMU_SMR_VALID) == 0 && Slot < HSP_SMMU_ALLOC_LIMIT) {
            if (FreeCount < RTL_NUMBER_OF(FreeSlot)) FreeSlot[FreeCount++] = Slot;
        }
        for (Index = 0; Index < 2; Index++) {
            if (HspSmmuMatch(Smr, Stream[Index])) {
                ExistingSlot[Index] = Slot;
                ExistingS2cr[Index] = HspRd(Ctx->Smmu, HSP_SMMU_S2CR(Slot));
                if (ExistingS2cr[Index] == MAXULONG || (ExistingS2cr[Index] & HSP_SMMU_TYPE_MASK) != 0) return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
        }
    }
    Ctx->SmmuSlot[0] = ExistingSlot[0];
    Ctx->SmmuSlot[1] = ExistingSlot[1];
    if (FoundUfs == MAXULONG) return STATUS_NOT_FOUND;
    for (Index = 0; Index < 2; Index++) {
        if (ExistingSlot[Index] != MAXULONG && ExistingS2cr[Index] != UfsS2cr) return STATUS_DEVICE_CONFIGURATION_ERROR;
        if (ExistingSlot[Index] == MAXULONG) {
            if (Needed >= FreeCount) return STATUS_INSUFFICIENT_RESOURCES;
            Ctx->SmmuSlot[Index] = FreeSlot[Needed++];
        }
    }
    if (Ctx->SmmuReady) return STATUS_INVALID_DEVICE_STATE;

    for (Index = 0; Index < 2; Index++) {
        if (ExistingSlot[Index] != MAXULONG) continue;
        Slot = Ctx->SmmuSlot[Index];
        Ctx->SmmuSavedSmr[Index] = HspRd(Ctx->Smmu, HSP_SMMU_SMR(Slot));
        Ctx->SmmuSavedS2cr[Index] = HspRd(Ctx->Smmu, HSP_SMMU_S2CR(Slot));
        if ((Ctx->SmmuSavedSmr[Index] & HSP_SMMU_SMR_VALID) != 0 || Ctx->SmmuSavedS2cr[Index] == MAXULONG) goto rollback;
        Ctx->SmmuSlot[Index] = Slot;
        Ctx->SmmuChanged[Index] = TRUE;
        HspWr(Ctx->Smmu, HSP_SMMU_S2CR(Slot), UfsS2cr);
        if (HspRd(Ctx->Smmu, HSP_SMMU_S2CR(Slot)) != UfsS2cr) goto rollback;
        HspWr(Ctx->Smmu, HSP_SMMU_SMR(Slot), HSP_SMMU_SMR_VALID | Stream[Index]);
        if (HspRd(Ctx->Smmu, HSP_SMMU_SMR(Slot)) != (HSP_SMMU_SMR_VALID | Stream[Index])) goto rollback;
    }
    Ctx->SmmuReady = TRUE;
    return STATUS_SUCCESS;

rollback:
    for (Index = 2; Index > 0; Index--) {
        ULONG Item = Index - 1;
        if (!Ctx->SmmuChanged[Item]) continue;
        Slot = Ctx->SmmuSlot[Item];
        HspWr(Ctx->Smmu, HSP_SMMU_SMR(Slot), Ctx->SmmuSavedSmr[Item]);
        HspWr(Ctx->Smmu, HSP_SMMU_S2CR(Slot), Ctx->SmmuSavedS2cr[Item]);
        if (HspRd(Ctx->Smmu, HSP_SMMU_SMR(Slot)) != Ctx->SmmuSavedSmr[Item] ||
            HspRd(Ctx->Smmu, HSP_SMMU_S2CR(Slot)) != Ctx->SmmuSavedS2cr[Item]) {
            Ctx->Faulted = TRUE;
            return STATUS_IO_DEVICE_ERROR;
        }
        Ctx->SmmuChanged[Item] = FALSE;
    }
    return STATUS_IO_DEVICE_ERROR;
}

static
VOID
HspWriteTre (
    _In_ PUCHAR Tre,
    _In_ ULONG  Type,
    _In_ ULONG  Flags,
    _In_ ULONG  Length,
    _In_ ULONG  Address,
    _In_ ULONG  Aux
    )
{
    PULONG Words = (PULONG)Tre;

    Words[0] = Address;
    Words[1] = Aux;
    Words[2] = Length;
    Words[3] = (Type << HSP_TRE_TYPE_SHIFT) | Flags;
}

static
NTSTATUS
HspCommand(PHSP_CTX Ctx, BOOLEAN Event, ULONG Channel, ULONG Opcode, ULONG Want)
{
    ULONG Index;
    ULONG Source = Event ? HSP_SRC_EV_CH_IRQ(Ctx->Gpii) : HSP_SRC_CH_IRQ(Ctx->Gpii);
    ULONG Bit = Event ? 1u : 1u << Channel;
    HspGpiWr(Ctx, Event ? HSP_SRC_EV_CH_IRQ_CLR(Ctx->Gpii) : HSP_SRC_CH_IRQ_CLR(Ctx->Gpii), Bit);
    if (Event) {
        HspGpiWr(Ctx, HSP_EV_CMD(Ctx->Gpii), (Opcode << 24) | HSP_EV_CHID);
    } else {
        HspChanCmd(Ctx, Channel, Opcode);
    }
    if (Opcode == HSP_CH_DE_ALLOC) {
        for (Index = 0; Index < HSP_POLL_TRIES; Index++) {
            ULONG Value = HspGpiRd(Ctx, Source);
            if (Value != MAXULONG && (Value & Bit) != 0) return STATUS_SUCCESS;
            HspWaitUs(100);
        }
        Ctx->CommandUncertain = TRUE;
        return STATUS_IO_TIMEOUT;
    }
    return HspCmdWait(Ctx, Event ? HSP_EV_CNTXT(Ctx->Gpii) : HSP_CH_CNTXT(Ctx->Gpii, Channel), Want);
}

static
VOID
HspMasks(PHSP_CTX Ctx, BOOLEAN Restore)
{
    ULONG Offsets[6] = { HSP_TYPE_IRQ_MSK(Ctx->Gpii), HSP_SRC_CH_IRQ_MSK(Ctx->Gpii),
        HSP_SRC_EV_IRQ_MSK(Ctx->Gpii), HSP_SRC_IEOB_MSK(Ctx->Gpii),
        HSP_GLOB_IRQ_EN(Ctx->Gpii), HSP_GPII_IRQ_EN(Ctx->Gpii) };
    ULONG Index;
    ULONG Values[6] = { 0, 3, 1, 1, 0, 0 };

    if (!Restore) {
        for (Index = 0; Index < 6; Index++) {
            Ctx->SavedMasks[Index] = HspGpiRd(Ctx, Offsets[Index]);
            if (Ctx->SavedMasks[Index] == MAXULONG) {
                Ctx->Faulted = TRUE;
                return;
            }
        }
        Ctx->MasksSaved = TRUE;
    }

    for (Index = 0; Index < 6; Index++) {
        HspGpiWr(Ctx, Offsets[Index], Restore ? Ctx->SavedMasks[Index] : Values[Index]);
        if (HspGpiRd(Ctx, Offsets[Index]) != (Restore ? Ctx->SavedMasks[Index] : Values[Index])) {
            Ctx->Faulted = TRUE;
            return;
        }
    }
    if (Restore) Ctx->MasksSaved = FALSE;
}

static
NTSTATUS
HspStopDma(PHSP_CTX Ctx)
{
    ULONG Channel;
    ULONG State;
    NTSTATUS Status;
    ULONG Index;

    Ctx->Ready = FALSE;
    if (Ctx->CommandUncertain) {
        return STATUS_DEVICE_BUSY;
    }
    for (Channel = 0; Channel < 2; Channel++) {
        if (!Ctx->ChannelOwned[Channel]) continue;
        State = HSP_C0_CHSTATE(HspGpiRd(Ctx, HSP_CH_CNTXT(Ctx->Gpii, Channel)));
        if (State == 2 || State == 4) {
            Status = HspCommand(Ctx, FALSE, Channel, HSP_CH_STOP, 3);
            if (!NT_SUCCESS(Status)) return Status;
        }
        Status = HspCommand(Ctx, FALSE, Channel, HSP_CH_RESET, 1);
        if (!NT_SUCCESS(Status)) return Status;
        Status = HspCommand(Ctx, FALSE, Channel, HSP_CH_DE_ALLOC, 0);
        if (!NT_SUCCESS(Status)) return Status;
        Ctx->ChannelOwned[Channel] = FALSE;
    }
    if (Ctx->EventOwned) {
        Status = HspCommand(Ctx, TRUE, 0, HSP_EV_RESET, 1);
        if (!NT_SUCCESS(Status)) return Status;
        Status = HspCommand(Ctx, TRUE, 0, HSP_EV_DE_ALLOC, 0);
        if (!NT_SUCCESS(Status)) return Status;
        Ctx->EventOwned = FALSE;
    }
    if (Ctx->MasksSaved) {
        HspGpiWr(Ctx, HSP_SRC_CH_IRQ_CLR(Ctx->Gpii), 3);
        HspGpiWr(Ctx, HSP_SRC_EV_CH_IRQ_CLR(Ctx->Gpii), 1);
        if (Ctx->MasksSaved) HspMasks(Ctx, TRUE);
    }
    for (Index = 0; Index < 2; Index++) {
        Ctx->SmmuChanged[Index] = FALSE;
    }
    Ctx->SmmuReady = FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS
HspInit (
    _Out_ PHSP_CTX Ctx,
    _In_ WDFDEVICE Device,
    _In_ PUCHAR Se,
    _In_ PUCHAR Gpi,
    _In_ PUCHAR Tlmm,
    _In_ PUCHAR Gcc,
    _In_ PUCHAR Smmu
    )
{
    WDF_DMA_ENABLER_CONFIG DmaConfig;
    WDF_COMMON_BUFFER_CONFIG BufferConfig;
    NTSTATUS Status;
    ULONG Index;
    size_t Sizes[5] = { HSP_EV_BYTES, HSP_TX_BYTES, HSP_RX_BYTES, HSP_DATA_BYTES, HSP_DATA_BYTES };
    PVOID Addresses[5];
    PHYSICAL_ADDRESS Logical[5];

    if (Ctx == NULL || Device == NULL || Se == NULL || Gpi == NULL || Tlmm == NULL || Gcc == NULL || Smmu == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Ctx, sizeof(*Ctx));
    Ctx->Se = Se; Ctx->Gpi = Gpi; Ctx->Tlmm = Tlmm; Ctx->Gcc = Gcc;
    Ctx->Smmu = Smmu; Ctx->Ee = HSP_EE_OFF;
    WDF_DMA_ENABLER_CONFIG_INIT(&DmaConfig, WdfDmaProfileScatterGather64Duplex, HSP_DATA_BYTES);
    DmaConfig.AddressWidthOverride = 32;
    Status = WdfDmaEnablerCreate(Device, &DmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &Ctx->DmaEnabler);
    if (!NT_SUCCESS(Status)) return Status;
    for (Index = 0; Index < 5; Index++) {
        WDF_COMMON_BUFFER_CONFIG_INIT(&BufferConfig, (ULONG)Sizes[Index] - 1);
        Status = WdfCommonBufferCreateWithConfig(Ctx->DmaEnabler, Sizes[Index], &BufferConfig,
            WDF_NO_OBJECT_ATTRIBUTES, &Ctx->Buffers[Index]);
        if (!NT_SUCCESS(Status)) goto fail;
        Addresses[Index] = WdfCommonBufferGetAlignedVirtualAddress(Ctx->Buffers[Index]);
        Logical[Index] = WdfCommonBufferGetAlignedLogicalAddress(Ctx->Buffers[Index]);
        if (Addresses[Index] == NULL || Logical[Index].QuadPart == 0 || Logical[Index].HighPart != 0 ||
            ((ULONGLONG)Logical[Index].QuadPart & (Sizes[Index] - 1)) != 0) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
            goto fail;
        }
        RtlZeroMemory(Addresses[Index], Sizes[Index]);
    }
    Ctx->EvVirt = Addresses[0]; Ctx->EvPhys = Logical[0];
    Ctx->TxVirt = Addresses[1]; Ctx->TxPhys = Logical[1];
    Ctx->RxVirt = Addresses[2]; Ctx->RxPhys = Logical[2];
    Ctx->TxData = Addresses[3]; Ctx->TxDataAddress = Logical[3];
    Ctx->RxData = Addresses[4]; Ctx->RxDataAddress = Logical[4];
    KeMemoryBarrier();
    return STATUS_SUCCESS;
fail:
    for (Index = 0; Index < 5; Index++) {
        if (Ctx->Buffers[Index] != NULL) {
            WdfObjectDelete(Ctx->Buffers[Index]);
            Ctx->Buffers[Index] = NULL;
        }
    }
    WdfObjectDelete(Ctx->DmaEnabler);
    Ctx->DmaEnabler = NULL;
    return Status;
}

NTSTATUS
HspStartDma (_Inout_ PHSP_CTX Ctx)
{
    ULONG Instance, Channel, Value, ErrorBefore;
    NTSTATUS Status;

    if (Ctx == NULL || Ctx->DmaEnabler == NULL || !Ctx->SmmuReady || Ctx->Ready || Ctx->MasksSaved) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    Status = HspClocksOn(Ctx);
    if (!NT_SUCCESS(Status)) return Status;
    HspSeGpiMode(Ctx);
    if (HspSeRd(Ctx, HSP_SE_OUTPUT_CTRL) != 0x7Fu ||
        (HspSeRd(Ctx, HSP_SE_CGC_CTRL) & 0x7Fu) != 0x7Fu ||
        (HspSeRd(Ctx, HSP_SE_GENI_DMA_MODE_EN) & 1u) == 0 ||
        (HspSeRd(Ctx, HSP_SE_GSI_EVENT_EN) & 0xFu) != 0xFu) return STATUS_IO_DEVICE_ERROR;
    for (Instance = 1; Instance <= 4; Instance++) {
        Value = HspGpiRd(Ctx, HSP_EE_STATUS(Instance));
        if (Value == MAXULONG || (Value & 1) == 0) continue;
        if (HSP_C0_CHSTATE(HspGpiRd(Ctx, HSP_CH_CNTXT(Instance, 0))) != 0 ||
            HSP_C0_CHSTATE(HspGpiRd(Ctx, HSP_CH_CNTXT(Instance, 1))) != 0 ||
            HSP_C0_CHSTATE(HspGpiRd(Ctx, HSP_EV_CNTXT(Instance))) != 0) continue;
        break;
    }
    if (Instance > 4) return STATUS_DEVICE_BUSY;
    Ctx->Gpii = Instance;
    HspMasks(Ctx, FALSE);
    if (Ctx->Faulted || !Ctx->MasksSaved) {
        if (Ctx->MasksSaved) {
            HspMasks(Ctx, TRUE);
        }
        return STATUS_IO_DEVICE_ERROR;
    }
    ErrorBefore = HspGpiRd(Ctx, HSP_ERRL(Ctx->Gpii));
    Status = HspCommand(Ctx, TRUE, 0, 0, 1);
    if (!NT_SUCCESS(Status)) goto fail;
    Ctx->EventOwned = TRUE;
    HspProgramEv(Ctx);
    Ctx->EvRp = 0;
    Ctx->EvWp = HSP_EV_ELEMS - 1;
    HspGpiWr(Ctx, HSP_EV_DB(Ctx->Gpii), (ULONG)(Ctx->EvPhys.QuadPart + Ctx->EvWp * HSP_TRE_BYTES));
    for (Channel = 0; Channel < 2; Channel++) {
        Status = HspCommand(Ctx, FALSE, Channel, HSP_CH_ALLOCATE, 1);
        if (!NT_SUCCESS(Status)) goto fail;
        Ctx->ChannelOwned[Channel] = TRUE;
        HspProgramChan(Ctx, Channel, Channel == 0,
            (ULONGLONG)(Channel == 0 ? Ctx->TxPhys.QuadPart : Ctx->RxPhys.QuadPart),
            Channel == 0 ? HSP_TX_BYTES : HSP_RX_BYTES);
    }
    for (Channel = 0; Channel < 2; Channel++) {
        Status = HspCommand(Ctx, FALSE, Channel, HSP_CH_START, 2);
        if (!NT_SUCCESS(Status)) goto fail;
    }
    Value = HspGpiRd(Ctx, HSP_ERRL(Ctx->Gpii));
    if (ErrorBefore == MAXULONG || Value != ErrorBefore) {
        Status = STATUS_IO_DEVICE_ERROR;
        goto fail;
    }
    Ctx->TxWp = 0; Ctx->RxWp = 0;
    Ctx->Ready = TRUE;
    return STATUS_SUCCESS;
fail:
    Ctx->Faulted = TRUE;
    (VOID)HspStopDma(Ctx);
    return Status;
}

NTSTATUS
HspDeinit (_In_ PHSP_CTX Ctx)
{
    ULONG Index;
    NTSTATUS Status;

    if (Ctx == NULL) return STATUS_INVALID_PARAMETER;
    if (Ctx->MasksSaved || Ctx->EventOwned || Ctx->ChannelOwned[0] || Ctx->ChannelOwned[1] ||
        Ctx->SmmuChanged[0] || Ctx->SmmuChanged[1]) {
        Status = HspStopDma(Ctx);
        if (!NT_SUCCESS(Status)) return Status;
    }
    for (Index = 0; Index < 5; Index++) {
        if (Ctx->Buffers[Index] != NULL) {
            WdfObjectDelete(Ctx->Buffers[Index]);
            Ctx->Buffers[Index] = NULL;
        }
    }
    if (Ctx->DmaEnabler != NULL) {
        WdfObjectDelete(Ctx->DmaEnabler);
        Ctx->DmaEnabler = NULL;
    }
    Ctx->Ready = FALSE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
HspTakeEvent(PHSP_CTX Ctx, ULONG ExpectedTre, ULONG ExpectedRxTre, ULONG ExpectedLength)
{
    ULONG Index, HwPointer, Delta, HwIndex, Count, Code, Type, Channel;
    ULONG TxLength = 0, RxLength = 0, ErrorBefore;
    ULONGLONG Pointer;
    PULONG Tre;
    BOOLEAN TxDone = FALSE, RxDone = FALSE;

    ErrorBefore = HspGpiRd(Ctx, HSP_ERRL(Ctx->Gpii));
    if (ErrorBefore == MAXULONG) return STATUS_IO_DEVICE_ERROR;
    for (Index = 0; Index < HSP_XFER_TRIES; Index++) {
        HwPointer = HspGpiRd(Ctx, HSP_EV_CNTXT(Ctx->Gpii) + HSP_C_RING_RP_LSB);
        Delta = HwPointer - (ULONG)Ctx->EvPhys.QuadPart;
        if (Delta >= HSP_EV_BYTES || Delta % HSP_TRE_BYTES != 0) {
            return STATUS_DEVICE_DATA_ERROR;
        }
        HwIndex = Delta / HSP_TRE_BYTES;
        Count = 0;
        while (Ctx->EvRp != HwIndex && Count++ < HSP_EV_ELEMS) {
            KeMemoryBarrier();
            Tre = (PULONG)((PUCHAR)Ctx->EvVirt + Ctx->EvRp * HSP_TRE_BYTES);
            Type = (Tre[3] >> 16) & 0xFFu;
            Channel = Tre[3] >> 24;
            Code = Tre[2] >> 24;
            Ctx->EvRp = (Ctx->EvRp + 1) % HSP_EV_ELEMS;
            Ctx->EvWp = (Ctx->EvWp + 1) % HSP_EV_ELEMS;
            HspGpiWr(Ctx, HSP_EV_DB(Ctx->Gpii), (ULONG)(Ctx->EvPhys.QuadPart + Ctx->EvWp * HSP_TRE_BYTES));
        if (Type != HSP_EV_XFER_COMPLETE || Channel > 1) return STATUS_DEVICE_PROTOCOL_ERROR;
            Pointer = ((ULONGLONG)Tre[1] << 32) | Tre[0];
            if (Code != HSP_TCE_SUCCESS && Code != HSP_TCE_EOT && Code != HSP_TCE_EOB) return STATUS_IO_DEVICE_ERROR;
            if (Channel == 0 && Pointer == (ULONGLONG)Ctx->TxPhys.QuadPart + ExpectedTre * HSP_TRE_BYTES) {
                TxLength = Tre[2] & 0xFFFFFFu;
                TxDone = TRUE;
            } else if (Channel == 1 && Pointer == (ULONGLONG)Ctx->RxPhys.QuadPart + ExpectedRxTre * HSP_TRE_BYTES) {
                RxLength = Tre[2] & 0xFFFFFFu;
                RxDone = TRUE;
            } else return STATUS_DEVICE_PROTOCOL_ERROR;
        }
        if (TxDone && RxDone && HspGpiRd(Ctx, HSP_CH_CNTXT(Ctx->Gpii, 1) + HSP_C_RING_RP_LSB) ==
            (ULONG)(Ctx->RxPhys.QuadPart + Ctx->RxWp * HSP_TRE_BYTES)) {
            return TxLength == ExpectedLength && RxLength == ExpectedLength ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR;
        }
        if (HspGpiRd(Ctx, HSP_ERRL(Ctx->Gpii)) != ErrorBefore) return STATUS_IO_DEVICE_ERROR;
        HspWaitUs(1000);
    }
    return STATUS_IO_TIMEOUT;
}

NTSTATUS
HspXfer (
    _In_ PHSP_CTX Ctx,
    _In_ const struct hsp_cfg *Cfg,
    _In_ const VOID *Tx,
    _Out_ VOID *Rx,
    _In_ ULONG Len
    )
{
    ULONG Tx0, Tx1, Tx2, Rx0;
    NTSTATUS Status;

    if (Ctx == NULL || Cfg == NULL || Tx == NULL || Rx == NULL || Len == 0 || Len > HSP_DATA_BYTES) return STATUS_INVALID_PARAMETER;
    if (!Ctx->Ready || Ctx->Faulted) return STATUS_INVALID_DEVICE_STATE;
    RtlCopyMemory(Ctx->TxData, Tx, Len);
    RtlZeroMemory(Ctx->RxData, Len);
    Tx0 = Ctx->TxWp; Tx1 = (Tx0 + 1) % HSP_CH_ELEMS; Tx2 = (Tx0 + 2) % HSP_CH_ELEMS;
    Rx0 = Ctx->RxWp;
    HspWriteTre((PUCHAR)Ctx->TxVirt + Tx0 * HSP_TRE_BYTES, HSP_TRE_TYPE_CONFIG0, HSP_TRE_CHAIN,
        (Cfg->clk_div & 0xFFFu) | ((Cfg->clk_src & 0xFu) << 16),
        (Cfg->word_len & 0x1Fu) | ((Cfg->cpha & 1u) << 12) | ((Cfg->cpol & 1u) << 13) | (3u << 24), 0);
    HspWriteTre((PUCHAR)Ctx->TxVirt + Tx1 * HSP_TRE_BYTES, HSP_TRE_TYPE_GO, HSP_TRE_CHAIN | HSP_TRE_LINK,
        0, ((Cfg->cs & 7u) << 8) | HSP_SPI_CMD_DUPLEX, 0);
    HspWriteTre((PUCHAR)Ctx->TxVirt + Tx2 * HSP_TRE_BYTES, HSP_TRE_TYPE_DMA, HSP_TRE_IEOT,
        Len, (ULONG)Ctx->TxDataAddress.QuadPart, (ULONG)((ULONGLONG)Ctx->TxDataAddress.QuadPart >> 32));
    HspWriteTre((PUCHAR)Ctx->RxVirt + Rx0 * HSP_TRE_BYTES, HSP_TRE_TYPE_DMA, 0,
        Len, (ULONG)Ctx->RxDataAddress.QuadPart, (ULONG)((ULONGLONG)Ctx->RxDataAddress.QuadPart >> 32));
    Ctx->TxWp = (Tx0 + 3) % HSP_CH_ELEMS;
    Ctx->RxWp = (Rx0 + 1) % HSP_CH_ELEMS;
    KeMemoryBarrier();
    HspGpiWr(Ctx, HSP_CH_DB(Ctx->Gpii, 1), (ULONG)(Ctx->RxPhys.QuadPart + Ctx->RxWp * HSP_TRE_BYTES));
    HspGpiWr(Ctx, HSP_CH_DB(Ctx->Gpii, 0), (ULONG)(Ctx->TxPhys.QuadPart + Ctx->TxWp * HSP_TRE_BYTES));
    Status = HspTakeEvent(Ctx, Tx2, Rx0, Len);
    if (!NT_SUCCESS(Status)) {
        (VOID)HspStopDma(Ctx);
        return Status;
    }
    KeMemoryBarrier();
    RtlCopyMemory(Rx, Ctx->RxData, Len);
    return STATUS_SUCCESS;
}

