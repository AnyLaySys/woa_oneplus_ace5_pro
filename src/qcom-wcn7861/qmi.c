/*
 * QMI client for the WLAN firmware service (WLFW, service 0x45), following
 * ath12k qmi.c for PEACH (hw_params: m3 loaded by the driver, cnss feature
 * bitmap WLAN_EN | AUX_UC, no internal sleep clock, MLO not advertised).
 *
 * The exchange is strictly one request at a time:
 *   server arrive  -> PHY_CAP, IND_REGISTER, HOST_CAP
 *   REQUEST_MEM_IND -> RESPOND_MEM with host DMA chunks
 *   FW_MEM_READY_IND -> CAP -> TME-L -> REGDB -> board ELF -> M3 -> AUX-UC
 *   -> FW_READY_IND
 */
#include "wcn7861.h"

#define QMI_REQUEST                 0
#define QMI_RESPONSE                2
#define QMI_INDICATION              4

#define QMI_WLFW_IND_REGISTER       0x0020
#define QMI_WLFW_CAP                0x0024
#define QMI_WLFW_HOST_CAP           0x0034
#define QMI_WLFW_REQUEST_MEM_IND    0x0035
#define QMI_WLFW_RESPOND_MEM        0x0036
#define QMI_WLFW_FW_MEM_READY_IND   0x0037
#define QMI_WLFW_FW_READY_IND       0x0038
#define QMI_WLFW_BDF_DOWNLOAD       0x0025
#define QMI_WLFW_M3_INFO            0x003C
#define QMI_WLFW_AUX_UC_INFO        0x005A
#define QMI_WLFW_TME_LITE_INFO      0x005B
#define QMI_WLFW_QDSS_CONFIG        0x0044
#define QMI_WLFW_PHY_CAP            0x0057
#define QMI_WLFW_WLAN_MODE          0x0022
#define QMI_WLFW_WLAN_CFG           0x0023
#define QMI_WLFW_WLAN_INI           0x002F

/* ath12k qmi.h target memory types the host backs with memory */
#define QMI_MEM_HOST_DDR            0x1
#define QMI_MEM_M3_DUMP             0x3
#define QMI_MEM_CALDB               0x4
#define QMI_MEM_MLO_GLOBAL          0x8
#define QMI_MEM_PAGEABLE            0x9
#define QMI_MEM_LPASS_SHARED        0xB
#define QMI_MAX_CHUNK_SIZE          (2 * 1024 * 1024)   /* ATH12K_QMI_MAX_CHUNK_SIZE */
#define QMI_MEM_ALIGNMENT           0x10000
#define QMI_BDF_TYPE_ELF            1
#define QMI_BDF_TYPE_REGDB          4
#define QMI_BOARD_ID_DEFAULT        0xFF
#define QMI_FW_CAP_AUX_UC           (1ULL << 2)
#define QMI_ERR_HARDWARE_RESTRICTED 0x53

#define WCN_FIRMWARE_DIR            L"\\SystemRoot\\System32\\drivers\\wcn7861\\"
#define WCN_REGDB_FILE              WCN_FIRMWARE_DIR L"regdb.bin"
#define WCN_BOARD_FILE              WCN_FIRMWARE_DIR L"board.bin"
#define WCN_M3_FILE                 WCN_FIRMWARE_DIR L"m3.bin"
#define WCN_TME_FILE                WCN_FIRMWARE_DIR L"tmel.bin"
#define WCN_AUX_UC_FILE             WCN_FIRMWARE_DIR L"aux_ucode.bin"
#define WCN_QDSS_FILE               WCN_FIRMWARE_DIR L"qdss_trace_config.bin"

#define QMI_CLIENT_ID               0x4B4E454C          /* QMI_WLANFW_CLIENT_ID */
#define QMI_CNSS_FEATURES           0x44                /* BIT(CNSS_WLAN_EN_SUPPORT) | BIT(CNSS_AUX_UC_SUPPORT) */

enum {
    QmiStepIdle,
    QmiStepPhyCap,
    QmiStepIndRegister,
    QmiStepHostCap,
    QmiStepWaitMem,
    QmiStepWaitMemReady,
    QmiStepCap,
    QmiStepTmeLite,
    QmiStepRegdb,
    QmiStepBoard,
    QmiStepM3,
    QmiStepAuxUc,
    QmiStepQdss,
    QmiStepWaitFwReady,
    QmiStepWlanIni,
    QmiStepWlanCfg,
    QmiStepWlanMode,
    QmiStepDone
};

static const PCSTR QmiStepNames[] = {
    "idle", "phy-cap", "ind-register", "host-cap", "wait-mem", "wait-mem-ready",
    "cap", "tme-lite", "regdb", "board", "m3", "aux-uc", "qdss", "wait-fw-ready",
    "wlan-ini", "wlan-cfg", "wlan-mode", "done"
};

#pragma pack(push, 1)
typedef struct _QMI_HDR {
    UCHAR   Type;
    USHORT  TxnId;
    USHORT  MsgId;
    USHORT  MsgLen;
} QMI_HDR;
#pragma pack(pop)

C_ASSERT (sizeof (QMI_HDR) == 7);

static
ULONG
QmiGet16 (
    _In_reads_bytes_ (2) const UCHAR *Data
    )
{
    return Data[0] | ((ULONG)Data[1] << 8);
}

static
ULONG
QmiGet32 (
    _In_reads_bytes_ (4) const UCHAR *Data
    )
{
    return Data[0] | ((ULONG)Data[1] << 8) | ((ULONG)Data[2] << 16) | ((ULONG)Data[3] << 24);
}

static
ULONG64
QmiGet64 (
    _In_reads_bytes_ (8) const UCHAR *Data
    )
{
    return QmiGet32 (Data) | ((ULONG64)QmiGet32 (Data + 4) << 32);
}

