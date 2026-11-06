/*
 * BAR0 access, endpoint configuration and the ath12k power-on reset
 * (ath12k pci.c: ath12k_pci_read32/write32, ath12k_pci_sw_reset,
 * ath12k_pci_msi_alloc; wifi7/pci.c PEACH probe).
 */
#include "wcn7861.h"

ULONG
WcnBarRead (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Bar + Offset));
}

VOID
WcnBarWrite (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Bar + Offset), Value);
}

ULONG
WcnCfgRead (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Config + Offset));
}

VOID
WcnCfgWrite (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Config + Offset), Value);
}

ULONG
WcnDbiRead (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Dbi + Offset));
}

VOID
WcnDbiWrite (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Dbi + Offset), Value);
}

static
VOID
WcnSelectWindow (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset
    )
{
    ULONG Window = (Offset >> WCN_WINDOW_SHIFT) & WCN_WINDOW_MASK;

    if (Window != Dev->Window) {
        WcnBarWrite (Dev, WCN_WINDOW_REG, WCN_WINDOW_ENABLE | Window);
        (VOID)WcnBarRead (Dev, WCN_WINDOW_REG);
        Dev->Window = Window;
    }
}

/* SoC address space access through the BAR0 sliding window. */
ULONG
WcnRead (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset
    )
{
    KIRQL Irql;
    ULONG Value;

    if (Offset < WCN_WINDOW_START) {
        return WcnBarRead (Dev, Offset);
    }
    KeAcquireSpinLock (&Dev->WindowLock, &Irql);
    WcnSelectWindow (Dev, Offset);
    if (Offset >= WCN_MHI_REGION_START && Offset <= WCN_MHI_REGION_END) {
        Value = WcnBarRead (Dev, (Offset - WCN_MHI_REGION_START) & WCN_WINDOW_RANGE);
    } else {
        Value = WcnBarRead (Dev, WCN_WINDOW_START + (Offset & WCN_WINDOW_RANGE));
    }
    KeReleaseSpinLock (&Dev->WindowLock, Irql);
    return Value;
}

VOID
WcnWrite (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset,
    _In_ ULONG Value
    )
{
    KIRQL Irql;

    if (Offset < WCN_WINDOW_START) {
        WcnBarWrite (Dev, Offset, Value);
        return;
    }
    KeAcquireSpinLock (&Dev->WindowLock, &Irql);
    WcnSelectWindow (Dev, Offset);
    if (Offset >= WCN_MHI_REGION_START && Offset <= WCN_MHI_REGION_END) {
        WcnBarWrite (Dev, (Offset - WCN_MHI_REGION_START) & WCN_WINDOW_RANGE, Value);
    } else {
        WcnBarWrite (Dev, WCN_WINDOW_START + (Offset & WCN_WINDOW_RANGE), Value);
    }
    KeReleaseSpinLock (&Dev->WindowLock, Irql);
}

static
ULONG
WcnCfgFindCapability (
    _In_ PWCN_DEVICE Dev,
    _In_ UCHAR Id
    )
{
    ULONG Offset = WcnCfgRead (Dev, 0x34) & 0xFC;
    ULONG Count;
    ULONG Header;

    for (Count = 0; Offset >= 0x40 && Count < 48; Count++) {
        Header = WcnCfgRead (Dev, Offset);
        if ((Header & 0xFF) == Id) {
            return Offset;
        }
        Offset = (Header >> 8) & 0xFC;
    }
    return 0;
}

/*
 * Identity, bus mastering, ASPM off (ath12k_pci_aspm_disable) and MSI pointed
 * at the DWC iMSI-RX target UEFI programmed into the root port. All 32 vectors
 * land in iMSI controller 0, i.e. on msi0. MSI itself is enabled after the
 * SoC reset, as ath12k_pci_power_up does.
 */
