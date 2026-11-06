#include <ntifs.h>
#include "pmic-glink.h"

static __inline ULONG QpgRd32 (PUCHAR Base, ULONG Offset)
{
    return READ_REGISTER_ULONG ((volatile ULONG *)(Base + Offset));
}

static __inline VOID QpgWr32 (PUCHAR Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Base + Offset), Value);
    KeMemoryBarrier ();
}

static __inline USHORT QpgRd16 (PUCHAR Base, ULONG Offset)
{
    return READ_REGISTER_USHORT ((volatile USHORT *)(Base + Offset));
}

static PUCHAR QpgSmemFind (PQPG_DEVICE Dev, ULONG Wanted, PULONG Size)
{
    PQPG_LINK Link = Dev->Link;
    ULONG Free = QpgRd32 (Dev->Smem, Link->Partition + QPG_PART_FREE_UNC_OFF);
    ULONG Cursor = QPG_PART_HDR_SIZE;
    ULONG Count = 0;

    *Size = 0;
    while (Cursor + QPG_PRIV_ENTRY_SIZE <= Free && Cursor + QPG_PRIV_ENTRY_SIZE <= Link->PartitionSize &&
           Count < QPG_WALK_MAX) {
        ULONG Base = Link->Partition + Cursor;
        ULONG ItemSize = QpgRd32 (Dev->Smem, Base + 4);
        USHORT PadData = QpgRd16 (Dev->Smem, Base + 8);
        USHORT PadHdr = QpgRd16 (Dev->Smem, Base + 10);

        if (QpgRd16 (Dev->Smem, Base) != QPG_PRIV_CANARY) {
            QpgLog (Dev, "smem walk stop %u canary=0x%04x", Count, QpgRd16 (Dev->Smem, Base));
            return NULL;
        }
        if (QpgRd16 (Dev->Smem, Base + 2) == (USHORT)Wanted) {
            *Size = ItemSize - PadData;
            return Dev->Smem + Base + QPG_PRIV_ENTRY_SIZE + PadHdr;
        }
        Cursor += QPG_PRIV_ENTRY_SIZE + PadHdr + ItemSize;
        Count++;
    }
    return NULL;
}

static VOID QpgKick (PQPG_DEVICE Dev)
{
    KeMemoryBarrier ();
    QpgWr32 (Dev->Ipcc, QPG_IPCC_REG_SEND_ID, (QPG_IPCC_CLIENT_LPASS << 16) | QPG_IPCC_SIGNAL_GLINK);
}

static VOID QpgFifoRead (PQPG_LINK Link, ULONG Offset, UCHAR *Data, ULONG Length)
{
    ULONG Index;
    ULONG Word;

    for (Index = 0; Index < Length; Index += 4) {
        Word = QpgRd32 (Link->RxFifo, ((Offset + Index) % Link->RxLength) & ~3u);
        RtlCopyMemory (Data + Index, &Word, min (4u, Length - Index));
    }
}

static BOOLEAN QpgFifoWrite (PQPG_DEVICE Dev, const UCHAR *Data, ULONG Length)
{
    PQPG_LINK Link = Dev->Link;
    ULONG Head;
    ULONG Tail;
    ULONG Avail;
    ULONG Padded;
    ULONG Index;
    ULONG Word;

    if (Link == NULL || Link->TxFifo == NULL || Length == 0) {
        return FALSE;
    }
    Head = QpgRd32 (Link->Desc, 4);
    Tail = QpgRd32 (Link->Desc, 0);
    if (Link->TxLength < 32 || Head >= Link->TxLength || Tail >= Link->TxLength) {
        return FALSE;
    }
    Avail = (Tail <= Head) ? Link->TxLength - Head + Tail : Tail - Head;
    Padded = (Length + 7) & ~7u;
    if (Avail < Padded + 16) {
        QpgLog (Dev, "tx fifo full avail=0x%x len=%u", Avail, Length);
        return FALSE;
    }
    for (Index = 0; Index < Padded; Index += 4) {
        Word = 0;
        if (Index < Length) {
            RtlCopyMemory (&Word, Data + Index, min (4u, Length - Index));
        }
        QpgWr32 (Link->TxFifo, (Head + Index) % Link->TxLength, Word);
    }
    KeMemoryBarrier ();
    QpgWr32 (Link->Desc, 4, (Head + Padded) % Link->TxLength);
    QpgKick (Dev);
    return TRUE;
}

