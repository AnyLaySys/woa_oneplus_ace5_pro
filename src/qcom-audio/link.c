#include "sia9177.h"

#define SMEM_PHYS               0x81D00000ull
#define SMEM_SIZE               0x00200000u
#define SMEM_TOC_MAGIC          0x434F5424u
#define SMEM_PART_MAGIC         0x54525024u
#define SMEM_CANARY             0xA5A5u
#define TCSR_MUTEX_PHYS         0x01F43000ull
#define IPCC_PHYS               0x00406000ull
#define IPCC_SEND_ID            0x0Cu
#define IPCC_CLIENT_LPASS       3u
#define IPCC_SIGNAL_GLINK       0u
#define IPCC_SIGNAL_SMP2P       2u

#define SMP2P_MAGIC             0x504D5324u
#define SMP2P_IN_ITEM           443u
#define SMP2P_OUT_ITEM          429u
#define SMP2P_ITEM_SIZE         0x158u

#define GLINK_DESC_ITEM         478u
#define GLINK_TX_ITEM           479u
#define GLINK_RX_ITEM           480u
#define GLINK_CMD_VERSION       0u
#define GLINK_CMD_VERSION_ACK   1u
#define GLINK_CMD_OPEN          2u
#define GLINK_CMD_CLOSE         3u
#define GLINK_CMD_OPEN_ACK      4u
#define GLINK_CMD_INTENT        5u
#define GLINK_CMD_RX_DONE       6u
#define GLINK_CMD_RX_INTENT_REQ 7u
#define GLINK_CMD_INTENT_REQ_ACK 8u
#define GLINK_CMD_TX_DATA       9u
#define GLINK_CMD_CLOSE_ACK     11u
#define GLINK_CMD_TX_DATA_CONT  12u
#define GLINK_CMD_READ_NOTIF    13u
#define GLINK_CMD_RX_DONE_REUSE 14u
#define GLINK_CMD_SIGNALS       15u
#define GLINK_FEATURES          1u
#define GLINK_AUDIO_LCID        1u
#define GLINK_LOCAL_INTENTS     16u
#define GLINK_LOCAL_INTENT_SIZE 2048u

#define APM_CMD_GET_SPF_STATE   0x01001021u
#define APM_CMD_RSP_SPF_STATE   0x02001007u

static
USHORT
Rd16 (
    _In_ PUCHAR Base,
    _In_ ULONG Offset
    )
{
    return READ_REGISTER_USHORT ((volatile USHORT *)(Base + Offset));
}

static
VOID
Wr16 (
    _In_ PUCHAR Base,
    _In_ ULONG Offset,
    _In_ USHORT Value
    )
{
    WRITE_REGISTER_USHORT ((volatile USHORT *)(Base + Offset), Value);
}

static
BOOLEAN
SiaSmemLock (
    _In_ PSIA_LINK Link
    )
{
    ULONG Retry;

    for (Retry = 0; Retry < 100000; Retry++) {
        SiaWrite32 (Link->Mutex, 0, 1);
        KeMemoryBarrier ();
        if (SiaRead32 (Link->Mutex, 0) == 1) {
            return TRUE;
        }
        KeStallExecutionProcessor (1);
    }
    return FALSE;
}

static
VOID
SiaSmemUnlock (
    _In_ PSIA_LINK Link
    )
{
    KeMemoryBarrier ();
    SiaWrite32 (Link->Mutex, 0, 0);
}

static
PUCHAR
SiaSmemFind (
    _In_ PSIA_LINK Link,
    _In_ ULONG Wanted,
    _Out_opt_ ULONG *Size
    )
{
    ULONG Free = SiaRead32 (Link->Smem, Link->Partition + 12);
    ULONG Cursor = 32;

    while (Cursor + 16 <= Free && Cursor + 16 <= Link->PartitionSize) {
        ULONG Base = Link->Partition + Cursor;
        ULONG ItemSize = SiaRead32 (Link->Smem, Base + 4);
        USHORT PadData = Rd16 (Link->Smem, Base + 8);
        USHORT PadHdr = Rd16 (Link->Smem, Base + 10);

        if (Rd16 (Link->Smem, Base) != SMEM_CANARY) {
            return NULL;
        }
        if (Rd16 (Link->Smem, Base + 2) == Wanted) {
            if (Size != NULL) {
                *Size = ItemSize - PadData;
            }
            return Link->Smem + Base + 16 + PadHdr;
        }
        Cursor += 16 + PadHdr + ItemSize;
    }
    return NULL;
}

