#pragma once
#include <ntifs.h>
#include <wdf.h>
#include <bthxddi.h>
#include "ibs.h"
#define QBT_TAG 'tBsA'
#define QBT_PACKET_MAX 4096u
#define QBT_RING_SIZE 65536u
#define QBT_RX_LIMIT 128u
#define QBT_FW_ROOT L"\\SystemRoot\\System32\\drivers\\wcn7861-bt\\"
typedef struct _QBT_PACKET { LIST_ENTRY Link; ULONG Length; UCHAR Data[1]; } QBT_PACKET;
typedef struct { PUCHAR Bytes; ULONG Length; } QBT_REQUEST;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QBT_REQUEST, QbtRequest)
typedef struct _QBT {
 WDFDEVICE Device;
 WDFINTERRUPT Interrupt;
 WDFDPC Dpc;
 WDFTIMER Timer;
 WDFSPINLOCK WorkLock;
 WDFQUEUE Writes, Reads[2];
 PUCHAR U, G, Pins, Core, Cfg, Obs, Ch;
 ULONG AddedVotes, OldRoot[4], OldDfs, OldPins[4], OldUart[30];
 BOOLEAN Voted, RootSaved, UartSaved, PinsSaved, ResetReleased;
 ULONGLONG BootDeadline;
 volatile LONG Online, Fault;
 BOOLEAN Paused, AckPending, TxDone;
 PUCHAR TxBytes;
 ULONG TxLength, TxAt;
 WDFREQUEST TxRequest, PendingWrite;
 QBT_IBS Ibs;
 UCHAR AckByte;
 ULONGLONG TxDeadline;
 UCHAR Ring[QBT_RING_SIZE];
 ULONG RingRead, RingWrite;
 UCHAR Frame[QBT_PACKET_MAX+1];
 ULONG FrameHave, PacketCount;
 LIST_ENTRY Packets[2];
} QBT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(QBT, QbtDevice)
static __forceinline ULONG QbtRead(PUCHAR p, ULONG o) { return READ_REGISTER_ULONG((PULONG)(p+o)); }
static __forceinline VOID QbtWrite(PUCHAR p, ULONG o, ULONG v) { WRITE_REGISTER_ULONG((PULONG)(p+o),v); KeMemoryBarrier(); }
NTSTATUS QbtMap(QBT*, WDFCMRESLIST);
VOID QbtUnmap(QBT*);
NTSTATUS QbtStart(QBT*);
VOID QbtStop(QBT*);
NTSTATUS QbtSend(QBT*, const UCHAR*, ULONG);
ULONG QbtReceive(QBT*, UCHAR*, ULONG);
NTSTATUS QbtFastClock(QBT*);
NTSTATUS QbtFirmware(QBT*);
VOID QbtIrqOff(QBT*);
VOID QbtRuntimeFill(QBT*);
EVT_WDF_INTERRUPT_ISR QbtIsr;
EVT_WDF_INTERRUPT_ENABLE QbtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE QbtInterruptDisable;
