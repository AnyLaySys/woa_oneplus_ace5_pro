/*
 * MHI host bring-up to mission mode, following drivers/bus/mhi/host:
 * init.c (mhi_init_dev_ctxt, mhi_init_mmio, mhi_prepare_for_power_up),
 * boot.c (BHI SBL load, BHIe AMSS load for FBC), pm.c (mhi_async_power_up,
 * ready/M0 transitions) and main.c (control event ring). WCN7861 PEACH uses
 * IPCR channels 20/21 on event ring 1, control events on ring 0, MSI vectors
 * 0 (BHI), 1 (control) and 2 (data).
 */
#include "wcn7861.h"

#define MHI_BHI_TIMEOUT_MS      10000
#define MHI_BHIE_TIMEOUT_MS     20000
#define MHI_READY_TIMEOUT_MS    30000
#define MHI_MISSION_TIMEOUT_MS  60000

static NTSTATUS MhiAllocBhie (_In_ PWCN_DEVICE Dev, _Out_ PWCN_BHIE_TABLE Table,
                              _In_reads_bytes_opt_ (Size) PUCHAR Image, _In_ SIZE_T Size);
static VOID MhiRddmPrepare (_In_ PWCN_DEVICE Dev);

static
ULONG
MhiSequence (
    VOID
    )
{
    ULONG64 Counter = (ULONG64)KeQueryPerformanceCounter (NULL).QuadPart;
    ULONG Value = (ULONG)((Counter * 2654435761ULL) >> 7) & 0x3FFFFFFF;

    return (Value != 0) ? Value : 1;
}

static
LONGLONG
MhiNowMs (
    VOID
    )
{
    return (LONGLONG)(KeQueryInterruptTime () / 10000);
}

/*
 * Context pointers are 64-bit fields at 4-byte alignment (the MHI layout), so
 * they are accessed as two 32-bit halves.
 */
static
VOID
MhiCtxtSet (
    _In_ PMHI_CTXT Ctxt,
    _In_ ULONG Offset,
    _In_ ULONG64 Value
    )
{
    volatile ULONG *Half = (volatile ULONG *)((PUCHAR)Ctxt + Offset);

    Half[0] = (ULONG)Value;
    Half[1] = (ULONG)(Value >> 32);
}

static
ULONG64
MhiCtxtGet (
    _In_ PMHI_CTXT Ctxt,
    _In_ ULONG Offset
    )
{
    volatile ULONG *Half = (volatile ULONG *)((PUCHAR)Ctxt + Offset);
    ULONG Low = Half[0];
    ULONG High = Half[1];

    return ((ULONG64)High << 32) | Low;
}

#define CTXT_RBASE  FIELD_OFFSET (MHI_CTXT, RBase)
#define CTXT_RLEN   FIELD_OFFSET (MHI_CTXT, RLen)
#define CTXT_RP     FIELD_OFFSET (MHI_CTXT, Rp)
#define CTXT_WP     FIELD_OFFSET (MHI_CTXT, Wp)

static
VOID
MhiWriteDoorbell (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Doorbell,
    _In_ ULONG64 Value
    )
{
    KeMemoryBarrier ();
    WcnBarWrite (Dev, Doorbell + 4, (ULONG)(Value >> 32));
    WcnBarWrite (Dev, Doorbell, (ULONG)Value);
}

static
ULONG
MhiState (
    _In_ PWCN_DEVICE Dev
    )
{
    return (WcnBarRead (Dev, MHI_STATUS) >> 8) & 0xFF;
}

static
ULONG
MhiExecEnv (
    _In_ PWCN_DEVICE Dev
    )
{
    return WcnBarRead (Dev, Dev->Mhi.Bhi + BHI_EXECENV);
}

static
VOID
MhiSetState (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG State
    )
{
    ULONG Value = WcnBarRead (Dev, MHI_CTRL);

    WcnBarWrite (Dev, MHI_CTRL, (Value & ~0xFF00UL) | (State << MHI_CTRL_STATE_SHIFT));
}

/* Polls a BAR0 register field; interrupts only shorten the 25 ms interval. */
static
BOOLEAN
MhiPoll (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Offset,
    _In_ ULONG Mask,
    _In_ ULONG Expected,
    _In_ ULONG TimeoutMs
    )
{
    LONGLONG Deadline = MhiNowMs () + TimeoutMs;

    for (;;) {
        if ((WcnBarRead (Dev, Offset) & Mask) == Expected) {
            return TRUE;
        }
        if (Dev->Stop || MhiNowMs () > Deadline) {
            return FALSE;
        }
        WcnWait (Dev, 25);
    }
}

