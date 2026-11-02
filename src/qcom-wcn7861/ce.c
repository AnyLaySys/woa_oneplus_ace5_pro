/* WCN7861 PEACH CE SRNG setup and HTC/WMI event decoding. */
#include "wcn7861.h"

#define HTC_HEADER_SIZE             8
#define HTC_MSG_READY               1
#define HTC_MSG_CONNECT_SERVICE     2
#define HTC_MSG_CONNECT_RESPONSE    3
#define HTC_MSG_SETUP_COMPLETE_EXT  5
#define HTC_SVC_RSVD_CTRL           0x0001
#define HTC_SVC_WMI_CONTROL         0x0100
#define HTC_SVC_HTT_DATA            0x0300
#define HTC_FLAG_DISABLE_CREDIT     (1u << 3)
#define HTC_MAX_CONTROL             2048
#define WMI_CMD_HDR_INIT            1
/* enum wmi_tlv_tag contains aliases: INIT/RESOURCE are 74/75, not
 * 78/79. Also independently confirmed in PEACH's command-1 TLV table. */
#define WMI_TAG_INIT_CMD            74
#define WMI_TAG_RESOURCE_CONFIG     75
#define WMI_TAG_ARRAY_STRUCT        18
#define WMI_SERVICE_READY_EXT_EVENT 0x4009
#define WMI_SERVICE_READY_EXT2_EVENT 0x4022
#define WMI_SERVICE_READY_EVENT     1
#define WMI_READY_EVENT             2
#define WMI_TAG_READY_EVENT         35

static const ULONG CeEntries[WCN_CE_RING_COUNT] = {
    WCN_CE_HTC_TX_ENTRIES, WCN_CE_HTC_RX_ENTRIES, WCN_CE_WMI_RX_ENTRIES,
    WCN_CE_WMI_TX_ENTRIES, WCN_CE_HTT_TX_ENTRIES
};

static const ULONG CeEntrySize[WCN_CE_RING_COUNT] = { 16, 8, 8, 16, 16 };
static const BOOLEAN CeSource[WCN_CE_RING_COUNT] = { TRUE, FALSE, FALSE, TRUE, TRUE };

static
NTSTATUS
WcnCeProgramRing (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Pipe,
    _In_ BOOLEAN StatusRing,
    _In_ PWCN_DMA Dma,
    _In_ ULONG Entries,
    _In_ ULONG EntrySize
    )
{
    ULONG Base;
    ULONG PointerLsb;
    ULONG BaseLsb;
    ULONG BaseMsb;
    ULONG RingId;
    ULONG Misc;
    ULONG Head;
    ULONG Tail;
    ULONG Id;
    ULONG64 Address;
    ULONG ShadowId;
    ULONG EntryDwords;

    if (CeSource[Pipe]) {
        Base = WCN_CE_SRC_BASE + Pipe * WCN_CE_STRIDE;
        BaseLsb = WCN_CE_RING_BASE_LSB;
        BaseMsb = WCN_CE_RING_BASE_MSB;
        RingId = WCN_CE_RING_ID;
        Misc = WCN_CE_RING_MISC;
        Head = WCN_CE_RING_HP;
        Tail = WCN_CE_RING_TP;
        PointerLsb = 0x1C;
        Id = 64 + Pipe;
    } else {
        Base = WCN_CE_DST_BASE + Pipe * WCN_CE_STRIDE;
        BaseLsb = StatusRing ? WCN_CE_DST_STATUS_BASE_LSB : WCN_CE_RING_BASE_LSB;
        BaseMsb = StatusRing ? WCN_CE_DST_STATUS_BASE_MSB : WCN_CE_RING_BASE_MSB;
        RingId = StatusRing ? WCN_CE_DST_STATUS_ID : WCN_CE_RING_ID;
        Misc = StatusRing ? WCN_CE_DST_STATUS_MISC : WCN_CE_RING_MISC;
        Head = StatusRing ? WCN_CE_DST_STATUS_HP : WCN_CE_RING_HP;
        Tail = StatusRing ? WCN_CE_DST_STATUS_TP : WCN_CE_RING_TP;
        PointerLsb = StatusRing ? 0x6C : 0x1C;
        Id = (StatusRing ? 100 : 81) + Pipe;
    }

    Address = Dma->La;
    EntryDwords = EntrySize / sizeof (ULONG);
    ShadowId = (CeSource[Pipe] ? 64 : (StatusRing ? 100 : 81)) + Pipe;
    WcnWrite (Dev, Base + BaseLsb, (ULONG)Address);
    WcnWrite (Dev, Base + BaseMsb,
              (ULONG)((Address >> 32) & 0xFF) | ((EntryDwords * Entries) << 8));
    WcnWrite (Dev, Base + RingId, (StatusRing ? (Id << 8) : 0) | EntryDwords);
    if (CeSource[Pipe] || !StatusRing) {
        WcnWrite (Dev, Base + WCN_CE_RING_ID, EntryDwords);
    }
    Address = Dev->Ce.Rdp.La + ShadowId * sizeof (ULONG);
    WcnWrite (Dev, Base + PointerLsb, (ULONG)Address);
    WcnWrite (Dev, Base + PointerLsb + 4, (ULONG)(Address >> 32));
    WcnWrite (Dev, Base + Head, 0);
    WcnWrite (Dev, Base + Tail, 0);
    WcnWrite (Dev, Base + Misc, WCN_CE_RING_ENABLE | (CeSource[Pipe] ? (1u << 1) : 0));
    return STATUS_SUCCESS;
}

