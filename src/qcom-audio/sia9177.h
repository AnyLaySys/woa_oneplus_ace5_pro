#pragma once
#include <ntddk.h>

#define SIA_PCM_RATE 48000u
#define SIA_PCM_FRAMES 960u
#define SIA_PCM_BYTES (SIA_PCM_FRAMES * SIA_PCM_FRAME)
#define SIA_PCM_FRAME 6u

#define SIA_GCC_PHYS        0x00100000ull
#define SIA_GCC_SIZE        0x00060000u
#define SIA_TLMM_PHYS       0x0F100000ull
#define SIA_TLMM_SIZE       0x000A6000u

#define SIA_GPR_HEADER      24u
#define SIA_GPR_MAX         512u
#define SIA_GPR_QUEUE       64u
#define SIA_REMOTE_INTENTS  256u

typedef struct _SIA_GPR_PACKET {
    ULONG Length;
    UCHAR Data[SIA_GPR_MAX];
} SIA_GPR_PACKET;

typedef struct _SIA_LINK {
    PUCHAR Smem;
    PUCHAR Mutex;
    PUCHAR Ipcc;
    ULONG Partition;
    ULONG PartitionSize;
    PUCHAR In;
    ULONG InSize;
    PUCHAR Out;
    PUCHAR Desc;
    PUCHAR TxFifo;
    ULONG TxLength;
    PUCHAR RxFifo;
    ULONG RxLength;
    ULONG AudioRcid;
    BOOLEAN AudioOpenAcked;
    BOOLEAN AudioOpening;
    ULONG IntentIid[SIA_REMOTE_INTENTS];
    ULONG IntentSize[SIA_REMOTE_INTENTS];
    BOOLEAN IntentFree[SIA_REMOTE_INTENTS];
    ULONG IntentCount;
    SIA_GPR_PACKET Queue[SIA_GPR_QUEUE];
    ULONG QueueHead;
    ULONG QueueTail;
} SIA_LINK, *PSIA_LINK;

typedef struct _SIA_PCM {
    PUCHAR Pcm;
    ULONG PcmSize;
    ULONG ReadOffset;
    volatile LONG64 PlayedBytes;
    volatile LONG StreamAllocated;
    KSPIN_LOCK NotifyLock;
    PKEVENT NotifyEvent;
    ULONG NotifyBytes;
    KEVENT IdleEvent;
    KEVENT ReadyEvent;
    volatile NTSTATUS Status;
} SIA_PCM, *PSIA_PCM;

typedef struct _SIA_DEVICE {
    PUCHAR Se;
    SIZE_T SeSize;
    PUCHAR Gcc;
    PUCHAR Tlmm;
    PSIA_LINK Link;
    HANDLE Thread;
    KEVENT WorkEvent;
    volatile LONG Stopping;
    volatile LONG Running;
    BOOLEAN RetainedDma;
    SIA_PCM Stream[2];
} SIA_DEVICE, *PSIA_DEVICE;

typedef struct _SIA_MIC SIA_MIC, *PSIA_MIC;
#define SIA_CAPTURE_FRAME 4u
#define SIA_CAPTURE_BYTES (SIA_PCM_FRAMES * SIA_CAPTURE_FRAME)

static __inline ULONG SiaRead32 (PUCHAR Base, ULONG Offset)
{
    return READ_REGISTER_ULONG ((volatile ULONG *)(Base + Offset));
}

static __inline VOID SiaWrite32 (PUCHAR Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG ((volatile ULONG *)(Base + Offset), Value);
}

static __inline VOID SiaGprHeader (UCHAR *Packet, ULONG Length, ULONG Source, ULONG Destination, ULONG Token, ULONG Opcode)
{
    *(ULONG UNALIGNED *)(Packet + 0) = (6u << 4) | (Length << 8);
    *(ULONG UNALIGNED *)(Packet + 4) = 2u | (3u << 8);
    *(ULONG UNALIGNED *)(Packet + 8) = Source;
    *(ULONG UNALIGNED *)(Packet + 12) = Destination;
    *(ULONG UNALIGNED *)(Packet + 16) = Token;
    *(ULONG UNALIGNED *)(Packet + 20) = Opcode;
}

#ifdef __cplusplus
extern "C" {
#endif
NTSTATUS SiaHardwareStart (PSIA_DEVICE Dev, PCM_PARTIAL_RESOURCE_DESCRIPTOR Resource);
VOID SiaHardwareStop (PSIA_DEVICE Dev);
NTSTATUS SiaStreamStart (PSIA_DEVICE Dev, BOOLEAN Capture);
VOID SiaStreamStop (PSIA_DEVICE Dev, BOOLEAN Capture);
VOID SiaSleep (_In_ ULONG Milliseconds);

NTSTATUS SiaI2cInit (_In_ PSIA_DEVICE Dev);
NTSTATUS SiaRegRead (_In_ PSIA_DEVICE Dev, _In_ UCHAR Address, _In_ UCHAR Reg, _Out_ USHORT *Value);
NTSTATUS SiaRegWrite (_In_ PSIA_DEVICE Dev, _In_ UCHAR Address, _In_ UCHAR Reg, _In_ USHORT Value);
VOID SiaGpioOutput (_In_ PSIA_DEVICE Dev, _In_ ULONG Pin, _In_ BOOLEAN High);

NTSTATUS SiaLinkStart (_In_ PSIA_DEVICE Dev);
VOID SiaLinkStop (_In_ PSIA_DEVICE Dev);
VOID SiaLinkPoll (_In_ PSIA_DEVICE Dev);
NTSTATUS SiaGprSendRaw (_In_ PSIA_DEVICE Dev, _In_reads_bytes_ (Length) const UCHAR *Packet, _In_ ULONG Length);
BOOLEAN SiaGprPop (_In_ PSIA_DEVICE Dev, _Out_ SIA_GPR_PACKET *Packet);

VOID SiaAudioRun (_In_ PSIA_DEVICE Dev);
NTSTATUS SiaAmpsProbe (PSIA_DEVICE Dev);
NTSTATUS SiaMicStart (PSIA_DEVICE Dev, PSIA_MIC *Mic, VOID (*Service)(PSIA_DEVICE, PVOID), PVOID Audio);
VOID SiaMicPoll (PSIA_MIC Mic);
VOID SiaMicStop (PSIA_DEVICE Dev, PSIA_MIC Mic);

#ifdef __cplusplus
}
#endif