/* Copies a QMI string into a printable, NUL-terminated buffer. */
static
VOID
QmiCopyString (
    _Out_writes_z_ (Size) PCHAR Buffer,
    _In_ ULONG Size,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    ULONG Index;

    if (Length > Size - 1) {
        Length = Size - 1;
    }
    for (Index = 0; Index < Length && Data[Index] != 0; Index++) {
        Buffer[Index] = (Data[Index] >= 0x20 && Data[Index] < 0x7F) ? (CHAR)Data[Index] : '?';
    }
    Buffer[Index] = 0;
}

PCSTR
WcnQmiStepName (
    _In_ PWCN_DEVICE Dev
    )
{
    return (Dev->Qmi.Step < RTL_NUMBER_OF (QmiStepNames)) ? QmiStepNames[Dev->Qmi.Step] : "?";
}

/* --- request encoding (qmi_encdec.c layout: type u8, length u16, value) --- */

static
VOID
QmiBegin (
    _In_ PWCN_QMI Qmi,
    _In_ USHORT MsgId
    )
{
    Qmi->TxMsgId = MsgId;
    Qmi->TxLength = sizeof (QMI_HDR);
}

static
VOID
QmiPut (
    _In_ PWCN_QMI Qmi,
    _In_ UCHAR Type,
    _In_reads_bytes_ (Length) const VOID *Value,
    _In_ ULONG Length
    )
{
    if (Qmi->TxLength + 3 + Length > WCN_QMI_TX_MAX) {
        Qmi->TxLength = WCN_QMI_TX_MAX + 1;
        return;
    }
    Qmi->Tx[Qmi->TxLength] = Type;
    Qmi->Tx[Qmi->TxLength + 1] = (UCHAR)Length;
    Qmi->Tx[Qmi->TxLength + 2] = (UCHAR)(Length >> 8);
    RtlCopyMemory (Qmi->Tx + Qmi->TxLength + 3, Value, Length);
    Qmi->TxLength += 3 + Length;
}

static
VOID
QmiPutU8 (
    _In_ PWCN_QMI Qmi,
    _In_ UCHAR Type,
    _In_ UCHAR Value
    )
{
    QmiPut (Qmi, Type, &Value, sizeof (Value));
}

static
VOID
QmiPutU32 (
    _In_ PWCN_QMI Qmi,
    _In_ UCHAR Type,
    _In_ ULONG Value
    )
{
    QmiPut (Qmi, Type, &Value, sizeof (Value));
}

static
VOID
QmiPutU64 (
    _In_ PWCN_QMI Qmi,
    _In_ UCHAR Type,
    _In_ ULONG64 Value
    )
{
    QmiPut (Qmi, Type, &Value, sizeof (Value));
}

static
VOID
QmiSend (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    QMI_HDR *Hdr = (QMI_HDR *)Qmi->Tx;
    NTSTATUS Status;

    if (Qmi->TxLength > WCN_QMI_TX_MAX) {
        WcnLog (Dev, "qmi tx: msg 0x%04x does not fit the buffer", Qmi->TxMsgId);
        Qmi->Failed = TRUE;
        return;
    }
    Qmi->TxnId++;
    Hdr->Type = QMI_REQUEST;
    Hdr->TxnId = Qmi->TxnId;
    Hdr->MsgId = Qmi->TxMsgId;
    Hdr->MsgLen = (USHORT)(Qmi->TxLength - sizeof (QMI_HDR));
    Status = WcnQrtrSendToService (Dev, Qmi->Tx, Qmi->TxLength);
    WcnLog (Dev, "qmi tx: msg 0x%04x txn %u len %u, status 0x%08x", Hdr->MsgId, Hdr->TxnId, Hdr->MsgLen, Status);
    if (!NT_SUCCESS (Status)) {
        Qmi->Failed = TRUE;
        return;
    }
    Qmi->Outstanding = Qmi->TxMsgId;
}

/* ath12k_qmi_host_cap_send for PEACH */
static
VOID
QmiSendHostCap (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;

    QmiBegin (Qmi, QMI_WLFW_HOST_CAP);
    QmiPutU32 (Qmi, 0x10, 1);                   /* num_clients */
    QmiPutU8 (Qmi, 0x14, 1);                    /* bdf_support */
    QmiPutU8 (Qmi, 0x16, 1);                    /* m3_support */
    QmiPutU8 (Qmi, 0x17, 1);                    /* m3_cache_support */
    QmiPutU8 (Qmi, 0x1A, 0);                    /* cal_done */
    QmiPutU8 (Qmi, 0x1C, 0);                    /* mem_cfg_mode = ATH12K_QMI_MEMORY_MODE_DEFAULT */
    QmiPutU64 (Qmi, 0x27, QMI_CNSS_FEATURES);   /* feature_list */
    QmiSend (Dev);
}

static
NTSTATUS
QmiLoadDmaFile (
    _In_ PWCN_DEVICE Dev,
    _In_z_ PCWSTR Path,
    _In_ ULONG Alignment,
    _Out_ PWCN_DMA Dma
    )
{
    PUCHAR Data;
    SIZE_T Size;
    NTSTATUS Status;

    RtlZeroMemory (Dma, sizeof (*Dma));
    Status = WcnReadFile (Path, &Data, &Size);
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "qmi: firmware open failed 0x%08x", Status);
        return Status;
    }
    if (Size == 0 || Size > MAXULONG) {
        ExFreePoolWithTag (Data, WCN_TAG);
        return STATUS_FILE_INVALID;
    }
    Status = WcnDmaAllocAligned (Dev, Size, Alignment - 1, Dma);
    if (NT_SUCCESS (Status)) {
        RtlCopyMemory (Dma->Va, Data, Size);
        KeMemoryBarrier ();
        WcnLog (Dev, "qmi: staged info image size 0x%Ix dma 0x%llx", Size, Dma->La);
    }
    ExFreePoolWithTag (Data, WCN_TAG);
    return Status;
}