NTSTATUS
WcnPcieSetup (
    _In_ PWCN_DEVICE Dev
    )
{
    ULONG Id;
    ULONG Express;
    ULONG Msi;
    ULONG Control;
    ULONG Mmc;
    ULONG Mme;
    ULONG AddressLow;
    ULONG AddressHigh;
    BOOLEAN Is64;
    ULONG Version;

    Id = WcnCfgRead (Dev, 0x00);

    if (Id != WCN_PCI_ID) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    WcnCfgWrite (Dev, 0x04, (WcnCfgRead (Dev, 0x04) & 0xFFFF) | 0x0406);

    Express = WcnCfgFindCapability (Dev, 0x10);
    if (Express != 0) {
        WcnCfgWrite (Dev, Express + 0x10, WcnCfgRead (Dev, Express + 0x10) & ~0x3UL);

    }

    Msi = WcnCfgFindCapability (Dev, 0x05);
    if (Msi == 0) {

        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    Control = WcnCfgRead (Dev, Msi) >> 16;
    Mmc = (Control >> 1) & 0x7;
    Is64 = (Control & 0x80) != 0;
    AddressLow = WcnDbiRead (Dev, DBI_MSI_ADDR_LO);
    AddressHigh = WcnDbiRead (Dev, DBI_MSI_ADDR_HI);
    if (AddressLow == 0 && AddressHigh == 0) {

        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    WcnCfgWrite (Dev, Msi + 0x04, AddressLow);
    if (Is64) {
        WcnCfgWrite (Dev, Msi + 0x08, AddressHigh);
        WcnCfgWrite (Dev, Msi + 0x0C, 0);
        if (Control & 0x100) {
            WcnCfgWrite (Dev, Msi + 0x10, 0);
        }
    } else {
        WcnCfgWrite (Dev, Msi + 0x08, 0);
        if (Control & 0x100) {
            WcnCfgWrite (Dev, Msi + 0x0C, 0);
        }
    }
    Mme = (Mmc >= 5) ? 5 : 0;
    Dev->MultiMsi = (Mme == 5);
    WcnCfgWrite (Dev, Msi, (WcnCfgRead (Dev, Msi) & ~((0x7UL << 20) | (1UL << 16))) | (Mme << 20));

    WcnDbiWrite (Dev, DBI_MSI_INTR0_MASK, 0);
    WcnDbiWrite (Dev, DBI_MSI_INTR0_STATUS, MAXULONG);
    WcnDbiWrite (Dev, DBI_MSI_INTR0_ENABLE, MAXULONG);

    Version = WcnRead (Dev, WCN_TCSR_SOC_HW_VERSION);

    if (((Version >> 8) & 0xF) != 2) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

VOID
WcnPcieMsiEnable (
    _In_ PWCN_DEVICE Dev,
    _In_ BOOLEAN Enable
    )
{
    ULONG Msi = WcnCfgFindCapability (Dev, 0x05);
    ULONG Value;

    if (Msi == 0) {
        return;
    }
    Value = WcnCfgRead (Dev, Msi);
    Value = Enable ? (Value | (1UL << 16)) : (Value & ~(1UL << 16));
    WcnCfgWrite (Dev, Msi, Value);
}

/* ath12k_pci_sw_reset */
VOID
WcnPcieSwReset (
    _In_ PWCN_DEVICE Dev,
    _In_ BOOLEAN PowerOn
    )
{
    ULONG Value;
    ULONG Index;

    if (PowerOn) {
        Value = WcnRead (Dev, WCN_PARF_LTSSM);
        for (Index = 0; Value != WCN_PARF_LTSSM_VALUE && Index < 5; Index++) {
            if (Value == MAXULONG) {
                WcnSleep (5);
            }
            WcnWrite (Dev, WCN_PARF_LTSSM, WCN_PARF_LTSSM_VALUE);
            Value = WcnRead (Dev, WCN_PARF_LTSSM);
        }

        WcnWrite (Dev, WCN_GCC_PCIE_HOT_RST, WcnRead (Dev, WCN_GCC_PCIE_HOT_RST) | 0x10);

        WcnSleep (5);

        WcnWrite (Dev, WCN_PCIE_INT_ALL_CLEAR, MAXULONG);
        WcnWrite (Dev, WCN_WLAON_QFPROM_PWR_CTRL, WcnRead (Dev, WCN_WLAON_QFPROM_PWR_CTRL) & ~0x4UL);
    }

    WcnWrite (Dev, WCN_TXVECDB, 0);
    WcnWrite (Dev, WCN_TXVECSTATUS, 0);
    WcnWrite (Dev, WCN_RXVECDB, 0);
    WcnWrite (Dev, WCN_RXVECSTATUS, 0);

    Value = WcnRead (Dev, WCN_Q6_COOKIE);

    WcnSleep (10);
    WcnWrite (Dev, WCN_WLAON_WARM_SW_ENTRY, 0);
    WcnSleep (10);
    Value = WcnRead (Dev, WCN_WLAON_WARM_SW_ENTRY);

    Value = WcnRead (Dev, WCN_SOC_GLOBAL_RESET) | 1;
    WcnWrite (Dev, WCN_SOC_GLOBAL_RESET, Value);
    (VOID)WcnRead (Dev, WCN_SOC_GLOBAL_RESET);
    WcnSleep (10);
    Value &= ~1UL;
    WcnWrite (Dev, WCN_SOC_GLOBAL_RESET, Value);
    (VOID)WcnRead (Dev, WCN_SOC_GLOBAL_RESET);
    WcnSleep (10);
    Value = WcnRead (Dev, WCN_SOC_GLOBAL_RESET);

    /* the window register is cleared by the global reset */
    WcnBarWrite (Dev, WCN_WINDOW_REG, WCN_WINDOW_ENABLE | Dev->Window);
    (VOID)WcnBarRead (Dev, WCN_WINDOW_REG);

    WcnBarWrite (Dev, MHI_CTRL, MHI_CTRL_RESET);
    WcnSleep (10);
}

static
UCHAR
WcnCrc8 (
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    UCHAR Crc = 0;
    ULONG Index;
    ULONG Bit;

    /* crc8_populate_msb with polynomial x^2 + x + 1 (QCOM_PCIE_CRC8_POLYNOMIAL) */
    for (Index = 0; Index < Length; Index++) {
        Crc ^= Data[Index];
        for (Bit = 0; Bit < 8; Bit++) {
            Crc = (Crc & 0x80) ? (UCHAR)((Crc << 1) ^ 0x07) : (UCHAR)(Crc << 1);
        }
    }
    return Crc;
}

/* pcie-qcom qcom_pcie_config_sid_1_9_0 for the two iommu-map entries */
NTSTATUS
WcnPcieConfigSid (
    _In_ PWCN_DEVICE Dev
    )
{
    static const ULONG Map[2][2] = { { 0x0000, WCN_SID_BASE }, { WCN_EP_BDF, WCN_SID_BASE + 1 } };
    ULONG Index;
    ULONG Entry;
    ULONG Value;
    UCHAR Bdf[2];
    UCHAR Hash;
    UCHAR Current;

    if (Dev->Parf == NULL || Dev->ParfLength < PARF_BDF_TO_SID_CFG + 4) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (Entry = 0; Entry < 256; Entry++) {
        WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_TABLE + 4 * Entry), 0);
    }
    for (Index = 0; Index < RTL_NUMBER_OF (Map); Index++) {
        Bdf[0] = (UCHAR)(Map[Index][0] >> 8);
        Bdf[1] = (UCHAR)Map[Index][0];
        Hash = WcnCrc8 (Bdf, sizeof (Bdf));
        Value = READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_TABLE + 4 * Hash));
        while (Value != 0) {
            Current = Hash++;
            if ((Value & 0xFF) == 0) {
                WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_TABLE + 4 * Current), Value | Hash);
            }
            Value = READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_TABLE + 4 * Hash));
        }
        Value = (Map[Index][0] << 16) | ((Map[Index][1] - WCN_SID_BASE) << 8);
        WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_TABLE + 4 * Hash), Value);

    }

    Value = READ_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_CFG));
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Dev->Parf + PARF_BDF_TO_SID_CFG), Value & ~PARF_BDF_TO_SID_BYPASS);

    return STATUS_SUCCESS;
}

