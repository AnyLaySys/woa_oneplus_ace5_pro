#include "sia9177.h"

#define SMMU_PHYS               0x15000000ull
#define SMMU_MAP_SIZE           0x1000u
#define SMMU_IDR0               0x020u
#define SMMU_SMR(i)             (0x800u + 4u * (i))
#define SMMU_S2CR(i)            (0xC00u + 4u * (i))
#define SMMU_SMR_VALID          0x80000000u
#define SMMU_MAX_SMR            256u
#define SMMU_UFS_STREAM         0x60u
#define SMMU_STREAMS            2u
#define SMMU_LPASS_SID          1u
#define SMMU_ALLOC_LIMIT        128u

#define TLMM_STRIDE             0x1000u
#define TLMM_I2S_FIRST          121u
#define TLMM_I2S_LAST           124u
#define TLMM_I2S_PINS           (TLMM_I2S_LAST - TLMM_I2S_FIRST + 1u)

#define SHM_SIZE                0x10000u
#define PCM_BUFFERS             2u
#define PCM_STRIDE              0x3000u

#define RPL_MAGIC               0x4C505253u
#define RPL_MAX_SIZE            0x100000u
#define RPL_CMD                 1u
#define RPL_OOB                 2u
#define RPL_AMP                 3u
#define RPL_STREAM              4u
#define RPL_DELAY               5u
#define RPL_FLAG_OOB            1u

#define GPR_PORT_APM            1u
#define GPR_PORT_PRM            2u
#define GPR_PORT_KERNEL         7u
#define GPR_PORT_STREAM         0x2012u
#define GPR_PORT_WR_EP          0x4001u
#define GPR_TOKEN_MAP           0x4D41u
#define GPR_TOKEN_UNMAP         0x4D55u
#define GPR_TOKEN_PRM           0x5052u

#define GPR_IBASIC_RSP_RESULT               0x02001005u
#define APM_CMD_SHARED_MEM_MAP_REGIONS      0x0100100Cu
#define APM_CMD_SHARED_MEM_UNMAP_REGIONS    0x0100100Du
#define APM_CMD_RSP_SHARED_MEM_MAP_REGIONS  0x02001001u
#define APM_MEMORY_MAP_SHMEM8_4K_POOL       3u
#define PRM_CMD_REQUEST_HW_RSC              0x0100100Fu
#define PRM_CMD_RELEASE_HW_RSC              0x01001010u
#define PRM_CMD_RSP_REQUEST_HW_RSC          0x02001002u
#define PRM_CMD_RSP_RELEASE_HW_RSC          0x02001003u
#define PRM_MODULE_INSTANCE_ID              2u
#define PARAM_ID_RSC_AUDIO_HW_CLK           0x0800102Cu
#define CLOCK_ID_SEC_MI2S_IBIT              0x102u
#define CLOCK_SEC_MI2S_HZ                   3072000u
#define CLOCK_ATTRIBUTE_COUPLE_NO           1u
#define DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2            0x0400100Au
#define DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2   0x05001004u

#define SIA_REPLAY_PATH         L"\\SystemRoot\\System32\\drivers\\sia9177\\replay.bin"
#define SIA_AUDIO_TAG           'oduA'

typedef struct _SIA_AUDIO_WAIT {
    ULONG Token;
    ULONG Opcode;
    ULONG RspOpcode;
    ULONG SourcePort;
    ULONG DestinationPort;
    BOOLEAN Matched;
    ULONG Status;
    ULONG Value;
} SIA_AUDIO_WAIT;

typedef struct _SIA_AUDIO {
    PUCHAR Smmu;
    ULONG SmmuSlot[SMMU_STREAMS];
    ULONG SmmuSmr[SMMU_STREAMS];
    BOOLEAN SmmuNewSlot[SMMU_STREAMS];
    ULONG SmmuActiveS2cr;
    PUCHAR Shm;
    PHYSICAL_ADDRESS ShmPhys;
    ULONGLONG DspAddress;
    ULONG Handle;
    BOOLEAN MapPending;
    BOOLEAN Cleanup;
    BOOLEAN ClockOn;
    BOOLEAN PinsOn;
    BOOLEAN AmpsOn;
    BOOLEAN Failed;
    BOOLEAN Streaming;
    ULONG PinCtl[TLMM_I2S_PINS];
    BOOLEAN Busy[PCM_BUFFERS];
    ULONG Sent;
    ULONG Done;
    ULONG BadStatus;
    ULONG Unexpected;
    LONGLONG LastDone;
    SIA_GPR_PACKET Rx;
} SIA_AUDIO, *PSIA_AUDIO;

static const ULONG SiaStreamMatches[SMMU_STREAMS] = { 0x80801001u, 0x80201041u };

static const UCHAR SiaAmpAddress[] = { 0x6C, 0x6D };

static
LONGLONG
AuNowMs (
    VOID
    )
{
    return (LONGLONG)(KeQueryInterruptTime () / 10000);
}

static
VOID
AuShmWrite (
    _In_ PSIA_AUDIO Au,
    _In_ ULONG Offset,
    _In_reads_bytes_opt_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Index;
    ULONG Word;

    for (Index = 0; Index < Length; Index += 4) {
        Word = 0;
        if (Data != NULL) RtlCopyMemory (&Word, Data + Index, min (4u, Length - Index));
        SiaWrite32 (Au->Shm, Offset + Index, Word);
    }
}