static
PUCHAR
SiaSmemAlloc (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_LINK Link,
    _In_ ULONG Item,
    _In_ ULONG Size
    )
{
    PUCHAR Found;
    ULONG Free;
    ULONG Cached;
    ULONG Aligned = (Size + 7) & ~7u;
    ULONG Base;

    if (!SiaSmemLock (Link)) {
        SiaLog (Dev, "smem lock timeout owner=%lu", SiaRead32 (Link->Mutex, 0));
        return NULL;
    }
    Found = SiaSmemFind (Link, Item, NULL);
    if (Found == NULL) {
        Free = SiaRead32 (Link->Smem, Link->Partition + 12);
        Cached = SiaRead32 (Link->Smem, Link->Partition + 16);
        if (Free + 16 + Aligned <= Cached) {
            Base = Link->Partition + Free;
            Wr16 (Link->Smem, Base, SMEM_CANARY);
            Wr16 (Link->Smem, Base + 2, (USHORT)Item);
            SiaWrite32 (Link->Smem, Base + 4, Aligned);
            Wr16 (Link->Smem, Base + 8, (USHORT)(Aligned - Size));
            Wr16 (Link->Smem, Base + 10, 0);
            SiaWrite32 (Link->Smem, Base + 12, 0);
            KeMemoryBarrier ();
            SiaWrite32 (Link->Smem, Link->Partition + 12, Free + 16 + Aligned);
            Found = Link->Smem + Base + 16;
            SiaLog (Dev, "smem alloc item %lu size 0x%x at 0x%x", Item, Aligned, Free);
        } else {
            SiaLog (Dev, "smem alloc item %lu no space free=0x%x cached=0x%x", Item, Free, Cached);
        }
    } else {
        SiaLog (Dev, "smem item %lu already exists", Item);
    }
    SiaSmemUnlock (Link);
    return Found;
}

static
VOID
SiaKick (
    _In_ PSIA_LINK Link,
    _In_ ULONG Signal
    )
{
    KeMemoryBarrier ();
    SiaWrite32 (Link->Ipcc, IPCC_SEND_ID, (IPCC_CLIENT_LPASS << 16) | Signal);
}

static
VOID
SiaSmp2pPoll (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_LINK Link
    )
{
    ULONG Valid;
    ULONG Index;
    ULONG Byte;
    ULONG Value;
    CHAR Name[17];

    if (Link->In == NULL) {
        Link->In = SiaSmemFind (Link, SMP2P_IN_ITEM, &Link->InSize);
        if (Link->In == NULL) {
            return;
        }
    }
    Valid = Rd16 (Link->In, 14);
    for (Index = 0; Index < Valid && Index < 16 && 20 + (Index + 1) * 20 <= Link->InSize; Index++) {
        Value = SiaRead32 (Link->In, 20 + Index * 20 + 16);
        if (Index < Link->LastInValid && Value == Link->LastInValues[Index]) {
            continue;
        }
        for (Byte = 0; Byte < 16; Byte += 4) {
            *(ULONG UNALIGNED *)(Name + Byte) = SiaRead32 (Link->In, 20 + Index * 20 + Byte);
        }
        Name[16] = 0;
        SiaLog (Dev, "smp2p in entry %lu \"%s\" = 0x%08x", Index, Name, Value);
        Link->LastInValues[Index] = Value;
    }
    if (Valid != Link->LastInValid) {
        Link->LastInValid = Valid;
    }
}

static
VOID
SiaFifoRead (
    _In_ PSIA_LINK Link,
    _In_ ULONG Offset,
    _Out_writes_bytes_ (Length) UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Index;
    ULONG Word;

    for (Index = 0; Index < Length; Index += 4) {
        Word = SiaRead32 (Link->RxFifo, ((Offset + Index) % Link->RxLength) & ~3u);
        RtlCopyMemory (Data + Index, &Word, min (4u, Length - Index));
    }
}