static
VOID
QmiFreeDma (
    _Inout_ PWCN_DMA Dma
    )
{
    if (Dma->Buffer != NULL) {
        WdfObjectDelete (Dma->Buffer);
    }
    RtlZeroMemory (Dma, sizeof (*Dma));
}

static
VOID
QmiSendAddressInfo (
    _In_ PWCN_DEVICE Dev,
    _In_ USHORT Message,
    _In_ ULONG Type,
    _In_ PWCN_DMA Dma
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    UCHAR AddressType;
    UCHAR SizeType;

    QmiBegin (Qmi, Message);
    if (Message == QMI_WLFW_TME_LITE_INFO) {
        QmiPutU32 (Qmi, 0x01, Type);
        AddressType = 0x02;
        SizeType = 0x03;
    } else {
        AddressType = 0x01;
        SizeType = 0x02;
    }
    QmiPutU64 (Qmi, AddressType, Dma->La);
    QmiPutU32 (Qmi, SizeType, (ULONG)Dma->Length);
    QmiSend (Dev);
}

static
VOID
QmiStartBdfDownload (
    _In_ PWCN_DEVICE Dev,
    _In_z_ PCWSTR Path,
    _In_ ULONG Type,
    _In_ ULONG NextStep
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    NTSTATUS Status;

    Status = WcnReadFile (Path, &Qmi->DownloadData, &Qmi->DownloadSize);
    if (!NT_SUCCESS (Status) || Qmi->DownloadSize == 0 || Qmi->DownloadSize > MAXULONG) {
        WcnLog (Dev, "qmi: BDF file unavailable status 0x%08x size 0x%Ix", Status, Qmi->DownloadSize);
        if (Qmi->DownloadData != NULL) {
            ExFreePoolWithTag (Qmi->DownloadData, WCN_TAG);
            Qmi->DownloadData = NULL;
        }
        Qmi->Failed = TRUE;
        return;
    }
    Qmi->DownloadOffset = 0;
    Qmi->DownloadSegment = 0;
    Qmi->DownloadType = Type;
    Qmi->DownloadNextStep = NextStep;
    WcnLog (Dev, "qmi: starting BDF download type %u file-id 0x%x size 0x%Ix",
            Type, Qmi->BoardId, Qmi->DownloadSize);
}

static
VOID
QmiSendBdfChunk (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    ULONG Remaining;
    ULONG Chunk;
    ULONG Position;

    if (Qmi->DownloadData == NULL || Qmi->DownloadOffset >= Qmi->DownloadSize) {
        Qmi->Failed = TRUE;
        return;
    }
    Remaining = (ULONG)(Qmi->DownloadSize - Qmi->DownloadOffset);
    Chunk = (Remaining < WCN_QMI_MAX_DATA) ? Remaining : WCN_QMI_MAX_DATA;

    QmiBegin (Qmi, QMI_WLFW_BDF_DOWNLOAD);
    QmiPutU8 (Qmi, 0x01, 1);
    QmiPutU32 (Qmi, 0x10, Qmi->BoardId);
    QmiPutU32 (Qmi, 0x11, Remaining);
    QmiPutU32 (Qmi, 0x12, Qmi->DownloadSegment);
    Position = Qmi->TxLength;
    Qmi->Tx[Position] = 0x13;
    Qmi->Tx[Position + 1] = (UCHAR)(Chunk + 2);
    Qmi->Tx[Position + 2] = (UCHAR)((Chunk + 2) >> 8);
    Qmi->Tx[Position + 3] = (UCHAR)Chunk;
    Qmi->Tx[Position + 4] = (UCHAR)(Chunk >> 8);
    RtlCopyMemory (Qmi->Tx + Position + 5, Qmi->DownloadData + Qmi->DownloadOffset, Chunk);
    Qmi->TxLength += 5 + Chunk;
    QmiPutU8 (Qmi, 0x14, (Remaining <= WCN_QMI_MAX_DATA) ? 1 : 0);
    QmiPutU8 (Qmi, 0x15, (UCHAR)Qmi->DownloadType);
    WcnLog (Dev, "qmi: BDF type %u segment %u offset 0x%Ix chunk 0x%x remaining 0x%x",
            Qmi->DownloadType, Qmi->DownloadSegment, Qmi->DownloadOffset, Chunk, Remaining);
    QmiSend (Dev);
}

static
VOID
QmiStartInfoRequest (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Step
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    PWCN_DMA Dma;
    PCWSTR Path;
    ULONG Alignment = 0x10000;
    ULONG Type = 0;
    USHORT Message;
    NTSTATUS Status;

    switch (Step) {
    case QmiStepTmeLite:
        Dma = &Qmi->TmeLite;
        Path = WCN_TME_FILE;
        Message = QMI_WLFW_TME_LITE_INFO;
        break;
    case QmiStepM3:
        Dma = &Qmi->M3;
        Path = WCN_M3_FILE;
        Message = QMI_WLFW_M3_INFO;
        break;
    default:
        Dma = &Qmi->AuxUc;
        Path = WCN_AUX_UC_FILE;
        Message = QMI_WLFW_AUX_UC_INFO;
        break;
    }
    Status = QmiLoadDmaFile (Dev, Path, Alignment, Dma);
    if (!NT_SUCCESS (Status)) {
        WcnLog (Dev, "qmi: info firmware stage %u failed to load: 0x%08x", Step, Status);
        Qmi->Failed = TRUE;
        return;
    }
    QmiSendAddressInfo (Dev, Message, Type, Dma);
}