static
BOOLEAN
WcnSmrOverlaps (
    _In_ ULONG Smr,
    _In_ ULONG Id,
    _In_ ULONG Mask
    )
{
    ULONG SmrMask = (Smr >> 16) & 0x7FFF;

    return (Smr & SMMU_SMR_VALID) != 0 && (((Smr ^ Id) & ~(SmrMask | Mask) & 0x7FFF) == 0);
}

/*
 * Route the PCIe0 streams (0x1400-0x1401, one SMR with mask 1) through the
 * context the UFS stream uses. Without a matching SMR the first WCN7861 DMA
 * read (BHI) is an unmatched-stream fault and the SoC drops to crash dump
 * mode (900E). Free slots are taken from the top so the touch driver, which
 * scans from the bottom, does not race for the same one.
 */
NTSTATUS
WcnSmmuSetup (VOID)
{
    const ULONG Id = WCN_SID_BASE;
    const ULONG Mask = 0x1;
    const ULONG Wanted = SMMU_SMR_VALID | (Mask << 16) | Id;
    PHYSICAL_ADDRESS Physical;
    volatile ULONG *Smmu;
    ULONG Count;
    ULONG Slot;
    ULONG Smr;
    ULONG UfsS2cr = MAXULONG;
    ULONG Existing = MAXULONG;
    ULONG Free = MAXULONG;
    NTSTATUS Status = STATUS_SUCCESS;

    Physical.QuadPart = SMMU_BASE;
    Smmu = MmMapIoSpaceEx (Physical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Smmu == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Count = READ_REGISTER_ULONG (&Smmu[SMMU_IDR0 / 4]) & 0xFF;

    if (Count == 0 || Count > 256) {
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        goto out;
    }

    for (Slot = 0; Slot < Count; Slot++) {
        Smr = READ_REGISTER_ULONG (&Smmu[SMMU_SMR (Slot) / 4]);
        if (WcnSmrOverlaps (Smr, SMMU_UFS_SID, 0) && UfsS2cr == MAXULONG) {
            UfsS2cr = READ_REGISTER_ULONG (&Smmu[SMMU_S2CR (Slot) / 4]);

        }
        if (WcnSmrOverlaps (Smr, Id, Mask)) {

            if (Existing != MAXULONG || Smr != Wanted) {
                Status = STATUS_DEVICE_CONFIGURATION_ERROR;
                goto out;
            }
            Existing = Slot;
        }
        if ((Smr & SMMU_SMR_VALID) == 0) {
            Free = Slot;
        }
    }

    if (UfsS2cr == MAXULONG || (UfsS2cr & 0x3) != 0) {

        Status = STATUS_NOT_FOUND;
        goto out;
    }
    if (Existing != MAXULONG) {
        if (READ_REGISTER_ULONG (&Smmu[SMMU_S2CR (Existing) / 4]) != UfsS2cr) {
            Status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
        goto out;
    }
    if (Free == MAXULONG) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    if ((READ_REGISTER_ULONG (&Smmu[SMMU_SMR (Free) / 4]) & SMMU_SMR_VALID) != 0) {
        Status = STATUS_DEVICE_BUSY;
        goto out;
    }
    WRITE_REGISTER_ULONG (&Smmu[SMMU_S2CR (Free) / 4], UfsS2cr);
    WRITE_REGISTER_ULONG (&Smmu[SMMU_SMR (Free) / 4], Wanted);

    if (READ_REGISTER_ULONG (&Smmu[SMMU_SMR (Free) / 4]) != Wanted ||
        READ_REGISTER_ULONG (&Smmu[SMMU_S2CR (Free) / 4]) != UfsS2cr) {
        WRITE_REGISTER_ULONG (&Smmu[SMMU_SMR (Free) / 4], 0);
        Status = STATUS_IO_DEVICE_ERROR;
    }

out:
    MmUnmapIoSpace ((PVOID)Smmu, PAGE_SIZE);
    return Status;
}