static
BOOLEAN
SiaGlinkSend (
    _In_ PSIA_LINK Link,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Head = SiaRead32 (Link->Desc, 4);
    ULONG Tail = SiaRead32 (Link->Desc, 0);
    ULONG Avail = (Tail <= Head) ? Link->TxLength - Head + Tail : Tail - Head;
    ULONG Padded = (Length + 7) & ~7u;
    ULONG Index;
    ULONG Word;

    if (Link->TxLength < 32 || Head >= Link->TxLength || Tail >= Link->TxLength) {
        return FALSE;
    }
    if (Avail < Padded + 16) {
        return FALSE;
    }
    for (Index = 0; Index < Padded; Index += 4) {
        Word = 0;
        if (Index < Length) {
            RtlCopyMemory (&Word, Data + Index, min (4u, Length - Index));
        }
        SiaWrite32 (Link->TxFifo, (Head + Index) % Link->TxLength, Word);
    }
    KeMemoryBarrier ();
    SiaWrite32 (Link->Desc, 4, (Head + Padded) % Link->TxLength);
    SiaKick (Link, IPCC_SIGNAL_GLINK);
    return TRUE;
}

static
BOOLEAN
SiaGlinkCmd (
    _In_ PSIA_LINK Link,
    _In_ USHORT Cmd,
    _In_ USHORT Param1,
    _In_ ULONG Param2
    )
{
    const ULONG Msg[] = { (ULONG)Cmd | ((ULONG)Param1 << 16), Param2 };
    return SiaGlinkSend (Link, (const UCHAR *)Msg, sizeof (Msg));
}

static VOID
SiaGlinkIntent (PSIA_LINK Link, ULONG Iid, ULONG Size)
{
    const ULONG Intent[] = { GLINK_CMD_INTENT | (GLINK_AUDIO_LCID << 16), 1, Size, Iid };
    SiaGlinkSend (Link, (const UCHAR *)Intent, sizeof (Intent));
}

static
VOID
SiaGlinkOpenAudio (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_LINK Link,
    _In_ ULONG Rcid
    )
{
    static const CHAR Name[] = "adsp_apps";
    UCHAR Msg[8 + sizeof (Name)];
    ULONG Index;

    Link->AudioRcid = Rcid;
    SiaGlinkCmd (Link, GLINK_CMD_OPEN_ACK, (USHORT)Rcid, 0);
    *(USHORT UNALIGNED *)(Msg + 0) = GLINK_CMD_OPEN;
    *(USHORT UNALIGNED *)(Msg + 2) = GLINK_AUDIO_LCID;
    *(ULONG UNALIGNED *)(Msg + 4) = sizeof (Name);
    RtlCopyMemory (Msg + 8, Name, sizeof (Name));
    SiaGlinkSend (Link, Msg, sizeof (Msg));
    for (Index = 1; Index <= GLINK_LOCAL_INTENTS; Index++) {
        SiaGlinkIntent (Link, Index, GLINK_LOCAL_INTENT_SIZE);
    }
    SiaLog (Dev, "glink adsp_apps rcid %lu opened, %u local intents", Rcid, GLINK_LOCAL_INTENTS);
}

static
VOID
SiaIntentAdd (
    _In_ PSIA_LINK Link,
    _In_ ULONG Iid,
    _In_ ULONG Size
    )
{
    ULONG Index;

    for (Index = 0; Index < Link->IntentCount; Index++) {
        if (Link->IntentIid[Index] == Iid) {
            Link->IntentSize[Index] = Size;
            Link->IntentFree[Index] = TRUE;
            return;
        }
    }
    if (Link->IntentCount < SIA_REMOTE_INTENTS) {
        Link->IntentIid[Link->IntentCount] = Iid;
        Link->IntentSize[Link->IntentCount] = Size;
        Link->IntentFree[Link->IntentCount] = TRUE;
        Link->IntentCount++;
    }
}

