/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * PEACH hw2.0 pre-WMI DP bring-up, ported from the local ath12k tree:
 * dp.c, dp_rx.c, wifi7/hal.c, hal_rx.c, hal_tx.c, hal_qcc2072.c.
 *
 * QCC2072 R0 offsets differ from WCN7850. R2 pointers stay the same.
 * This prepares resources, not a working TX/RX stack. DP MSI/HTT ring
 * advertisement and completion handling belong to the later data stage.
 */
#include "wcn7861.h"
#include "stage6-protocol.h"

#define DP_REO 0x00a38000u
#define DP_TCL 0x00a44000u
#define DP_WBM 0x00a34000u
#define DP_LINK_COUNT 32768u
#define DP_LINK_SIZE 128u
#define DP_LINK_BANK_SIZE 0x200000u
#define DP_BUFFER_SIZE 2048u
#define DP_BUFFER_CHUNK (256u * DP_BUFFER_SIZE)
#define DP_SPT_ENTRIES 512u

/* ath12k default profile. Hardware ring sizes are in DWORDS, not bytes. */
static NTSTATUS
DpRingCreate (PWCN_DEVICE Dev, ULONG Id, ULONG Entries, ULONG EntryDwords,
              ULONG RegBase, ULONG PointerReg, BOOLEAN Source, BOOLEAN Lmac,
              ULONG Batch, ULONG Timer, PWCN_DP_RING *Result)
{
    PWCN_DP_RING Ring;
    ULONG64 Address;
    ULONG Misc, PointerOffset;
    NTSTATUS Status;

    if (Dev->Dp.RingCount >= WCN_DP_MAX_RINGS || Id >= 256 ||
        !Entries || !EntryDwords || Entries > 0xfffffu / EntryDwords) {
        return STATUS_INVALID_PARAMETER;
    }
    Ring = &Dev->Dp.Ring[Dev->Dp.RingCount++];
    Ring->Id = Id; Ring->Entries = Entries; Ring->EntryDwords = EntryDwords;
    Ring->RegBase = RegBase; Ring->PointerReg = PointerReg;
    Ring->Source = Source; Ring->Lmac = Lmac;
    Status = WcnDmaAllocAligned (Dev, (SIZE_T)Entries * EntryDwords * 4, 0xfff, &Ring->Desc);
    if (!NT_SUCCESS (Status)) return Status;
    if (Result) *Result = Ring;
    if (!Lmac) {
        /* Clear stale enable/MSI state before writing a new DMA address. */
        WcnWrite (Dev, RegBase + 0x10, 0);
        WcnWrite (Dev, RegBase + 0x4c, 0); /* MSI1_BASE_MSB: MSI disabled */
        Address = Ring->Desc.La;
        WcnWrite (Dev, RegBase, (ULONG)Address);
        WcnWrite (Dev, RegBase + 4, (ULONG)(Address >> 32) | (Entries * EntryDwords << 8));
        WcnWrite (Dev, RegBase + 8, EntryDwords | (Source ? 0 : Id << 8));
        WcnWrite (Dev, RegBase + (Source ? 0x30 : 0x24),
                  (Batch * EntryDwords & 0x7fff) | ((Source ? Timer : Timer >> 3) << 16));
        if (Source) WcnWrite (Dev, RegBase + 0x34, 0);
        if (Id != 120) {
            PointerOffset = Source ? 0x1c : 0x14;
            Address = Dev->Dp.Rdp.La + Id * 4;
            WcnWrite (Dev, RegBase + PointerOffset, (ULONG)Address);
            WcnWrite (Dev, RegBase + PointerOffset + 4, (ULONG)(Address >> 32));
        }
        WcnWrite (Dev, PointerReg, 0);
        WcnWrite (Dev, PointerReg + 4, 0);
        KeMemoryBarrier ();
        Misc = 0x40 | (Source ? 2 : 0) | (Id == 120 ? 1 : 0);
        Ring->Active = TRUE;
        WcnWrite (Dev, RegBase + 0x10, Misc);
        if (WcnRead (Dev, RegBase) != (ULONG)Ring->Desc.La ||
            (WcnRead (Dev, RegBase + 0x10) & 0x40) == 0) {
            WcnLog (Dev, "dp: ring %u register readback failed", Id);
            return STATUS_DEVICE_HARDWARE_ERROR;
        }
    }

    return STATUS_SUCCESS;
}