static
NTSTATUS
MhiRingAlloc (
    _In_ PWCN_DEVICE Dev,
    _Out_ PWCN_RING Ring,
    _In_ ULONG Elements,
    _In_ PMHI_CTXT Ctxt
    )
{
    NTSTATUS Status;

    RtlZeroMemory (Ring, sizeof (*Ring));
    Ring->Length = Elements * sizeof (MHI_ELEMENT);
    Status = WcnDmaAlloc (Dev, Ring->Length, &Ring->Mem);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }
    /* MHI rings must be aligned to their (power of two) length */
    if ((Ring->Mem.La & (Ring->Length - 1)) != 0) {
        return STATUS_DATATYPE_MISALIGNMENT;
    }
    Ring->Ctxt = Ctxt;
    MhiCtxtSet (Ctxt, CTXT_RBASE, Ring->Mem.La);
    MhiCtxtSet (Ctxt, CTXT_RLEN, Ring->Length);
    MhiCtxtSet (Ctxt, CTXT_RP, Ring->Mem.La);
    MhiCtxtSet (Ctxt, CTXT_WP, Ring->Mem.La);
    return STATUS_SUCCESS;
}

NTSTATUS
WcnMhiPrepare (
    _In_ PWCN_DEVICE Dev
    )
{
    static const ULONG Elements[MHI_EVENT_RINGS] = { MHI_CTRL_EV_ELEMENTS, MHI_DATA_EV_ELEMENTS };
    static const ULONG IntMod[MHI_EVENT_RINGS] = { 0, 1 };
    PWCN_MHI Mhi = &Dev->Mhi;
    PMHI_CTXT Chan;
    PMHI_CTXT Event;
    ULONG Index;
    NTSTATUS Status;

    Mhi->Bhi = WcnBarRead (Dev, MHI_BHIOFF);
    Mhi->Bhie = WcnBarRead (Dev, MHI_BHIEOFF);

    if (Mhi->Bhi == 0 || Mhi->Bhi >= Dev->BarLength || Mhi->Bhie == 0 || Mhi->Bhie >= Dev->BarLength) {
        Mhi->Bhi = 0;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* PEACH sets rddm_size: clear the BHIe RX registers, then hand over the RDDM table */
    for (Index = BHIE_RXVECADDR_LOW; Index <= BHIE_RXVECSTATUS; Index += 4) {
        WcnBarWrite (Dev, Mhi->Bhie + Index, 0);
    }
    Status = MhiAllocBhie (Dev, &Mhi->Rddm, NULL, MHI_RDDM_SIZE);
    if (NT_SUCCESS (Status)) {
        MhiRddmPrepare (Dev);
    }

    Status = WcnDmaAlloc (Dev, MHI_MAX_CHANNELS * sizeof (MHI_CTXT), &Mhi->ChanCtxt);
    if (NT_SUCCESS (Status)) {
        Status = WcnDmaAlloc (Dev, MHI_EVENT_RINGS * sizeof (MHI_CTXT), &Mhi->EventCtxt);
    }
    if (NT_SUCCESS (Status)) {
        Status = WcnDmaAlloc (Dev, sizeof (MHI_CTXT), &Mhi->CmdCtxt);
    }
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "mhi: context allocation failed 0x%08x", Status);
        return Status;
    }

    /* IPCR: 20 to the device, 21 from the device, both on event ring 1 */
    Chan = (PMHI_CTXT)Mhi->ChanCtxt.Va;
    for (Index = 20; Index <= 21; Index++) {
        Chan[Index].Dword0 = MHI_DB_BRST_DISABLE << 8;
        Chan[Index].Dword1 = (Index == 20) ? 1 : 2;
        Chan[Index].Dword2 = 1;
    }

    Event = (PMHI_CTXT)Mhi->EventCtxt.Va;
    for (Index = 0; Index < MHI_EVENT_RINGS; Index++) {
        Event[Index].Dword0 = IntMod[Index] << 16;
        Event[Index].Dword1 = 1;
        Event[Index].Dword2 = Index + 1;
        Status = MhiRingAlloc (Dev, &Mhi->Event[Index], Elements[Index], &Event[Index]);
        if (!NT_SUCCESS (Status)) {
            WcnLog (Dev, "mhi: event ring %u allocation failed 0x%08x", Index, Status);
            return Status;
        }
    }

    Status = MhiRingAlloc (Dev, &Mhi->Cmd, MHI_CMD_ELEMENTS, (PMHI_CTXT)Mhi->CmdCtxt.Va);
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "mhi: command ring allocation failed 0x%08x", Status);
        return Status;
    }

    return STATUS_SUCCESS;
}