BOOLEAN QpgLinkSend (PQPG_DEVICE Dev, const UCHAR *Data, ULONG Length)
{
    PQPG_LINK Link = Dev->Link;
    UCHAR Msg[16 + 1024];
    ULONG Slot = 0;
    ULONG Iid;

    if (Link == NULL || Length > sizeof (Msg) - 16) {
        return FALSE;
    }
    if (Link->Intentless) {
        Iid = Link->TxIid;
    } else {
        for (Slot = 0; Slot < Link->IntentCount; Slot++) {
            if (Link->IntentFree[Slot] && Link->IntentSize[Slot] >= Length) {
                break;
            }
        }
        if (Slot >= Link->IntentCount) {
            QpgLog (Dev, "send no intent len=%u intents=%u", Length, Link->IntentCount);
            return FALSE;
        }
        Iid = Link->IntentIid[Slot];
    }

    *(USHORT UNALIGNED *)(Msg + 0) = QPG_GC_TX_DATA;
    *(USHORT UNALIGNED *)(Msg + 2) = (USHORT)Link->Lcid;
    *(ULONG UNALIGNED *)(Msg + 4) = Iid;
    *(ULONG UNALIGNED *)(Msg + 8) = Length;
    *(ULONG UNALIGNED *)(Msg + 12) = 0;
    RtlCopyMemory (Msg + 16, Data, Length);
    if (!QpgFifoWrite (Dev, Msg, 16 + Length)) {
        return FALSE;
    }
    if (!Link->Intentless) {
        Link->IntentFree[Slot] = FALSE;
    }
    Link->Sends++;
    return TRUE;
}

VOID QpgLinkKick (PQPG_DEVICE Dev)
{
    if (Dev->Link != NULL && Dev->Link->TxFifo != NULL) {
        QpgKick (Dev);
    }
}

static BOOLEAN QpgCmd (PQPG_DEVICE Dev, USHORT Cmd, USHORT Param1, ULONG Param2)
{
    UCHAR Msg[8];

    *(USHORT UNALIGNED *)(Msg + 0) = Cmd;
    *(USHORT UNALIGNED *)(Msg + 2) = Param1;
    *(ULONG UNALIGNED *)(Msg + 4) = Param2;
    return QpgFifoWrite (Dev, Msg, sizeof (Msg));
}

static VOID QpgIntentSend (PQPG_DEVICE Dev, ULONG Iid, ULONG Size)
{
    PQPG_LINK Link = Dev->Link;
    UCHAR Msg[16];

    *(USHORT UNALIGNED *)(Msg + 0) = QPG_GC_INTENT;
    *(USHORT UNALIGNED *)(Msg + 2) = (USHORT)Link->Lcid;
    *(ULONG UNALIGNED *)(Msg + 4) = 1;
    *(ULONG UNALIGNED *)(Msg + 8) = Size;
    *(ULONG UNALIGNED *)(Msg + 12) = Iid;
    QpgFifoWrite (Dev, Msg, sizeof (Msg));
}

static VOID QpgChanRequest (PQPG_DEVICE Dev)
{
    static const CHAR Name[] = QPG_CHANNEL_NAME;
    PQPG_LINK Link = Dev->Link;
    UCHAR Msg[8 + sizeof (Name)];

    if (Link->Opening) {
        return;
    }
    *(USHORT UNALIGNED *)(Msg + 0) = QPG_GC_OPEN;
    *(USHORT UNALIGNED *)(Msg + 2) = (USHORT)Link->Lcid;
    *(ULONG UNALIGNED *)(Msg + 4) = sizeof (Name);
    RtlCopyMemory (Msg + 8, Name, sizeof (Name));
    Link->Opening = QpgFifoWrite (Dev, Msg, sizeof (Msg));
    QpgLog (Dev, "chan open sent=%u", (ULONG)Link->Opening);
}

static VOID QpgIntentRequest (PQPG_DEVICE Dev)
{
    PQPG_LINK Link = Dev->Link;

    QpgCmd (Dev, QPG_GC_RX_INTENT_REQ, (USHORT)Link->Lcid, 32);
}

static VOID QpgChanGrant (PQPG_DEVICE Dev)
{
    PQPG_LINK Link = Dev->Link;
    ULONG Index;

    if (Link->Intentless || Link->IntentCount != 0) {
        return;
    }
    for (Index = 1; Index <= QPG_GLINK_INTENTS; Index++) {
        QpgIntentSend (Dev, Index, QPG_GLINK_INTENT_SIZE);
    }
    QpgLog (Dev, "granted %u intents of %u", QPG_GLINK_INTENTS, QPG_GLINK_INTENT_SIZE);
    QpgIntentRequest (Dev);
}

