#pragma once

#include <ntddk.h>

#define QPG_LOG_PATH            L"\\SystemRoot\\qcom_pmic_glink.log"
#define QPG_LOG_LINE            160u
#define QPG_LOG_RING            128u
#define QPG_POLL_MS             2000u

#define QPG_SMEM_PHYS           0x81D00000ull
#define QPG_SMEM_SIZE           0x00200000u
#define QPG_PTABLE_OFFSET       (QPG_SMEM_SIZE - 0x1000u)
#define QPG_IPCC_PHYS           0x00406000ull
#define QPG_IPCC_SIZE           0x00001000u
#define QPG_TCSR_PHYS           0x01F43000ull

#define QPG_HDR_VERSION_SBL     0x05Cu
#define QPG_HDR_INITIALIZED     0x0C0u
#define QPG_HDR_FREE_OFFSET     0x0C4u
#define QPG_HDR_AVAILABLE       0x0C8u

#define QPG_PTOC_MAGIC          0x434F5424u
#define QPG_PTOC_VERSION_OFF    0x04u
#define QPG_PTOC_ENTRIES_OFF    0x08u
#define QPG_PTOC_ENTRY0         0x20u
#define QPG_PTOC_ENTRY_SIZE     48u

#define QPG_PART_MAGIC          0x54525024u
#define QPG_PART_HDR_SIZE       32u
#define QPG_PART_HOSTS_OFF      0x04u
#define QPG_PART_SIZE_OFF       0x08u
#define QPG_PART_FREE_UNC_OFF   0x0Cu

#define QPG_PRIV_ENTRY_SIZE     16u
#define QPG_PRIV_CANARY         0xA5A5u

#define QPG_HOST_APPS           0u
#define QPG_HOST_ADSP           2u
#define QPG_HOST_GLOBAL         0xFFFEu

#define QPG_ITEM_DESC           478u
#define QPG_ITEM_FIFO0          479u
#define QPG_ITEM_FIFO1          480u

#define QPG_IPCC_CLIENT_LPASS   3u
#define QPG_IPCC_SIGNAL_GLINK   0u
#define QPG_IPCC_REG_SEND_ID    0x0Cu
#define QPG_IPCC_REG_RECV_ID    0x10u
#define QPG_IPCC_REG_RECV_EN    0x14u
#define QPG_IPCC_REG_RECV_CLR   0x1Cu
#define QPG_IPCC_NO_PENDING     0xFFFFFFFFu

#define QPG_WALK_MAX            256u

#define QPG_GLINK_VERSION       1u
#define QPG_GLINK_FEATURES      1u
#define QPG_GLINK_LCID          5u
#define QPG_GLINK_INTENTS       8u
#define QPG_GLINK_INTENT_SIZE   2048u
#define QPG_GLINK_REMOTE_INTENTS 32u
#define QPG_GLINK_TRIES         6000u
#define QPG_CHANNEL_NAME        "PMIC_RTR_ADSP_APPS"

#define QPG_GC_VERSION          0u
#define QPG_GC_VERSION_ACK      1u
#define QPG_GC_OPEN             2u
#define QPG_GC_CLOSE            3u
#define QPG_GC_OPEN_ACK         4u
#define QPG_GC_INTENT           5u
#define QPG_GC_RX_DONE          6u
#define QPG_GC_RX_INTENT_REQ    7u
#define QPG_GC_INTENT_REQ_ACK   8u
#define QPG_GC_TX_DATA          9u
#define QPG_GC_CLOSE_ACK        11u
#define QPG_GC_TX_DATA_CONT     12u
#define QPG_GC_READ_NOTIF       13u
#define QPG_GC_RX_DONE_REUSE    14u
#define QPG_GC_SIGNALS          15u

#define QPG_OWNER_BATTMGR       32778u
#define QPG_TYPE_REQ_RESP       1u
#define QPG_TYPE_NOTIFY         2u

#define QPG_OP_SET_NOTIFY_REQ   0x04u
#define QPG_OP_BATTERY_GET      0x30u
#define QPG_OP_BATTERY_SET      0x31u
#define QPG_OP_USB_GET          0x32u
#define QPG_OP_USB_SET          0x33u
#define QPG_OP_OPLUS_GET        0x0301u
#define QPG_OP_OPLUS_SET        0x0300u