/* boot.c mhi_fw_load_bhi: one contiguous image, PBL pulls it in. */
static
NTSTATUS
MhiLoadBhi (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Size) PUCHAR Image,
    _In_ SIZE_T Size
    )
{
    ULONG Bhi = Dev->Mhi.Bhi;
    LONGLONG Start = MhiNowMs ();
    ULONG Sequence;
    ULONG TxStatus = 0;
    WCN_DMA Dma;
    NTSTATUS Status;

    Status = WcnDmaAlloc (Dev, Size, &Dma);
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "bhi: buffer allocation failed 0x%08x", Status);
        return Status;
    }
    RtlCopyMemory (Dma.Va, Image, Size);
    KeMemoryBarrier ();

    Sequence = MhiSequence ();
    WcnBarWrite (Dev, Bhi + BHI_STATUS, 0);
    WcnBarWrite (Dev, Bhi + BHI_IMGADDR_HIGH, (ULONG)(Dma.La >> 32));
    WcnBarWrite (Dev, Bhi + BHI_IMGADDR_LOW, (ULONG)Dma.La);
    WcnBarWrite (Dev, Bhi + BHI_IMGSIZE, (ULONG)Size);
    WcnBarWrite (Dev, Bhi + BHI_IMGTXDB, Sequence);

    while (!Dev->Stop && MhiNowMs () - Start < MHI_BHI_TIMEOUT_MS) {
        TxStatus = WcnBarRead (Dev, Bhi + BHI_STATUS) >> 30;
        if (TxStatus != 0) {
            break;
        }
        WcnWait (Dev, 5);
    }

    WdfObjectDelete (Dma.Buffer);

    if (TxStatus == BHI_STATUS_SUCCESS) {
        return STATUS_SUCCESS;
    }

    return (TxStatus == 0) ? STATUS_IO_TIMEOUT : STATUS_DEVICE_HARDWARE_ERROR;
}

/*
 * boot.c mhi_alloc_bhie_table + mhi_firmware_copy_bhie: seg_len segments and a
 * trailing vector table. Image is NULL for the RDDM (receive) table.
 */
static
NTSTATUS
MhiAllocBhie (
    _In_ PWCN_DEVICE Dev,
    _Out_ PWCN_BHIE_TABLE Table,
    _In_reads_bytes_opt_ (Size) PUCHAR Image,
    _In_ SIZE_T Size
    )
{
    ULONG Segments = (ULONG)((Size + MHI_SEG_LEN - 1) / MHI_SEG_LEN);
    PULONG64 Vector;
    SIZE_T Offset;
    SIZE_T Chunk;
    ULONG Index;
    NTSTATUS Status;

    RtlZeroMemory (Table, sizeof (*Table));
    if (Segments == 0 || Segments > MHI_MAX_SEGMENTS) {
        return STATUS_INVALID_IMAGE_FORMAT;
    }
    Status = WcnDmaAlloc (Dev, Segments * 2 * sizeof (ULONG64), &Table->Vector);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }
    Vector = (PULONG64)Table->Vector.Va;
    for (Index = 0, Offset = 0; Index < Segments; Index++, Offset += MHI_SEG_LEN) {
        Chunk = Size - Offset;
        if (Chunk > MHI_SEG_LEN) {
            Chunk = MHI_SEG_LEN;
        }
        Status = WcnDmaAlloc (Dev, MHI_SEG_LEN, &Table->Segment[Index]);
        if (!NT_SUCCESS (Status)) {
            WcnLog (Dev, "bhie: segment %u allocation failed 0x%08x", Index, Status);
            return Status;
        }
        if (Image != NULL) {
            RtlCopyMemory (Table->Segment[Index].Va, Image + Offset, Chunk);
        } else {
            Chunk = MHI_SEG_LEN;
        }
        Vector[Index * 2] = Table->Segment[Index].La;
        Vector[Index * 2 + 1] = Chunk;
    }
    Table->Segments = Segments;
    KeMemoryBarrier ();
    return STATUS_SUCCESS;
}

/* boot.c mhi_rddm_prepare: where the device uploads its RAM if it crashes */
static
VOID
MhiRddmPrepare (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    ULONG Value;

    WcnBarWrite (Dev, Mhi->Bhie + BHIE_RXVECADDR_HIGH, (ULONG)(Mhi->Rddm.Vector.La >> 32));
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_RXVECADDR_LOW, (ULONG)Mhi->Rddm.Vector.La);
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_RXVECSIZE, Mhi->Rddm.Segments * 2 * sizeof (ULONG64));
    Value = WcnBarRead (Dev, Mhi->Bhie + BHIE_RXVECDB);
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_RXVECDB, (Value & ~0x3FFFFFFFUL) | MhiSequence ());
}

/* boot.c mhi_fw_load_bhie, issued when SBL reports its EE */
static
NTSTATUS
MhiLoadBhie (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    LONGLONG Start = MhiNowMs ();
    ULONG Sequence;
    ULONG Value;
    ULONG TxStatus = 0;

    if (Mhi->Amss.Segments == 0) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    Sequence = MhiSequence ();
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_TXVECADDR_HIGH, (ULONG)(Mhi->Amss.Vector.La >> 32));
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_TXVECADDR_LOW, (ULONG)Mhi->Amss.Vector.La);
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_TXVECSIZE, Mhi->Amss.Segments * 2 * sizeof (ULONG64));
    Value = WcnBarRead (Dev, Mhi->Bhie + BHIE_TXVECDB);
    WcnBarWrite (Dev, Mhi->Bhie + BHIE_TXVECDB, (Value & ~0x3FFFFFFFUL) | Sequence);

    while (!Dev->Stop && MhiNowMs () - Start < MHI_BHIE_TIMEOUT_MS) {
        Value = WcnBarRead (Dev, Mhi->Bhie + BHIE_TXVECSTATUS);
        TxStatus = Value >> 30;
        if (TxStatus != 0) {
            break;
        }
        WcnWait (Dev, 5);
    }

    if (TxStatus == BHIE_STATUS_XFER_COMPL) {
        return STATUS_SUCCESS;
    }

    return (TxStatus == 0) ? STATUS_IO_TIMEOUT : STATUS_DEVICE_HARDWARE_ERROR;
}