static VOID
DpPublish (PWCN_DEVICE Dev, PWCN_DP_RING Ring, ULONG Entries)
{
    Ring->Head = Entries * Ring->EntryDwords;
    KeMemoryBarrier ();
    if (Ring->Lmac) {
        ((volatile ULONG *)Dev->Dp.Wrp.Va)[Ring->Id - 160] = Ring->Head;
    } else {
        WcnWrite (Dev, Ring->PointerReg, Ring->Head);
    }
}

static NTSTATUS
DpLinksInitialize (PWCN_DEVICE Dev)
{
    PWCN_DP_RING Ring;
    ULONG Bank, Index, Count, Filled = 0, Cookie, Size;
    ULONG64 Address;
    ULONG *Desc;
    NTSTATUS Status;

    Status = DpRingCreate (Dev, 120, DP_LINK_COUNT, 2, DP_WBM + 0xd3c,
                          DP_WBM + 0x30b8, TRUE, FALSE, 1, 256, &Ring);
    if (!NT_SUCCESS (Status)) return Status;
    /* 4 MiB of 128-byte links plus alignment space: same three-bank split
     * as ath12k. The IDLE RING is only 256 KiB, so scatter is NOT needed.
     * Scatter selection depends on idle-ring size, not link-bank size. */
    for (Bank = 0; Bank < 3; Bank++) {
        Size = Bank == 2 ? 384 : DP_LINK_BANK_SIZE;
        Status = WcnDmaAllocAligned (Dev, Size, 0xfff, &Dev->Dp.LinkBank[Bank]);
        if (!NT_SUCCESS (Status)) return Status;
        Count = Size / DP_LINK_SIZE;
        for (Index = 0; Index < Count && Filled < DP_LINK_COUNT - 1; Index++, Filled++) {
            Address = Dev->Dp.LinkBank[Bank].La + (ULONG64)Index * DP_LINK_SIZE;
            Cookie = ((Count - Index + 0x4000) << 3) | Bank;
            Desc = (ULONG *)(Ring->Desc.Va + Filled * 8);
            Desc[0] = (ULONG)Address;
            Desc[1] = (ULONG)(Address >> 32) | (1u << 8) | (Cookie << 12);
        }
    }
    DpPublish (Dev, Ring, Filled);

    return STATUS_SUCCESS;
}

static NTSTATUS
DpCookiesInitialize (PWCN_DEVICE Dev)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    NTSTATUS Status;
    ULONG Page, Entry, Cmem = (ULONG)Dev->Qmi.CmemBase;
    ULONG64 *Spt;

    if (!Cmem || Dev->Qmi.CmemBase > MAXULONG ||
        Dev->Qmi.CmemSize < WCN_DP_SPT_PAGES * 4 ||
        Dev->Qmi.CmemBase + Dev->Qmi.CmemSize > 0x100000000ull) {
        WcnLog (Dev, "dp: invalid CAP CMEM base 0x%llx size 0x%llx",
                Dev->Qmi.CmemBase, Dev->Qmi.CmemSize);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    WDF_OBJECT_ATTRIBUTES_INIT (&Attributes);
    Attributes.ParentObject = Dev->Device;
    Status = WdfMemoryCreate (&Attributes, NonPagedPoolNx, WCN_TAG,
                              (SIZE_T)WCN_DP_SPT_PAGES * DP_SPT_ENTRIES * sizeof (WCN_DP_COOKIE),
                              &Dev->Dp.CookieMemory, (PVOID *)&Dev->Dp.Cookies);
    if (!NT_SUCCESS (Status)) return Status;
    RtlZeroMemory (Dev->Dp.Cookies,
                   (SIZE_T)WCN_DP_SPT_PAGES * DP_SPT_ENTRIES * sizeof (WCN_DP_COOKIE));
    for (Page = 0; Page < WCN_DP_SPT_PAGES; Page++) {
        Status = WcnDmaAllocAligned (Dev, 4096, 0xfff, &Dev->Dp.Spt[Page]);
        if (!NT_SUCCESS (Status)) return Status;
        Spt = (ULONG64 *)Dev->Dp.Spt[Page].Va;
        for (Entry = 0; Entry < DP_SPT_ENTRIES; Entry++) {
            Spt[Entry] = (ULONG64)(ULONG_PTR)&Dev->Dp.Cookies[Page * DP_SPT_ENTRIES + Entry];
        }
        KeMemoryBarrier ();
        WcnWrite (Dev, Cmem + Page * 4, (ULONG)(Dev->Dp.Spt[Page].La >> 12));
        if (WcnRead (Dev, Cmem + Page * 4) != (ULONG)(Dev->Dp.Spt[Page].La >> 12)) {
            WcnLog (Dev, "dp: CMEM PPT readback failed page %u", Page);
            return STATUS_DEVICE_HARDWARE_ERROR;
        }
    }

    return STATUS_SUCCESS;
}