static
VOID
AuDispatch (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_ const SIA_GPR_PACKET *Packet,
    _Inout_opt_ SIA_AUDIO_WAIT *Wait
    )
{
    ULONG Header;
    ULONG Token;
    ULONG Opcode;
    ULONG PayloadLength;
    const UCHAR *Payload;
    ULONG Word0;
    ULONG Word1;

    if (Packet->Length < SIA_GPR_HEADER) {
        return;
    }
    Header = ((Packet->Data[0] >> 4) & 0xFu) * 4u;
    if (Header < SIA_GPR_HEADER || Header > Packet->Length) {
        return;
    }
    Token = *(const ULONG UNALIGNED *)(Packet->Data + 16);
    Opcode = *(const ULONG UNALIGNED *)(Packet->Data + 20);
    Payload = Packet->Data + Header;
    PayloadLength = Packet->Length - Header;
    Word0 = PayloadLength >= 4 ? *(const ULONG UNALIGNED *)Payload : 0;
    Word1 = PayloadLength >= 8 ? *(const ULONG UNALIGNED *)(Payload + 4) : 0;

    if (Opcode == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 ||
        (Opcode == GPR_IBASIC_RSP_RESULT && Word0 == DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2)) {
        ULONG Status = Opcode == GPR_IBASIC_RSP_RESULT ? Word1 : (PayloadLength >= 16 ? *(const ULONG UNALIGNED *)(Payload + 12) : 0);
        ULONGLONG Address = Au->DspAddress + (ULONGLONG)Token * PCM_STRIDE;
        if (*(const ULONG UNALIGNED *)(Packet->Data + 8) != GPR_PORT_WR_EP ||
            *(const ULONG UNALIGNED *)(Packet->Data + 12) != GPR_PORT_STREAM ||
            (Opcode == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 &&
             (PayloadLength < 16 || Word0 != (ULONG)Address || Word1 != (ULONG)(Address >> 32) ||
              *(const ULONG UNALIGNED *)(Payload + 8) != Au->Handle))) {
            SiaLog (Dev, "pcm completion ignored: invalid port, address or handle, token 0x%x", Token);
            return;
        }
        if (Token < PCM_BUFFERS && Au->Busy[Token]) {
            Au->Busy[Token] = FALSE;
            Au->Done++;
            Au->LastDone = AuNowMs ();
            {
                KIRQL Irql;
                ULONGLONG Before = (ULONGLONG)InterlockedExchangeAdd64 (&Dev->PlayedBytes, SIA_PCM_BYTES);
                KeAcquireSpinLock (&Dev->NotifyLock, &Irql);
                if (Dev->NotifyEvent && Dev->NotifyBytes && Before / Dev->NotifyBytes != (Before + SIA_PCM_BYTES) / Dev->NotifyBytes)
                    KeSetEvent (Dev->NotifyEvent, IO_NO_INCREMENT, FALSE);
                KeReleaseSpinLock (&Dev->NotifyLock, Irql);
            }
        }
        if (Status != 0 || Opcode == GPR_IBASIC_RSP_RESULT) {
            Au->BadStatus++;
            if (Au->BadStatus <= 8) {
                SiaLog (Dev, "pcm buffer %lu rsp op 0x%08x status 0x%x", Token, Opcode, Status);
            }
        }
        return;
    }

    if (Wait != NULL && !Wait->Matched && Token == Wait->Token &&
        *(const ULONG UNALIGNED *)(Packet->Data + 8) == Wait->SourcePort &&
        *(const ULONG UNALIGNED *)(Packet->Data + 12) == Wait->DestinationPort) {
        if (Opcode == GPR_IBASIC_RSP_RESULT && PayloadLength >= 8 && Word0 == Wait->Opcode &&
            (Wait->RspOpcode == 0 || Word1 != 0)) {
            Wait->Matched = TRUE;
            Wait->Status = Word1;
            return;
        }
        if (Wait->RspOpcode != 0 && Opcode == Wait->RspOpcode &&
            PayloadLength >= (Opcode == APM_CMD_RSP_SHARED_MEM_MAP_REGIONS ? 4u : 8u)) {
            Wait->Matched = TRUE;
            Wait->Value = Word0;
            Wait->Status = Opcode == APM_CMD_RSP_SHARED_MEM_MAP_REGIONS ? 0 : Word1;
            return;
        }
    }

    Au->Unexpected++;
    if (Au->Unexpected <= 32) {
        SiaLog (Dev, "gpr rx op 0x%08x token 0x%x src 0x%x dst 0x%x len %lu p0 0x%08x p1 0x%08x",
                Opcode, Token, *(const ULONG UNALIGNED *)(Packet->Data + 8), *(const ULONG UNALIGNED *)(Packet->Data + 12),
                Packet->Length, Word0, Word1);
    }
}

static
VOID
AuPump (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _Inout_opt_ SIA_AUDIO_WAIT *Wait
    )
{
    SiaLinkPoll (Dev);
    while (SiaGprPop (Dev, &Au->Rx)) {
        AuDispatch (Dev, Au, &Au->Rx, Wait);
    }
}

static
NTSTATUS
AuTransact (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_reads_bytes_ (Length) const UCHAR *Packet,
    _In_ ULONG Length,
    _In_ ULONG RspOpcode,
    _In_ ULONG TimeoutMs,
    _Out_opt_ ULONG *Status,
    _Out_opt_ ULONG *Value
    )
{
    SIA_AUDIO_WAIT Wait;
    LONGLONG Start;
    NTSTATUS Result;

    RtlZeroMemory (&Wait, sizeof (Wait));
    Wait.Token = *(const ULONG UNALIGNED *)(Packet + 16);
    Wait.Opcode = *(const ULONG UNALIGNED *)(Packet + 20);
    Wait.RspOpcode = RspOpcode;
    Wait.SourcePort = *(const ULONG UNALIGNED *)(Packet + 12);
    Wait.DestinationPort = *(const ULONG UNALIGNED *)(Packet + 8);
    if (Status != NULL) {
        *Status = MAXULONG;
    }
    if (Value != NULL) {
        *Value = 0;
    }
    if ((Dev->Stopping != 0 || !Dev->Running) && !Au->Cleanup) {
        return STATUS_CANCELLED;
    }

    KeMemoryBarrier ();
    Result = SiaGprSendRaw (Dev, Packet, Length);
    if (!NT_SUCCESS (Result)) {
        return Result;
    }
    Start = AuNowMs ();
    for (;;) {
        AuPump (Dev, Au, &Wait);
        if (Wait.Matched) {
            break;
        }
        if ((Dev->Stopping != 0 || !Dev->Running) && !Au->Cleanup) {
            return STATUS_CANCELLED;
        }
        if (AuNowMs () - Start > (LONGLONG)TimeoutMs) {
            return STATUS_IO_TIMEOUT;
        }
        SiaSleep (1);
    }
    if (Status != NULL) {
        *Status = Wait.Status;
    }
    if (Value != NULL) {
        *Value = Wait.Value;
    }
    return STATUS_SUCCESS;
}