static
VOID
QmiSendQdssChunk (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    ULONG Remaining;
    ULONG Chunk;
    ULONG Position;

    if (Qmi->DownloadData == NULL || Qmi->DownloadOffset >= Qmi->DownloadSize) {
        Qmi->Failed = TRUE;
        return;
    }
    Remaining = (ULONG)(Qmi->DownloadSize - Qmi->DownloadOffset);
    Chunk = (Remaining < WCN_QMI_MAX_DATA) ? Remaining : WCN_QMI_MAX_DATA;

    QmiBegin (Qmi, QMI_WLFW_QDSS_CONFIG);
    QmiPutU32 (Qmi, 0x10, Remaining);
    QmiPutU32 (Qmi, 0x11, Qmi->DownloadSegment);
    Position = Qmi->TxLength;
    Qmi->Tx[Position] = 0x12;
    Qmi->Tx[Position + 1] = (UCHAR)(Chunk + 2);
    Qmi->Tx[Position + 2] = (UCHAR)((Chunk + 2) >> 8);
    Qmi->Tx[Position + 3] = (UCHAR)Chunk;
    Qmi->Tx[Position + 4] = (UCHAR)(Chunk >> 8);
    RtlCopyMemory (Qmi->Tx + Position + 5, Qmi->DownloadData + Qmi->DownloadOffset, Chunk);
    Qmi->TxLength += 5 + Chunk;
    QmiPutU8 (Qmi, 0x13, (Remaining <= WCN_QMI_MAX_DATA) ? 1 : 0);
    WcnLog (Dev, "qmi: QDSS config segment %u offset 0x%Ix chunk 0x%x remaining 0x%x",
            Qmi->DownloadSegment, Qmi->DownloadOffset, Chunk, Remaining);
    QmiSend (Dev);
}

/* ath12k_qmi_fw_ind_register_send */
static
VOID
QmiSendIndRegister (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;

    QmiBegin (Qmi, QMI_WLFW_IND_REGISTER);
    QmiPutU8 (Qmi, 0x10, 1);                    /* fw_ready_enable */
    QmiPutU32 (Qmi, 0x15, QMI_CLIENT_ID);       /* client_id */
    QmiPutU8 (Qmi, 0x16, 1);                    /* request_mem_enable */
    QmiPutU8 (Qmi, 0x17, 1);                    /* fw_mem_ready_enable */
    QmiPutU8 (Qmi, 0x18, 1);                    /* fw_init_done_enable */
    QmiPutU8 (Qmi, 0x1B, 1);                    /* cal_done_enable */
    QmiSend (Dev);
}

/* ath12k_qmi_respond_fw_mem_request; an empty list asks for smaller chunks */
static
VOID
QmiSendRespondMem (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    UCHAR Value[1 + WCN_QMI_MAX_MEM_SEG * 17];
    ULONG Length = 1;
    ULONG Count = Qmi->MemDelayed ? 0 : Qmi->MemSegCount;
    ULONG Index;
    ULONG64 Address;
    ULONG Type;

    Value[0] = (UCHAR)Count;
    for (Index = 0; Index < Count; Index++) {
        Address = Qmi->Mem[Index].Dma.La;
        Type = Qmi->Mem[Index].Type;
        RtlCopyMemory (Value + Length, &Address, 8);
        RtlCopyMemory (Value + Length + 8, &Qmi->Mem[Index].Size, 4);
        RtlCopyMemory (Value + Length + 12, &Type, 4);
        Value[Length + 16] = 0;                 /* restore */
        Length += 17;
    }
    QmiBegin (Qmi, QMI_WLFW_RESPOND_MEM);
    QmiPut (Qmi, 0x01, Value, Length);
    QmiSend (Dev);
}

static
VOID
QmiFreeMem (
    _In_ PWCN_QMI Qmi
    )
{
    ULONG Index;

    for (Index = 0; Index < WCN_QMI_MAX_MEM_SEG; Index++) {
        if (Qmi->Mem[Index].Dma.Buffer != NULL) {
            WdfObjectDelete (Qmi->Mem[Index].Dma.Buffer);
        }
        RtlZeroMemory (&Qmi->Mem[Index].Dma, sizeof (Qmi->Mem[Index].Dma));
    }
}