NTSTATUS
WcnCeInitialize (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_CE_RING Ring;
    ULONG Pipe;
    ULONG Index;
    ULONG Size;
    ULONG Base;
    ULONG64 BufferAddress;
    ULONG *Descriptor;
    NTSTATUS Status;

    if (Dev->Ce.Initialized) {
        return STATUS_SUCCESS;
    }

    Status = WcnDmaAllocAligned (Dev, 128 * sizeof (ULONG), 0xFFF, &Dev->Ce.Rdp);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    for (Pipe = 0; Pipe < WCN_CE_RING_COUNT; Pipe++) {
        Ring = &Dev->Ce.Ring[Pipe];
        Ring->Entries = CeEntries[Pipe];
        Ring->EntrySize = CeEntrySize[Pipe];
        Ring->Source = CeSource[Pipe];
        Size = Ring->Entries * Ring->EntrySize;
        Status = WcnDmaAllocAligned (Dev, Size, 0xFFF, &Ring->Desc);
        if (!NT_SUCCESS (Status)) {
            return Status;
        }
        if (!Ring->Source) {
            Ring->BufferCount = Ring->Entries;
            Status = WcnDmaAllocAligned (Dev, Ring->Entries * WCN_CE_RX_BUFFER_SIZE,
                                         0xFFF, &Ring->Buffers);
            if (!NT_SUCCESS (Status)) {
                return Status;
            }
            Status = WcnDmaAllocAligned (Dev, Ring->Entries * 16, 0xFFF, &Ring->Status);
            if (!NT_SUCCESS (Status)) {
                return Status;
            }
            Descriptor = (ULONG *)Ring->Desc.Va;
            for (Index = 0; Index < Ring->Entries; Index++) {
                BufferAddress = Ring->Buffers.La + (ULONG64)Index * WCN_CE_RX_BUFFER_SIZE;
                Descriptor[Index * 2] = (ULONG)BufferAddress;
                Descriptor[Index * 2 + 1] = (ULONG)(BufferAddress >> 32) & 0xFF;
            }
            Base = WCN_CE_DST_BASE + Pipe * WCN_CE_STRIDE;
            WcnWrite (Dev, Base + WCN_CE_DST_RING_CTRL, WCN_CE_RX_BUFFER_SIZE);
            Status = WcnCeProgramRing (Dev, Pipe, FALSE, &Ring->Desc, Ring->Entries, Ring->EntrySize);
            if (!NT_SUCCESS (Status)) {
                return Status;
            }
            Status = WcnCeProgramRing (Dev, Pipe, TRUE, &Ring->Status, Ring->Entries, 16);
            if (!NT_SUCCESS (Status)) {
                return Status;
            }
            /* CE destination descriptors are a host-produced ring; leave two slots empty. */
            Ring->Head = Ring->Entries - 2;
            WcnWrite (Dev, Base + WCN_CE_RING_HP,
                      Ring->Head * (Ring->EntrySize / sizeof (ULONG)));
        } else {
            if (Pipe == 0 || Pipe == 3 || Pipe == 4) {
                Status = WcnDmaAllocAligned (Dev, Ring->Entries * HTC_MAX_CONTROL,
                                             0xFFF, &Ring->Buffers);
                if (!NT_SUCCESS (Status)) {
                    return Status;
                }
            }
            Status = WcnCeProgramRing (Dev, Pipe, FALSE, &Ring->Desc, Ring->Entries, Ring->EntrySize);
            if (!NT_SUCCESS (Status)) {
                return Status;
            }
        }
        Ring->Active = TRUE;
        WcnLog (Dev, "ce%u %s: entries %u desc %u bytes dma 0x%llx", Pipe,
                Ring->Source ? "source" : "destination", Ring->Entries,
                Ring->EntrySize, Ring->Desc.La);
        if (Pipe == 1 || Pipe == 2) {
            ULONG RingBase = WCN_CE_DST_BASE + Pipe * WCN_CE_STRIDE;
            WcnLog (Dev, "ce%u regs: desc %08x %08x id %08x misc %08x hpaddr %08x:%08x hp/tp %08x/%08x",
                    Pipe, WcnRead (Dev, RingBase + WCN_CE_RING_BASE_LSB),
                    WcnRead (Dev, RingBase + WCN_CE_RING_BASE_MSB),
                    WcnRead (Dev, RingBase + WCN_CE_RING_ID),
                    WcnRead (Dev, RingBase + WCN_CE_RING_MISC),
                    WcnRead (Dev, RingBase + 0x1C), WcnRead (Dev, RingBase + 0x20),
                    WcnRead (Dev, RingBase + WCN_CE_RING_HP),
                    WcnRead (Dev, RingBase + WCN_CE_RING_TP));
            WcnLog (Dev, "ce%u status regs: desc %08x %08x id %08x misc %08x hpaddr %08x:%08x hp/tp %08x/%08x",
                    Pipe, WcnRead (Dev, RingBase + WCN_CE_DST_STATUS_BASE_LSB),
                    WcnRead (Dev, RingBase + WCN_CE_DST_STATUS_BASE_MSB),
                    WcnRead (Dev, RingBase + WCN_CE_DST_STATUS_ID),
                    WcnRead (Dev, RingBase + WCN_CE_DST_STATUS_MISC),
                    WcnRead (Dev, RingBase + 0x6C), WcnRead (Dev, RingBase + 0x70),
                    WcnRead (Dev, RingBase + WCN_CE_DST_STATUS_HP),
                    WcnRead (Dev, RingBase + WCN_CE_DST_STATUS_TP));
        }
    }

    Dev->Ce.Initialized = TRUE;
    WcnLog (Dev, "ce: rings initialized, src0 0x%08x dst0 0x%08x stride 0x%x",
            WCN_CE_SRC_BASE, WCN_CE_DST_BASE, WCN_CE_STRIDE);
    return STATUS_SUCCESS;
}