static
BOOLEAN
AuSmrOverlap (
    _In_ ULONG Smr,
    _In_ ULONG Id,
    _In_ ULONG Mask
    )
{
    ULONG SmrId = Smr & 0x7FFFu;
    ULONG SmrMask = (Smr >> 16) & 0x7FFFu;

    return (Smr & SMMU_SMR_VALID) != 0 && ((SmrId ^ Id) & ~(SmrMask | Mask) & 0x7FFFu) == 0;
}

static
NTSTATUS
AuSmmuSetup (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au
    )
{
    PHYSICAL_ADDRESS Physical;
    ULONG Count;
    ULONG Slot;
    ULONG Index;
    ULONG Smr;
    ULONG Wanted;
    ULONG ActiveS2cr;
    ULONG SavedS2cr;
    ULONG UfsS2cr = MAXULONG;
    ULONG FreeSlots[SMMU_STREAMS] = { MAXULONG, MAXULONG };
    ULONG FreeCount = 0;
    ULONG FreeUsed = 0;

    Physical.QuadPart = SMMU_PHYS;
    Au->Smmu = (PUCHAR)MmMapIoSpaceEx (Physical, SMMU_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (Au->Smmu == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Count = SiaRead32 (Au->Smmu, SMMU_IDR0) & 0xFFu;
    SiaLog (Dev, "smmu idr0=0x%08x smr groups %lu", SiaRead32 (Au->Smmu, SMMU_IDR0), Count);
    if (Count == 0 || Count > SMMU_MAX_SMR) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (Index = 0; Index < SMMU_STREAMS; Index++) {
        Au->SmmuSlot[Index] = MAXULONG;
    }
    for (Slot = 0; Slot < Count; Slot++) {
        Smr = SiaRead32 (Au->Smmu, SMMU_SMR (Slot));
        if (Smr == MAXULONG) {
            return STATUS_IO_DEVICE_ERROR;
        }
        if ((Smr & SMMU_SMR_VALID) == 0) {
            continue;
        }
        if (UfsS2cr == MAXULONG && AuSmrOverlap (Smr, SMMU_UFS_STREAM, 0)) {
            UfsS2cr = SiaRead32 (Au->Smmu, SMMU_S2CR (Slot));
            SiaLog (Dev, "smmu ufs slot %lu smr=0x%08x s2cr=0x%08x", Slot, Smr, UfsS2cr);
        }
        for (Index = 0; Index < SMMU_STREAMS; Index++) {
            Wanted = SiaStreamMatches[Index];
            if (AuSmrOverlap (Smr, Wanted & 0x7FFFu, (Wanted >> 16) & 0x7FFFu)) {
                SiaLog (Dev, "smmu stream 0x%x already in slot %lu smr=0x%08x s2cr=0x%08x",
                        (Wanted & 0x7FFFu), Slot, Smr, SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)));
                if (Smr != Wanted || Au->SmmuSlot[Index] != MAXULONG) {
                    SiaLog (Dev, "smmu overlapping stream configuration, refusing slot %lu", Slot);
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                }
                Au->SmmuSlot[Index] = Slot;
            }
        }
    }
    if (UfsS2cr == MAXULONG) {
        SiaLog (Dev, "smmu ufs stream 0x%x not found", SMMU_UFS_STREAM);
        return STATUS_NOT_FOUND;
    }
    for (Slot = min (Count, SMMU_ALLOC_LIMIT); Slot-- > 0 && FreeCount < SMMU_STREAMS;) {
        if ((SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) & SMMU_SMR_VALID) == 0) {
            FreeSlots[FreeCount++] = Slot;
        }
    }

    for (Index = 0; Index < SMMU_STREAMS; Index++) {
        Wanted = SiaStreamMatches[Index];
        Slot = Au->SmmuSlot[Index];
        if (Slot != MAXULONG) {
            ActiveS2cr = SiaRead32 (Au->Smmu, SMMU_S2CR (Slot));
            if (SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != Wanted ||
                (ActiveS2cr != UfsS2cr && ((ActiveS2cr >> 16) & 3u) != 1u)) {
                SiaLog (Dev, "smmu existing slot %lu is owned by another configuration", Slot);
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            continue;
        }
        if (FreeUsed >= FreeCount) {
            SiaLog (Dev, "smmu no free smr for stream 0x%x", (Wanted & 0x7FFFu));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Slot = FreeSlots[FreeUsed++];
        Au->SmmuSlot[Index] = Slot;
        Au->SmmuSmr[Index] = SiaRead32 (Au->Smmu, SMMU_SMR (Slot));
        SavedS2cr = SiaRead32 (Au->Smmu, SMMU_S2CR (Slot));
        if ((Au->SmmuSmr[Index] & SMMU_SMR_VALID) != 0 || SavedS2cr == MAXULONG ||
            SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != Au->SmmuSmr[Index] ||
            SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != SavedS2cr) {
            SiaLog (Dev, "smmu slot %lu changed during allocation", Slot);
            return STATUS_DEVICE_BUSY;
        }
        Au->SmmuActiveS2cr = UfsS2cr;
        Au->SmmuNewSlot[Index] = TRUE;
        SiaWrite32 (Au->Smmu, SMMU_S2CR (Slot), UfsS2cr);
        if (SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != UfsS2cr) {
            return STATUS_IO_DEVICE_ERROR;
        }
        KeMemoryBarrier ();
        SiaWrite32 (Au->Smmu, SMMU_SMR (Slot), Wanted);
        SiaLog (Dev, "smmu slot %lu stream 0x%x mask 0x%x: smr=0x%08x s2cr=0x%08x", Slot, (Wanted & 0x7FFFu), ((Wanted >> 16) & 0x7FFFu),
                SiaRead32 (Au->Smmu, SMMU_SMR (Slot)), SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)));
        if (SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != Wanted || SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != UfsS2cr) {
            return STATUS_IO_DEVICE_ERROR;
        }
    }
    return STATUS_SUCCESS;
}