/* ath12k_qmi_msg_mem_request_cb + ath12k_qmi_alloc_target_mem_chunk */
static
VOID
QmiRequestMem (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Value,
    _In_ ULONG Length
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;
    ULONG Count;
    ULONG Offset = 1;
    ULONG Index;
    ULONG Configs;
    ULONG Config;
    NTSTATUS Status;
    ULONG Alignment;
    SIZE_T AllocationSize;

    QmiFreeMem (Qmi);
    Qmi->MemSegCount = 0;
    Qmi->MemDelayed = FALSE;
    if (Length < 1) {
        return;
    }
    Count = Value[0];
    if (Count > WCN_QMI_MAX_MEM_SEG) {
        WcnLog (Dev, "qmi: %u memory segments requested, more than %u", Count, WCN_QMI_MAX_MEM_SEG);
        Count = WCN_QMI_MAX_MEM_SEG;
    }

    for (Index = 0; Index < Count; Index++) {
        if (Offset + 9 > Length) {
            WcnLog (Dev, "qmi: memory request truncated at segment %u", Index);
            break;
        }
        Qmi->Mem[Index].Size = QmiGet32 (Value + Offset);
        Qmi->Mem[Index].Type = QmiGet32 (Value + Offset + 4);
        Configs = Value[Offset + 8];
        WcnLog (Dev, "qmi: mem seg %u type %u size 0x%x (%u configs)", Index, Qmi->Mem[Index].Type, Qmi->Mem[Index].Size, Configs);
        Offset += 9;
        for (Config = 0; Config < Configs && Offset + 13 <= Length; Config++, Offset += 13) {
            WcnLog (Dev, "qmi:   cfg %u offset 0x%llx size 0x%x secure %u",
                    Config, QmiGet64 (Value + Offset), QmiGet32 (Value + Offset + 8), Value[Offset + 12]);
        }
        Qmi->MemSegCount = Index + 1;
    }

    for (Index = 0; Index < Qmi->MemSegCount; Index++) {
        switch (Qmi->Mem[Index].Type) {
        case QMI_MEM_HOST_DDR:
        case QMI_MEM_M3_DUMP:
        case QMI_MEM_CALDB:
        case QMI_MEM_MLO_GLOBAL:
        case QMI_MEM_PAGEABLE:
        case QMI_MEM_LPASS_SHARED:
            /* QuRT maps these chunks into its fixed virtual regions. Match
             * Linux coherent allocations' power-of-two address alignment;
             * v0.5 asserted while mapping the page-aligned CALDB chunk.
             * Keep the firmware's requested size in RESPOND_MEM.
             */
            Alignment = QMI_MEM_ALIGNMENT;
            while (Alignment < Qmi->Mem[Index].Size && Alignment < 0x80000000UL) {
                Alignment <<= 1;
            }
            AllocationSize = ((SIZE_T)Qmi->Mem[Index].Size + QMI_MEM_ALIGNMENT - 1) &
                             ~((SIZE_T)QMI_MEM_ALIGNMENT - 1);
            Status = WcnDmaAllocAligned (Dev, AllocationSize, Alignment - 1, &Qmi->Mem[Index].Dma);
            if (!NT_SUCCESS (Status)) {
                WcnLog (Dev, "qmi: mem seg %u (0x%x bytes) allocation failed 0x%08x", Index, Qmi->Mem[Index].Size, Status);
                QmiFreeMem (Qmi);
                if (Qmi->Mem[Index].Size > QMI_MAX_CHUNK_SIZE) {
                    Qmi->MemDelayed = TRUE;
                    WcnLog (Dev, "qmi: asking the firmware for smaller chunks");
                } else {
                    Qmi->Failed = TRUE;
                }
                Qmi->MemRequestPending = !Qmi->Failed;
                return;
            }
            WcnLog (Dev, "qmi: mem seg %u at 0x%llx (requested 0x%x, allocated 0x%Ix, alignment 0x%x)",
                    Index, Qmi->Mem[Index].Dma.La, Qmi->Mem[Index].Size, AllocationSize, Alignment);
            break;
        default:
            WcnLog (Dev, "qmi: mem seg %u type %u not backed", Index, Qmi->Mem[Index].Type);
            break;
        }
    }
    Qmi->MemRequestPending = TRUE;
}

/* ath12k_qmi_request_target_cap: decode and log the reply */
static
VOID
QmiLogCap (
    _In_ PWCN_DEVICE Dev,
    _In_ UCHAR Type,
    _In_reads_bytes_ (Length) const UCHAR *Value,
    _In_ ULONG Length
    )
{
    CHAR Text[160];
    CHAR Pin[33];
    ULONG Count;
    ULONG Index;
    const UCHAR *Entry;

    switch (Type) {
    case 0x10:
        if (Length >= 8) {
            WcnLog (Dev, "cap: chip_id 0x%x chip_family 0x%x", QmiGet32 (Value), QmiGet32 (Value + 4));
            return;
        }
        break;
    case 0x11:
        if (Length >= 4) {
            Dev->Qmi.BoardId = QmiGet32 (Value);
            WcnLog (Dev, "cap: board_id 0x%x", Dev->Qmi.BoardId);
            return;
        }
        break;
    case 0x12:
        if (Length >= 4) {
            WcnLog (Dev, "cap: soc_id 0x%x", QmiGet32 (Value));
            return;
        }
        break;
    case 0x13:
        if (Length >= 5 && 5 + (ULONG)Value[4] <= Length) {
            QmiCopyString (Text, sizeof (Text), Value + 5, Value[4]);
            WcnLog (Dev, "cap: fw_version 0x%x build timestamp %s", QmiGet32 (Value), Text);
            return;
        }
        break;
    case 0x14:
        QmiCopyString (Text, sizeof (Text), Value, Length);
        WcnLog (Dev, "cap: fw_build_id %s", Text);
        return;
    case 0x1A:
        if (Length >= 8) {
            Dev->Qmi.FirmwareCaps = QmiGet64 (Value);
            WcnLog (Dev, "cap: fw_caps 0x%llx", Dev->Qmi.FirmwareCaps);
            return;
        }
        break;
    case 0x1C:
        for (Index = 0; Index + 16 <= Length; Index += 16) {
            WcnLog (Dev, "cap: dev_mem start 0x%llx size 0x%llx", QmiGet64 (Value + Index), QmiGet64 (Value + Index + 8));
            if (Index == 0) {
                Dev->Qmi.CmemBase = QmiGet64 (Value);
                Dev->Qmi.CmemSize = QmiGet64 (Value + 8);
            }
        }
        return;
    case 0x22:
        if (Length >= 1) {
            Count = Value[0];
            WcnLog (Dev, "cap: ol_cpr_cfg %u entries (logged only)", Count);
            for (Index = 0; Index < Count && 1 + (Index + 1) * 48 <= Length; Index++) {
                Entry = Value + 1 + Index * 48;
                QmiCopyString (Pin, sizeof (Pin), Entry, 32);
                WcnLog (Dev, "cap:   %s wake %u (valid %u) sleep %u (valid %u)",
                        Pin, QmiGet32 (Entry + 36), QmiGet32 (Entry + 32), QmiGet32 (Entry + 44), QmiGet32 (Entry + 40));
            }
            return;
        }
        break;
    default:
        break;
    }
    WcnLog (Dev, "cap: tlv 0x%02x length %u", Type, Length);
    WcnLogHex (Dev, "cap:   ", Value, Length);
}

