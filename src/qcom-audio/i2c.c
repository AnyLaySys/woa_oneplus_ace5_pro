#include "sia9177.h"

#define GENI_FORCE_DEFAULT_REG      0x020u
#define GENI_OUTPUT_CTRL            0x024u
#define SE_GENI_CGC_CTRL            0x028u
#define SE_GENI_STATUS              0x040u
#define GENI_SER_M_CLK_CFG          0x048u
#define GENI_IF_DISABLE_RO          0x064u
#define GENI_FW_REVISION_RO         0x068u
#define SE_GENI_CLK_SEL             0x07Cu
#define SE_GENI_BYTE_GRAN           0x254u
#define SE_GENI_DMA_MODE_EN         0x258u
#define SE_GENI_TX_PACKING_CFG0     0x260u
#define SE_GENI_TX_PACKING_CFG1     0x264u
#define SE_I2C_TX_TRANS_LEN         0x26Cu
#define SE_I2C_RX_TRANS_LEN         0x270u
#define SE_I2C_SCL_COUNTERS         0x278u
#define SE_GENI_RX_PACKING_CFG0     0x284u
#define SE_GENI_RX_PACKING_CFG1     0x288u
#define SE_GENI_M_CMD0              0x600u
#define SE_GENI_M_CMD_CTRL_REG      0x604u
#define SE_GENI_M_IRQ_STATUS        0x610u
#define SE_GENI_M_IRQ_CLEAR         0x618u
#define SE_GENI_S_IRQ_CLEAR         0x648u
#define SE_GENI_TX_FIFOn            0x700u
#define SE_GENI_RX_FIFOn            0x780u
#define SE_GENI_RX_FIFO_STATUS      0x804u
#define SE_GENI_TX_WATERMARK_REG    0x80Cu
#define SE_GENI_RX_WATERMARK_REG    0x810u
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814u
#define SE_GENI_IOS                 0x908u
#define SE_DMA_TX_IRQ_CLR           0xC44u
#define SE_DMA_RX_IRQ_CLR           0xD44u
#define SE_GSI_EVENT_EN             0xE18u
#define SE_IRQ_EN                   0xE1Cu
#define SE_DMA_GENERAL_CFG          0xE30u

#define M_CMD_DONE                  (1u << 0)
#define M_CMD_OVERRUN               (1u << 1)
#define M_ILLEGAL_CMD               (1u << 2)
#define M_CMD_FAILURE               (1u << 3)
#define M_CMD_CANCEL                (1u << 4)
#define M_CMD_ABORT                 (1u << 5)
#define M_GP_IRQ_1                  (1u << 10)
#define M_GP_IRQ_3                  (1u << 12)
#define M_GP_IRQ_4                  (1u << 13)
#define M_RX_FIFO_WM                (1u << 26)
#define M_RX_FIFO_LAST              (1u << 27)
#define M_ERRORS                    (M_CMD_OVERRUN | M_ILLEGAL_CMD | M_CMD_FAILURE | M_CMD_CANCEL | M_CMD_ABORT | M_GP_IRQ_1 | M_GP_IRQ_3 | M_GP_IRQ_4)

#define I2C_WRITE                   1u
#define I2C_READ                    2u
#define I2C_STOP_STRETCH            (1u << 2)
#define I2C_ADDR_SHIFT              9u
#define M_OPCODE_SHIFT              27u
#define GENI_PROTO_I2C              3u

#define GCC_VOTE                    0x52008u
#define GCC_VOTE_S_AHB              (1u << 7)
#define GCC_VOTE_CORE               (1u << 8)
#define GCC_VOTE_S4                 (1u << 14)
#define GCC_HALT_S_AHB              0x23000u
#define GCC_HALT_CORE               0x23004u
#define GCC_HALT_S4                 0x17074u
#define GCC_S4_RCG                  0x17078u
#define GCC_CLK_OFF                 (1u << 31)
#define GCC_HWCG                    (1u << 1)

#define TLMM_STRIDE                 0x1000u
#define TLMM_OE                     (1u << 9)
#define TLMM_OUT                    (1u << 1)