static
VOID
AuSmmuRestore (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au
    )
{
    ULONG Index;
    ULONG Slot;

    for (Index = SMMU_STREAMS; Index-- > 0;) {
        Slot = Au->SmmuSlot[Index];
        if (!Au->SmmuNewSlot[Index]) {
            continue;
        }
        if (Au->Handle || Au->MapPending) {
            SiaLog (Dev, "smmu slot %lu retained while ADSP may own shared memory", Slot);
            continue;
        }
        if (SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != SiaStreamMatches[Index] ||
            SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != Au->SmmuActiveS2cr) {
            SiaLog (Dev, "smmu slot %lu ownership changed, leaving it untouched", Slot);
            continue;
        }
        SiaWrite32 (Au->Smmu, SMMU_SMR (Slot), Au->SmmuSmr[Index]);
        SiaLog (Dev, "smmu slot %lu released without writing inactive S2CR", Slot);
    }
    SiaLog (Dev, "smmu restored");
    MmUnmapIoSpace (Au->Smmu, SMMU_MAP_SIZE);
    Au->Smmu = NULL;
}

static
VOID
AuPins (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_ BOOLEAN On
    )
{
    ULONG Pin;
    ULONG Offset;

    for (Pin = TLMM_I2S_FIRST; Pin <= TLMM_I2S_LAST; Pin++) {
        Offset = Pin * TLMM_STRIDE;
        if (On) {
            Au->PinCtl[Pin - TLMM_I2S_FIRST] = SiaRead32 (Dev->Tlmm, Offset);
            SiaWrite32 (Dev->Tlmm, Offset, (Au->PinCtl[Pin - TLMM_I2S_FIRST] & ~0x3FFu) | (1u << 2) | (1u << 6) | (Pin == TLMM_I2S_LAST ? 1u : 0u));
        } else {
            SiaWrite32 (Dev->Tlmm, Offset, Au->PinCtl[Pin - TLMM_I2S_FIRST]);
        }
    }
    Au->PinsOn = On;
    SiaLog (Dev, "i2s pins %s: gpio121=0x%x gpio122=0x%x gpio123=0x%x gpio124=0x%x", On ? "active" : "restored",
            SiaRead32 (Dev->Tlmm, 121u * TLMM_STRIDE), SiaRead32 (Dev->Tlmm, 122u * TLMM_STRIDE),
            SiaRead32 (Dev->Tlmm, 123u * TLMM_STRIDE), SiaRead32 (Dev->Tlmm, 124u * TLMM_STRIDE));
}

static
NTSTATUS
AuMap (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au
    )
{
    UCHAR Packet[SIA_GPR_HEADER + 20];
    ULONG Status;
    ULONG Handle;
    NTSTATUS Result;

    SiaGprHeader (Packet, sizeof (Packet), GPR_PORT_STREAM, GPR_PORT_APM, GPR_TOKEN_MAP, APM_CMD_SHARED_MEM_MAP_REGIONS);
    *(USHORT UNALIGNED *)(Packet + 24) = APM_MEMORY_MAP_SHMEM8_4K_POOL;
    *(USHORT UNALIGNED *)(Packet + 26) = 1;
    *(ULONG UNALIGNED *)(Packet + 28) = 0;
    *(ULONG UNALIGNED *)(Packet + 32) = (ULONG)Au->DspAddress;
    *(ULONG UNALIGNED *)(Packet + 36) = (ULONG)(Au->DspAddress >> 32);
    *(ULONG UNALIGNED *)(Packet + 40) = SHM_SIZE;
    Au->MapPending = TRUE;
    Result = AuTransact (Dev, Au, Packet, sizeof (Packet), APM_CMD_RSP_SHARED_MEM_MAP_REGIONS, 2000, &Status, &Handle);
    SiaLog (Dev, "apm map shm 0x%llx size 0x%x: 0x%08x status 0x%x handle 0x%08x",
            Au->ShmPhys.QuadPart, SHM_SIZE, Result, Status, Handle);
    if (!NT_SUCCESS (Result)) {
        return Result;
    }
    if (Status != 0) {
        Au->MapPending = FALSE;
        return STATUS_UNSUCCESSFUL;
    }
    if (Handle == 0) {
        return STATUS_UNSUCCESSFUL;
    }
    Au->Handle = Handle;
    Au->MapPending = FALSE;
    return STATUS_SUCCESS;
}

