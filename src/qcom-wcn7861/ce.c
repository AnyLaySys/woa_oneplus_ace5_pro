/* WCN7861 PEACH CE SRNG setup and HTC/WMI event decoding. */
#include "wcn7861.h"
#include "hotspot.h"

#define HTC_HEADER_SIZE             8
#define HTC_MSG_READY               1
#define HTC_MSG_CONNECT_SERVICE     2
#define HTC_MSG_CONNECT_RESPONSE    3
#define HTC_MSG_SETUP_COMPLETE_EXT  5
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

    }

    Dev->Ce.Initialized = TRUE;

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
    if(Switch){Ring->SourceMmio=TRUE;}
    Slot=Ring->Head;
    for(I=0;I<Advance;I++){
        RtlSecureZeroMemory(Ring->Buffers.Va+Slot*WCN_CE_RX_BUFFER_SIZE,WCN_CE_RX_BUFFER_SIZE);
        RtlZeroMemory(Ring->Desc.Va+Slot*Ring->EntrySize,Ring->EntrySize);
        Slot=(Slot+1)%Ring->Entries;
    }
    Ring->Head=Raw/Words;
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
    NTSTATUS Status;

    *(ULONG *)(Packet + 0) = 8u << 16;
    *(ULONG *)(Packet + 8) = HTC_MSG_CONNECT_SERVICE | ((ULONG)Service << 16);
    if (Service == HTC_SVC_WMI_CONTROL) {
        /* WMI control keeps HTC credit flow enabled; PEACH advertises two
           startup credits and expects them allocated in this request. */
        Flags = 2u << 8;
    }
    *(ULONG *)(Packet + 12) = Flags;
    Status = WcnCeSendHtc (Dev, Packet, sizeof (Packet));

    return Status;
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

        } else {
            WcnLog (Dev, "htc: setup-complete send failed 0x%08x", Status);
        }
        break;
    }

    }
}

static
VOID
WcnWmiSendInitPeach (
    _In_ PWCN_DEVICE Dev
    )
{
    ULONG Packet[(32 + 292 + 4) / 4] = { 0 };
    ULONG *R = Packet + 9;
    ULONG ChainMask = Dev->Ce.NumRfChains ? ((1u << Dev->Ce.NumRfChains) - 1) : 1;
    NTSTATUS Status;

    Status = WcnDpPrepareWmi (Dev);
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "wmi: init blocked by DP setup failure 0x%08x", Status);
        return;
    }

    Packet[0] = (WMI_TAG_INIT_CMD << 16) | 28;
    Packet[8] = (WMI_TAG_RESOURCE_CONFIG << 16) | 288;
    R[0] = 4; R[1] = 16; R[2] = 3; R[3] = 3; R[4] = 2; R[5] = 32;
    R[6] = 16; R[7] = ChainMask; R[8] = ChainMask;
    R[9] = 100; R[10] = 100; R[11] = 100; R[12] = 40;
    R[13] = 2; /* Ethernet RX, converted to Native802.11 at the OS boundary */ R[14] = 4; R[15] = 3; R[16] = 3; R[17] = 8;
    R[21] = 1024; R[27] = 2; R[28] = 0x400; R[29] = 0xa;
    R[30] = WcnService(Dev,363)?2:1; R[31] = 8; R[32] = 2; R[33] = 0x20; R[34] = 0x16;
    R[43] = 0x40020; R[53] = 1; R[56] = 1;
    R[67] = 0x40200 | ((ULONG)Dev->Ce.PeerMetadataVersion << 4);
    Packet[81] = WMI_TAG_ARRAY_STRUCT << 16;

    Status = WcnWmiSend (Dev, WMI_CMD_HDR_INIT, Packet, sizeof (Packet));
    if (NT_SUCCESS (Status)) {
        Dev->Ce.WmiInitSent = TRUE;

    } else {
        WcnLog (Dev, "wmi: init send failed 0x%08x", Status);
    }
}