static VOID
DpReoInitialize (PWCN_DEVICE Dev)
{
    ULONG Value, Index;
    Value = WcnRead (Dev, DP_REO);
    WcnWrite (Dev, DP_REO, Value | 0xc);
    Value = WcnRead (Dev, DP_REO + 0xba0);
    WcnWrite (Dev, DP_REO + 0xba0, Value & ~0x1fe0000u);
    for (Index = 0; Index < 4; Index++) {
        WcnWrite (Dev, DP_REO + 0xb2c + Index * 4, Index == 3 ? 40000 : 100000);
    }
    WcnWrite (Dev, DP_REO + 0xc, 0x43214321);
    WcnWrite (Dev, DP_REO + 0x10, 0x43214321);
}

static VOID
DpDscpInitialize (PWCN_DEVICE Dev)
{
    ULONG Table, Dscp, Bit, Byte, Value;
    UCHAR Map[24];
    Value = WcnRead (Dev, DP_TCL + 0x20);
    WcnWrite (Dev, DP_TCL + 0x20, Value | (1u << 23));
    RtlZeroMemory (Map, sizeof (Map));
    /* Default dscp2tid(dscp) == dscp >> 3, tightly packed 3-bit entries. */
    for (Dscp = 0; Dscp < 64; Dscp++) {
        Bit = Dscp * 3;
        Map[Bit / 8] |= (UCHAR)((Dscp >> 3) << (Bit % 8));
        if (Bit % 8 > 5) Map[Bit / 8 + 1] |= (UCHAR)((Dscp >> 3) >> (8 - Bit % 8));
    }
    for (Table = 0; Table < 32; Table++) {
        for (Byte = 0; Byte < 24; Byte += 4) {
            RtlCopyMemory (&Value, Map + Byte, 4);
            WcnWrite (Dev, DP_TCL + 0x240 + Table * 24 + Byte, Value);
        }
    }
    Value = WcnRead (Dev, DP_TCL + 0x20);
    WcnWrite (Dev, DP_TCL + 0x20, Value & ~(1u << 23));
}

static NTSTATUS
DpFillBuffers (PWCN_DEVICE Dev, PWCN_DP_RING Ring, ULONG ChunkBase, BOOLEAN Monitor)
{
    ULONG Chunk, Index, Cookie;
    ULONG *Desc;
    ULONG64 Address;
    NTSTATUS Status;
    for (Chunk = 0; Chunk < (Ring->Entries + 255) / 256; Chunk++) {
        if (ChunkBase + Chunk >= WCN_DP_RX_CHUNKS) return STATUS_INVALID_PARAMETER;
        Status = WcnDmaAllocAligned (Dev, DP_BUFFER_CHUNK, 0xfff,
                                     &Dev->Dp.RxBuffer[ChunkBase + Chunk]);
        if (!NT_SUCCESS (Status)) return Status;
    }
    for (Index = 0; Index < Ring->Entries - 1; Index++) {
        Address = Dev->Dp.RxBuffer[ChunkBase + Index / 256].La +
                  (ULONG64)(Index % 256) * DP_BUFFER_SIZE;
        /* Monitor uses per-ring ID cookies, ordinary RX uses CC cookies. */
        Cookie = Monitor ? Index : (WCN_DP_RX_PPT_BASE << 9) + Index;
        Desc = (ULONG *)(Ring->Desc.Va + Index * 8);
        Desc[0] = (ULONG)Address;
        Desc[1] = (ULONG)(Address >> 32) | (6u << 8) | (Cookie << 12);
        if (!Monitor) Dev->Dp.Cookies[Cookie].BufferAddress = Address;
    }
    DpPublish (Dev, Ring, Ring->Entries - 1);
    return STATUS_SUCCESS;
}