/* Source TP is a hardware consumer measured in DWORDS. Head is the
 * corresponding software-reaped slot. HTC credits are a separate resource. */
static NTSTATUS
WcnCeReapSource (PWCN_DEVICE Dev, ULONG Pipe)
{
    PWCN_CE_RING Ring;ULONG Raw,Other,Words,Used,Advance,Next,Slot,I;BOOLEAN Switch=FALSE;
    if(Pipe>=WCN_CE_RING_COUNT)return STATUS_INVALID_PARAMETER;
    Ring=&Dev->Ce.Ring[Pipe];
    if(!Ring->Source||!Dev->Ce.Rdp.Va||!Ring->Desc.Va||!Ring->Buffers.Va)return STATUS_DEVICE_NOT_READY;
    if(Dev->Ce.SourceFailed)return STATUS_DEVICE_HARDWARE_ERROR;
    if(!Ring->Entries||!Ring->EntrySize||Ring->EntrySize%4||Ring->Head>=Ring->Entries||Ring->Tail>=Ring->Entries)goto Bad;
    Words=Ring->EntrySize/4;
    if(Ring->Entries>MAXULONG/Words||Ring->Buffers.Length<(SIZE_T)Ring->Entries*WCN_CE_RX_BUFFER_SIZE||Ring->Desc.Length<(SIZE_T)Ring->Entries*Ring->EntrySize)goto Bad;
    Raw=Ring->SourceMmio?WcnRead(Dev,WCN_CE_SRC_BASE+Pipe*WCN_CE_STRIDE+WCN_CE_RING_TP):((volatile ULONG*)Dev->Ce.Rdp.Va)[64+Pipe];
    KeMemoryBarrier();
    if(Raw>=Ring->Entries*Words||Raw%Words)goto BadPointer;
    Used=(Ring->Tail+Ring->Entries-Ring->Head)%Ring->Entries;
    Next=(Ring->Tail+1)%Ring->Entries;
    if(!Ring->SourceMmio&&Raw/Words==Ring->Head&&Next==Ring->Head){
        /* Bounded fallback if DMA shadow stalls at a full ring. Once MMIO
         * becomes authoritative, never mix in a stale shadow after wrap. */
        Other=WcnRead(Dev,WCN_CE_SRC_BASE+Pipe*WCN_CE_STRIDE+WCN_CE_RING_TP);
        if(Other!=Raw){Raw=Other;Switch=TRUE;}
        if(Raw>=Ring->Entries*Words||Raw%Words)goto BadPointer;
    }
    Advance=(Raw/Words+Ring->Entries-Ring->Head)%Ring->Entries;
    if(Advance>Used)goto BadPointer; /* Never release an uncompleted/unpublished slot. */
    if(Switch){Ring->SourceMmio=TRUE;WcnLog(Dev,"ce%u: source consumer uses validated MMIO TP after stalled DMA shadow",Pipe);}
    Slot=Ring->Head;
    for(I=0;I<Advance;I++){
        RtlSecureZeroMemory(Ring->Buffers.Va+Slot*WCN_CE_RX_BUFFER_SIZE,WCN_CE_RX_BUFFER_SIZE);
        RtlZeroMemory(Ring->Desc.Va+Slot*Ring->EntrySize,Ring->EntrySize);
        Slot=(Slot+1)%Ring->Entries;
    }
    Ring->Head=Raw/Words;Dev->Ce.TxReclaimed+=Advance;
    return STATUS_SUCCESS;
BadPointer:
    WcnLog(Dev,"ce%u: invalid source consumer TP=%u reaped=%u producer=%u entries=%u entry-bytes=%u; no DMA reuse",Pipe,Raw,Ring->Head,Ring->Tail,Ring->Entries,Ring->EntrySize);
Bad:
    Dev->Ce.SourceFailed=TRUE;
    return STATUS_DEVICE_HARDWARE_ERROR;
}
static NTSTATUS
WcnCeSendSource (PWCN_DEVICE Dev, ULONG Pipe, const UCHAR *Data, ULONG Length)
{
    PWCN_CE_RING Ring;ULONG Slot,Next,*Desc;ULONG64 Address;NTSTATUS Status;
    if(Pipe>=WCN_CE_RING_COUNT||!Data||!Length||Length>WCN_CE_RX_BUFFER_SIZE)return STATUS_INVALID_PARAMETER;
    if(!Dev->Ce.Initialized||Dev->Mhi.SysErr||Dev->Mhi.RddmSaved)return STATUS_DEVICE_NOT_READY;
    Ring=&Dev->Ce.Ring[Pipe];
    Status=WcnCeReapSource(Dev,Pipe);if(!NT_SUCCESS(Status))return Status;
    Slot=Ring->Tail;Next=(Slot+1)%Ring->Entries;
    if(Next==Ring->Head)return STATUS_DEVICE_BUSY;
    RtlCopyMemory(Ring->Buffers.Va+Slot*WCN_CE_RX_BUFFER_SIZE,Data,Length);
    Desc=(ULONG*)(Ring->Desc.Va+Slot*Ring->EntrySize);
    Address=Ring->Buffers.La+(ULONG64)Slot*WCN_CE_RX_BUFFER_SIZE;
    Desc[0]=(ULONG)Address;Desc[1]=(ULONG)((Address>>32)&255)|(Length<<16);Desc[2]=Slot;Desc[3]=0;
    KeMemoryBarrier();Ring->Tail=Next;
    WcnWrite(Dev,WCN_CE_SRC_BASE+Pipe*WCN_CE_STRIDE+WCN_CE_RING_HP,Next*(Ring->EntrySize/4));
    Dev->Ce.TxPackets++;
    return STATUS_SUCCESS;
}