static
VOID
SiaWr (
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    SiaWrite32 (Base, Offset, Value);
    KeMemoryBarrier ();
}

VOID
SiaGpioOutput (
    _In_ PSIA_DEVICE Dev,
    _In_ ULONG Pin,
    _In_ BOOLEAN High
    )
{
    SiaWr (Dev->Tlmm, Pin * TLMM_STRIDE + 4, High ? TLMM_OUT : 0);
    SiaWr (Dev->Tlmm, Pin * TLMM_STRIDE, TLMM_OE | (2u << 6));
}

static
NTSTATUS
SiaClocksOn (
    _In_ PSIA_DEVICE Dev
    )
{
    ULONG Index;
    ULONG Value;

    SiaWr (Dev->Gcc, GCC_S4_RCG + 4, 0);
    SiaWr (Dev->Gcc, GCC_S4_RCG, SiaRead32 (Dev->Gcc, GCC_S4_RCG) | 1u);
    for (Index = 0; Index < 500 && (SiaRead32 (Dev->Gcc, GCC_S4_RCG) & 1u) != 0; Index++) {
        KeStallExecutionProcessor (10);
    }
    SiaLog (Dev, "gcc s4 rcg cmd=0x%08x cfg=0x%08x", SiaRead32 (Dev->Gcc, GCC_S4_RCG), SiaRead32 (Dev->Gcc, GCC_S4_RCG + 4));

    Value = SiaRead32 (Dev->Gcc, GCC_VOTE);
    SiaLog (Dev, "gcc vote before=0x%08x", Value);
    SiaWr (Dev->Gcc, GCC_VOTE, Value | GCC_VOTE_S_AHB | GCC_VOTE_CORE | GCC_VOTE_S4);

    for (Index = 0; Index < 200; Index++) {
        ULONG Ahb = SiaRead32 (Dev->Gcc, GCC_HALT_S_AHB);
        if (((Ahb & GCC_HWCG) != 0 || (Ahb & GCC_CLK_OFF) == 0) &&
            (SiaRead32 (Dev->Gcc, GCC_HALT_CORE) & GCC_CLK_OFF) == 0 &&
            (SiaRead32 (Dev->Gcc, GCC_HALT_S4) & GCC_CLK_OFF) == 0) {
            break;
        }
        KeStallExecutionProcessor (100);
    }
    SiaLog (Dev, "gcc vote after=0x%08x s_ahb=0x%08x core=0x%08x s4=0x%08x",
            SiaRead32 (Dev->Gcc, GCC_VOTE), SiaRead32 (Dev->Gcc, GCC_HALT_S_AHB), SiaRead32 (Dev->Gcc, GCC_HALT_CORE), SiaRead32 (Dev->Gcc, GCC_HALT_S4));
    return Index < 200 ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;
}

static
VOID
SiaPinsOn (
    _In_ PSIA_DEVICE Dev
    )
{
    ULONG Pin;

    for (Pin = 72; Pin <= 73; Pin++) {
        SiaWr (Dev->Tlmm, Pin * TLMM_STRIDE, (1u << 2) | 3u);
        SiaLog (Dev, "gpio%lu ctl=0x%08x io=0x%08x", Pin, SiaRead32 (Dev->Tlmm, Pin * TLMM_STRIDE), SiaRead32 (Dev->Tlmm, Pin * TLMM_STRIDE + 4));
    }
}