static
VOID
QmiSendWlanCfg (
    _In_ PWCN_DEVICE Dev
    )
{
    static const ULONG CeConfig[9][5] = {
        { 0, 2, 32, 2048, 0 }, { 1, 1, 32, 2048, 0 },
        { 2, 1, 32, 2048, 0 }, { 3, 2, 32, 2048, 0 },
        { 4, 2, 256, 256, 8 }, { 5, 1, 32, 2048, 0 },
        { 6, 3, 32, 16384, 0 }, { 7, 4, 0, 0, 8 },
        { 8, 3, 32, 16384, 0 }
    };
    static const ULONG ServiceMap[14][3] = {
        { 0x0104, 2, 3 }, { 0x0104, 1, 2 }, { 0x0102, 2, 3 }, { 0x0102, 1, 2 },
        { 0x0101, 2, 3 }, { 0x0101, 1, 2 }, { 0x0103, 2, 3 }, { 0x0103, 1, 2 },
        { 0x0100, 2, 3 }, { 0x0100, 1, 2 }, { 0x0001, 2, 0 }, { 0x0001, 1, 2 },
        { 0x0300, 2, 4 }, { 0x0300, 1, 1 }
    };
    ULONG ShadowReg[36];
    UCHAR Target[1 + RTL_NUMBER_OF (CeConfig) * 20];
    UCHAR Services[1 + RTL_NUMBER_OF (ServiceMap) * 12];
    UCHAR Shadow[1 + RTL_NUMBER_OF (ShadowReg) * 4];
    static const UCHAR HostVersion[] = { 'W', 'I', 'N' };
    ULONG Index;
    ULONG Field;
    ULONG ShadowCount;

    RtlZeroMemory (Target, sizeof (Target));
    RtlZeroMemory (Services, sizeof (Services));
    RtlZeroMemory (Shadow, sizeof (Shadow));
    Target[0] = RTL_NUMBER_OF (CeConfig);
    Services[0] = RTL_NUMBER_OF (ServiceMap);
    ShadowCount = 0;
    for (Index = 0; Index < 8; Index++) {
        ShadowReg[ShadowCount++] = 0x00A3B04C + Index * 8;
    }
    ShadowReg[ShadowCount++] = 0x00A3B08C;
    ShadowReg[ShadowCount++] = 0x00A3B028;
    ShadowReg[ShadowCount++] = 0x00A3B020;
    ShadowReg[ShadowCount++] = 0x00A3B0AC;
    for (Index = 0; Index < 5; Index++) {
        ShadowReg[ShadowCount++] = 0x00A46000 + Index * 8;
    }
    ShadowReg[ShadowCount++] = 0x00A46028;
    ShadowReg[ShadowCount++] = 0x00A4604C;
    ShadowReg[ShadowCount++] = 0x00A370B8;
    ShadowReg[ShadowCount++] = 0x00A37010;
    for (Index = 0; Index < 8; Index++) {
        ShadowReg[ShadowCount++] = 0x00A370CC + Index * 8;
    }
    ShadowReg[ShadowCount++] = WCN_CE_SRC_BASE + 0x400;
    ShadowReg[ShadowCount++] = WCN_CE_DST_BASE + WCN_CE_STRIDE + 0x400;
    ShadowReg[ShadowCount++] = WCN_CE_DST_BASE + WCN_CE_STRIDE + 0x40C;
    ShadowReg[ShadowCount++] = WCN_CE_DST_BASE + 2 * WCN_CE_STRIDE + 0x400;
    ShadowReg[ShadowCount++] = WCN_CE_DST_BASE + 2 * WCN_CE_STRIDE + 0x40C;
    ShadowReg[ShadowCount++] = WCN_CE_SRC_BASE + 3 * WCN_CE_STRIDE + 0x400;
    ShadowReg[ShadowCount++] = WCN_CE_SRC_BASE + 4 * WCN_CE_STRIDE + 0x400;
    Shadow[0] = (UCHAR)ShadowCount;
    for (Index = 0; Index < RTL_NUMBER_OF (CeConfig); Index++) {
        for (Field = 0; Field < 5; Field++) {
            RtlCopyMemory (Target + 1 + Index * 20 + Field * 4, &CeConfig[Index][Field], 4);
        }
    }
    for (Index = 0; Index < RTL_NUMBER_OF (ServiceMap); Index++) {
        for (Field = 0; Field < 3; Field++) {
            RtlCopyMemory (Services + 1 + Index * 12 + Field * 4, &ServiceMap[Index][Field], 4);
        }
    }
    for (Index = 0; Index < ShadowCount; Index++) {
        RtlCopyMemory (Shadow + 1 + Index * 4, &ShadowReg[Index], 4);
    }

    QmiBegin (&Dev->Qmi, QMI_WLFW_WLAN_CFG);
    QmiPut (&Dev->Qmi, 0x10, HostVersion, sizeof (HostVersion));
    QmiPut (&Dev->Qmi, 0x11, Target, sizeof (Target));
    QmiPut (&Dev->Qmi, 0x12, Services, sizeof (Services));
    QmiPut (&Dev->Qmi, 0x17, Shadow, sizeof (Shadow));
    QmiSend (Dev);
}