NTSTATUS
WcnCeSendHtc (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    NTSTATUS Status;

    if (!Dev->Ce.Initialized || Length > HTC_MAX_CONTROL) {
        return STATUS_INVALID_PARAMETER;
    }
    Status = WcnCeSendSource (Dev, 0, Data, Length);
    if (NT_SUCCESS (Status)) WcnLog (Dev, "htc tx: %u bytes", Length);
    return Status;
}

/* KMDF worker serializes these sends. Each WMI command consumes target HTC
 * credits; request updates and replenish them from HTC trailer records. */
NTSTATUS
WcnWmiSend (PWCN_DEVICE Dev, ULONG Command, const VOID *Body, ULONG Length)
{
    ULONG Packet[512] = { 0 };
    ULONG Credits;
    NTSTATUS Status;
    if (!Dev->Ce.Initialized || !Dev->Ce.EndpointWmi || !Dev->Ce.WmiCreditInitialized ||
        Dev->Mhi.SysErr || Dev->Mhi.RddmSaved) return STATUS_DEVICE_NOT_READY;
    if (Length > sizeof (Packet) - 12 || (Length & 3) || !Dev->Ce.HtcCreditSize) return STATUS_INVALID_PARAMETER;
    if (Dev->Ce.SourceFailed) return STATUS_DEVICE_HARDWARE_ERROR;
    Credits = (Length + 12 + Dev->Ce.HtcCreditSize - 1) / Dev->Ce.HtcCreditSize;
    if (Dev->Ce.WmiCredits < Credits) return STATUS_DEVICE_BUSY;
    Packet[0] = ((Length + 4) << 16) | (1u << 8) | Dev->Ce.EndpointWmi;
    Packet[1] = (Dev->Ce.WmiSequence & 0xff) << 8;
    Packet[2] = Command;
    if (Length) RtlCopyMemory (Packet + 3, Body, Length);
    Status = WcnCeSendSource (Dev, 3, (const UCHAR *)Packet, Length + 12);
    if (Command == 0x5009) RtlSecureZeroMemory (Packet, sizeof (Packet));
    if (NT_SUCCESS (Status)) {
        Dev->Ce.WmiCredits -= Credits;
        Dev->Ce.WmiSequence++;
    }
    return Status;
}

/* HTT endpoints negotiate disabled credit flow. Preserve one CE4 buffer
 * per packet and publish descriptors only after the payload is complete. */
NTSTATUS
WcnHttSend (PWCN_DEVICE Dev, const VOID *Body, ULONG Length)
{
    ULONG Packet[64] = { 0 };
    NTSTATUS Status;
    if (!Dev->Ce.Initialized || !Dev->Ce.EndpointHtt || Dev->Mhi.SysErr || Dev->Mhi.RddmSaved) return STATUS_DEVICE_NOT_READY;
    if (!Length || Length > sizeof (Packet) - 8 || (Length & 3)) return STATUS_INVALID_PARAMETER;
    Packet[0] = (Length << 16) | Dev->Ce.EndpointHtt;
    Packet[1] = (Dev->Ce.HttSequence & 0xff) << 8;
    RtlCopyMemory (Packet + 2, Body, Length);
    Status = WcnCeSendSource (Dev, 4, (const UCHAR *)Packet, Length + 8);
    if (NT_SUCCESS (Status)) Dev->Ce.HttSequence++;
    return Status;
}

static BOOLEAN
WcnHtcCredits (PWCN_DEVICE Dev, const UCHAR *Data, ULONG Length)
{
    ULONG Offset = 0, Size, Index;
    /* Validate the entire trailer before accepting any credits. */
    while (Offset < Length) {
        if (Length - Offset < 4) return FALSE;
        Size = Data[Offset + 1];
        if (Size > Length - Offset - 4 || (Data[Offset] == 1 && (Size & 3))) return FALSE;
        Offset += 4 + Size;
    }
    for (Offset = 0; Offset < Length; Offset += 4 + Size) {
        Size = Data[Offset + 1];
        if (Data[Offset] != 1) continue;
        for (Index = Offset + 4; Index < Offset + 4 + Size; Index += 4) {
            if (Data[Index] == Dev->Ce.EndpointWmi && Dev->Ce.EndpointWmi) {
                Dev->Ce.WmiCredits += Data[Index + 1];
                /* Only two credits were negotiated for WMI. */
                if (Dev->Ce.WmiCredits > 2) Dev->Ce.WmiCredits = 2;
            }
        }
    }
    return TRUE;
}

static
NTSTATUS
WcnHtcSendConnect (
    _In_ PWCN_DEVICE Dev,
    _In_ USHORT Service
    )
{
    UCHAR Packet[HTC_HEADER_SIZE + 8] = { 0 };
    ULONG Flags = HTC_FLAG_DISABLE_CREDIT;
    ULONG Status;

    *(ULONG *)(Packet + 0) = 8u << 16;
    *(ULONG *)(Packet + 4) = 0;
    *(ULONG *)(Packet + 8) = HTC_MSG_CONNECT_SERVICE | ((ULONG)Service << 16);
    if (Service == HTC_SVC_WMI_CONTROL) {
        /* WMI control keeps HTC credit flow enabled; PEACH advertises two
           startup credits and expects them allocated in this request. */
        Flags = 2u << 8;
    }
    *(ULONG *)(Packet + 12) = Flags;
    Status = WcnCeSendHtc (Dev, Packet, sizeof (Packet));
    if (NT_SUCCESS ((NTSTATUS)Status)) {
        Dev->Ce.HtcPendingService = Service;
        WcnLog (Dev, "htc: connect service 0x%04x sent", Service);
    }
    return (NTSTATUS)Status;
}