static
VOID
SiaIntentReturn (
    _In_ PSIA_LINK Link,
    _In_ ULONG Iid,
    _In_ BOOLEAN Reuse
    )
{
    ULONG Index;

    for (Index = 0; Index < Link->IntentCount; Index++) {
        if (Link->IntentIid[Index] == Iid) {
            if (Reuse) {
                Link->IntentFree[Index] = TRUE;
            } else {
                Link->IntentCount--;
                Link->IntentIid[Index] = Link->IntentIid[Link->IntentCount];
                Link->IntentSize[Index] = Link->IntentSize[Link->IntentCount];
                Link->IntentFree[Index] = Link->IntentFree[Link->IntentCount];
            }
            return;
        }
    }
}

static
VOID
SiaQueuePush (
    _In_ PSIA_LINK Link,
    _In_ ULONG Offset,
    _In_ ULONG Length
    )
{
    ULONG Next = (Link->QueueHead + 1) % SIA_GPR_QUEUE;

    if (Next == Link->QueueTail || Length > SIA_GPR_MAX) {
        return;
    }
    SiaFifoRead (Link, Offset, Link->Queue[Link->QueueHead].Data, (Length + 3) & ~3u);
    Link->Queue[Link->QueueHead].Length = Length;
    Link->QueueHead = Next;
}