/* pm.c mhi_ready_state_transition + init.c mhi_init_mmio */
static
NTSTATUS
MhiReady (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    PWCN_RING Ring;
    ULONG Config;
    ULONG Index;

    if (!MhiPoll (Dev, MHI_CTRL, MHI_CTRL_RESET, 0, 10000)) {

        return STATUS_IO_TIMEOUT;
    }
    if (!MhiPoll (Dev, MHI_STATUS, MHI_STATUS_READY, MHI_STATUS_READY, MHI_READY_TIMEOUT_MS)) {

        return STATUS_IO_TIMEOUT;
    }

    Mhi->ChDb = WcnBarRead (Dev, MHI_CHDBOFF);
    Mhi->ErDb = WcnBarRead (Dev, MHI_ERDBOFF);

    if (Mhi->ChDb + 8 * MHI_MAX_CHANNELS > Dev->BarLength || Mhi->ErDb + 8 * MHI_EVENT_RINGS > Dev->BarLength) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (Index = 0; Index < MHI_EVENT_RINGS; Index++) {
        Mhi->Event[Index].Doorbell = Mhi->ErDb + 8 * Index;
    }
    Mhi->Cmd.Doorbell = MHI_CRDB_LOWER;

    WcnBarWrite (Dev, MHI_CCABAP_HIGHER, (ULONG)(Mhi->ChanCtxt.La >> 32));
    WcnBarWrite (Dev, MHI_CCABAP_LOWER, (ULONG)Mhi->ChanCtxt.La);
    WcnBarWrite (Dev, MHI_ECABAP_HIGHER, (ULONG)(Mhi->EventCtxt.La >> 32));
    WcnBarWrite (Dev, MHI_ECABAP_LOWER, (ULONG)Mhi->EventCtxt.La);
    WcnBarWrite (Dev, MHI_CRCBAP_HIGHER, (ULONG)(Mhi->CmdCtxt.La >> 32));
    WcnBarWrite (Dev, MHI_CRCBAP_LOWER, (ULONG)Mhi->CmdCtxt.La);
    WcnBarWrite (Dev, MHI_CTRLBASE_HIGHER, 0);
    WcnBarWrite (Dev, MHI_CTRLBASE_LOWER, 0);
    WcnBarWrite (Dev, MHI_DATABASE_HIGHER, 0);
    WcnBarWrite (Dev, MHI_DATABASE_LOWER, 0);
    WcnBarWrite (Dev, MHI_CTRLLIMIT_HIGHER, (ULONG)(MHI_DMA_LIMIT >> 32));
    WcnBarWrite (Dev, MHI_CTRLLIMIT_LOWER, (ULONG)(MHI_DMA_LIMIT & 0xFFFFFFFF));
    WcnBarWrite (Dev, MHI_DATALIMIT_HIGHER, (ULONG)(MHI_DMA_LIMIT >> 32));
    WcnBarWrite (Dev, MHI_DATALIMIT_LOWER, (ULONG)(MHI_DMA_LIMIT & 0xFFFFFFFF));

    Config = WcnBarRead (Dev, MHI_CFG);
    Config = (Config & ~0x00FF0000UL) | (MHI_EVENT_RINGS << 16);
    WcnBarWrite (Dev, MHI_CFG, Config);
    Config &= ~0xFF000000UL;
    WcnBarWrite (Dev, MHI_CFG, Config);

    /* hand every software event ring to the device, minus one element */
    for (Index = 0; Index < MHI_EVENT_RINGS; Index++) {
        Ring = &Mhi->Event[Index];
        Ring->Wp = Ring->Length - sizeof (MHI_ELEMENT);
        MhiCtxtSet (Ring->Ctxt, CTXT_WP, Ring->Mem.La + Ring->Wp);
        MhiWriteDoorbell (Dev, Ring->Doorbell, Ring->Mem.La + Ring->Wp);
    }

    MhiSetState (Dev, MHI_STATE_M0);
    Mhi->Ready = TRUE;

    return STATUS_SUCCESS;
}

static
VOID
MhiRingEventDoorbells (
    _In_ PWCN_DEVICE Dev
    )
{
    ULONG Index;

    for (Index = 0; Index < MHI_EVENT_RINGS; Index++) {
        MhiWriteDoorbell (Dev, Dev->Mhi.Event[Index].Doorbell, Dev->Mhi.Event[Index].Mem.La + Dev->Mhi.Event[Index].Wp);
    }
}