static
VOID
WcnHtcAdvance (
    _In_ PWCN_DEVICE Dev
    )
{
    NTSTATUS Status;

    switch (Dev->Ce.HtcState) {
    case 1:
        /* The pseudo control service is endpoint 0 and has no response. */
        Dev->Ce.HtcState = 2;
        Status = WcnHtcSendConnect (Dev, HTC_SVC_WMI_CONTROL);
        if (!NT_SUCCESS (Status)) {
            WcnLog (Dev, "htc: WMI connect send failed 0x%08x", Status);
        }
        break;
    case 4:
        Status = WcnHtcSendConnect (Dev, HTC_SVC_HTT_DATA);
        if (!NT_SUCCESS (Status)) {
            WcnLog (Dev, "htc: HTT connect send failed 0x%08x", Status);
        }
        break;
    case 6: {
        UCHAR Packet[HTC_HEADER_SIZE + 12] = { 0 };
        *(ULONG *)(Packet + 0) = 12u << 16;
        *(ULONG *)(Packet + 8) = HTC_MSG_SETUP_COMPLETE_EXT;
        Status = WcnCeSendHtc (Dev, Packet, sizeof (Packet));
        if (NT_SUCCESS (Status)) {
            Dev->Ce.HtcState = 7;
            WcnLog (Dev, "htc: setup-complete-ext sent");
        } else {
            WcnLog (Dev, "htc: setup-complete send failed 0x%08x", Status);
        }
        break;
    }
    default:
        break;
    }
}

static
VOID
WcnWmiSendInitPeach (
    _In_ PWCN_DEVICE Dev
    )
{
    UCHAR Packet[8 + 4 + 32 + 292 + 4] = { 0 };
    ULONG *P = (ULONG *)(Packet + 8);
    ULONG *R;
    ULONG Offset = 8 + 4 + 32;
    ULONG ChainMask = Dev->Ce.NumRfChains ? ((1u << Dev->Ce.NumRfChains) - 1) : 1;
    NTSTATUS Status;

    Status = WcnDpPrepareWmi (Dev);
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "wmi: init blocked by DP setup failure 0x%08x", Status);
        return;
    }

    *(ULONG *)(Packet + 0) = ((ULONG)(4 + 32 + 292 + 4) << 16) | Dev->Ce.EndpointWmi;
    P[0] = WMI_CMD_HDR_INIT;
    P[1] = (WMI_TAG_INIT_CMD << 16) | 28;
    P[9] = (WMI_TAG_RESOURCE_CONFIG << 16) | 288;
    R = (ULONG *)(Packet + Offset + 4);
    R[0] = 4; R[1] = 16; R[2] = 3; R[3] = 3; R[4] = 2; R[5] = 32;
    R[6] = 16; R[7] = ChainMask; R[8] = ChainMask;
    R[9] = 100; R[10] = 100; R[11] = 100; R[12] = 40;
    R[13] = 2; /* Ethernet RX, converted to Native802.11 at the OS boundary */ R[14] = 4; R[15] = 3; R[16] = 3; R[17] = 8;
    R[21] = 1024; R[27] = 2; R[28] = 0x400; R[29] = 0xa;
    R[30] = 1; R[31] = 8; R[32] = 2; R[33] = 0x20; R[34] = 0x16;
    R[43] = 0x40020; R[53] = 1; R[56] = 1;
    R[67] = 0x40200 | ((ULONG)Dev->Ce.PeerMetadataVersion << 4);
    *(ULONG *)(Packet + Offset + 4 + 288) = WMI_TAG_ARRAY_STRUCT << 16;
    WcnLog (Dev, "wmi: init TLVs tag=%u len=%u resource-tag=%u resource-len=%u host-mem=0",
            P[1] >> 16, P[1] & 0xffff, P[9] >> 16, P[9] & 0xffff);
    WcnLogHex (Dev, "wmi init header ", Packet, 64);
    Status = WcnWmiSend (Dev, WMI_CMD_HDR_INIT, Packet + 12, sizeof (Packet) - 12);
    if (NT_SUCCESS (Status)) {
        Dev->Ce.WmiInitSent = TRUE;
        WcnLog (Dev, "wmi: init command sent (%u bytes)", (ULONG)sizeof (Packet));
    } else {
        WcnLog (Dev, "wmi: init send failed 0x%08x", Status);
    }
}

/* Same post-READY order as ath12k_core_start: DBS mode, then HTT version.
 * Only the control handshake is enabled, not HTT data-ring advertisement. */