#define QPG_BATT_CAPACITY       4u
#define QPG_BATT_VOLT_OCV       6u
#define QPG_BATT_VOLT_NOW       7u
#define QPG_BATT_VOLT_MAX       8u
#define QPG_BATT_CURR_NOW       9u
#define QPG_BATT_TEMP           12u
#define QPG_BATT_CYCLE_COUNT    15u
#define QPG_BATT_CHG_FULL_DESIGN 16u
#define QPG_BATT_CHG_FULL       17u
#define QPG_BATT_CHG_EN         24u
#define QPG_BATT_SET_PDO        25u
#define QPG_BATT_UEFI_INPUT_CURRENT 32u
#define QPG_BATT_UEFI_CHG_EN    34u
#define QPG_BATT_UEFI_LOAD_ADSP 35u

#define QPG_USB_ONLINE          0u
#define QPG_USB_CURR_NOW        3u
#define QPG_USB_INPUT_LIMIT     5u
#define QPG_USB_TYPE            6u
#define QPG_USB_ADAP_TYPE       7u

#define QPG_REG_CHARGE_MODE     L"QpgChargeMode"
#define QPG_REG_CHARGE_MA       L"QpgChargeMa"

#define QPG_MODE_READONLY       0u
#define QPG_MODE_CHARGE_LEGACY  1u
#define QPG_MODE_CHARGE_OPLUS   2u
#define QPG_CHARGE_MA_DEFAULT   500u

#define QPG_PROP_SET_ICL        0u
#define QPG_PROP_CHARGING_ENABLE 2u
#define QPG_PROP_USB_INPUT_CURR_LIMIT 5u
#define QPG_PROP_USB_ONLINE     6u
#define QPG_PROP_CHG_EN         70u
#define QPG_PROP_SET_PDO        71u
#define QPG_PROP_UEFI_INPUT_CURRENT 78u
#define QPG_PROP_UEFI_CHG_EN    80u
#define QPG_PROP_UEFI_LOAD_ADSP 81u

typedef struct _QPG_LOG_ENTRY
{
    CHAR Text[QPG_LOG_LINE];
} QPG_LOG_ENTRY;

typedef struct _QPG_LINK
{
    PUCHAR  Mutex;
    ULONG   Partition;
    ULONG   PartitionSize;
    PUCHAR  Desc;
    PUCHAR  TxFifo;
    ULONG   TxLength;
    PUCHAR  RxFifo;
    ULONG   RxLength;
    ULONG   Rcid;
    ULONG   Lcid;
    BOOLEAN OpenAcked;
    BOOLEAN Opening;
    BOOLEAN VersionAcked;
    BOOLEAN Intentless;
    ULONG   IntentIid[QPG_GLINK_REMOTE_INTENTS];
    ULONG   IntentSize[QPG_GLINK_REMOTE_INTENTS];
    BOOLEAN IntentFree[QPG_GLINK_REMOTE_INTENTS];
    ULONG   IntentCount;
    ULONG   TxIid;
    ULONG   LogRx;
    ULONG   Frames;
    ULONG   Foreign;
    ULONG   Sends;
    ULONG   Replies;
    ULONG   Notifies;
    ULONG   LastCapacity;
    ULONG   LastVoltage;
    LONG    LastCurrent;
    ULONG   LastUsbOnline;
    ULONG   LastUsbType;
} QPG_LINK, *PQPG_LINK;

typedef struct _QPG_DEVICE
{
    PDEVICE_OBJECT Self;
    PUCHAR         Smem;
    PUCHAR         Ipcc;
    PQPG_LINK      Link;
    HANDLE         LogFile;
    HANDLE         Thread;
    KEVENT         StopEvent;
    volatile LONG  Stopping;
    ULONG          ChargeMode;
    ULONG          ChargeMa;
    KSPIN_LOCK     Lock;
    QPG_LOG_ENTRY  LogRing[QPG_LOG_RING];
    volatile LONG  LogHead;
    volatile LONG  LogTail;
} QPG_DEVICE, *PQPG_DEVICE;

VOID
QpgLog (
    _In_ PQPG_DEVICE Dev,
    _In_z_ _Printf_format_string_ PCSTR Format,
    ...
    );

VOID
QpgLogFlush (
    _In_ PQPG_DEVICE Dev
    );

NTSTATUS
QpgLinkStart (
    _In_ PQPG_DEVICE Dev
    );

VOID
QpgLinkStop (
    _In_ PQPG_DEVICE Dev
    );

VOID
QpgLinkPoll (
    _In_ PQPG_DEVICE Dev
    );

BOOLEAN
QpgLinkSend (
    _In_ PQPG_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    );

VOID
QpgLinkKick (
    _In_ PQPG_DEVICE Dev
    );

VOID
QpgProtoRx (
    _In_ PQPG_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    );