static
PWCN_CHAN
MhiChannel (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Id
    )
{
    if (Id == MHI_IPCR_UL) {
        return &Dev->Mhi.Chan[0];
    }
    if (Id == MHI_IPCR_DL) {
        return &Dev->Mhi.Chan[1];
    }
    return NULL;
}

/* main.c mhi_ring_chan_db */
static
VOID
MhiRingChannelDoorbell (
    _In_ PWCN_DEVICE Dev,
    _In_ PWCN_CHAN Chan
    )
{
    ULONG64 Wp = Chan->Ring.Mem.La + Chan->Ring.Wp;

    KeMemoryBarrier ();
    MhiCtxtSet (Chan->Ring.Ctxt, CTXT_WP, Wp);
    MhiWriteDoorbell (Dev, Chan->Ring.Doorbell, Wp);
}

/* main.c mhi_gen_tre with MHI_EOT: one buffer per transfer, event at its end */
static
BOOLEAN
MhiQueueTre (
    _In_ PWCN_CHAN Chan,
    _In_ ULONG Slot,
    _In_ ULONG Length
    )
{
    PWCN_RING Ring = &Chan->Ring;
    PMHI_ELEMENT Tre;
    ULONG Next = (Ring->Wp + sizeof (MHI_ELEMENT)) % Ring->Length;

    if (Next == Ring->Rp) {
        return FALSE;
    }
    Tre = (PMHI_ELEMENT)(Ring->Mem.Va + Ring->Wp);
    Tre->Ptr = Chan->Buffers.La + (ULONG64)Slot * MHI_CHAN_BUF_LEN;
    Tre->Dword[0] = Length & 0xFFFF;
    Tre->Dword[1] = MHI_TRE_DWORD1_EOT;
    Chan->Slot[Ring->Wp / sizeof (MHI_ELEMENT)] = (UCHAR)Slot;
    Ring->Wp = Next;
    return TRUE;
}

/* main.c mhi_send_cmd + mhi_ring_cmd_db */
static
NTSTATUS
MhiSendCommand (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Channel,
    _In_ ULONG Command
    )
{
    PWCN_RING Ring = &Dev->Mhi.Cmd;
    PMHI_ELEMENT Tre;
    ULONG Next = (Ring->Wp + sizeof (MHI_ELEMENT)) % Ring->Length;
    ULONG64 Wp;

    if (Next == Ring->Rp) {
        return STATUS_DEVICE_BUSY;
    }
    Tre = (PMHI_ELEMENT)(Ring->Mem.Va + Ring->Wp);
    Tre->Ptr = 0;
    Tre->Dword[0] = 0;
    Tre->Dword[1] = (Channel << 24) | (Command << 16);
    Ring->Wp = Next;
    Wp = Ring->Mem.La + Ring->Wp;
    KeMemoryBarrier ();
    MhiCtxtSet (Ring->Ctxt, CTXT_WP, Wp);
    MhiWriteDoorbell (Dev, Ring->Doorbell, Wp);
    return STATUS_SUCCESS;
}

/* main.c mhi_process_cmd_completion */
static
VOID
MhiCommandCompletion (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG64 Ptr,
    _In_ ULONG Code
    )
{
    PWCN_RING Ring = &Dev->Mhi.Cmd;
    PMHI_ELEMENT Tre;
    PWCN_CHAN Chan;

    if (Ptr < Ring->Mem.La || Ptr >= Ring->Mem.La + Ring->Length || (Ptr & 0xF) != 0) {

        return;
    }
    Tre = (PMHI_ELEMENT)(Ring->Mem.Va + (ULONG)(Ptr - Ring->Mem.La));
    Chan = MhiChannel (Dev, Tre->Dword[1] >> 24);
    if (Chan != NULL) {
        Chan->Ccs = Code;
        Chan->Completed = TRUE;
    }
    Ring->Rp = (Ring->Rp + sizeof (MHI_ELEMENT)) % Ring->Length;
}