static VOID
WcnCeAdvanceReady (PWCN_DEVICE Dev)
{
    NTSTATUS Status;
    if (!Dev->Ce.WmiReady || Dev->Mhi.SysErr || Dev->Mhi.RddmSaved) return;
    if (!Dev->Ce.HwModeSent) {
        ULONG Packet[7] = { 0 };
        Packet[0] = (20u << 16) | Dev->Ce.EndpointWmi;
        Packet[2] = 0x401e; /* WMI_PDEV_SET_HW_MODE_CMDID */
        Packet[3] = (515u << 16) | 12;
        Packet[4] = 0; /* WMI_PDEV_ID_SOC */
        Packet[5] = 1; /* WMI_HOST_HW_MODE_DBS */
        Status = WcnWmiSend (Dev, 0x401e, Packet + 3, sizeof (Packet) - 12);
        if (!NT_SUCCESS (Status)) return;
        Dev->Ce.HwModeSent = TRUE;
        WcnLog (Dev, "wmi: SET_HW_MODE DBS sent");
    }
    if (!Dev->Ce.HttVersionSent) {
        ULONG Packet[4] = { 0 };
        Packet[0] = (8u << 16) | Dev->Ce.EndpointHtt;
        Packet[2] = 0; /* HTT_H2T_MSG_TYPE_VERSION_REQ */
        Packet[3] = 5u | (4u << 8) | (2u << 16); /* TCL metadata v2 option */
        Status = WcnHttSend (Dev, Packet + 2, sizeof (Packet) - 8);
        if (!NT_SUCCESS (Status)) return;
        Dev->Ce.HttVersionSent = TRUE;
        WcnLog (Dev, "htt: version request sent (TCL metadata v2)");
    }
}

static
VOID
WcnHtcReceive (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Info;
    ULONG PayloadLength;
    ULONG Message;

    if (Length < HTC_HEADER_SIZE) {
        WcnLog (Dev, "htc: short packet (%u bytes)", Length);
        return;
    }
    Info = *(const ULONG *)Data;
    PayloadLength = (Info >> 16) & 0xFFFF;
    if (PayloadLength > Length - HTC_HEADER_SIZE) {
        WcnLog (Dev, "htc: payload length %u exceeds CE buffer (%u)", PayloadLength, Length);
        return;
    }
    if (((Info >> 8) & 2) != 0) {
        ULONG TrailerLength = *(const ULONG *)(Data + 4) & 0xff;
        if (TrailerLength < 4 || TrailerLength > PayloadLength) {
            WcnLog (Dev, "htc: invalid trailer length %u", TrailerLength);
            return;
        }
        if (!WcnHtcCredits (Dev, Data + HTC_HEADER_SIZE + PayloadLength - TrailerLength, TrailerLength)) {
            WcnLog (Dev, "htc: malformed credit trailer rejected");
            return;
        }
        PayloadLength -= TrailerLength;
    }
    if ((Info & 0xFF) != 0) {
        ULONG Endpoint = Info & 0xff;
        if (Endpoint == Dev->Ce.EndpointWmi && Dev->Ce.EndpointWmi) {
            WcnWmiReceive (Dev, Data + HTC_HEADER_SIZE, PayloadLength);
        } else if (Endpoint == Dev->Ce.EndpointHtt && Dev->Ce.EndpointHtt && PayloadLength >= 4) {
            ULONG Version = *(const ULONG *)(Data + HTC_HEADER_SIZE);
            WcnStage7Htt (Dev, Data + HTC_HEADER_SIZE, PayloadLength);
            if ((Version & 0xff) == 0 && Dev->Ce.HttVersionSent) {
                Dev->Ce.HttMajor = (UCHAR)(Version >> 16);
                Dev->Ce.HttMinor = (UCHAR)(Version >> 8);
                Dev->Ce.HttVersionReceived = TRUE;
                WcnLog (Dev, "htt: target version %u.%u%s", Dev->Ce.HttMajor, Dev->Ce.HttMinor,
                        Dev->Ce.HttMajor == 3 ? " (supported)" : " (unsupported, stage 5 incomplete)");
            } else {
                WcnLogHex (Dev, "htt: event ", Data + HTC_HEADER_SIZE, PayloadLength);
            }
        }
        return;
    }
    if (PayloadLength < 4) {
        return;
    }
    Message = *(const ULONG *)(Data + HTC_HEADER_SIZE) & 0xFFFF;
    if (Message == HTC_MSG_READY && PayloadLength >= 8) {
        ULONG CreditCount = (*(const ULONG *)(Data + HTC_HEADER_SIZE) >> 16) & 0xFFFF;
        ULONG CreditSize = *(const ULONG *)(Data + HTC_HEADER_SIZE + 4) & 0xFFFF;
        WcnLog (Dev, "htc: target ready credits=%u credit-size=%u", CreditCount, CreditSize);
        Dev->Ce.HtcCreditSize = CreditSize;
        Dev->Ce.HtcReady = TRUE;
        if (Dev->Ce.HtcState == 0) {
            Dev->Ce.HtcState = 1;
            WcnHtcAdvance (Dev);
        }
    } else if (Message == HTC_MSG_CONNECT_RESPONSE && PayloadLength >= 12) {
        ULONG Service = (*(const ULONG *)(Data + HTC_HEADER_SIZE) >> 16) & 0xFFFF;
        ULONG Flags = *(const ULONG *)(Data + HTC_HEADER_SIZE + 4);
        ULONG Response = Flags & 0xFF;
        ULONG Endpoint = (Flags >> 8) & 0xFF;
        ULONG MaxMessage = Flags >> 16;
        WcnLog (Dev, "htc: service 0x%04x response status %u eid %u max %u",
                Service, Response, Endpoint, MaxMessage);
        if (Response != 0 || MaxMessage == 0) {
            WcnLog (Dev, "htc: service 0x%04x rejected", Service);
            return;
        }
        if (Service == HTC_SVC_WMI_CONTROL) {
            Dev->Ce.EndpointWmi = (UCHAR)Endpoint;
            Dev->Ce.WmiCredits = 2;
            Dev->Ce.WmiCreditInitialized = TRUE;
            Dev->Ce.HtcState = 4;
            WcnHtcAdvance (Dev);
        } else if (Service == HTC_SVC_HTT_DATA) {
            Dev->Ce.EndpointHtt = (UCHAR)Endpoint;
            Dev->Ce.HtcState = 6;
            WcnHtcAdvance (Dev);
        }
    } else {
        WcnLogHex (Dev, "htc: control ", Data + HTC_HEADER_SIZE, PayloadLength);
    }
}