static
VOID
QmiAdvance (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;

    if (Qmi->Outstanding != 0 || Qmi->Failed) {
        return;
    }
    switch (Qmi->Step) {
    case QmiStepPhyCap:
        QmiBegin (Qmi, QMI_WLFW_PHY_CAP);
        QmiSend (Dev);
        break;
    case QmiStepIndRegister:
        QmiSendIndRegister (Dev);
        break;
    case QmiStepHostCap:
        QmiSendHostCap (Dev);
        break;
    case QmiStepWaitMem:
    case QmiStepWaitMemReady:
        if (Qmi->MemRequestPending) {
            Qmi->MemRequestPending = FALSE;
            QmiSendRespondMem (Dev);
        } else if (Qmi->Step == QmiStepWaitMemReady && Qmi->MemReadyPending) {
            Qmi->MemReadyPending = FALSE;
            Qmi->Step = QmiStepCap;
            QmiBegin (Qmi, QMI_WLFW_CAP);
            QmiSend (Dev);
        }
        break;
    case QmiStepTmeLite:
    case QmiStepM3:
    case QmiStepAuxUc:
        QmiStartInfoRequest (Dev, Qmi->Step);
        break;
    case QmiStepRegdb:
        if (Qmi->DownloadData == NULL) {
            QmiStartBdfDownload (Dev, WCN_REGDB_FILE, QMI_BDF_TYPE_REGDB, QmiStepBoard);
        }
        if (!Qmi->Failed) {
            QmiSendBdfChunk (Dev);
        }
        break;
    case QmiStepBoard:
        if (Qmi->DownloadData == NULL) {
            QmiStartBdfDownload (Dev, WCN_BOARD_FILE, QMI_BDF_TYPE_ELF, QmiStepM3);
        }
        if (!Qmi->Failed) {
            QmiSendBdfChunk (Dev);
        }
        break;
    case QmiStepQdss:
        if (Qmi->DownloadData == NULL) {
            QmiStartBdfDownload (Dev, WCN_QDSS_FILE, 0, QmiStepWaitFwReady);
        }
        if (!Qmi->Failed) {
            QmiSendQdssChunk (Dev);
        }
        break;
    case QmiStepWlanIni:
        QmiBegin (Qmi, QMI_WLFW_WLAN_INI);
        QmiPutU8 (Qmi, 0x10, 1);
        QmiSend (Dev);
        break;
    case QmiStepWlanCfg:
        QmiSendWlanCfg (Dev);
        break;
    case QmiStepWlanMode:
        QmiBegin (Qmi, QMI_WLFW_WLAN_MODE);
        QmiPutU32 (Qmi, 0x01, 0);
        QmiPutU8 (Qmi, 0x10, 0);
        QmiSend (Dev);
        break;
    default:
        break;
    }
}

static
VOID
QmiResponse (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG MsgId,
    _In_ ULONG Result,
    _In_ ULONG Error
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;

    if (MsgId != Qmi->Outstanding) {
        WcnLog (Dev, "qmi: response 0x%04x while waiting for 0x%04x", MsgId, Qmi->Outstanding);
        return;
    }
    Qmi->Outstanding = 0;

    switch (MsgId) {
    case QMI_WLFW_PHY_CAP:
        /* ath12k falls back to the default link count when this fails */
        Qmi->Step = QmiStepIndRegister;
        return;
    case QMI_WLFW_IND_REGISTER:
        Qmi->Step = QmiStepHostCap;
        break;
    case QMI_WLFW_HOST_CAP:
        Qmi->Step = QmiStepWaitMem;
        break;
    case QMI_WLFW_RESPOND_MEM:
        if (Result != 0 && Qmi->MemDelayed && Error == 0) {
            /* expected: the firmware comes back with a new REQUEST_MEM_IND */
            return;
        }
        Qmi->Step = QmiStepWaitMemReady;
        break;
    case QMI_WLFW_BDF_DOWNLOAD:
        if (Result == 0 && Qmi->DownloadData != NULL) {
            ULONG Remaining = (ULONG)(Qmi->DownloadSize - Qmi->DownloadOffset);
            ULONG Chunk = (Remaining < WCN_QMI_MAX_DATA) ? Remaining : WCN_QMI_MAX_DATA;

            Qmi->DownloadOffset += Chunk;
            Qmi->DownloadSegment++;
            if (Qmi->DownloadOffset == Qmi->DownloadSize) {
                WcnLog (Dev, "qmi: BDF type %u download complete", Qmi->DownloadType);
                ExFreePoolWithTag (Qmi->DownloadData, WCN_TAG);
                Qmi->DownloadData = NULL;
                Qmi->DownloadSize = 0;
                Qmi->DownloadOffset = 0;
                Qmi->DownloadSegment = 0;
                Qmi->Step = Qmi->DownloadNextStep;
            }
        }
        break;
    case QMI_WLFW_QDSS_CONFIG:
        if (Result == 0 && Qmi->DownloadData != NULL) {
            ULONG Remaining = (ULONG)(Qmi->DownloadSize - Qmi->DownloadOffset);
            ULONG Chunk = (Remaining < WCN_QMI_MAX_DATA) ? Remaining : WCN_QMI_MAX_DATA;

            Qmi->DownloadOffset += Chunk;
            Qmi->DownloadSegment++;
            if (Qmi->DownloadOffset == Qmi->DownloadSize) {
                WcnLog (Dev, "qmi: QDSS config download complete");
                ExFreePoolWithTag (Qmi->DownloadData, WCN_TAG);
                Qmi->DownloadData = NULL;
                Qmi->DownloadSize = 0;
                Qmi->DownloadOffset = 0;
                Qmi->DownloadSegment = 0;
                Qmi->Step = QmiStepWaitFwReady;
            }
        }
        break;
    case QMI_WLFW_TME_LITE_INFO:
        QmiFreeDma (&Qmi->TmeLite);
        Qmi->Step = QmiStepRegdb;
        if (Result != 0 && Error == QMI_ERR_HARDWARE_RESTRICTED) {
            WcnLog (Dev, "qmi: TME-L restricted by firmware power policy; continuing like ath12k");
            Result = 0;
        }
        break;
    case QMI_WLFW_M3_INFO:
        if (Result == 0) {
            WcnLog (Dev, "qmi: M3 info accepted");
            Qmi->Step = (Qmi->FirmwareCaps & QMI_FW_CAP_AUX_UC) ? QmiStepAuxUc : QmiStepQdss;
        }
        break;
    case QMI_WLFW_AUX_UC_INFO:
        if (Result == 0) {
            WcnLog (Dev, "qmi: AUX-UC info accepted");
            Qmi->Step = QmiStepQdss;
        }
        break;
    case QMI_WLFW_CAP:
        Qmi->Step = QmiStepTmeLite;
        WcnLog (Dev, "qmi: CAP accepted, board file-id 0x%x fw caps 0x%llx", Qmi->BoardId, Qmi->FirmwareCaps);
        break;
    case QMI_WLFW_WLAN_INI:
        Qmi->WlanIniReady = TRUE;
        Qmi->Step = QmiStepWlanCfg;
        break;
    case QMI_WLFW_WLAN_CFG:
        Qmi->WlanCfgReady = TRUE;
        Qmi->Step = QmiStepWlanMode;
        break;
    case QMI_WLFW_WLAN_MODE:
        if (Result == 0) {
            Qmi->WlanModeReady = TRUE;
            Qmi->Step = QmiStepDone;
            {
                NTSTATUS CeStatus = WcnDpInitialize (Dev);
                WcnLog (Dev, "dp: init status 0x%08x", CeStatus);
                if (NT_SUCCESS (CeStatus)) {
                    CeStatus = WcnCeInitialize (Dev);
                    WcnLog (Dev, "ce: init status 0x%08x", CeStatus);
                }
                if (!NT_SUCCESS (CeStatus)) Qmi->Failed = TRUE;
            }
        }
        break;
    default:
        break;
    }
    if (Result != 0) {
        WcnLog (Dev, "qmi: msg 0x%04x failed, result %u error %u", MsgId, Result, Error);
        Qmi->Failed = TRUE;
        if (Qmi->DownloadData != NULL) {
            ExFreePoolWithTag (Qmi->DownloadData, WCN_TAG);
            Qmi->DownloadData = NULL;
            Qmi->DownloadSize = 0;
            Qmi->DownloadOffset = 0;
        }
        QmiFreeDma (&Qmi->M3);
        QmiFreeDma (&Qmi->TmeLite);
        QmiFreeDma (&Qmi->AuxUc);
    }
}