VOID
SiaLinkPoll (
    _In_ PSIA_DEVICE Dev
    )
{
    PSIA_LINK Link = Dev->Link;
    UCHAR Msg[8];
    UCHAR Name[64];
    ULONG Head;
    ULONG Tail;
    ULONG Avail;
    ULONG Size;
    USHORT Cmd;
    USHORT Param1;
    ULONG Param2;
    ULONG Index;
    ULONG Messages;

    if (Link == NULL || Link->Desc == NULL) {
        return;
    }
    if (Link->RxFifo == NULL) {
        Link->RxFifo = SiaSmemFind (Link, GLINK_RX_ITEM, &Link->RxLength);
        if (Link->RxFifo == NULL || Link->RxLength < 32) {
            Link->RxFifo = NULL;
            return;
        }
    }

    for (Messages = 0; Messages < 128; Messages++) {
        Head = SiaRead32 (Link->Desc, 12);
        Tail = SiaRead32 (Link->Desc, 8);
        if (Head >= Link->RxLength || Tail >= Link->RxLength) {
            SiaLog (Dev, "glink invalid rx pointers head %lu tail %lu length %lu", Head, Tail, Link->RxLength);
            return;
        }
        Avail = (Head < Tail) ? Link->RxLength - Tail + Head : Head - Tail;
        if (Avail < 8) {
            return;
        }
        SiaFifoRead (Link, Tail, Msg, 8);
        Cmd = *(USHORT UNALIGNED *)(Msg + 0);
        Param1 = *(USHORT UNALIGNED *)(Msg + 2);
        Param2 = *(ULONG UNALIGNED *)(Msg + 4);
        Size = 8;

        switch (Cmd) {
        case GLINK_CMD_VERSION:
            SiaLog (Dev, "glink rx VERSION %u features 0x%x", Param1, Param2);
            SiaGlinkCmd (Link, GLINK_CMD_VERSION_ACK, 1, GLINK_FEATURES & Param2);
            break;
        case GLINK_CMD_VERSION_ACK:
            SiaLog (Dev, "glink rx VERSION_ACK %u features 0x%x", Param1, Param2);
            break;
        case GLINK_CMD_OPEN:
            Size = 8 + (((Param2 & 0xFFFF) + 7) & ~7u);
            if (Avail < Size) {
                return;
            }
            RtlZeroMemory (Name, sizeof (Name));
            SiaFifoRead (Link, Tail + 8, Name, min ((Param2 & 0xFFFF), sizeof (Name) - 1));
            Name[sizeof (Name) - 1] = 0;
            SiaLog (Dev, "glink rx OPEN rcid %u name \"%s\"", Param1, Name);
            if (strcmp ((const char *)Name, "adsp_apps") == 0) {
                SiaGlinkOpenAudio (Dev, Link, Param1);
            }
            break;
        case GLINK_CMD_OPEN_ACK:
            if (Param1 == GLINK_AUDIO_LCID) {
                Link->AudioOpenAcked = TRUE;
                SiaLog (Dev, "glink rx OPEN_ACK adsp_apps");
            }
            break;
        case GLINK_CMD_INTENT:
            if (Param2 > (Link->RxLength - 8) / 8) {
                SiaLog (Dev, "glink invalid intent count %lu", Param2);
                return;
            }
            Size = 8 + Param2 * 8;
            if (Avail < Size) {
                return;
            }
            if (Link->AudioRcid != 0 && Param1 == Link->AudioRcid) {
                for (Index = 0; Index < Param2; Index++) {
                    SiaFifoRead (Link, Tail + 8 + Index * 8, Name, 8);
                    SiaIntentAdd (Link, *(ULONG UNALIGNED *)(Name + 4), *(ULONG UNALIGNED *)Name);
                }
            }
            break;
        case GLINK_CMD_RX_DONE:
        case GLINK_CMD_RX_DONE_REUSE:
            if (Link->AudioRcid != 0 && Param1 == Link->AudioRcid) {
                SiaIntentReturn (Link, Param2, Cmd == GLINK_CMD_RX_DONE_REUSE);
            }
            break;
        case GLINK_CMD_RX_INTENT_REQ:
            SiaLog (Dev, "glink rx RX_INTENT_REQ rcid %u size %lu", Param1, Param2);
            if (Link->AudioRcid != 0 && Param1 == Link->AudioRcid) {
                SiaGlinkIntent (Link, 100, max (Param2, GLINK_LOCAL_INTENT_SIZE));
                SiaGlinkCmd (Link, GLINK_CMD_INTENT_REQ_ACK, GLINK_AUDIO_LCID, 1);
            }
            break;
        case GLINK_CMD_TX_DATA:
        case GLINK_CMD_TX_DATA_CONT:
            if (Avail < 16) {
                return;
            }
            SiaFifoRead (Link, Tail + 8, Name, 8);
            if (*(ULONG UNALIGNED *)Name > Link->RxLength - 16) {
                SiaLog (Dev, "glink invalid data chunk %lu", *(ULONG UNALIGNED *)Name);
                return;
            }
            Size = (16 + *(ULONG UNALIGNED *)Name + 7) & ~7u;
            if (Avail < Size) {
                return;
            }
            if (Link->AudioRcid != 0 && Param1 == Link->AudioRcid) {
                if (*(ULONG UNALIGNED *)(Name + 4) != 0 || Cmd == GLINK_CMD_TX_DATA_CONT) {
                    SiaLog (Dev, "glink rx multi-chunk data chunk %lu left %lu", *(ULONG UNALIGNED *)Name, *(ULONG UNALIGNED *)(Name + 4));
                } else {
                    SiaQueuePush (Link, Tail + 16, *(ULONG UNALIGNED *)Name);
                }
                SiaWrite32 (Link->Desc, 8, (Tail + Size) % Link->RxLength);
                SiaGlinkCmd (Link, GLINK_CMD_RX_DONE_REUSE, GLINK_AUDIO_LCID, Param2);
                continue;
            }
            break;
        case GLINK_CMD_CLOSE:
        case GLINK_CMD_CLOSE_ACK:
        case GLINK_CMD_READ_NOTIF:
        case GLINK_CMD_SIGNALS:
        case GLINK_CMD_INTENT_REQ_ACK:
            break;
        default:
            SiaLog (Dev, "glink rx cmd %u p1 %u p2 0x%x", Cmd, Param1, Param2);
            break;
        }
        SiaWrite32 (Link->Desc, 8, (Tail + Size) % Link->RxLength);
    }
}

