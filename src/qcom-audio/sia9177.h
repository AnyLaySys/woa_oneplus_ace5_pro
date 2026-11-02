#pragma once
#include <ntddk.h>
#include <ntstrsafe.h>

#ifndef SIA_DRIVER_VERSION
#define SIA_DRIVER_VERSION "26.10.31.1"
#endif
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
    ULONG LastInValid;
    ULONG LastInValues[16];
    ULONG AudioRcid;
    BOOLEAN AudioOpenAcked;
    ULONG IntentIid[SIA_REMOTE_INTENTS];
    ULONG IntentSize[SIA_REMOTE_INTENTS];
    BOOLEAN IntentFree[SIA_REMOTE_INTENTS];
    ULONG IntentCount;
    SIA_GPR_PACKET Queue[SIA_GPR_QUEUE];
    ULONG QueueHead;
    ULONG QueueTail;
} SIA_LINK, *PSIA_LINK;

typedef struct _SIA_DEVICE {
    PUCHAR Se;
    SIZE_T SeSize;
    PUCHAR Gcc;
    PUCHAR Tlmm;
    PSIA_LINK Link;
    HANDLE Thread;
    KEVENT WorkEvent;
    KEVENT IdleEvent;
    volatile LONG Stopping;
    volatile LONG Running;
    volatile LONG StreamAllocated;
    BOOLEAN RetainedDma;
    PUCHAR Pcm;
    ULONG PcmSize;
    ULONG ReadOffset;
    volatile LONG64 PlayedBytes;
    KSPIN_LOCK NotifyLock;
    PKEVENT NotifyEvent;
    ULONG NotifyBytes;
} SIA_DEVICE, *PSIA_DEVICE;

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
NTSTATUS SiaStreamStart (PSIA_DEVICE Dev);
VOID SiaStreamStop (PSIA_DEVICE Dev);
VOID SiaLog (_In_ PSIA_DEVICE Dev, _In_z_ _Printf_format_string_ PCSTR Format, ...);
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

VOID SiaAudioPlay (_In_ PSIA_DEVICE Dev);

#ifdef __cplusplus
}
#endif