VOID
WcnCeService (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_CE_RING Ring;
    ULONG Pipe;
    ULONG Base;
    ULONG HwHead;
    ULONG *Status;
    ULONG Length;
    ULONG Index;
    const UCHAR *Packet;

    if (!Dev->Ce.Initialized || Dev->Mhi.SysErr || Dev->Mhi.RddmSaved) {
        return;
    }
    for (Pipe = 0; Pipe < WCN_CE_RING_COUNT; Pipe++) {
        if (Dev->Ce.Ring[Pipe].Source && !NT_SUCCESS(WcnCeReapSource(Dev, Pipe))) return;
    }
    if (++Dev->Ce.ServiceTicks % 50 == 0) {
        WcnLog(Dev,"ce tx: source3 reaped=%u producer=%u dma-TP=%u MMIO-mode=%u credits=%u DMA-reclaimed-total=%u",
               Dev->Ce.Ring[3].Head,Dev->Ce.Ring[3].Tail,((volatile ULONG*)Dev->Ce.Rdp.Va)[67],Dev->Ce.Ring[3].SourceMmio,Dev->Ce.WmiCredits,Dev->Ce.TxReclaimed);
        ULONG Dst1 = WCN_CE_DST_BASE + WCN_CE_STRIDE;
        ULONG Dst2 = WCN_CE_DST_BASE + 2 * WCN_CE_STRIDE;
        volatile ULONG *Rdp = (volatile ULONG *)Dev->Ce.Rdp.Va;
        WcnLog (Dev, "ce poll: rx %u, ce1 tail/status %u/%u mmio %08x/%08x, ce2 %u/%u mmio %08x/%08x",
                Dev->Ce.RxPackets, Rdp[82], Rdp[101],
                WcnRead (Dev, Dst1 + WCN_CE_RING_TP), WcnRead (Dev, Dst1 + WCN_CE_DST_STATUS_HP),
                Rdp[83], Rdp[102], WcnRead (Dev, Dst2 + WCN_CE_RING_TP),
                WcnRead (Dev, Dst2 + WCN_CE_DST_STATUS_HP));
    }
    for (Pipe = 1; Pipe <= 2; Pipe++) {
        Ring = &Dev->Ce.Ring[Pipe];
        Base = WCN_CE_DST_BASE + Pipe * WCN_CE_STRIDE;
        HwHead = ((volatile ULONG *)Dev->Ce.Rdp.Va)[(100 + Pipe)] % (Ring->Entries * 4);
        while (Ring->StatusHead * 4 != HwHead) {
            Status = (ULONG *)(Ring->Status.Va + Ring->StatusHead * 16);
            Length = (Status[0] >> 16) & 0xFFFF;
            Index = Ring->BufferIndex;
            Packet = Ring->Buffers.Va + Index * WCN_CE_RX_BUFFER_SIZE;
            if (Length == 0 || Length > WCN_CE_RX_BUFFER_SIZE) {
                WcnLog (Dev, "ce%u: invalid rx length %u status 0x%08x", Pipe, Length, Status[0]);
                Length = 0;
            }
            if (Length != 0) {
                Dev->Ce.RxPackets++;
                /* Beacons arrive frequently. Stage6 deduplicates/logs APs;
                 * avoid synchronous hex/file logging for every beacon. */
                if (Length < 12 || ((*(const ULONG *)(Packet + 8) & 0xffffff) != 0x7001 &&
                                    (*(const ULONG *)(Packet + 8) & 0xffffff) != 0x1d011)) {
                    WcnLog (Dev, "ce%u rx #%u: %u bytes status %08x %08x", Pipe,
                            Dev->Ce.RxPackets - 1, Length, Status[0], Status[1]);
                    /* Complete the small VDEV_START reply, including newer trailing
                     * fields. Other event dumps retain the old 64-byte cap. */
                    ULONG HexLimit = Length >= 12 &&
                        ((*(const ULONG *)(Packet + 8) & 0xffffff) == 0x5001) ? 128u : 64u;
                    WcnLogHex (Dev, "ce rx ", Packet, Length > HexLimit ? HexLimit : Length);
                }
                WcnHtcReceive (Dev, Packet, Length);
            }
            Ring->StatusHead = (Ring->StatusHead + 1) % Ring->Entries;
            Ring->BufferIndex = (Ring->BufferIndex + 1) % Ring->Entries;
            Ring->Head = (Ring->Head + 1) % Ring->Entries;
            WcnWrite (Dev, Base + WCN_CE_RING_HP,
                      Ring->Head * (Ring->EntrySize / sizeof (ULONG)));
            WcnWrite (Dev, Base + WCN_CE_DST_STATUS_TP, Ring->StatusHead * 4);
            HwHead = ((volatile ULONG *)Dev->Ce.Rdp.Va)[(100 + Pipe)] % (Ring->Entries * 4);
        }
    }
    if (Dev->Ce.WmiServiceReadyExt2 && !Dev->Ce.WmiInitSent && !Dev->Dp.Failed) WcnWmiSendInitPeach (Dev);
    WcnCeAdvanceReady (Dev);
}