NTSTATUS
SiaGprSendRaw (
    _In_ PSIA_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Packet,
    _In_ ULONG Length
    )
{
    PSIA_LINK Link = Dev->Link;
    UCHAR Msg[16 + SIA_GPR_MAX];
    ULONG Index;
    ULONG Tries;

    if (Link == NULL || !Link->AudioOpenAcked || Length > SIA_GPR_MAX) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (Tries = 0; Tries < 2000; Tries++) {
        for (Index = 0; Index < Link->IntentCount; Index++) {
            if (Link->IntentFree[Index] && Link->IntentSize[Index] >= Length) {
                break;
            }
        }
        if (Index < Link->IntentCount) {
            break;
        }
        SiaLinkPoll (Dev);
        SiaSleep (1);
    }
    if (Index >= Link->IntentCount) {
        SiaLog (Dev, "gpr send: no remote intent for %lu bytes (count %lu)", Length, Link->IntentCount);
        return STATUS_DEVICE_BUSY;
    }
    *(USHORT UNALIGNED *)(Msg + 0) = GLINK_CMD_TX_DATA;
    *(USHORT UNALIGNED *)(Msg + 2) = GLINK_AUDIO_LCID;
    *(ULONG UNALIGNED *)(Msg + 4) = Link->IntentIid[Index];
    *(ULONG UNALIGNED *)(Msg + 8) = Length;
    *(ULONG UNALIGNED *)(Msg + 12) = 0;
    RtlCopyMemory (Msg + 16, Packet, Length);
    for (Tries = 0; Tries < 1000; Tries++) {
        if (SiaGlinkSend (Link, Msg, 16 + Length)) {
            Link->IntentFree[Index] = FALSE;
            return STATUS_SUCCESS;
        }
        SiaLinkPoll (Dev);
        SiaSleep (1);
    }
    SiaLog (Dev, "gpr send: tx fifo full");
    return STATUS_DEVICE_BUSY;
}

BOOLEAN
SiaGprPop (
    _In_ PSIA_DEVICE Dev,
    _Out_ SIA_GPR_PACKET *Packet
    )
{
    PSIA_LINK Link = Dev->Link;

    if (Link == NULL || Link->QueueTail == Link->QueueHead) {
        return FALSE;
    }
    RtlCopyMemory (Packet, &Link->Queue[Link->QueueTail], sizeof (*Packet));
    Link->QueueTail = (Link->QueueTail + 1) % SIA_GPR_QUEUE;
    return TRUE;
}