/* main.c mhi_process_ctrl_ev_ring, limited to what boot needs */
static
VOID
MhiProcessControlEvents (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    PWCN_RING Ring = &Mhi->Event[0];
    PMHI_ELEMENT Element;
    ULONG64 DeviceRp;
    ULONG Type;
    ULONG Code;
    ULONG Count = 0;
    NTSTATUS Status;

    for (;;) {
        KeMemoryBarrier ();
        DeviceRp = MhiCtxtGet (Ring->Ctxt, CTXT_RP);
        if (DeviceRp < Ring->Mem.La || DeviceRp >= Ring->Mem.La + Ring->Length) {

            Mhi->SysErr = TRUE;
            break;
        }
        if ((ULONG)(DeviceRp - Ring->Mem.La) == Ring->Rp) {
            break;
        }

        Element = (PMHI_ELEMENT)(Ring->Mem.Va + Ring->Rp);
        Type = (Element->Dword[1] >> 16) & 0xFF;
        Code = Element->Dword[0] >> 24;

        switch (Type) {
        case MHI_PKT_STATE_CHANGE:
            if (Code == MHI_STATE_M0) {
                if (!Mhi->WakeAsserted) {
                    MhiWriteDoorbell (Dev, Mhi->ChDb + 8 * MHI_DEV_WAKE_DB, 1);
                    Mhi->WakeAsserted = TRUE;
                }
                if (Mhi->MissionMode) {
                    MhiRingEventDoorbells (Dev);
                }

            } else if (Code == MHI_STATE_SYS_ERR) {

                Mhi->SysErr = TRUE;
            }
            break;

        case MHI_PKT_CMD_COMPLETION:
            MhiCommandCompletion (Dev, Element->Ptr, Code);
            break;

        case MHI_PKT_EE_EVENT:
            Mhi->Ee = Code;
            if (Code == MHI_EE_SBL) {
                Status = MhiLoadBhie (Dev);
                if (!NT_SUCCESS (Status)) {
                    Mhi->SysErr = TRUE;
                }
            } else if (Code == MHI_EE_AMSS || Code == MHI_EE_WFW) {
                Mhi->Ee = MhiExecEnv (Dev);
                Mhi->MissionMode = (Mhi->Ee == MHI_EE_AMSS || Mhi->Ee == MHI_EE_WFW);
            } else if (Code == MHI_EE_RDDM) {
                Mhi->SysErr = TRUE;
            }
            break;

        }

        Ring->Wp = (Ring->Wp + sizeof (MHI_ELEMENT)) % Ring->Length;
        MhiCtxtSet (Ring->Ctxt, CTXT_WP, Ring->Mem.La + Ring->Wp);
        Ring->Rp = (Ring->Rp + sizeof (MHI_ELEMENT)) % Ring->Length;
        Count++;
    }

    if (Count != 0) {
        MhiWriteDoorbell (Dev, Ring->Doorbell, Ring->Mem.La + Ring->Wp);
    }
}

/* pm.c mhi_async_power_up + mhi_sync_power_up, FBC download */
NTSTATUS
WcnMhiPowerUp (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    LONGLONG Start = MhiNowMs ();
    LONGLONG Report = Start + 2000;
    PUCHAR Rest;
    SIZE_T RestSize;
    ULONG Ee;
    ULONG State;
    ULONG Value;
    NTSTATUS Status;

    WcnBarWrite (Dev, Mhi->Bhi + BHI_INTVEC, 0);
    Ee = MhiExecEnv (Dev);
    State = MhiState (Dev);

    if (State == MHI_STATE_SYS_ERR) {
        WcnBarWrite (Dev, MHI_CTRL, WcnBarRead (Dev, MHI_CTRL) | MHI_CTRL_RESET);
        if (!MhiPoll (Dev, MHI_CTRL, MHI_CTRL_RESET, 0, 10000)) {

            return STATUS_IO_TIMEOUT;
        }
        WcnBarWrite (Dev, Mhi->Bhi + BHI_INTVEC, 0);
    }

    if (Ee == MHI_EE_PBL) {

        Status = MhiLoadBhi (Dev, Dev->Firmware, (Dev->FirmwareSize < MHI_SBL_SIZE) ? Dev->FirmwareSize : MHI_SBL_SIZE);
        if (!NT_SUCCESS (Status)) {
            return Status;
        }

        /* amss.bin carries SBL and the WLAN image as two ELF files */
        Rest = Dev->Firmware;
        RestSize = Dev->FirmwareSize;
        if (Dev->FirmwareSize > MHI_SBL_SIZE + 4 &&
            RtlCompareMemory (Dev->Firmware + MHI_SBL_SIZE, "\x7F" "ELF", 4) == 4) {
            Rest += MHI_SBL_SIZE;
            RestSize -= MHI_SBL_SIZE;
        }
        Status = MhiAllocBhie (Dev, &Mhi->Amss, Rest, RestSize);

        if (!NT_SUCCESS (Status)) {
            return Status;
        }
    } else if (Ee != MHI_EE_AMSS) {

        return STATUS_DEVICE_NOT_READY;
    }

    Status = MhiReady (Dev);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    while (!Dev->Stop) {
        MhiProcessControlEvents (Dev);
        if (Mhi->MissionMode || Mhi->SysErr) {
            break;
        }
        Value = WcnBarRead (Dev, MHI_STATUS);
        if ((Value & MHI_STATUS_SYSERR) != 0) {

            Mhi->SysErr = TRUE;
            break;
        }
        if (MhiNowMs () > Report) {

            Report += 2000;
        }
        if (MhiNowMs () - Start > MHI_MISSION_TIMEOUT_MS) {

            return STATUS_IO_TIMEOUT;
        }
        WcnWait (Dev, 10);
    }

    if (Mhi->SysErr || !Mhi->MissionMode) {

        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    return STATUS_SUCCESS;
}

/* ath12k_pci_power_down: MSI off, MHI reset, SoC reset */
VOID
WcnMhiPowerDown (
    _In_ PWCN_DEVICE Dev
    )
{
    if (Dev->Bar == NULL || Dev->Mhi.Bhi == 0) {
        return;
    }
    WcnPcieMsiEnable (Dev, FALSE);
    WcnBarWrite (Dev, MHI_CTRL, WcnBarRead (Dev, MHI_CTRL) | MHI_CTRL_RESET);
    WcnSleep (10);
    WcnPcieSwReset (Dev, FALSE);
    Dev->Mhi.MissionMode = FALSE;
    Dev->Mhi.WakeAsserted = FALSE;
}

/* main.c mhi_prepare_channel: init.c mhi_init_chan_ctxt, then START and wait */
static
NTSTATUS
MhiStartChannel (
    _In_ PWCN_DEVICE Dev,
    _In_ PWCN_CHAN Chan,
    _In_ ULONG Id
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    PMHI_CTXT Ctxt = (PMHI_CTXT)Mhi->ChanCtxt.Va + Id;
    LONGLONG Deadline;
    NTSTATUS Status;

    Chan->Id = Id;
    Status = MhiRingAlloc (Dev, &Chan->Ring, MHI_CHAN_ELEMENTS, Ctxt);
    if (NT_SUCCESS (Status)) {
        Status = WcnDmaAlloc (Dev, (SIZE_T)MHI_CHAN_ELEMENTS * MHI_CHAN_BUF_LEN, &Chan->Buffers);
    }
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "chan %u: allocation failed 0x%08x", Id, Status);
        return Status;
    }
    Chan->Ring.Doorbell = Mhi->ChDb + 8 * Id;
    Ctxt->Dword0 = (Ctxt->Dword0 & ~0xFFUL) | MHI_CH_STATE_ENABLED;
    KeMemoryBarrier ();

    Chan->Completed = FALSE;
    Status = MhiSendCommand (Dev, Id, MHI_CMD_START_CHAN);
    if (!NT_SUCCESS (Status)) {

        return Status;
    }
    Deadline = MhiNowMs () + 2000;
    for (;;) {
        MhiProcessControlEvents (Dev);
        if (Chan->Completed || Dev->Stop || MhiNowMs () > Deadline) {
            break;
        }
        WcnWait (Dev, 10);
    }

    if (!Chan->Completed) {
        return STATUS_IO_TIMEOUT;
    }
    if (Chan->Ccs != MHI_EV_CC_SUCCESS) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }
    Chan->Running = TRUE;
    return STATUS_SUCCESS;
}