static
VOID
SiaSeIrqClear (
    _In_ PSIA_DEVICE Dev
    )
{
    SiaWr (Dev->Se, SE_GSI_EVENT_EN, 0);
    SiaWr (Dev->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    SiaWr (Dev->Se, SE_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    SiaWr (Dev->Se, SE_DMA_TX_IRQ_CLR, 0xFFFFFFFFu);
    SiaWr (Dev->Se, SE_DMA_RX_IRQ_CLR, 0xFFFFFFFFu);
    SiaWr (Dev->Se, SE_IRQ_EN, 0xFFFFFFFFu);
}

NTSTATUS
SiaI2cInit (
    _In_ PSIA_DEVICE Dev
    )
{
    NTSTATUS Status;
    ULONG Fw;
    ULONG Proto;

    Status = SiaClocksOn (Dev);
    if (!NT_SUCCESS (Status)) {
        SiaLog (Dev, "clock enable timeout");
        return Status;
    }
    SiaPinsOn (Dev);

    Fw = SiaRead32 (Dev->Se, GENI_FW_REVISION_RO);
    Proto = (Fw >> 8) & 0xFFu;
    SiaLog (Dev, "se fw_rev=0x%08x proto=%lu if_disable=0x%08x status=0x%08x ios=0x%08x",
            Fw, Proto, SiaRead32 (Dev->Se, GENI_IF_DISABLE_RO), SiaRead32 (Dev->Se, SE_GENI_STATUS), SiaRead32 (Dev->Se, SE_GENI_IOS));
    if (Proto != GENI_PROTO_I2C) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    SiaSeIrqClear (Dev);
    SiaWr (Dev->Se, SE_GENI_CGC_CTRL, SiaRead32 (Dev->Se, SE_GENI_CGC_CTRL) | 0x7Fu);
    SiaWr (Dev->Se, SE_DMA_GENERAL_CFG, SiaRead32 (Dev->Se, SE_DMA_GENERAL_CFG) | 0xFu);
    SiaWr (Dev->Se, GENI_OUTPUT_CTRL, 0x7Fu);
    SiaWr (Dev->Se, GENI_FORCE_DEFAULT_REG, 1u);
    SiaWr (Dev->Se, SE_IRQ_EN, SiaRead32 (Dev->Se, SE_IRQ_EN) | 0xFu);
    SiaWr (Dev->Se, SE_GENI_DMA_MODE_EN, SiaRead32 (Dev->Se, SE_GENI_DMA_MODE_EN) & ~1u);
    SiaWr (Dev->Se, SE_GSI_EVENT_EN, 0);
    SiaWr (Dev->Se, SE_GENI_RX_WATERMARK_REG, 15);
    SiaWr (Dev->Se, SE_GENI_RX_RFR_WATERMARK_REG, 16);
    SiaWr (Dev->Se, SE_GENI_TX_PACKING_CFG0, 0x7F8FEu);
    SiaWr (Dev->Se, SE_GENI_TX_PACKING_CFG1, 0xFFEFEu);
    SiaWr (Dev->Se, SE_GENI_RX_PACKING_CFG0, 0x7F8FEu);
    SiaWr (Dev->Se, SE_GENI_RX_PACKING_CFG1, 0xFFEFEu);
    SiaWr (Dev->Se, SE_GENI_BYTE_GRAN, 0);
    SiaWr (Dev->Se, SE_GENI_TX_WATERMARK_REG, 0);
    SiaWr (Dev->Se, SE_GENI_CLK_SEL, 0);
    SiaWr (Dev->Se, GENI_SER_M_CLK_CFG, (2u << 4) | 1u);
    SiaWr (Dev->Se, SE_I2C_SCL_COUNTERS, (5u << 20) | (11u << 10) | 22u);
    SiaLog (Dev, "se configured clk_cfg=0x%08x scl=0x%08x", SiaRead32 (Dev->Se, GENI_SER_M_CLK_CFG), SiaRead32 (Dev->Se, SE_I2C_SCL_COUNTERS));
    return STATUS_SUCCESS;
}

static
NTSTATUS
SiaWaitDone (
    _In_ PSIA_DEVICE Dev,
    _Out_writes_bytes_opt_ (Length) UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Index;
    ULONG Irq;
    ULONG Read = 0;

    for (Index = 0; Index < 20000; Index++) {
        Irq = SiaRead32 (Dev->Se, SE_GENI_M_IRQ_STATUS);
        if (Data != NULL && (Irq & (M_RX_FIFO_WM | M_RX_FIFO_LAST)) != 0) {
            ULONG Words = SiaRead32 (Dev->Se, SE_GENI_RX_FIFO_STATUS) & 0x1FFFFFFu;
            while (Words-- > 0) {
                ULONG Word = SiaRead32 (Dev->Se, SE_GENI_RX_FIFOn);
                ULONG Byte;
                for (Byte = 0; Byte < 4 && Read < Length; Byte++) {
                    Data[Read++] = (UCHAR)(Word & 0xFFu);
                    Word >>= 8;
                }
            }
        }
        if ((Irq & (M_ERRORS | M_CMD_DONE)) != 0) {
            SiaWr (Dev->Se, SE_GENI_M_IRQ_CLEAR, Irq);
            if ((Irq & M_ERRORS) != 0) {
                SiaLog (Dev, "i2c error irq=0x%08x status=0x%08x ios=0x%08x", Irq, SiaRead32 (Dev->Se, SE_GENI_STATUS), SiaRead32 (Dev->Se, SE_GENI_IOS));
                return (Irq & M_GP_IRQ_1) != 0 ? STATUS_NO_SUCH_DEVICE : STATUS_IO_DEVICE_ERROR;
            }
            if (Data != NULL && Read < Length) {
                SiaLog (Dev, "i2c short read %lu/%lu", Read, Length);
                return STATUS_DATA_ERROR;
            }
            return STATUS_SUCCESS;
        }
        if (Irq != 0) {
            SiaWr (Dev->Se, SE_GENI_M_IRQ_CLEAR, Irq & ~(M_RX_FIFO_WM | M_RX_FIFO_LAST));
        }
        KeStallExecutionProcessor (5);
    }
    SiaLog (Dev, "i2c timeout irq=0x%08x status=0x%08x ios=0x%08x", SiaRead32 (Dev->Se, SE_GENI_M_IRQ_STATUS), SiaRead32 (Dev->Se, SE_GENI_STATUS), SiaRead32 (Dev->Se, SE_GENI_IOS));
    SiaWr (Dev->Se, SE_GENI_M_CMD_CTRL_REG, 1u << 1);
    KeStallExecutionProcessor (1000);
    SiaWr (Dev->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    return STATUS_IO_TIMEOUT;
}

static VOID
SiaI2cStart (PSIA_DEVICE Dev, UCHAR Address, ULONG Opcode, ULONG Length, BOOLEAN Stop)
{
    ULONG Param = (ULONG)Address << I2C_ADDR_SHIFT;
    if (!Stop) Param |= I2C_STOP_STRETCH;
    SiaWr (Dev->Se, SE_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    SiaWr (Dev->Se, Opcode == I2C_READ ? SE_I2C_RX_TRANS_LEN : SE_I2C_TX_TRANS_LEN, Length);
    SiaWr (Dev->Se, SE_GENI_M_CMD0, (Opcode << M_OPCODE_SHIFT) | Param);
}

NTSTATUS
SiaRegRead (PSIA_DEVICE Dev, UCHAR Address, UCHAR Reg, USHORT *Value)
{
    UCHAR Buffer[2];
    NTSTATUS Status;
    *Value = 0;
    SiaI2cStart (Dev, Address, I2C_WRITE, 1, FALSE);
    SiaWr (Dev->Se, SE_GENI_TX_FIFOn, Reg);
    Status = SiaWaitDone (Dev, NULL, 0);
    if (!NT_SUCCESS (Status)) return Status;
    SiaI2cStart (Dev, Address, I2C_READ, sizeof (Buffer), TRUE);
    Status = SiaWaitDone (Dev, Buffer, sizeof (Buffer));
    if (NT_SUCCESS (Status)) *Value = (USHORT)((Buffer[0] << 8) | Buffer[1]);
    return Status;
}

NTSTATUS
SiaRegWrite (PSIA_DEVICE Dev, UCHAR Address, UCHAR Reg, USHORT Value)
{
    SiaI2cStart (Dev, Address, I2C_WRITE, 3, TRUE);
    SiaWr (Dev->Se, SE_GENI_TX_FIFOn, (ULONG)Reg | ((ULONG)RtlUshortByteSwap (Value) << 8));
    return SiaWaitDone (Dev, NULL, 0);
}