static
VOID
AuUnmap (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au
    )
{
    UCHAR Packet[SIA_GPR_HEADER + 4];
    ULONG Status;
    NTSTATUS Result;

    SiaGprHeader (Packet, sizeof (Packet), GPR_PORT_STREAM, GPR_PORT_APM, GPR_TOKEN_UNMAP, APM_CMD_SHARED_MEM_UNMAP_REGIONS);
    *(ULONG UNALIGNED *)(Packet + 24) = Au->Handle;
    Result = AuTransact (Dev, Au, Packet, sizeof (Packet), 0, 2000, &Status, NULL);
    SiaLog (Dev, "apm unmap handle 0x%08x: 0x%08x status 0x%x", Au->Handle, Result, Status);
    if (NT_SUCCESS (Result) && Status == 0) {
        Au->Handle = 0;
        Au->MapPending = FALSE;
    }
}

static
NTSTATUS
AuClock (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_ BOOLEAN Enable
    )
{
    UCHAR Packet[SIA_GPR_HEADER + 52];
    ULONG Length = Enable ? SIA_GPR_HEADER + 52 : SIA_GPR_HEADER + 40;
    ULONG Status;
    NTSTATUS Result;

    RtlZeroMemory (Packet, sizeof (Packet));
    SiaGprHeader (Packet, Length, GPR_PORT_KERNEL, GPR_PORT_PRM, GPR_TOKEN_PRM,
              Enable ? PRM_CMD_REQUEST_HW_RSC : PRM_CMD_RELEASE_HW_RSC);
    *(ULONG UNALIGNED *)(Packet + 36) = Length - SIA_GPR_HEADER - 16;
    *(ULONG UNALIGNED *)(Packet + 40) = PRM_MODULE_INSTANCE_ID;
    *(ULONG UNALIGNED *)(Packet + 44) = PARAM_ID_RSC_AUDIO_HW_CLK;
    *(ULONG UNALIGNED *)(Packet + 48) = Length - SIA_GPR_HEADER - 32;
    *(ULONG UNALIGNED *)(Packet + 56) = 1;
    *(ULONG UNALIGNED *)(Packet + 60) = CLOCK_ID_SEC_MI2S_IBIT;
    if (Enable) {
        *(ULONG UNALIGNED *)(Packet + 64) = CLOCK_SEC_MI2S_HZ;
        *(ULONG UNALIGNED *)(Packet + 68) = CLOCK_ATTRIBUTE_COUPLE_NO;
        *(ULONG UNALIGNED *)(Packet + 72) = 0;
    }
    Result = AuTransact (Dev, Au, Packet, Length, Enable ? PRM_CMD_RSP_REQUEST_HW_RSC : PRM_CMD_RSP_RELEASE_HW_RSC,
                         2000, &Status, NULL);
    SiaLog (Dev, "prm %s sec mi2s ibit %u Hz: 0x%08x status 0x%x", Enable ? "request" : "release",
            CLOCK_SEC_MI2S_HZ, Result, Status);
    if (!NT_SUCCESS (Result)) {
        return Result;
    }
    if (Status != 0) {
        return STATUS_UNSUCCESSFUL;
    }
    Au->ClockOn = Enable;
    return STATUS_SUCCESS;
}

static NTSTATUS
AuLoadReplay (PSIA_DEVICE Dev, PUCHAR *Blob, ULONG *Size)
{
    HANDLE File;
    OBJECT_ATTRIBUTES Attributes;
    UNICODE_STRING Name;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Info;
    ULONG Read = 0;
    NTSTATUS Status;

    *Blob = NULL;
    *Size = 0;
    RtlInitUnicodeString (&Name, SIA_REPLAY_PATH);
    InitializeObjectAttributes (&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile (&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS (Status)) {
        SiaLog (Dev, "replay.bin open failed 0x%08x", Status);
        return Status;
    }
    Status = ZwQueryInformationFile (File, &Iosb, &Info, sizeof (Info), FileStandardInformation);
    if (!NT_SUCCESS (Status)) goto close;
    if (Info.EndOfFile.QuadPart < 8 || Info.EndOfFile.QuadPart > RPL_MAX_SIZE) {
        SiaLog (Dev, "replay.bin bad size %lld", Info.EndOfFile.QuadPart);
        Status = STATUS_INVALID_IMAGE_FORMAT;
        goto close;
    }
    *Size = (ULONG)Info.EndOfFile.QuadPart;
    *Blob = ExAllocatePool2 (POOL_FLAG_PAGED, *Size, SIA_AUDIO_TAG);
    if (*Blob == NULL) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto close;
    }
    Status = ZwReadFile (File, NULL, NULL, NULL, &Iosb, *Blob, *Size, NULL, NULL);
    if (NT_SUCCESS (Status)) Read = (ULONG)Iosb.Information;
    if (NT_SUCCESS (Status) && (Read != *Size || *(ULONG UNALIGNED *)*Blob != RPL_MAGIC))
        Status = STATUS_INVALID_IMAGE_FORMAT;
    if (NT_SUCCESS (Status))
        SiaLog (Dev, "replay.bin %lu bytes, %lu records", *Size, *(ULONG UNALIGNED *)(*Blob + 4));
    else
        SiaLog (Dev, "replay.bin read failed 0x%08x (%lu/%lu bytes)", Status, Read, *Size);
close:
    ZwClose (File);
    if (!NT_SUCCESS (Status)) {
        if (*Blob != NULL) ExFreePoolWithTag (*Blob, SIA_AUDIO_TAG);
        *Blob = NULL;
        *Size = 0;
    }
    return Status;
}