VOID
WcnWmiReceive (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG EventId;
    ULONG Header;
    ULONG Offset;
    ULONG TlvHeader;
    ULONG TlvLength;
    const UCHAR *TlvValue;

    if (Length < 4) {
        return;
    }
    Header = *(const ULONG *)Data;
    EventId = Header & 0x00FFFFFF;
    if (EventId == 0x1d011) {
        Dev->Scan.DiagnosticEvents++;
        if (Dev->Scan.DiagnosticEvents <= 2 || Dev->Scan.DiagnosticEvents % 100 == 0) {
            WcnLog (Dev, "stage6: firmware diagnostic events=%u (rate-limited; not an AP event)", Dev->Scan.DiagnosticEvents);
        }
        if (Dev->Scan.DiagnosticEvents <= 2 || (Dev->Scan.RuntimeScan && Dev->Scan.DiagDumps < 2)) {
            if (Dev->Scan.RuntimeScan) Dev->Scan.DiagDumps++;
            WcnLogHex (Dev, "stage6 diag payload ", Data, Length > 160 ? 160 : Length);
        }
        return;
    }
WcnStage6Receive (Dev, EventId, Data + 4, Length - 4);
    WcnStage7Receive (Dev, EventId, Data + 4, Length - 4);
    if (EventId == 0x7001) return;
    WcnLog (Dev, "wmi: event id 0x%08x length %u", EventId, Length);
    TlvLength = 0;
    for (Offset = 4; Offset + 4 <= Length; Offset += 4 + TlvLength) {
        TlvHeader = *(const ULONG *)(Data + Offset);
        TlvLength = TlvHeader & 0xFFFF;
        TlvValue = Data + Offset + 4;
        WcnLog (Dev, "wmi: tlv tag %u length %u", TlvHeader >> 16, TlvLength);
        if (Offset + 4 + TlvLength > Length) break;
        if (EventId == WMI_SERVICE_READY_EVENT && (TlvHeader >> 16) == 32 && TlvLength >= 40) {
            Dev->Ce.NumRfChains = TlvValue[36];
            WcnLog (Dev, "wcn7861: target rf chains %u", Dev->Ce.NumRfChains);
            if (TlvLength >= 76) {
                WcnLog (Dev, "wcn7861: target host memory requests %u",
                        *(const ULONG *)(TlvValue + 72));
            }
        }
        if (EventId == WMI_SERVICE_READY_EXT2_EVENT && (TlvHeader >> 16) == 820 && TlvLength >= 32) {
            Dev->Ce.PeerMetadataVersion = (UCHAR)(*(const ULONG *)(TlvValue + 28) & 0x3);
            WcnLog (Dev, "wcn7861: peer metadata version %u", Dev->Ce.PeerMetadataVersion);
        }
    }
    if (EventId == WMI_SERVICE_READY_EVENT) {
        Dev->Ce.WmiServiceReady = TRUE;
        WcnLog (Dev, "wmi: service-ready event received (%u bytes)", Length);
    } else if (EventId == WMI_SERVICE_READY_EXT_EVENT) {
        Dev->Ce.WmiServiceReadyExt = TRUE;
        WcnLog (Dev, "wcn7861: service-ready-ext received");
    } else if (EventId == WMI_SERVICE_READY_EXT2_EVENT) {
        Dev->Ce.WmiServiceReadyExt2 = TRUE;
        WcnLog (Dev, "wcn7861: service-ready-ext2 received");
        if (Dev->Ce.WmiServiceReady && !Dev->Ce.WmiInitSent) {
            WcnWmiSendInitPeach (Dev);
        }
    } else if (EventId == 0x4010) {
        Dev->Ce.HwModeResponseReceived = TRUE;
        WcnLogHex (Dev, "wmi: SET_HW_MODE response (raw, status not interpreted) ", Data + 4, Length - 4);
    } else if (EventId == WMI_READY_EVENT) {
        if (!Dev->Ce.WmiInitSent || !Dev->Dp.ReadyForWmi || Dev->Ce.WmiReady) return;
        if (Length >= 4 + 4 + 52) {
            ULONG Tlv = *(const ULONG *)(Data + 4);
            if ((Tlv >> 16) != WMI_TAG_READY_EVENT || (Tlv & 0xFFFF) < 52 ||
                (Tlv & 0xFFFF) > Length - 8) {
                WcnLog (Dev, "wmi: ready event has unexpected TLV 0x%08x", Tlv);
                return;
            }
            if (*(const ULONG *)(Data + 8 + 32) != 0) {
                WcnLog (Dev, "wmi: firmware ready status failed 0x%08x",
                        *(const ULONG *)(Data + 8 + 32));
                return;
            }
            RtlCopyMemory (Dev->Ce.MacAddress, Data + 4 + 4 + 24, sizeof (Dev->Ce.MacAddress));
            if ((Dev->Ce.MacAddress[0] & 1) ||
                (Dev->Ce.MacAddress[0] | Dev->Ce.MacAddress[1] | Dev->Ce.MacAddress[2] |
                 Dev->Ce.MacAddress[3] | Dev->Ce.MacAddress[4] | Dev->Ce.MacAddress[5]) == 0) {
                WcnLog (Dev, "wmi: invalid ready MAC, stage 5 not complete");
                return;
            }
            Dev->Ce.WmiReady = TRUE;
            Dev->Dp.ReadyAt = WcnNowMs ();
            WcnLog (Dev, "wmi: ready event MAC %02x:%02x:%02x:%02x:%02x:%02x",
                    Dev->Ce.MacAddress[0], Dev->Ce.MacAddress[1], Dev->Ce.MacAddress[2],
                    Dev->Ce.MacAddress[3], Dev->Ce.MacAddress[4], Dev->Ce.MacAddress[5]);
        } else {
            WcnLog (Dev, "wmi: ready event received but payload is short (%u bytes)", Length);
        }
    }
}