/*
 * qrtr mhi.c probe: mhi_prepare_for_transfer (UL, then DL) and fill the DL
 * ring with receive buffers.
 */
NTSTATUS
WcnMhiStartChannels (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_CHAN Ul = &Dev->Mhi.Chan[0];
    PWCN_CHAN Dl = &Dev->Mhi.Chan[1];
    ULONG Slot;
    NTSTATUS Status;

    Status = MhiStartChannel (Dev, Ul, MHI_IPCR_UL);
    if (NT_SUCCESS (Status)) {
        Status = MhiStartChannel (Dev, Dl, MHI_IPCR_DL);
    }
    if (!NT_SUCCESS (Status)) {
        return Status;
    }

    for (Slot = 0; Slot < MHI_CHAN_ELEMENTS - 1; Slot++) {
        MhiQueueTre (Dl, Slot, MHI_CHAN_BUF_LEN);
    }
    MhiRingChannelDoorbell (Dev, Dl);

    WcnQrtrStart (Dev);
    return STATUS_SUCCESS;
}

/* One QRTR packet on IPCR UL: header and payload, payload padded to 4 bytes. */
NTSTATUS
WcnMhiSend (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (HeaderLength) const VOID *Header,
    _In_ ULONG HeaderLength,
    _In_reads_bytes_opt_ (PayloadLength) const VOID *Payload,
    _In_ ULONG PayloadLength
    )
{
    PWCN_CHAN Ul = &Dev->Mhi.Chan[0];
    ULONG Slot = Ul->Ring.Wp / sizeof (MHI_ELEMENT);
    ULONG Padded = (PayloadLength + 3) & ~3UL;
    PUCHAR Buffer;

    if (!Ul->Running) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (HeaderLength + Padded > MHI_CHAN_BUF_LEN) {
        return STATUS_BUFFER_OVERFLOW;
    }
    Buffer = Ul->Buffers.Va + (SIZE_T)Slot * MHI_CHAN_BUF_LEN;
    RtlCopyMemory (Buffer, Header, HeaderLength);
    if (Payload != NULL && PayloadLength != 0) {
        RtlCopyMemory (Buffer + HeaderLength, Payload, PayloadLength);
    }
    RtlZeroMemory (Buffer + HeaderLength + PayloadLength, Padded - PayloadLength);
    if (!MhiQueueTre (Ul, Slot, HeaderLength + Padded)) {
        return STATUS_DEVICE_BUSY;
    }
    MhiRingChannelDoorbell (Dev, Ul);
    return STATUS_SUCCESS;
}