NTSTATUS
WcnDpInitialize (_In_ PWCN_DEVICE Dev)
{
    static const ULONG TxCompletion[3] = { 0, 2, 4 };
    PWCN_DP_RING Ring;
    ULONG Index;
    NTSTATUS Status;

    if (Dev->Dp.Initialized) return STATUS_SUCCESS;
    if (Dev->Dp.Failed) return STATUS_DEVICE_NOT_READY;
#define DP_TRY(Call) do { Status = (Call); if (!NT_SUCCESS (Status)) goto Fail; } while (0)

    DP_TRY (WcnDmaAllocAligned (Dev, 256 * 4, 0xfff, &Dev->Dp.Rdp));
    DP_TRY (WcnDmaAllocAligned (Dev, 96 * 4, 0xfff, &Dev->Dp.Wrp));
    DP_TRY (DpLinksInitialize (Dev));
    DP_TRY (DpCookiesInitialize (Dev));
    DP_TRY (DpRingCreate (Dev, 121, 64, 8, DP_WBM + 0x37c, DP_WBM + 0x3010,
                          TRUE, FALSE, 1, 256, NULL));
    for (Index = 0; Index < 3; Index++) {
        DP_TRY (DpRingCreate (Dev, 24 + Index, 512, 8, DP_TCL + 0x918 + Index * 0x78,
                              DP_TCL + 0x2000 + Index * 8, TRUE, FALSE, 1, 256, NULL));
        DP_TRY (DpRingCreate (Dev, 128 + TxCompletion[Index], 32768, 8,
                              DP_WBM + 0xe08 + TxCompletion[Index] * 0x78,
                              DP_WBM + 0x30c8 + TxCompletion[Index] * 8,
                              FALSE, FALSE, 256, 1000, NULL));
    }
    DP_TRY (DpRingCreate (Dev, 16, 32, 8, DP_REO + 0x320, DP_REO + 0x3028,
                          TRUE, FALSE, 1, 256, NULL));
    DP_TRY (DpRingCreate (Dev, 131, 1024, 8, DP_WBM + 0xf70, DP_WBM + 0x30e0,
                          FALSE, FALSE, 1, 256, NULL));
    DP_TRY (DpRingCreate (Dev, 0, 128, 8, DP_REO + 0x8c0, DP_REO + 0x3088,
                          FALSE, FALSE, 1, 256, NULL));
    /* QCC2072 uses a 32-bit TLV header, not the WCN7850 64-bit layout. */
    DP_TRY (DpRingCreate (Dev, 20, 256, 10, DP_REO + 0x2a8, DP_REO + 0x3020,
                          TRUE, FALSE, 1, 256, &Ring));
    for (Index = 0; Index < Ring->Entries; Index++) {
        ((ULONG *)(Ring->Desc.Va + Index * 40))[1] = Index + 1;
    }
    DP_TRY (DpRingCreate (Dev, 21, 2048, 28, DP_REO + 0xaa0, DP_REO + 0x30a8,
                          FALSE, FALSE, 1, 256, NULL));
    DpReoInitialize (Dev);
    DpDscpInitialize (Dev);
    /* LMAC rings are host memory only here, as in ath12k_hal_srng_setup.
     * Their MMIO is firmware-owned and is advertised later by HTT. */
    DP_TRY (DpRingCreate (Dev, 160, 4096, 2, 0, 0, TRUE, TRUE, 0, 0, &Ring));
    DP_TRY (DpFillBuffers (Dev, Ring, 0, FALSE));
    for (Index = 0; Index < 2; Index++) {
        DP_TRY (DpRingCreate (Dev, 161 + Index * 8, 2048, 2, 0, 0,
                              TRUE, TRUE, 0, 0, NULL));
    }
    DP_TRY (DpRingCreate (Dev, 186, 1024, 8, 0, 0, FALSE, TRUE, 0, 0, NULL));
    for (Index = 0; Index < 2; Index++) {
        DP_TRY (DpRingCreate (Dev, 185 + Index * 8, 1024, 2, 0, 0,
                              TRUE, TRUE, 0, 0, &Ring));
        DP_TRY (DpFillBuffers (Dev, Ring, 16 + Index * 4, TRUE));
    }
    Dev->Dp.Initialized = TRUE;

    return STATUS_SUCCESS;
Fail:
    Dev->Dp.Failed = TRUE;
    WcnLog (Dev, "dp: setup failed 0x%08x; WMI_INIT blocked, DMA retained until device teardown", Status);
    return Status;
#undef DP_TRY
}