static VOID QpgIntentAdd (PQPG_DEVICE Dev, ULONG Iid, ULONG Size)
{
    PQPG_LINK Link = Dev->Link;
    ULONG Index;

    for (Index = 0; Index < Link->IntentCount; Index++) {
        if (Link->IntentIid[Index] == Iid) {
            Link->IntentSize[Index] = Size;
            Link->IntentFree[Index] = TRUE;
            return;
        }
    }
    if (Link->IntentCount < QPG_GLINK_REMOTE_INTENTS) {
        Link->IntentIid[Link->IntentCount] = Iid;
        Link->IntentSize[Link->IntentCount] = Size;
        Link->IntentFree[Link->IntentCount] = TRUE;
        Link->IntentCount++;
        QpgLog (Dev, "remote intent iid=%u size=%u count=%u", Iid, Size, Link->IntentCount);
    }
}

static VOID QpgIntentReturn (PQPG_DEVICE Dev, ULONG Iid, BOOLEAN Reuse)
{
    PQPG_LINK Link = Dev->Link;
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

VOID QpgLinkPoll (PQPG_DEVICE Dev)
{
    PQPG_LINK Link = Dev->Link;
    UCHAR Msg[8];
    UCHAR Name[1024];
    ULONG Head;
    ULONG Tail;
    ULONG Avail;
    ULONG Size;
    ULONG Length;
    USHORT Cmd;
    USHORT Param1;
    ULONG Param2;
    ULONG Index;
    ULONG Messages;

    if (Link == NULL || Link->Desc == NULL) {
        return;
    }
    if (Link->RxFifo == NULL) {
        Link->RxFifo = QpgSmemFind (Dev, QPG_ITEM_FIFO1, &Link->RxLength);
        if (Link->RxFifo == NULL || Link->RxLength < 32) {
            Link->RxFifo = NULL;
            return;
        }
        QpgLog (Dev, "rx fifo size=0x%x", Link->RxLength);
    }

    for (Messages = 0; Messages < 64; Messages++) {
        Head = QpgRd32 (Link->Desc, 12);
        Tail = QpgRd32 (Link->Desc, 8);
        if (Head >= Link->RxLength || Tail >= Link->RxLength) {
            return;
        }
        Avail = (Head < Tail) ? Link->RxLength - Tail + Head : Head - Tail;
        if (Avail < 8) {
            return;
        }
        QpgFifoRead (Link, Tail, Msg, 8);
        Cmd = *(USHORT UNALIGNED *)(Msg + 0);
        Param1 = *(USHORT UNALIGNED *)(Msg + 2);
        Param2 = *(ULONG UNALIGNED *)(Msg + 4);
        Size = 8;
        if (Link->LogRx < 40) {
            QpgLog (Dev, "rx cmd=%u p1=%u p2=0x%x avail=0x%x tail=%u head=%u", Cmd, Param1, Param2, Avail,
                    Tail, Head);
            Link->LogRx++;
        }

        switch (Cmd) {
        case QPG_GC_VERSION:
            QpgCmd (Dev, QPG_GC_VERSION_ACK, QPG_GLINK_VERSION, QPG_GLINK_FEATURES & Param2);
            break;

        case QPG_GC_VERSION_ACK:
            Link->VersionAcked = TRUE;
            break;

        case QPG_GC_OPEN:
            Size = 8 + (((Param2 & 0xFFFFu) + 7) & ~7u);
            if (Avail < Size) {
                return;
            }
            RtlZeroMemory (Name, 64);
            QpgFifoRead (Link, Tail + 8, Name, min ((Param2 & 0xFFFFu), 63u));
            QpgLog (Dev, "remote open name=%s rcid=%u", Name, Param1);
            if (strcmp ((const char *)Name, QPG_CHANNEL_NAME) == 0) {
                Link->Rcid = Param1;
                QpgCmd (Dev, QPG_GC_OPEN_ACK, Param1, 0);
                QpgChanRequest (Dev);
                QpgChanGrant (Dev);
            } else {
                QpgCmd (Dev, QPG_GC_CLOSE, Param1, 0);
            }
            break;

        case QPG_GC_OPEN_ACK:
            QpgLog (Dev, "open ack lcid=%u rcid=%u", Param1, Param2);
            if (Param1 == Link->Lcid) {
                Link->Rcid = Param2;
                Link->OpenAcked = TRUE;
                QpgChanGrant (Dev);
            }
            break;

        case QPG_GC_CLOSE:
            QpgCmd (Dev, QPG_GC_CLOSE_ACK, Param1, 0);
            if (Param1 == Link->Rcid) {
                Link->Rcid = 0;
                Link->OpenAcked = FALSE;
                Link->IntentCount = 0;
            }
            break;

        case QPG_GC_INTENT:
            Size = 8 + Param2 * 8;
            if (Param2 > (Link->RxLength - 8) / 8 || Avail < Size) {
                return;
            }
            Link->Rcid = Param1;
            QpgLog (Dev, "intent cid=%u count=%u", Param1, Param2);
            for (Index = 0; Index < Param2; Index++) {
                QpgFifoRead (Link, Tail + 8 + Index * 8, Msg, 8);
                QpgIntentAdd (Dev, *(ULONG UNALIGNED *)(Msg + 4), *(ULONG UNALIGNED *)Msg);
            }
            break;

        case QPG_GC_RX_DONE:
        case QPG_GC_RX_DONE_REUSE:
            QpgIntentReturn (Dev, Param2, Cmd == QPG_GC_RX_DONE_REUSE);
            break;

        case QPG_GC_RX_INTENT_REQ:
            if (Param1 == Link->Rcid) {
                QpgIntentSend (Dev, 100, max (Param2, QPG_GLINK_INTENT_SIZE));
                QpgCmd (Dev, QPG_GC_INTENT_REQ_ACK, (USHORT)Link->Lcid, 1);
            }
            break;

        case QPG_GC_TX_DATA:
        case QPG_GC_TX_DATA_CONT:
            if (Avail < 16) {
                return;
            }
            QpgFifoRead (Link, Tail + 8, Msg, 8);
            Length = *(ULONG UNALIGNED *)(Msg + 0);
            if (Length > sizeof (Name) || Length > Link->RxLength - 16) {
                return;
            }
            Size = (16 + Length + 7) & ~7u;
            if (Avail < Size) {
                return;
            }
            QpgLog (Dev, "data cid=%u len=%u left=0x%x", Param1, Length, *(ULONG UNALIGNED *)(Msg + 4));
            if (*(ULONG UNALIGNED *)(Msg + 4) == 0 && Cmd != QPG_GC_TX_DATA_CONT) {
                RtlZeroMemory (Name, sizeof (Name));
                QpgFifoRead (Link, Tail + 16, Name, Length);
                Link->Frames++;
                QpgProtoRx (Dev, Name, Length);
            }
            QpgWr32 (Link->Desc, 8, (Tail + Size) % Link->RxLength);
            QpgCmd (Dev, QPG_GC_RX_DONE_REUSE, (USHORT)Link->Lcid, Param2);
            continue;

        case QPG_GC_INTENT_REQ_ACK:
            if (Param2 != 0) {
                Link->TxIid = Param2;
                if (Link->Rcid == 0) {
                    Link->Rcid = Param1;
                }
                QpgLog (Dev, "intent ack cid=%u iid=%u", Param1, Param2);
            }
            break;

        case QPG_GC_READ_NOTIF:
        case QPG_GC_SIGNALS:
            break;

        case QPG_GC_CLOSE_ACK:
            QpgLog (Dev, "close ack lcid=%u", Param1);
            break;

        default:
            QpgLog (Dev, "rx unknown cmd=%u p1=%u p2=0x%x", Cmd, Param1, Param2);
            break;
        }
        QpgWr32 (Link->Desc, 8, (Tail + Size) % Link->RxLength);
    }
}

NTSTATUS QpgLinkStart (PQPG_DEVICE Dev)
{
    PQPG_LINK Link;
    PHYSICAL_ADDRESS Physical;
    ULONG Toc;
    ULONG Count;
    ULONG Index;
    ULONG Tick;
    ULONG Size;

    Link = (PQPG_LINK)ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (QPG_LINK), 'kggQ');
    if (Link == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Dev->Link = Link;
    Link->Lcid = QPG_GLINK_LCID;
    Link->Intentless = TRUE;

    Physical.QuadPart = QPG_TCSR_PHYS;
    Link->Mutex = (PUCHAR)MmMapIoSpaceEx (Physical, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Link->Mutex == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Toc = QPG_PTABLE_OFFSET;
    if (QpgRd32 (Dev->Smem, Toc) != QPG_PTOC_MAGIC) {
        return STATUS_DEVICE_NOT_READY;
    }
    Count = QpgRd32 (Dev->Smem, Toc + QPG_PTOC_ENTRIES_OFF);
    for (Index = 0; Index < Count && Index < 64; Index++) {
        ULONG Entry = Toc + QPG_PTOC_ENTRY0 + QPG_PTOC_ENTRY_SIZE * Index;
        ULONG Offset = QpgRd32 (Dev->Smem, Entry);
        ULONG PartSize = QpgRd32 (Dev->Smem, Entry + 4);
        USHORT Host0 = QpgRd16 (Dev->Smem, Entry + 12);
        USHORT Host1 = QpgRd16 (Dev->Smem, Entry + 14);

        if (Offset == 0 || PartSize == 0 || Offset + PartSize > QPG_SMEM_SIZE) {
            continue;
        }
        if ((Host0 == QPG_HOST_APPS && Host1 == QPG_HOST_ADSP) ||
            (Host0 == QPG_HOST_ADSP && Host1 == QPG_HOST_APPS)) {
            Link->Partition = Offset;
            Link->PartitionSize = PartSize;
        }
    }
    if (Link->Partition == 0 || QpgRd32 (Dev->Smem, Link->Partition) != QPG_PART_MAGIC) {
        return STATUS_DEVICE_NOT_READY;
    }
    QpgLog (Dev, "partition off=0x%x size=0x%x magic=0x%08x entries=%u", Link->Partition,
            Link->PartitionSize, QpgRd32 (Dev->Smem, Link->Partition), Count);

    Link->Desc = QpgSmemFind (Dev, QPG_ITEM_DESC, &Size);
    Link->TxFifo = QpgSmemFind (Dev, QPG_ITEM_FIFO0, &Link->TxLength);
    if (Link->Desc == NULL || Link->TxFifo == NULL) {
        QpgLog (Dev, "desc=%p tx=%p missing", Link->Desc, Link->TxFifo);
        return STATUS_DEVICE_NOT_READY;
    }
    QpgLog (Dev, "desc size=0x%x tx size=0x%x tail=%u head=%u rxtail=%u rxhead=%u", Size,
            Link->TxLength, QpgRd32 (Link->Desc, 0), QpgRd32 (Link->Desc, 4),
            QpgRd32 (Link->Desc, 8), QpgRd32 (Link->Desc, 12));

    for (Index = 2; Index <= 4; Index++) {
        QpgCmd (Dev, QPG_GC_CLOSE, (USHORT)Index, 0);
    }
    for (Tick = 0; Tick < 50; Tick++) {
        QpgLinkPoll (Dev);
        KeStallExecutionProcessor (1000);
    }

    QpgCmd (Dev, QPG_GC_VERSION, QPG_GLINK_VERSION, QPG_GLINK_FEATURES);
    for (Tick = 0; Tick < 500 && !Link->VersionAcked; Tick++) {
        QpgLinkPoll (Dev);
        KeStallExecutionProcessor (1000);
    }
    QpgLog (Dev, "version acked=%u", (ULONG)Link->VersionAcked);

    QpgChanRequest (Dev);
    for (Tick = 0; Tick < QPG_GLINK_TRIES && !Link->OpenAcked; Tick++) {
        if (InterlockedCompareExchange (&Dev->Stopping, 0, 0) != 0) {
            return STATUS_CANCELLED;
        }
        QpgLinkPoll (Dev);
        if ((Tick % 2000) == 1999) {
            Link->Opening = FALSE;
            QpgChanRequest (Dev);
        }
        KeStallExecutionProcessor (1000);
    }
    if (!Link->OpenAcked) {
        QpgLog (Dev, "chan open not acked frames=%u sends=%u", Link->Frames, Link->Sends);
        return STATUS_IO_TIMEOUT;
    }

    QpgIntentRequest (Dev);
    for (Tick = 0; Tick < 2000 && Link->TxIid == 0 && Link->IntentCount == 0; Tick++) {
        if (InterlockedCompareExchange (&Dev->Stopping, 0, 0) != 0) {
            return STATUS_CANCELLED;
        }
        QpgLinkPoll (Dev);
        if ((Tick % 300) == 299) {
            QpgIntentRequest (Dev);
        }
        KeStallExecutionProcessor (1000);
    }

    QpgLog (Dev, "link up rcid=%u intents=%u iid=%u", Link->Rcid, Link->IntentCount, Link->TxIid);
    return STATUS_SUCCESS;
}

VOID QpgLinkStop (PQPG_DEVICE Dev)
{
    PQPG_LINK Link = Dev->Link;

    if (Link == NULL) {
        return;
    }
    if (Link->Rcid != 0) {
        QpgCmd (Dev, QPG_GC_CLOSE, (USHORT)Link->Lcid, 0);
    }
    if (Link->Mutex != NULL) {
        MmUnmapIoSpace (Link->Mutex, PAGE_SIZE);
    }
    ExFreePoolWithTag (Link, 'kggQ');
    Dev->Link = NULL;
}