static
NTSTATUS
AuSubmit (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_ ULONG Index
    )
{
    UCHAR Packet[SIA_GPR_HEADER + 44];
    ULONGLONG Address = Au->DspAddress + (ULONGLONG)Index * PCM_STRIDE;
    NTSTATUS Status;
    ULONG First = min (SIA_PCM_BYTES, Dev->PcmSize - Dev->ReadOffset);

    KeMemoryBarrier ();
    AuShmWrite (Au, Index * PCM_STRIDE, Dev->Pcm + Dev->ReadOffset, First);
    if (First < SIA_PCM_BYTES) AuShmWrite (Au, Index * PCM_STRIDE + First, Dev->Pcm, SIA_PCM_BYTES - First);
    if (Au->Sent % 50 == 0) {
        ULONG Offset;
        ULONG Active = 0;
        ULONG RingActive = 0;
        for (Offset = 0; Offset < SIA_PCM_BYTES; Offset += 4)
            Active |= SiaRead32 (Au->Shm, Index * PCM_STRIDE + Offset);
        for (Offset = 0; Offset < Dev->PcmSize; Offset += 4)
            RingActive |= *(volatile ULONG *)(Dev->Pcm + Offset);
        SiaLog (Dev, "pcm payload: packet %lu active bits 0x%08x ring bits 0x%08x offset %lu played %lld",
                Au->Sent, Active, RingActive, Dev->ReadOffset, Dev->PlayedBytes);
    }
    Dev->ReadOffset = (Dev->ReadOffset + SIA_PCM_BYTES) % Dev->PcmSize;
    RtlZeroMemory (Packet, sizeof (Packet));
    SiaGprHeader (Packet, sizeof (Packet), GPR_PORT_STREAM, GPR_PORT_WR_EP, Index, DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2);
    *(ULONG UNALIGNED *)(Packet + 24) = (ULONG)Address;
    *(ULONG UNALIGNED *)(Packet + 28) = (ULONG)(Address >> 32);
    *(ULONG UNALIGNED *)(Packet + 32) = Au->Handle;
    *(ULONG UNALIGNED *)(Packet + 36) = SIA_PCM_BYTES;
    KeMemoryBarrier ();
    Au->Busy[Index] = TRUE;
    Status = SiaGprSendRaw (Dev, Packet, sizeof (Packet));
    if (!NT_SUCCESS (Status)) {
        Au->Busy[Index] = FALSE;
        SiaLog (Dev, "pcm buffer %lu send failed 0x%08x", Index, Status);
        return Status;
    }
    Au->Sent++;
    return STATUS_SUCCESS;
}

static
VOID
AuAmpsStart (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Index;
    ULONG Failures = 0;
    NTSTATUS Status;

    for (Index = 0; Index + 4 <= Length; Index += 4) {
        if (Index >= 4 && Data[Index] != Data[Index - 4]) {
            SiaSleep (3);
        }
        Status = SiaRegWrite (Dev, Data[Index], Data[Index + 1], *(const USHORT UNALIGNED *)(Data + Index + 2));
        if (!NT_SUCCESS (Status)) {
            Failures++;
            if (Failures <= 4) {
                SiaLog (Dev, "amp 0x%02x reg 0x%02x write failed 0x%08x", Data[Index], Data[Index + 1], Status);
            }
        }
        if (Data[Index + 1] == 0x31) {
            SiaSleep (1);
        }
    }
    Au->AmpsOn = TRUE;
    SiaLog (Dev, "amp start: %lu writes, %lu failed", Length / 4, Failures);
    for (Index = 0; Index < ARRAYSIZE (SiaAmpAddress); Index++) {
        USHORT Value = 0;
        Status = SiaRegRead (Dev, SiaAmpAddress[Index], 0x13, &Value);
        SiaLog (Dev, "amp 0x%02x power readback 0x%04x status 0x%08x", SiaAmpAddress[Index], Value, Status);
    }
}

static
VOID
AuAmpsMute (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au
    )
{
    ULONG Amp;

    if (!Au->AmpsOn) {
        return;
    }
    for (Amp = 0; Amp < ARRAYSIZE (SiaAmpAddress); Amp++) {
        SiaRegWrite (Dev, SiaAmpAddress[Amp], 0x13, 0x0380);
        SiaSleep (2);
        SiaRegWrite (Dev, SiaAmpAddress[Amp], 0x13, 0x0381);
        SiaSleep (2);
    }
    Au->AmpsOn = FALSE;
    SiaLog (Dev, "amps muted");
}

static VOID
AuStreamStart (PSIA_DEVICE Dev, PSIA_AUDIO Au, const UCHAR *Data, ULONG Length)
{
    ULONG Index;
    if (Length != 16 || *(const ULONG UNALIGNED *)Data != 0 ||
        *(const ULONG UNALIGNED *)(Data + 4) != PCM_STRIDE ||
        *(const ULONG UNALIGNED *)(Data + 8) != SIA_PCM_BYTES ||
        *(const ULONG UNALIGNED *)(Data + 12) != SIA_PCM_FRAME) {
        Au->Failed = TRUE;
        return;
    }
    Au->Streaming = TRUE;
    Au->LastDone = AuNowMs ();
    for (Index = 0; Index < PCM_BUFFERS; Index++) {
        if (!NT_SUCCESS (AuSubmit (Dev, Au, Index))) {
            Au->Failed = TRUE;
            break;
        }
    }
    SiaLog (Dev, "Windows PCM started: 48000 Hz, stereo, packed 24-bit, software volume by Windows");
}

static VOID
AuStreamRun (PSIA_DEVICE Dev, PSIA_AUDIO Au)
{
    ULONG Index;
    while (Au->Streaming && Dev->Running && !Dev->Stopping && !Au->Failed) {
        AuPump (Dev, Au, NULL);
        for (Index = 0; Index < PCM_BUFFERS; Index++) {
            if (!Au->Busy[Index]) {
                if (!NT_SUCCESS (AuSubmit (Dev, Au, Index))) Au->Failed = TRUE;
            }
        }
        if (AuNowMs () - Au->LastDone > 3000 || Au->BadStatus) Au->Failed = TRUE;
        SiaSleep (1);
    }
    Au->Streaming = FALSE;
    AuAmpsMute (Dev, Au);
    SiaLog (Dev, "Windows PCM stopped: sent %lu done %lu bad %lu", Au->Sent, Au->Done, Au->BadStatus);
}

