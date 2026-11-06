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

#define SHM_SIZE                0x20000u
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

typedef struct _SIA_FLOW {
    PUCHAR Blob;
    ULONG BlobSize;
    ULONG Offset;
    ULONG Record;
    ULONG Bytes;
    ULONG Port;
    ULONG Endpoint;
    ULONG CleanupOffset, CleanupRecord;
    BOOLEAN Opened;
    BOOLEAN Streaming;
    BOOLEAN Failed;
    BOOLEAN Busy[PCM_BUFFERS];
    LONGLONG LastDone;
} SIA_FLOW, *PSIA_FLOW;

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
    ULONG PinCtl[TLMM_I2S_PINS];
    ULONG Current;
    ULONG MicClocks;
    PSIA_MIC Mic;
    SIA_FLOW Flow[2];
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

    if (Opcode == DATA_CMD_RSP_WR_SH_MEM_EP_DATA_BUFFER_DONE_V2 || Opcode == 0x05001005u ||
        (Opcode == GPR_IBASIC_RSP_RESULT && (Word0 == DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 || Word0 == 0x0400100Bu))) {
        ULONG Direction = *(const ULONG UNALIGNED *)(Packet->Data + 12) == 0x2011u;
        PSIA_FLOW Flow = &Au->Flow[Direction];
        PSIA_PCM Pcm = &Dev->Stream[Direction];
        ULONGLONG Address = Au->DspAddress + Direction * 0x10000u + (ULONGLONG)Token * PCM_STRIDE;
        ULONG Bytes = Flow->Bytes;
        ULONG Status;
        if (Token >= PCM_BUFFERS || !Flow->Busy[Token] ||
            *(const ULONG UNALIGNED *)(Packet->Data + 8) != Flow->Endpoint ||
            *(const ULONG UNALIGNED *)(Packet->Data + 12) != Flow->Port) return;
        if (!Flow->Streaming) { Flow->Busy[Token] = FALSE; return; }
        if (Opcode == GPR_IBASIC_RSP_RESULT) {
            Status = Word1 ? Word1 : 1;
        } else if (Direction) {
            if (PayloadLength < 56 || Word1 != (ULONG)Address ||
                *(const ULONG UNALIGNED *)(Payload + 8) != (ULONG)(Address >> 32) ||
                *(const ULONG UNALIGNED *)(Payload + 12) != Au->Handle) return;
            Status = Word0;
            Bytes = *(const ULONG UNALIGNED *)(Payload + 16);
            // This firmware's V2 response has a timestamp here, not a sample offset.
            if (!Status && (!Bytes || Bytes > Flow->Bytes || (Bytes % SIA_CAPTURE_FRAME))) Status = 1;
            if (!Status && Pcm->Pcm && Flow->Streaming && (Dev->Running & 2)) {
                KeMemoryBarrier ();
                for (ULONG I = 0; I < Bytes; I += 4) {
                    ULONG Word = SiaRead32(Au->Shm, 0x10000u + Token * PCM_STRIDE + I);
                    *(ULONG *)(Pcm->Pcm + Pcm->ReadOffset) = Word;
                    Pcm->ReadOffset = (Pcm->ReadOffset + 4) % Pcm->PcmSize;
                }
            }
        } else {
            if (PayloadLength < 16 || Word0 != (ULONG)Address || Word1 != (ULONG)(Address >> 32) ||
                *(const ULONG UNALIGNED *)(Payload + 8) != Au->Handle) return;
            Status = *(const ULONG UNALIGNED *)(Payload + 12);
        }
        Flow->Busy[Token] = FALSE;
        Flow->LastDone = AuNowMs ();
        if (Status) { Flow->Failed = TRUE;  return; }
        if (Flow->Streaming && Pcm->PcmSize) {
            KIRQL Irql;
            ULONGLONG Before = (ULONGLONG)InterlockedExchangeAdd64(&Pcm->PlayedBytes, Bytes);
            KeAcquireSpinLock(&Pcm->NotifyLock, &Irql);
            if (Pcm->NotifyEvent && Pcm->NotifyBytes && Before / Pcm->NotifyBytes != (Before + Bytes) / Pcm->NotifyBytes)
                KeSetEvent(Pcm->NotifyEvent, IO_NO_INCREMENT, FALSE);
            KeReleaseSpinLock(&Pcm->NotifyLock, Irql);
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

}

static NTSTATUS AuSubmit(PSIA_DEVICE Dev, PSIA_AUDIO Au, ULONG Direction, ULONG Index);

static
VOID
AuPump (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
    _Inout_opt_ SIA_AUDIO_WAIT *Wait
    )
{
    if(Au->Mic)SiaMicPoll(Au->Mic);
    SiaLinkPoll (Dev);
    while (SiaGprPop (Dev, &Au->Rx)) {
        AuDispatch (Dev, Au, &Au->Rx, Wait);
    }
    for (ULONG Direction = 0; Direction < 2; Direction++) {
        PSIA_FLOW Flow = &Au->Flow[Direction];
        if (!Flow->Streaming || Flow->Failed || !(Dev->Running & (1u << Direction)) || Dev->Stopping) continue;
        for (ULONG Index = 0; Index < PCM_BUFFERS; Index++)
            if (!Flow->Busy[Index] && !NT_SUCCESS(AuSubmit(Dev, Au, Direction, Index))) Flow->Failed = TRUE;
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
    if ((Dev->Stopping != 0 || !(Dev->Running & (1u << Au->Current))) && !Au->Cleanup) {
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
        if ((Dev->Stopping != 0 || !(Dev->Running & (1u << Au->Current))) && !Au->Cleanup) {
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
        }
        for (Index = 0; Index < SMMU_STREAMS; Index++) {
            Wanted = SiaStreamMatches[Index];
            if (AuSmrOverlap (Smr, Wanted & 0x7FFFu, (Wanted >> 16) & 0x7FFFu)) {
                if (Smr != Wanted || Au->SmmuSlot[Index] != MAXULONG) {
                    return STATUS_DEVICE_CONFIGURATION_ERROR;
                }
                Au->SmmuSlot[Index] = Slot;
            }
        }
    }
    if (UfsS2cr == MAXULONG) {
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
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            continue;
        }
        if (FreeUsed >= FreeCount) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Slot = FreeSlots[FreeUsed++];
        Au->SmmuSlot[Index] = Slot;
        Au->SmmuSmr[Index] = SiaRead32 (Au->Smmu, SMMU_SMR (Slot));
        SavedS2cr = SiaRead32 (Au->Smmu, SMMU_S2CR (Slot));
        if ((Au->SmmuSmr[Index] & SMMU_SMR_VALID) != 0 || SavedS2cr == MAXULONG ||
            SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != Au->SmmuSmr[Index] ||
            SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != SavedS2cr) {
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
        if (SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != Wanted || SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != UfsS2cr) {
            return STATUS_IO_DEVICE_ERROR;
        }
    }
    return STATUS_SUCCESS;
}

static
VOID
AuSmmuRestore (
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
            continue;
        }
        if (SiaRead32 (Au->Smmu, SMMU_SMR (Slot)) != SiaStreamMatches[Index] ||
            SiaRead32 (Au->Smmu, SMMU_S2CR (Slot)) != Au->SmmuActiveS2cr) {
            continue;
        }
        SiaWrite32 (Au->Smmu, SMMU_SMR (Slot), Au->SmmuSmr[Index]);
    }
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
AuLoadReplay (BOOLEAN Capture, PUCHAR *Blob, ULONG *Size)
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
    RtlInitUnicodeString (&Name, Capture ? L"\\SystemRoot\\System32\\drivers\\wcd9395\\replay.bin" : SIA_REPLAY_PATH);
    InitializeObjectAttributes (&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile (&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS (Status)) {
        return Status;
    }
    Status = ZwQueryInformationFile (File, &Iosb, &Info, sizeof (Info), FileStandardInformation);
    if (!NT_SUCCESS (Status)) goto close;
    if (Info.EndOfFile.QuadPart < 8 || Info.EndOfFile.QuadPart > RPL_MAX_SIZE) {
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
close:
    ZwClose (File);
    if (!NT_SUCCESS (Status)) {
        if (*Blob != NULL) ExFreePoolWithTag (*Blob, SIA_AUDIO_TAG);
        *Blob = NULL;
        *Size = 0;
    }
    return Status;
}

static NTSTATUS
AuSubmit (PSIA_DEVICE Dev, PSIA_AUDIO Au, ULONG Direction, ULONG Index)
{
    UCHAR Packet[SIA_GPR_HEADER + 44] = {0};
    PSIA_FLOW Flow = &Au->Flow[Direction];
    PSIA_PCM Pcm = &Dev->Stream[Direction];
    ULONGLONG Address = Au->DspAddress + Direction * 0x10000u + (ULONGLONG)Index * PCM_STRIDE;
    ULONG Length = Direction ? 56 : sizeof(Packet);
    if (!Direction) {
        ULONG First = min(Flow->Bytes, Pcm->PcmSize - Pcm->ReadOffset);
        KeMemoryBarrier();
        AuShmWrite(Au, Index * PCM_STRIDE, Pcm->Pcm + Pcm->ReadOffset, First);
        if (First < Flow->Bytes) AuShmWrite(Au, Index * PCM_STRIDE + First, Pcm->Pcm, Flow->Bytes - First);
        Pcm->ReadOffset = (Pcm->ReadOffset + Flow->Bytes) % Pcm->PcmSize;
    }
    SiaGprHeader(Packet, Length, Flow->Port, Flow->Endpoint, Index, Direction ? 0x0400100Bu : DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2);
    *(ULONG UNALIGNED *)(Packet + 24) = (ULONG)Address;
    *(ULONG UNALIGNED *)(Packet + 28) = (ULONG)(Address >> 32);
    *(ULONG UNALIGNED *)(Packet + 32) = Au->Handle;
    *(ULONG UNALIGNED *)(Packet + 36) = Flow->Bytes;
    KeMemoryBarrier();
    Flow->Busy[Index] = TRUE;
    NTSTATUS Status = SiaGprSendRaw(Dev, Packet, Length);
    if (!NT_SUCCESS(Status)) Flow->Busy[Index] = FALSE;
    return Status;
}

static VOID
AuAmpsStart(PSIA_DEVICE Dev, PSIA_AUDIO Au, const UCHAR *Data, ULONG Length)
{
    Au->AmpsOn = TRUE;
    for(ULONG Index = 0; Index + 4 <= Length; Index += 4) {
        if(Index >= 4 && Data[Index] != Data[Index - 4]) SiaSleep(3);
        if(!NT_SUCCESS(SiaRegWrite(Dev, Data[Index], Data[Index + 1], *(const USHORT UNALIGNED *)(Data + Index + 2)))) {
            Au->Flow[Au->Current].Failed = TRUE;
            break;
        }
        if(Data[Index + 1] == 0x31) SiaSleep(1);
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
}

static VOID
AuStreamStart (PSIA_DEVICE Dev, PSIA_AUDIO Au, const UCHAR *Data, ULONG Length)
{
    PSIA_FLOW Flow = &Au->Flow[Au->Current];
    ULONG Frame = Au->Current ? SIA_CAPTURE_FRAME : SIA_PCM_FRAME;
    if (Length != 16 || *(const ULONG UNALIGNED *)Data != 0 ||
        *(const ULONG UNALIGNED *)(Data + 4) != PCM_STRIDE ||
        *(const ULONG UNALIGNED *)(Data + 8) != Flow->Bytes ||
        *(const ULONG UNALIGNED *)(Data + 12) != Frame) { Flow->Failed = TRUE; return; }
    Flow->Streaming = TRUE;
    Flow->LastDone = AuNowMs();
    for (ULONG Index = 0; Index < PCM_BUFFERS; Index++)
        if (!NT_SUCCESS(AuSubmit(Dev, Au, Au->Current, Index))) Flow->Failed = TRUE;
}

static
VOID
AuReplayCmd (
    _In_ PSIA_DEVICE Dev,
    _In_ PSIA_AUDIO Au,
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
    NTSTATUS Result;

    if (Length < SIA_GPR_HEADER || Length > SIA_GPR_MAX) {
        return;
    }
    RtlCopyMemory (Packet, Data, Length);
    Header = ((Packet[0] >> 4) & 0xFu) * 4u;
    if (Header != SIA_GPR_HEADER || (*(const ULONG UNALIGNED *)Packet >> 8) != Length) {
        Au->Flow[Au->Current].Failed = TRUE;
        return;
    }
    Opcode = *(ULONG UNALIGNED *)(Packet + 20);
    if ((Flags & RPL_FLAG_OOB) != 0) {
        if (Length < Header + 16) {
            return;
        }
        OobOffset = *(ULONG UNALIGNED *)(Packet + Header);
        OobSize = *(ULONG UNALIGNED *)(Packet + Header + 12);
        if ((OobOffset & 3u) || OobOffset > 0x10000u || OobSize > 0x10000u - OobOffset) {
            Au->Flow[Au->Current].Failed = TRUE;
            return;
        }
        Address = Au->DspAddress + Au->Current * 0x10000u + OobOffset;
        *(ULONG UNALIGNED *)(Packet + Header) = (ULONG)Address;
        *(ULONG UNALIGNED *)(Packet + Header + 4) = (ULONG)(Address >> 32);
        *(ULONG UNALIGNED *)(Packet + Header + 8) = Au->Handle;
    }

    Result = AuTransact (Dev, Au, Packet, Length, 0, 3000, &Status, NULL);
    if (NT_SUCCESS (Result) && Status == 0) {
        if (Opcode == 0x01001000u) Au->Flow[Au->Current].Opened = TRUE;
        return;
    }
    if (!Au->Cleanup) {
        Au->Flow[Au->Current].Failed = TRUE;
    }
}

static VOID
AuReplay (PSIA_DEVICE Dev, PSIA_AUDIO Au)
{
    PSIA_FLOW Flow = &Au->Flow[Au->Current];
    ULONG Count = *(const ULONG UNALIGNED *)(Flow->Blob + 4);
    while (Flow->Record < Count) {
        ULONG Offset = Flow->Offset, Size = Flow->BlobSize;
        if (Offset > Size || Size - Offset < 8) { Flow->Failed = TRUE; return; }
        ULONG Type = *(const USHORT UNALIGNED *)(Flow->Blob + Offset);
        ULONG Flags = *(const USHORT UNALIGNED *)(Flow->Blob + Offset + 2);
        ULONG Length = *(const ULONG UNALIGNED *)(Flow->Blob + Offset + 4);
        const UCHAR *Data = Flow->Blob + Offset + 8;
        if (Length > Size - Offset - 8 || ((Length + 3) & ~3u) > Size - Offset - 8) { Flow->Failed = TRUE; return; }
        if (Type == RPL_CMD && Flow->Streaming) return;
        Flow->Offset += 8 + ((Length + 3) & ~3u);
        Flow->Record++;
        if ((Flow->Failed || Dev->Stopping || !(Dev->Running & (1u << Au->Current))) && !Au->Cleanup && Type != RPL_STREAM && Type != RPL_AMP) continue;
        switch (Type) {
        case RPL_OOB:
            if (Length < 4) { Flow->Failed = TRUE; break; }
            Offset = *(const ULONG UNALIGNED *)Data;
            if ((Offset & 3u) || Offset > 0x10000u || Length - 4 > 0x10000u - Offset) { Flow->Failed = TRUE; break; }
            AuShmWrite(Au, Au->Current * 0x10000u + Offset, Data + 4, Length - 4);
            break;
        case RPL_CMD: AuReplayCmd(Dev, Au, Data, Length, Flags); break;
        case RPL_STREAM:
            if (!Flow->Failed && !Dev->Stopping && (Dev->Running & (1u << Au->Current))) AuStreamStart(Dev, Au, Data, Length);
            break;
        case RPL_AMP:
            if (!Au->Current && !Flow->Failed && !Dev->Stopping && (Dev->Running & 1)) AuAmpsStart(Dev, Au, Data, Length);
            Au->Cleanup = TRUE;
            break;
        case RPL_DELAY: if (Length == 4) SiaSleep(*(const ULONG UNALIGNED *)Data); else Flow->Failed = TRUE; break;
        default: Flow->Failed = TRUE; break;
        }
    }
}

static NTSTATUS
AuMicClocks(PSIA_DEVICE Dev, PSIA_AUDIO Au, BOOLEAN Enable)
{
    const ULONG Param[] = {0x08001A6Eu, 0x08001032u, PARAM_ID_RSC_AUDIO_HW_CLK, PARAM_ID_RSC_AUDIO_HW_CLK};
    const ULONG Value[] = {1, 2, 0x30C, 0x307};
    for (ULONG I = 0; I < 4; I++) {
        ULONG Index = Enable ? I : 3 - I;
        if (!Enable && !(Au->MicClocks & (1u << Index))) continue;
        ULONG Length = Index < 2 ? 60 : Enable ? 76 : 64, Status;
        UCHAR Packet[76] = {0};
        SiaGprHeader(Packet,Length,7,2,0x6300+Index,Enable?PRM_CMD_REQUEST_HW_RSC:PRM_CMD_RELEASE_HW_RSC);
        *(ULONG UNALIGNED *)(Packet+36)=Length-40;
        *(ULONG UNALIGNED *)(Packet+40)=2;
        *(ULONG UNALIGNED *)(Packet+44)=Param[Index];
        *(ULONG UNALIGNED *)(Packet+48)=Length-56;
        *(ULONG UNALIGNED *)(Packet+56)=Index<2?Value[Index]:1;
        if(Index>=2) *(ULONG UNALIGNED *)(Packet+60)=Value[Index];
        if(Index>=2 && Enable) { *(ULONG UNALIGNED *)(Packet+64)=19200000; *(ULONG UNALIGNED *)(Packet+68)=1; }
        NTSTATUS Result=AuTransact(Dev,Au,Packet,Length,Enable?PRM_CMD_RSP_REQUEST_HW_RSC:PRM_CMD_RSP_RELEASE_HW_RSC,2000,&Status,NULL);
        if(!NT_SUCCESS(Result)||Status) { if(Enable) return NT_SUCCESS(Result)?STATUS_DEVICE_NOT_READY:Result; }
        else if(Enable) Au->MicClocks|=1u<<Index;
        else Au->MicClocks&=~(1u<<Index);
    }
    return STATUS_SUCCESS;
}

static VOID AuService(PSIA_DEVICE Dev, PVOID Au) { AuPump(Dev,(PSIA_AUDIO)Au,NULL); }

static VOID
AuStopFlow(PSIA_DEVICE Dev, PSIA_AUDIO Au)
{
    PSIA_FLOW Flow=&Au->Flow[Au->Current];
    Au->Cleanup=TRUE;
    Flow->Streaming=FALSE;
    Flow->Failed=TRUE;
    if(Flow->Blob && Flow->Opened) {
        if(Flow->Offset < Flow->CleanupOffset) { Flow->Offset=Flow->CleanupOffset; Flow->Record=Flow->CleanupRecord; }
        AuReplay(Dev,Au);
    }
    if(Au->Current) {
        if(Au->Mic) { SiaMicStop(Dev,Au->Mic); Au->Mic=NULL; }
        AuMicClocks(Dev,Au,FALSE);
    } else {
        AuAmpsMute(Dev,Au);
        if(Au->ClockOn) AuClock(Dev,Au,FALSE);
        if(Au->PinsOn) AuPins(Dev,Au,FALSE);
    }
    if(Flow->Blob) ExFreePoolWithTag(Flow->Blob,SIA_AUDIO_TAG);
    RtlZeroMemory(Flow,sizeof(*Flow));
    Au->Cleanup=FALSE;
}

VOID
SiaAudioRun(PSIA_DEVICE Dev)
{
    PSIA_AUDIO Au=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*Au),SIA_AUDIO_TAG);
    NTSTATUS Status=STATUS_INSUFFICIENT_RESOURCES;
    if(!Au) goto failed;
    Status=AuSmmuSetup(Au);
    if(!NT_SUCCESS(Status)) goto done;
    PHYSICAL_ADDRESS Low={0}, High, Boundary={0}; High.QuadPart=0xffffffffLL;
    Au->Shm=MmAllocateContiguousMemorySpecifyCache(SHM_SIZE,Low,High,Boundary,MmNonCached);
    if(!Au->Shm){Status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    Au->ShmPhys=MmGetPhysicalAddress(Au->Shm);
    if(Au->ShmPhys.HighPart || (ULONGLONG)Au->ShmPhys.QuadPart+SHM_SIZE>0x100000000ULL){Status=STATUS_CONFLICTING_ADDRESSES;goto done;}
    Au->DspAddress=(ULONGLONG)Au->ShmPhys.LowPart|((ULONGLONG)SMMU_LPASS_SID<<32);
    AuShmWrite(Au,0,NULL,SHM_SIZE);
    Au->Current=(Dev->Running&1)?0:1;
    Status=AuMap(Dev,Au);
    if(!NT_SUCCESS(Status)) goto done;
    while(Dev->Running && !Dev->Stopping) {
        for(ULONG Direction=0;Direction<2;Direction++) {
            PSIA_FLOW Flow=&Au->Flow[Direction]; PSIA_PCM Pcm=&Dev->Stream[Direction];
            Au->Current=Direction;
            if((Dev->Running&(1u<<Direction)) && !Flow->Blob) {
                Flow->Bytes=Direction?SIA_CAPTURE_BYTES:SIA_PCM_BYTES;
                Flow->Port=Direction?0x2011u:GPR_PORT_STREAM;
                Flow->Endpoint=Direction?0x46F7u:GPR_PORT_WR_EP;
                Flow->Offset=8;
                NTSTATUS StartStatus=AuLoadReplay((BOOLEAN)Direction,&Flow->Blob,&Flow->BlobSize);
                if(NT_SUCCESS(StartStatus)) {
                    ULONG Offset=8, Count=*(ULONG UNALIGNED *)(Flow->Blob+4);
                    for(ULONG I=0;I<Count;I++) {
                        if(Offset>Flow->BlobSize || Flow->BlobSize-Offset<8) { StartStatus=STATUS_INVALID_IMAGE_FORMAT; break; }
                        ULONG Length=*(ULONG UNALIGNED *)(Flow->Blob+Offset+4);
                        if(Length>Flow->BlobSize-Offset-8 || ((Length+3)&~3u)>Flow->BlobSize-Offset-8) { StartStatus=STATUS_INVALID_IMAGE_FORMAT; break; }
                        ULONG Type=*(USHORT UNALIGNED *)(Flow->Blob+Offset);
                        Offset+=8+((Length+3)&~3u);
                        if(Type==RPL_AMP) { Flow->CleanupOffset=Offset; Flow->CleanupRecord=I+1; }
                    }
                    if(!Flow->CleanupOffset) StartStatus=STATUS_INVALID_IMAGE_FORMAT;
                }
                if(NT_SUCCESS(StartStatus) && Direction) { StartStatus=AuMicClocks(Dev,Au,TRUE); if(NT_SUCCESS(StartStatus)) StartStatus=SiaMicStart(Dev,&Au->Mic,AuService,Au); }
                if(NT_SUCCESS(StartStatus) && !Direction) { StartStatus=SiaAmpsProbe(Dev); if(NT_SUCCESS(StartStatus)) { AuPins(Dev,Au,TRUE); StartStatus=AuClock(Dev,Au,TRUE); } }
                if(NT_SUCCESS(StartStatus)) { AuReplay(Dev,Au); if(Flow->Failed||!Flow->Streaming) StartStatus=STATUS_DEVICE_NOT_READY; }
                Au->Cleanup=FALSE;
                Pcm->Status=StartStatus;
                if(!NT_SUCCESS(StartStatus)) { AuStopFlow(Dev,Au); InterlockedAnd(&Dev->Running,~(1L<<Direction)); KeSetEvent(&Pcm->IdleEvent,IO_NO_INCREMENT,FALSE); }
                KeSetEvent(&Pcm->ReadyEvent,IO_NO_INCREMENT,FALSE);
            }
            if(Flow->Blob && (!(Dev->Running&(1u<<Direction)) || Flow->Failed || AuNowMs()-Flow->LastDone>3000)) {
                if(Dev->Running&(1u<<Direction)) Pcm->Status=STATUS_IO_DEVICE_ERROR;
                AuStopFlow(Dev,Au); InterlockedAnd(&Dev->Running,~(1L<<Direction));
                KeSetEvent(&Pcm->IdleEvent,IO_NO_INCREMENT,FALSE);
            }
        }
        AuPump(Dev,Au,NULL); SiaSleep(1);
    }
done:
    if(Au) {
        for(ULONG Direction=0;Direction<2;Direction++) { Au->Current=Direction; if(Au->Flow[Direction].Blob) AuStopFlow(Dev,Au); }
        Au->Cleanup=TRUE;
        if(Au->Handle) AuUnmap(Dev,Au);
        if(Au->Shm) {
            if(!Au->Handle&&!Au->MapPending) MmFreeContiguousMemorySpecifyCache(Au->Shm,SHM_SIZE,MmNonCached);
            else Dev->RetainedDma=TRUE;
        }
        if(Au->Smmu) AuSmmuRestore(Au);
        ExFreePoolWithTag(Au,SIA_AUDIO_TAG);
    }
failed:
    for(ULONG I=0;I<2;I++) {
        if(!NT_SUCCESS(Status)) {
            InterlockedAnd(&Dev->Running,~(1L<<I));
            if(Dev->Stream[I].Status==STATUS_PENDING) Dev->Stream[I].Status=Status;
        }
        if(!(Dev->Running&(1u<<I)) || Dev->Stopping) {
            KeSetEvent(&Dev->Stream[I].ReadyEvent,IO_NO_INCREMENT,FALSE);
            KeSetEvent(&Dev->Stream[I].IdleEvent,IO_NO_INCREMENT,FALSE);
        }
    }
}