/* Same post-READY order as ath12k_core_start: DBS mode, then HTT version. */
static VOID
WcnCeAdvanceReady (PWCN_DEVICE Dev)
{
    NTSTATUS Status;
    if (!Dev->Ce.WmiReady || Dev->Mhi.SysErr || Dev->Mhi.RddmSaved) return;
    if (!Dev->Ce.HwModeSent) {
        /* WMI_PDEV_SET_HW_MODE: SOC pdev0, WMI_HOST_HW_MODE_DBS, no mappings. */
        ULONG Packet[] = { (515u << 16) | 12, 0, 1, 0 };
        Status = WcnWmiSend (Dev, 0x401e, Packet, sizeof (Packet));
        if (!NT_SUCCESS (Status)) return;
        Dev->Ce.HwModeSent = TRUE;

    }
    if (!Dev->Ce.HttVersionSent) {
        /* HTT version request, TCL metadata v2; WcnHttSend owns the HTC header. */
        ULONG Packet[] = { 0, 5u | (4u << 8) | (2u << 16) };
        Status = WcnHttSend (Dev, Packet, sizeof (Packet));
        if (!NT_SUCCESS (Status)) return;
        Dev->Ce.HttVersionSent = TRUE;

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

        return;
    }
    Info = *(const ULONG *)Data;
    PayloadLength = (Info >> 16) & 0xFFFF;
    if (PayloadLength > Length - HTC_HEADER_SIZE) {

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
            WcnHotspotHtt(Dev,Data+HTC_HEADER_SIZE,PayloadLength);
            if ((Version & 0xff) == 0 && Dev->Ce.HttVersionSent) {
                Dev->Ce.HttMajor = (UCHAR)(Version >> 16);
                Dev->Ce.HttVersionReceived = TRUE;

            }
        }
        return;
    }
    if (PayloadLength < 4) {
        return;
    }
    Message = *(const ULONG *)(Data + HTC_HEADER_SIZE) & 0xFFFF;
    if (Message == HTC_MSG_READY && PayloadLength >= 8) {
        ULONG CreditSize = *(const ULONG *)(Data + HTC_HEADER_SIZE + 4) & 0xFFFF;

        Dev->Ce.HtcCreditSize = CreditSize;
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
    if(EventId==0x1d011)return;
    WcnCapabilitiesEvent(Dev,EventId,Data+4,Length-4);
    WcnStage6Receive (Dev, EventId, Data + 4, Length - 4);
    WcnStage7Receive (Dev, EventId, Data + 4, Length - 4);
    WcnHotspotWmi(Dev,EventId,Data+4,Length-4);
    if (EventId == 0x7001 || EventId == 0x7006) return;

    TlvLength = 0;
    for (Offset = 4; Offset + 4 <= Length; Offset += 4 + TlvLength) {
        TlvHeader = *(const ULONG *)(Data + Offset);
        TlvLength = TlvHeader & 0xFFFF;
        TlvValue = Data + Offset + 4;

        if (Offset + 4 + TlvLength > Length) break;
        if (EventId == WMI_SERVICE_READY_EVENT && (TlvHeader >> 16) == 32 && TlvLength >= 40) {
            Dev->Ce.NumRfChains = TlvValue[36];
            if(TlvLength>=52){Dev->Ce.HtCap=*(const ULONG*)(TlvValue+40);Dev->Ce.VhtCap=*(const ULONG*)(TlvValue+44);
                }

        }
        if (EventId == WMI_SERVICE_READY_EXT2_EVENT && (TlvHeader >> 16) == 820 && TlvLength >= 32) {
            Dev->Ce.PeerMetadataVersion = (UCHAR)(*(const ULONG *)(TlvValue + 28) & 0x3);

        }
    }
    if (EventId == WMI_SERVICE_READY_EXT2_EVENT) {
        Dev->Ce.WmiServiceReadyExt2 = TRUE;

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

        }
    }
}