NTSTATUS
WcnDpPrepareWmi (_In_ PWCN_DEVICE Dev)
{
    ULONG Value, Index, Cmem = (ULONG)Dev->Qmi.CmemBase;
    NTSTATUS Status;
    if (Dev->Dp.ReadyForWmi) return STATUS_SUCCESS;
    if (!Dev->Dp.Initialized || Dev->Dp.Failed) return STATUS_DEVICE_NOT_READY;
    WcnWrite (Dev, DP_REO + 0x6c, Cmem);
    Value = 0x10 | (19u << 8) | (8u << 13) | (1u << 18);
    WcnWrite (Dev, DP_REO + 0x70, Value | (1u << 19) | (1u << 20));
    WcnWrite (Dev, DP_WBM + 0x40, Cmem);
    WcnWrite (Dev, DP_WBM + 0x44, Value);
    Value = WcnRead (Dev, DP_WBM + 0x90);
    WcnWrite (Dev, DP_WBM + 0x90, Value | 0xb);
    Value = WcnRead (Dev, DP_WBM + 0x94);
    WcnWrite (Dev, DP_WBM + 0x94, Value | 0x13a); /* WBM rings 0,2,3,4, global */
    if (WcnRead (Dev, DP_REO + 0x6c) != Cmem ||
        (WcnRead (Dev, DP_REO + 0x70) & 0x1fffff) != 0x1d1310) {
        Dev->Dp.Failed = TRUE;
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    for (Index = 0; Index < 8; Index++) {
        Status = DpRingCreate (Dev, 1 + Index, 2048, 8,
                               DP_REO + 0x500 + Index * 0x78,
                               DP_REO + 0x3048 + Index * 8, FALSE, FALSE, 128, 500, NULL);
        if (!NT_SUCCESS (Status)) {
            Dev->Dp.Failed = TRUE;
            return Status;
        }
    }
    KeMemoryBarrier ();
    Dev->Dp.ReadyForWmi = TRUE;

    return STATUS_SUCCESS;
}

VOID
WcnDpCleanup (_In_ PWCN_DEVICE Dev)
{
    ULONG Index;
    /* Caller must have stopped endpoint DMA. WDF owns all allocations;
     * do not free on an initialization error while rings may still run. */
#define DP_FREE(Dma) do { if ((Dma).Buffer) WdfObjectDelete ((Dma).Buffer); } while (0)
    for (Index = 0; Index < Dev->Dp.RingCount; Index++) DP_FREE (Dev->Dp.Ring[Index].Desc);
    for (Index = 0; Index < 3; Index++) DP_FREE (Dev->Dp.LinkBank[Index]);
    for (Index = 0; Index < WCN_DP_SPT_PAGES; Index++) DP_FREE (Dev->Dp.Spt[Index]);
    for (Index = 0; Index < WCN_DP_RX_CHUNKS; Index++) DP_FREE (Dev->Dp.RxBuffer[Index]);
    DP_FREE (Dev->Dp.Rdp);
    DP_FREE (Dev->Dp.Wrp);
    if (Dev->Dp.CookieMemory) WdfObjectDelete (Dev->Dp.CookieMemory);
    RtlZeroMemory (&Dev->Dp, sizeof (Dev->Dp));
#undef DP_FREE
}

/* Post-READY counterpart of ath12k_dp_rx_htt_setup. Ring memory/buffers
 * already exist; firmware must learn their addresses before receiver use.
 * No data TX rings are advertised, no network connection is initiated. */
static PWCN_DP_RING
DpFindRxRing (PWCN_DEVICE Dev, ULONG Id)
{
    ULONG I;
    for (I = 0; I < Dev->Dp.RingCount; I++) if (Dev->Dp.Ring[I].Id == Id) return &Dev->Dp.Ring[I];
    return NULL;
}

NTSTATUS
WcnDpHttRxPrepare (PWCN_DEVICE Dev)
{
    static const ULONG Ids[6] = {160, 161, 169, 186, 185, 193};
    static const ULONG Types[6] = {2, 1, 1, 0, 1, 1}; /* SW2SW, SW2HW, HW2SW */
    static const ULONG HttIds[6] = {5, 0, 0, 7, 1, 1};
    static const ULONG Macs[6] = {0, 0, 1, 0, 0, 1};
    PWCN_DP_RING Ring;
    ULONG B[17] = {0}, Step = Dev->Dp.HttRxStep, Mac, Type, HttId, Batch, Timer, Low;
    ULONG64 Hp, Tp;
    NTSTATUS Status;
    if (Dev->Dp.HttRxReady) return STATUS_SUCCESS;
    if (!Dev->Dp.Initialized || !Dev->Dp.ReadyForWmi || !Dev->Ce.HttVersionReceived || Dev->Ce.HttMajor != 3) return STATUS_DEVICE_NOT_READY;
    if (Step >= 8) return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (Step < 6) {
        Ring = DpFindRxRing (Dev, Ids[Step]);
        if (!Ring || !Ring->Lmac || !Ring->Desc.Buffer) return STATUS_DEVICE_CONFIGURATION_ERROR;
        Type = Types[Step]; HttId = HttIds[Step]; Mac = Macs[Step];
        B[0] = 0xbu | ((Type == 2 ? Mac : Mac + 1) << 8) | (HttId << 16) | (Type << 24);
        B[1] = (ULONG)Ring->Desc.La; B[2] = (ULONG)(Ring->Desc.La >> 32);
        B[3] = Ring->Entries * Ring->EntryDwords | (Ring->EntryDwords << 16);
        if (Type == 1) B[3] |= 1u << 25;
        Hp = Ring->Source ? Dev->Dp.Wrp.La + (Ring->Id - 160) * 4 : Dev->Dp.Rdp.La + Ring->Id * 4;
        Tp = Ring->Source ? Dev->Dp.Rdp.La + Ring->Id * 4 : Dev->Dp.Wrp.La + (Ring->Id - 160) * 4;
        B[4] = (ULONG)Hp; B[5] = (ULONG)(Hp >> 32);
        B[6] = (ULONG)Tp; B[7] = (ULONG)(Tp >> 32);
        /* MSI is not advertised: the stage6 worker/firmware owns these
         * resources. Data-path MSI and OS receive queues remain stage7. */
        Batch = Step == 3 || Step >= 4 ? 1 : 0; Timer = Step == 3 ? 256 : 500;
        Low = Step == 3 ? 0 : Ring->Entries / 8;
        B[11] = Batch * Ring->EntryDwords | ((Timer >> 3) << 16);
        B[12] = Low;
        Status = WcnHttSend (Dev, B, 52);

    } else {
        /* ath12k_dp_rxdma_ring_sel_config_qcc2072, normal receive filter.
         * No monitor-mode capture and no direct data-packet injection. */
        Mac = Step - 6;
        B[0] = 0xcu | ((Mac + 1) << 8) | (1u << 26) | (1u << 28);
        B[1] = 2048;
        B[4] = 1u << 24; /* filter-pass BAR */
        B[5] = (1u << 18) | (1u << 21) | (1u << 24); /* data MC/UC/null */
        B[6] = 0xdu; /* MPDU_START | RX_PACKET | MSDU_END */
        B[7] = S6_QCC_DESCRIPTOR_BYTES | (S6_QCC_PACKET_HEADER_OFFSET << 16);
        B[8] = S6_QCC_MPDU_START_TAG_OFFSET << 16;
        B[9] = S6_QCC_MSDU_END_TAG_OFFSET;
        Status = WcnHttSend (Dev, B, sizeof (B));

    }
    if (!NT_SUCCESS (Status)) return Status;
    Dev->Dp.HttRxStep++;
    if (Dev->Dp.HttRxStep == 8) {
        Dev->Dp.HttRxReady = TRUE;
        KeMemoryBarrier ();

        return STATUS_SUCCESS;
    }
    return STATUS_PENDING;
}