VOID
WcnQmiServiceArrive (
    _In_ PWCN_DEVICE Dev
    )
{
    PWCN_QMI Qmi = &Dev->Qmi;

    if (Qmi->Started) {
        return;
    }
    Qmi->Started = TRUE;
    Qmi->BoardId = QMI_BOARD_ID_DEFAULT;
    Qmi->Step = QmiStepPhyCap;
    QmiAdvance (Dev);
}

VOID
WcnQmiReceive (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Size) const UCHAR *Data,
    _In_ ULONG Size
    )
{
    const QMI_HDR *Hdr = (const QMI_HDR *)Data;
    const UCHAR *Value;
    ULONG End;
    ULONG Offset;
    ULONG Length;
    ULONG Result = 0;
    ULONG Error = 0;
    UCHAR Type;

    if (Size < sizeof (QMI_HDR)) {
        WcnLog (Dev, "qmi rx: short message (%u bytes)", Size);
        return;
    }
    WcnLog (Dev, "qmi rx: type %u txn %u msg 0x%04x len %u", Hdr->Type, Hdr->TxnId, Hdr->MsgId, Hdr->MsgLen);

    End = sizeof (QMI_HDR) + Hdr->MsgLen;
    if (End > Size) {
        WcnLog (Dev, "qmi rx: message length %u exceeds the packet (%u)", Hdr->MsgLen, Size);
        End = Size;
    }
    for (Offset = sizeof (QMI_HDR); Offset + 3 <= End; Offset += 3 + Length) {
        Type = Data[Offset];
        Length = QmiGet16 (Data + Offset + 1);
        Value = Data + Offset + 3;
        if (Offset + 3 + Length > End) {
            WcnLog (Dev, "qmi rx: tlv 0x%02x length %u runs past the message", Type, Length);
            break;
        }
        if (Hdr->Type == QMI_RESPONSE && Type == 0x02 && Length >= 4) {
            Result = QmiGet16 (Value);
            Error = QmiGet16 (Value + 2);
            WcnLog (Dev, "qmi rx: result %u error %u", Result, Error);
        } else if (Hdr->Type == QMI_RESPONSE && Hdr->MsgId == QMI_WLFW_CAP) {
            QmiLogCap (Dev, Type, Value, Length);
        } else if (Hdr->Type == QMI_INDICATION && Hdr->MsgId == QMI_WLFW_REQUEST_MEM_IND && Type == 0x01) {
            QmiRequestMem (Dev, Value, Length);
        } else {
            WcnLog (Dev, "qmi rx: tlv 0x%02x length %u", Type, Length);
            WcnLogHex (Dev, "qmi rx:   ", Value, Length);
        }
    }

    if (Hdr->Type == QMI_RESPONSE) {
        QmiResponse (Dev, Hdr->MsgId, Result, Error);
    } else if (Hdr->Type == QMI_INDICATION) {
        switch (Hdr->MsgId) {
        case QMI_WLFW_FW_MEM_READY_IND:
            WcnLog (Dev, "qmi: firmware memory ready");
            Dev->Qmi.MemReadyPending = TRUE;
            break;
        case QMI_WLFW_FW_READY_IND:
            WcnLog (Dev, "qmi: firmware init done; configuring CE and WLAN mode");
            /* FW_READY completes QMI setup; firmware may still access M3/AUX-UC. */
            Dev->Qmi.FirmwareReady = TRUE;
            Dev->Qmi.Step = QmiStepWlanIni;
            break;
        default:
            break;
        }
    }
    QmiAdvance (Dev);
}