/* main.c parse_xfer_event for single-TRE transfers */
static
VOID
MhiTransferEvent (
    _In_ PWCN_DEVICE Dev,
    _In_ PMHI_ELEMENT Event
    )
{
    ULONG Id = Event->Dword[1] >> 24;
    ULONG Code = Event->Dword[0] >> 24;
    ULONG Length = Event->Dword[0] & 0xFFFF;
    PWCN_CHAN Chan = MhiChannel (Dev, Id);
    PWCN_RING Ring;
    PMHI_ELEMENT Tre;
    ULONG EventPos;
    ULONG DeviceNext;
    ULONG Slot;
    ULONG Bytes;
    BOOLEAN Requeued = FALSE;

    if (Chan == NULL || !Chan->Running) {

        return;
    }
    Ring = &Chan->Ring;

    switch (Code) {
    case MHI_EV_CC_EOT:
    case MHI_EV_CC_EOB:
    case MHI_EV_CC_OVERFLOW:
        if (Event->Ptr < Ring->Mem.La || Event->Ptr >= Ring->Mem.La + Ring->Length || (Event->Ptr & 0xF) != 0) {

            return;
        }
        if (Code == MHI_EV_CC_OVERFLOW) {
            WcnLog (Dev, "chan %u: overflow, length %u", Id, Length);
        }
        EventPos = (ULONG)(Event->Ptr - Ring->Mem.La);
        DeviceNext = (EventPos + sizeof (MHI_ELEMENT)) % Ring->Length;
        while (Ring->Rp != DeviceNext) {
            Tre = (PMHI_ELEMENT)(Ring->Mem.Va + Ring->Rp);
            Bytes = (Ring->Rp == EventPos) ? Length : (Tre->Dword[0] & 0xFFFF);
            if (Bytes > MHI_CHAN_BUF_LEN) {
                Bytes = MHI_CHAN_BUF_LEN;
            }
            Slot = Chan->Slot[Ring->Rp / sizeof (MHI_ELEMENT)];
            Ring->Rp = (Ring->Rp + sizeof (MHI_ELEMENT)) % Ring->Length;
            if (Id == MHI_IPCR_DL) {
                WcnQrtrReceive (Dev, Chan->Buffers.Va + (SIZE_T)Slot * MHI_CHAN_BUF_LEN, Bytes);
                MhiQueueTre (Chan, Slot, MHI_CHAN_BUF_LEN);
                Requeued = TRUE;
            }
        }
        if (Requeued) {
            MhiRingChannelDoorbell (Dev, Chan);
        }
        break;

    case MHI_EV_CC_OOB:
    case MHI_EV_CC_DB_MODE:
        if (Ring->Wp != Ring->Rp) {
            MhiRingChannelDoorbell (Dev, Chan);
        }
        break;

    }
}

/* main.c mhi_process_data_event_ring for event ring 1 (IPCR) */
static
VOID
MhiProcessDataEvents (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;
    PWCN_RING Ring = &Mhi->Event[1];
    PMHI_ELEMENT Element;
    ULONG64 DeviceRp;
    ULONG Type;
    ULONG Count = 0;

    while (Count < 256) {
        KeMemoryBarrier ();
        DeviceRp = MhiCtxtGet (Ring->Ctxt, CTXT_RP);
        if (DeviceRp < Ring->Mem.La || DeviceRp >= Ring->Mem.La + Ring->Length) {
            break;
        }
        if ((ULONG)(DeviceRp - Ring->Mem.La) == Ring->Rp) {
            break;
        }

        Element = (PMHI_ELEMENT)(Ring->Mem.Va + Ring->Rp);
        Type = (Element->Dword[1] >> 16) & 0xFF;
        if (Type == MHI_PKT_TX_EVENT) {
            MhiTransferEvent (Dev, Element);
        }

        Ring->Wp = (Ring->Wp + sizeof (MHI_ELEMENT)) % Ring->Length;
        MhiCtxtSet (Ring->Ctxt, CTXT_WP, Ring->Mem.La + Ring->Wp);
        Ring->Rp = (Ring->Rp + sizeof (MHI_ELEMENT)) % Ring->Length;
        Count++;
    }

    if (Count != 0) {
        MhiWriteDoorbell (Dev, Ring->Doorbell, Ring->Mem.La + Ring->Wp);
    }
}

/* main.c / ath12k coredump: collect the RDDM upload once the device asserts */
static VOID MhiLatchRddm(PWCN_DEVICE Dev)
{
    Dev->Mhi.RddmSaved=TRUE;WcnLog(Dev,"firmware RDDM failure; data path stopped");
}

/* Called from the bring-up thread after power up: keeps the event rings drained. */
VOID
WcnMhiService (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_MHI Mhi = &Dev->Mhi;

    if (!Mhi->Ready) {
        return;
    }
    MhiProcessControlEvents (Dev);
    if (Mhi->Chan[1].Running) {
        MhiProcessDataEvents (Dev);
    }
    if (!Mhi->RddmSaved && MhiExecEnv (Dev) == MHI_EE_RDDM) {
        MhiLatchRddm (Dev);
    }
}