static
VOID
AuParamErrors (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_ ULONG Offset,
    _In_ ULONG Size
    )
{
    ULONG Position = 0;
    ULONG Reported = 0;
    ULONG Base;
    ULONG ParamSize;
    ULONG Error;

    while (Position + 16 <= Size && Reported < 16) {
        Base = Offset + Position;
        if (Base + 16 > SHM_SIZE) {
            break;
        }
        ParamSize = SiaRead32 (Au->Shm, Base + 8);
        Error = SiaRead32 (Au->Shm, Base + 12);
        if (Error != 0) {
            SiaLog (Dev, "  param miid 0x%x pid 0x%08x size %lu error 0x%x", SiaRead32 (Au->Shm, Base), SiaRead32 (Au->Shm, Base + 4),
                    ParamSize, Error);
            Reported++;
        }
        if (ParamSize > Size) {
            break;
        }
        Position += 16 + ((ParamSize + 7) & ~7u);
    }
}

static
VOID
AuReplayCmd (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_ ULONG Record,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length,
    _In_ ULONG Flags
    )
{
    UCHAR Packet[SIA_GPR_MAX];
    ULONG Header;
    ULONG Opcode;
    ULONG OobOffset = 0;
    ULONG OobSize = 0;
    ULONG Status;
    ULONGLONG Address = 0;
    LONGLONG Start;
    NTSTATUS Result;

    if (Length < SIA_GPR_HEADER || Length > SIA_GPR_MAX) {
        SiaLog (Dev, "rpl %lu bad cmd length %lu", Record, Length);
        return;
    }
    RtlCopyMemory (Packet, Data, Length);
    Header = ((Packet[0] >> 4) & 0xFu) * 4u;
    if (Header != SIA_GPR_HEADER || (*(const ULONG UNALIGNED *)Packet >> 8) != Length) {
        SiaLog (Dev, "rpl %lu invalid gpr header", Record);
        Au->Failed = TRUE;
        return;
    }
    Opcode = *(ULONG UNALIGNED *)(Packet + 20);
    if ((Flags & RPL_FLAG_OOB) != 0) {
        if (Length < Header + 16) {
            SiaLog (Dev, "rpl %lu oob cmd too short", Record);
            return;
        }
        OobOffset = *(ULONG UNALIGNED *)(Packet + Header);
        OobSize = *(ULONG UNALIGNED *)(Packet + Header + 12);
        if (OobOffset > SHM_SIZE || OobSize > SHM_SIZE - OobOffset) {
            SiaLog (Dev, "rpl %lu oob command range invalid", Record);
            Au->Failed = TRUE;
            return;
        }
        Address = Au->DspAddress + OobOffset;
        *(ULONG UNALIGNED *)(Packet + Header) = (ULONG)Address;
        *(ULONG UNALIGNED *)(Packet + Header + 4) = (ULONG)(Address >> 32);
        *(ULONG UNALIGNED *)(Packet + Header + 8) = Au->Handle;
    }

    SiaLog (Dev, "rpl %lu begin op 0x%08x%s dsp address 0x%llx size %lu", Record, Opcode,
            (Flags & RPL_FLAG_OOB) != 0 ? " oob" : " inline", (Flags & RPL_FLAG_OOB) != 0 ? Address : 0, OobSize);
    Start = AuNowMs ();
    Result = AuTransact (Dev, Au, Packet, Length, 0, 3000, &Status, NULL);
    SiaLog (Dev, "rpl %2lu op 0x%08x dst 0x%x tok 0x%x len %lu%s: 0x%08x status 0x%x %lld ms", Record, Opcode,
            *(ULONG UNALIGNED *)(Packet + 12), *(ULONG UNALIGNED *)(Packet + 16), Length,
            (Flags & RPL_FLAG_OOB) != 0 ? " oob" : "", Result, Status, AuNowMs () - Start);
    if (NT_SUCCESS (Result) && Status == 0) {
        return;
    }
    if ((Flags & RPL_FLAG_OOB) != 0) {
        AuParamErrors (Dev, Au, OobOffset, OobSize);
    }
    if (!Au->Cleanup) {
        Au->Failed = TRUE;
    }
}