NTSTATUS
SiaLinkStart (
    _In_ PSIA_DEVICE Dev
    )
{
    PSIA_LINK Link;
    PHYSICAL_ADDRESS Physical;
    ULONG Toc;
    ULONG Count;
    ULONG Index;
    ULONG Tick;
    UCHAR Packet[24];
    SIA_GPR_PACKET Rsp;

    Link = (PSIA_LINK)ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (SIA_LINK), 'kniS');
    if (Link == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Dev->Link = Link;
    Physical.QuadPart = SMEM_PHYS;
    Link->Smem = (PUCHAR)MmMapIoSpaceEx (Physical, SMEM_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Physical.QuadPart = TCSR_MUTEX_PHYS;
    Link->Mutex = (PUCHAR)MmMapIoSpaceEx (Physical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    Physical.QuadPart = IPCC_PHYS;
    Link->Ipcc = (PUCHAR)MmMapIoSpaceEx (Physical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Link->Smem == NULL || Link->Mutex == NULL || Link->Ipcc == NULL) {
        SiaLog (Dev, "link map failed");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Toc = SMEM_SIZE - 0x1000;
    if (SiaRead32 (Link->Smem, Toc) != SMEM_TOC_MAGIC) {
        SiaLog (Dev, "smem toc magic 0x%08x", SiaRead32 (Link->Smem, Toc));
        return STATUS_DEVICE_NOT_READY;
    }
    Count = SiaRead32 (Link->Smem, Toc + 8);
    for (Index = 0; Index < Count && Index < 64; Index++) {
        ULONG Entry = Toc + 32 + Index * 48;
        USHORT Host0 = Rd16 (Link->Smem, Entry + 12);
        USHORT Host1 = Rd16 (Link->Smem, Entry + 14);
        if ((Host0 == 0 && Host1 == 2) || (Host0 == 2 && Host1 == 0)) {
            Link->Partition = SiaRead32 (Link->Smem, Entry);
            Link->PartitionSize = SiaRead32 (Link->Smem, Entry + 4);
        }
    }
    if (Link->Partition == 0 || SiaRead32 (Link->Smem, Link->Partition) != SMEM_PART_MAGIC) {
        SiaLog (Dev, "apps-adsp partition missing");
        return STATUS_DEVICE_NOT_READY;
    }

    SiaSmp2pPoll (Dev, Link);
    Link->Out = SiaSmemAlloc (Dev, Link, SMP2P_OUT_ITEM, SMP2P_ITEM_SIZE);
    if (Link->Out == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    for (Index = 0; Index < SMP2P_ITEM_SIZE; Index += 4) {
        SiaWrite32 (Link->Out, Index, 0);
    }
    SiaWrite32 (Link->Out, 0, SMP2P_MAGIC);
    Wr16 (Link->Out, 8, 0);
    Wr16 (Link->Out, 10, 2);
    Wr16 (Link->Out, 12, 16);
    Wr16 (Link->Out, 14, 0);
    KeMemoryBarrier ();
    SiaWrite32 (Link->Out, 4, (1u << 8) | ((Link->In != NULL && READ_REGISTER_UCHAR (Link->In + 4) != 0) ? READ_REGISTER_UCHAR (Link->In + 4) : 2u));
    SiaKick (Link, IPCC_SIGNAL_SMP2P);

    Link->Desc = SiaSmemFind (Link, GLINK_DESC_ITEM, &Count);
    Link->TxFifo = SiaSmemFind (Link, GLINK_TX_ITEM, &Link->TxLength);
    if (Link->Desc == NULL || Link->TxFifo == NULL) {
        SiaLog (Dev, "glink smem items missing");
        return STATUS_DEVICE_NOT_READY;
    }
    SiaWrite32 (Link->Desc, 8, 0);
    SiaWrite32 (Link->Desc, 4, 0);
    SiaGlinkCmd (Link, GLINK_CMD_VERSION, 1, GLINK_FEATURES);

    for (Tick = 0; Tick < 3000 && !(Link->AudioOpenAcked && Link->IntentCount > 0); Tick++) {
        if (Dev->Stopping != 0) {
            return STATUS_CANCELLED;
        }
        SiaSmp2pPoll (Dev, Link);
        SiaLinkPoll (Dev);
        SiaSleep (1);
    }
    SiaLog (Dev, "link: adsp_apps open %u remote intents %lu after %lu ms", Link->AudioOpenAcked, Link->IntentCount, Tick);
    if (!Link->AudioOpenAcked || Link->IntentCount == 0) {
        return STATUS_IO_TIMEOUT;
    }

    SiaGprHeader (Packet, sizeof (Packet), 1, 1, 0x5A, APM_CMD_GET_SPF_STATE);
    for (Count = 0; Count < 20; Count++) {
        SiaGprSendRaw (Dev, Packet, sizeof (Packet));
        for (Tick = 0; Tick < 200; Tick++) {
            if (Dev->Stopping != 0) {
                return STATUS_CANCELLED;
            }
            SiaLinkPoll (Dev);
            while (SiaGprPop (Dev, &Rsp)) {
                if (Rsp.Length >= 28 && *(ULONG UNALIGNED *)(Rsp.Data + 20) == APM_CMD_RSP_SPF_STATE) {
                    SiaLog (Dev, "apm spf state %lu", *(ULONG UNALIGNED *)(Rsp.Data + 24));
                    if (*(ULONG UNALIGNED *)(Rsp.Data + 24) == 1) {
                        return STATUS_SUCCESS;
                    }
                }
            }
            SiaSleep (1);
        }
        SiaSleep (100);
    }
    return STATUS_DEVICE_NOT_READY;
}

VOID
SiaLinkStop (
    _In_ PSIA_DEVICE Dev
    )
{
    PSIA_LINK Link = Dev->Link;

    if (Link == NULL) {
        return;
    }
    if (Link->Ipcc != NULL) {
        MmUnmapIoSpace (Link->Ipcc, PAGE_SIZE);
    }
    if (Link->Mutex != NULL) {
        MmUnmapIoSpace (Link->Mutex, PAGE_SIZE);
    }
    if (Link->Smem != NULL) {
        MmUnmapIoSpace (Link->Smem, SMEM_SIZE);
    }
    ExFreePoolWithTag (Link, 'kniS');
    Dev->Link = NULL;
}