static
VOID
AuReplay (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _In_reads_bytes_ (Size) const UCHAR *Blob,
    _In_ ULONG Size
    )
{
    ULONG Count = *(const ULONG UNALIGNED *)(Blob + 4);
    ULONG Offset = 8;
    ULONG Record;
    ULONG Type;
    ULONG Flags;
    ULONG Length;
    const UCHAR *Data;
    ULONG OobOffset;

    for (Record = 0; Record < Count; Record++) {
        if (Offset + 8 > Size) {
            SiaLog (Dev, "rpl truncated at record %lu", Record);
            break;
        }
        Type = *(const USHORT UNALIGNED *)(Blob + Offset);
        Flags = *(const USHORT UNALIGNED *)(Blob + Offset + 2);
        Length = *(const ULONG UNALIGNED *)(Blob + Offset + 4);
        Data = Blob + Offset + 8;
        if (Length > Size - Offset - 8) {
            SiaLog (Dev, "rpl record %lu length %lu out of range", Record, Length);
            break;
        }
        Offset += 8 + ((Length + 3) & ~3u);
        if ((Au->Failed || Dev->Stopping != 0 || !Dev->Running) && !Au->Cleanup && Type != RPL_STREAM && Type != RPL_AMP) {
            continue;
        }

        switch (Type) {
        case RPL_OOB:
            if (Length < 4) {
                break;
            }
            OobOffset = *(const ULONG UNALIGNED *)Data;
            if ((OobOffset & 3u) != 0 || OobOffset > SHM_SIZE || ((Length - 4 + 3) & ~3u) > SHM_SIZE - OobOffset) {
                SiaLog (Dev, "rpl %lu oob 0x%x+%lu out of range", Record, OobOffset, Length - 4);
                break;
            }
            SiaLog (Dev, "rpl %lu oob write offset 0x%x length %lu", Record, OobOffset, Length - 4);
            AuShmWrite (Au, OobOffset, Data + 4, Length - 4);
            break;
        case RPL_CMD:
            if (Au->Streaming) {
                AuStreamRun (Dev, Au);
            }
            AuReplayCmd (Dev, Au, Record, Data, Length, Flags);
            break;
        case RPL_STREAM:
            if (Au->Failed || Dev->Stopping != 0 || !Dev->Running) {
                SiaLog (Dev, "rpl %lu stream skipped after failure", Record);
            } else {
                AuStreamStart (Dev, Au, Data, Length);
            }
            break;
        case RPL_AMP:
            if (Au->Failed || Dev->Stopping != 0 || !Dev->Running) {
                SiaLog (Dev, "rpl %lu amp start skipped after failure", Record);
            } else {
                AuAmpsStart (Dev, Au, Data, Length);
            }
            Au->Cleanup = TRUE;
            break;
        case RPL_DELAY:
            if (Length >= 4) {
                SiaSleep (*(const ULONG UNALIGNED *)Data);
            }
            break;
        default:
            SiaLog (Dev, "rpl %lu unknown type %lu", Record, Type);
            break;
        }
    }
    if (Au->Streaming) {
        AuStreamRun (Dev, Au);
    }
}

VOID
SiaAudioPlay (
    _In_ PSIA_DEVICE Dev
    )
{
    PSIA_AUDIO Au;
    PUCHAR Blob = NULL;
    ULONG BlobSize = 0;
    PHYSICAL_ADDRESS Lowest;
    PHYSICAL_ADDRESS Highest;
    PHYSICAL_ADDRESS Boundary;
    NTSTATUS Status;

    if (Dev->Link == NULL || Dev->Tlmm == NULL) {
        return;
    }
    Au = (PSIA_AUDIO)ExAllocatePool2 (POOL_FLAG_NON_PAGED, sizeof (SIA_AUDIO), SIA_AUDIO_TAG);
    if (Au == NULL) {
        SiaLog (Dev, "audio context allocation failed");
        return;
    }

    Status = AuLoadReplay (Dev, &Blob, &BlobSize);
    if (!NT_SUCCESS (Status)) {
        goto done;
    }

    Status = AuSmmuSetup (Dev, Au);
    SiaLog (Dev, "smmu setup 0x%08x", Status);
    if (!NT_SUCCESS (Status)) {
        goto done;
    }

    Lowest.QuadPart = 0;
    Highest.QuadPart = 0xFFFFFFFFLL;
    Boundary.QuadPart = 0;
    Au->Shm = (PUCHAR)MmAllocateContiguousMemorySpecifyCache (SHM_SIZE, Lowest, Highest, Boundary, MmNonCached);
    if (Au->Shm == NULL) {
        SiaLog (Dev, "shm allocation failed");
        goto done;
    }
    Au->ShmPhys = MmGetPhysicalAddress (Au->Shm);
    if (Au->ShmPhys.HighPart != 0 || (ULONGLONG)Au->ShmPhys.QuadPart + SHM_SIZE > 0x100000000ull) {
        SiaLog (Dev, "shm outside the 32-bit DMA window");
        goto done;
    }
    Au->DspAddress = (ULONGLONG)Au->ShmPhys.LowPart | ((ULONGLONG)SMMU_LPASS_SID << 32);
    AuShmWrite (Au, 0, NULL, SHM_SIZE);
    SiaLog (Dev, "shm va %p pa 0x%llx dsp 0x%llx sid %lu size 0x%x", Au->Shm, Au->ShmPhys.QuadPart,
            Au->DspAddress, SMMU_LPASS_SID, SHM_SIZE);

    Status = AuMap (Dev, Au);
    if (!NT_SUCCESS (Status)) {
        goto done;
    }
    AuPins (Dev, Au, TRUE);
    Status = AuClock (Dev, Au, TRUE);
    if (!NT_SUCCESS (Status)) {
        SiaLog (Dev, "audio aborted because i2s clock was not confirmed");
        Au->Failed = TRUE;
        goto done;
    }

    AuReplay (Dev, Au, Blob, BlobSize);

done:
    Au->Cleanup = TRUE;
    if (Au->Streaming) {
        Au->Streaming = FALSE;
    }
    AuAmpsMute (Dev, Au);
    if (Au->ClockOn) {
        AuClock (Dev, Au, FALSE);
    }
    if (Au->Handle) {
        AuUnmap (Dev, Au);
    }
    if (Au->PinsOn) {
        AuPins (Dev, Au, FALSE);
    }
    if (Au->Smmu != NULL) {
        AuSmmuRestore (Dev, Au);
    }
    if (Au->Shm != NULL && !Au->Handle && !Au->MapPending) {
        MmFreeContiguousMemorySpecifyCache (Au->Shm, SHM_SIZE, MmNonCached);
    } else if (Au->Shm != NULL) {
        Dev->RetainedDma = TRUE;
        SiaLog (Dev, "shm retained until reboot because ADSP release was not confirmed: pa 0x%llx size 0x%x", Au->ShmPhys.QuadPart, SHM_SIZE);
    }
    if (Blob != NULL) {
        ExFreePoolWithTag (Blob, SIA_AUDIO_TAG);
    }
    SiaLog (Dev, "audio play finished%s", Au->Failed ? " (graph failure)" : "");
    ExFreePoolWithTag (Au, SIA_AUDIO_TAG);
}
