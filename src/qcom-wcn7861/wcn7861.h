/*
 * WCN7861 (Qualcomm PEACH, 17cb:110e) on hummer (OnePlus Ace 5 Pro, SM8750).
 *
 * Windows never enumerates the DWC root complex: WindowsBoot (UEFI Pcie0.c)
 * trains the link, places BAR0 and sets up the DWC internal MSI controller,
 * and the WLN0 ACPI node hands this driver the raw windows (Dsdt.asl):
 *   0  BAR0        0x40400000  2 MB   MHI/BHI registers + windowed SoC space
 *   1  config      0x40100000  4 KB   endpoint config space (iATU CFG0)
 *   2  DBI         0x40000000  4 KB   root port, iMSI-RX at +0x820
 *   3  PARF        0x01C00000  12 KB
 *   IRQ 173 = msi0 (SPI 141), level, all 32 iMSI vectors of controller 0.
 *
 * The bring-up follows the Linux ath12k PEACH hardware profile
 * (wifi7/pci.c PEACH entry, wifi7/hal_qcc2072.c and mhi.c)
 * and the MHI host stack (drivers/bus/mhi/host boot.c, pm.c, init.c, main.c).
 */
#pragma once

#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>

#define WCN_HOTSPOT_CLIENTS 8u
#define WCN_TAG                     'nCWh'
#define WCN_TX_SLOTS               256u

#define WCN_PCI_ID                  0x110E17CB

/* WCN7861 PEACH PCIe window register (shared PEACH/QCC2072 aperture). */
#define WCN_WINDOW_ENABLE           0x40000000
#define WCN_WINDOW_SHIFT            19
#define WCN_WINDOW_MASK             0x3F
#define WCN_WINDOW_START            0x80000
#define WCN_WINDOW_RANGE            0x7FFFF
#define WCN_WINDOW_REG              0x3278
#define WCN_MHI_REGION_START        0x1E0E100
#define WCN_MHI_REGION_END          0x1E0EFFC

#define WCN_SOC_GLOBAL_RESET        0x3008
#define WCN_WLAON_WARM_SW_ENTRY     0x1F80504
#define WCN_Q6_COOKIE               0x1F80500
#define WCN_WLAON_QFPROM_PWR_CTRL   0x1F8031C
#define WCN_PARF_LTSSM              0x1E081B0
#define WCN_PARF_LTSSM_VALUE        0x111
#define WCN_GCC_PCIE_HOT_RST        0x1E65304
#define WCN_PCIE_INT_ALL_CLEAR      0x1E08228
#define WCN_TCSR_SOC_HW_VERSION     0x1B00000
#define WCN_TXVECDB                 0x360
#define WCN_TXVECSTATUS             0x368
#define WCN_RXVECDB                 0x394
#define WCN_RXVECSTATUS             0x39C

/* WCN7861 PEACH CE SRNG windows, accessed through BAR0's SoC window. */
#define WCN_CE_SRC_BASE             0x01B80000
#define WCN_CE_DST_BASE             0x01B81000
#define WCN_CE_STRIDE               0x2000
#define WCN_CE_RING_BASE_LSB        0x000
#define WCN_CE_RING_BASE_MSB        0x004
#define WCN_CE_RING_ID              0x008
#define WCN_CE_RING_MISC            0x010
#define WCN_CE_DST_RING_CTRL        0x0B0
#define WCN_CE_DST_STATUS_BASE_LSB  0x058
#define WCN_CE_DST_STATUS_BASE_MSB  0x05C
#define WCN_CE_DST_STATUS_ID        0x060
#define WCN_CE_DST_STATUS_MISC      0x068
#define WCN_CE_RING_HP              0x400
#define WCN_CE_RING_TP              0x404
#define WCN_CE_DST_STATUS_HP        0x408
#define WCN_CE_DST_STATUS_TP        0x40C
#define WCN_CE_RING_ENABLE          (1u << 6)
#define WCN_CE_HTC_TX_ENTRIES       16
#define WCN_CE_HTC_RX_ENTRIES       512
#define WCN_CE_WMI_RX_ENTRIES       64
#define WCN_CE_WMI_TX_ENTRIES       32
#define WCN_CE_HTT_TX_ENTRIES       2048
#define WCN_CE_RX_BUFFER_SIZE       2048
#define WCN_CE_RING_COUNT           5

/* MHI registers (drivers/bus/mhi/common.h), relative to BAR0 */
#define MHI_CFG                     0x10
#define MHI_CHDBOFF                 0x18
#define MHI_ERDBOFF                 0x20
#define MHI_BHIOFF                  0x28
#define MHI_BHIEOFF                 0x2C
#define MHI_CTRL                    0x38
#define MHI_STATUS                  0x48
#define MHI_CCABAP_LOWER            0x58
#define MHI_CCABAP_HIGHER           0x5C
#define MHI_ECABAP_LOWER            0x60
#define MHI_ECABAP_HIGHER           0x64
#define MHI_CRCBAP_LOWER            0x68
#define MHI_CRCBAP_HIGHER           0x6C
#define MHI_CRDB_LOWER              0x70
#define MHI_CTRLBASE_LOWER          0x80
#define MHI_CTRLBASE_HIGHER         0x84
#define MHI_CTRLLIMIT_LOWER         0x88
#define MHI_CTRLLIMIT_HIGHER        0x8C
#define MHI_DATABASE_LOWER          0x98
#define MHI_DATABASE_HIGHER         0x9C
#define MHI_DATALIMIT_LOWER         0xA0
#define MHI_DATALIMIT_HIGHER        0xA4

#define MHI_CTRL_RESET              0x2
#define MHI_CTRL_STATE_SHIFT        8
#define MHI_STATUS_READY            0x1
#define MHI_STATUS_SYSERR           0x4

#define BHI_INTVEC                  0x20
#define BHI_IMGADDR_LOW             0x08
#define BHI_IMGADDR_HIGH            0x0C
#define BHI_IMGSIZE                 0x10
#define BHI_IMGTXDB                 0x18
#define BHI_EXECENV                 0x28
#define BHI_STATUS                  0x2C
#define BHI_STATUS_SUCCESS          2

#define BHIE_TXVECADDR_LOW          0x2C
#define BHIE_TXVECADDR_HIGH         0x30
#define BHIE_TXVECSIZE              0x34
#define BHIE_TXVECDB                0x3C
#define BHIE_TXVECSTATUS            0x44
#define BHIE_RXVECADDR_LOW          0x60
#define BHIE_RXVECADDR_HIGH         0x64
#define BHIE_RXVECSIZE              0x68
#define BHIE_RXVECDB                0x70
#define BHIE_RXVECSTATUS            0x78
#define BHIE_STATUS_XFER_COMPL      2
#define MHI_STATE_M0                0x2
#define MHI_STATE_SYS_ERR           0xFF

#define MHI_EE_PBL                  0
#define MHI_EE_SBL                  1
#define MHI_EE_AMSS                 2
#define MHI_EE_RDDM                 3
#define MHI_EE_WFW                  4

#define MHI_PKT_STATE_CHANGE        0x20
#define MHI_PKT_CMD_COMPLETION      0x21
#define MHI_PKT_TX_EVENT            0x22
#define MHI_PKT_EE_EVENT            0x40

#define MHI_CMD_START_CHAN          18
#define MHI_EV_CC_SUCCESS           0x1
#define MHI_EV_CC_EOT               0x2
#define MHI_EV_CC_OVERFLOW          0x3
#define MHI_EV_CC_EOB               0x4
#define MHI_EV_CC_OOB               0x5
#define MHI_EV_CC_DB_MODE           0x6
#define MHI_CH_STATE_ENABLED        1

/* WCN7861 PEACH MHI/IPCR channel layout. */
#define MHI_MAX_CHANNELS            128
#define MHI_DEV_WAKE_DB             127
#define MHI_EVENT_RINGS             2
#define MHI_CTRL_EV_ELEMENTS        32
#define MHI_DATA_EV_ELEMENTS        256
#define MHI_CMD_ELEMENTS            128
#define MHI_DB_BRST_DISABLE         2
#define MHI_SBL_SIZE                (512 * 1024)
#define MHI_SEG_LEN                 (512 * 1024)
#define MHI_MAX_SEGMENTS            32
#define MHI_DMA_LIMIT               0xFFFFFFFFFULL      /* 36-bit, ATH12K_PCI_DMA_MASK */
#define MHI_RDDM_SIZE               0x780000            /* PEACH hw_params rddm_size */

/* PEACH IPCR channels (QRTR) and buffer geometry. */
#define MHI_IPCR_UL                 20
#define MHI_IPCR_DL                 21
#define MHI_CHAN_ELEMENTS           64
#define MHI_CHAN_BUF_LEN            8192
#define MHI_TRE_DWORD1_EOT          ((2UL << 16) | (1UL << 9))  /* transfer TRE, IEOT */

/*
 * PCIe0 stream IDs (sm8750.dtsi iommu-map): root port 0x1400, endpoint 01:00.0
 * 0x1401, through the PARF BDF-to-SID hash table (pcie-qcom config_sid_1_9_0).
 */
#define PARF_BDF_TO_SID_TABLE       0x2000
#define PARF_BDF_TO_SID_CFG         0x2C00
#define PARF_BDF_TO_SID_BYPASS      0x1
#define WCN_SID_BASE                0x1400
#define WCN_EP_BDF                  0x0100

/*
 * apps SMMU (MMU-500) global space 0. The UFS stream (0x60) is mapped by boot
 * firmware to a context that passes Windows physical addresses through; the
 * PCIe0 streams get the same S2CR (the s3910 touch driver does this for GPI).
 */
#define SMMU_BASE                   0x15000000
#define SMMU_IDR0                   0x020
#define SMMU_SMR(n)                 (0x800 + 4 * (n))
#define SMMU_S2CR(n)                (0xC00 + 4 * (n))
#define SMMU_SMR_VALID              0x80000000
#define SMMU_UFS_SID                0x60

/* DWC iMSI-RX, controller 0 */
#define DBI_MSI_ADDR_LO             0x820
#define DBI_MSI_ADDR_HI             0x824
#define DBI_MSI_INTR0_ENABLE        0x828
#define DBI_MSI_INTR0_MASK          0x82C
#define DBI_MSI_INTR0_STATUS        0x830

#pragma pack(push, 4)
typedef struct _MHI_CTXT {          /* event, channel and command contexts share the layout */
    ULONG   Dword0;                 /* intmod / chcfg */
    ULONG   Dword1;                 /* ertype / chtype */
    ULONG   Dword2;                 /* msivec / erindex */
    ULONG64 RBase;
    ULONG64 RLen;
    ULONG64 Rp;
    ULONG64 Wp;
} MHI_CTXT, *PMHI_CTXT;
#pragma pack(pop)
C_ASSERT (sizeof (MHI_CTXT) == 44);

typedef struct _MHI_ELEMENT {
    ULONG64 Ptr;
    ULONG   Dword[2];
} MHI_ELEMENT, *PMHI_ELEMENT;

typedef struct _WCN_DMA {
    WDFCOMMONBUFFER Buffer;
    PUCHAR          Va;
    ULONG64         La;
    SIZE_T          Length;
} WCN_DMA, *PWCN_DMA;

typedef struct _WCN_RING {
    WCN_DMA         Mem;
    ULONG           Length;
    ULONG           Rp;             /* byte offsets into Mem */
    ULONG           Wp;
    PMHI_CTXT       Ctxt;
    ULONG           Doorbell;       /* BAR0 offset */
} WCN_RING, *PWCN_RING;

typedef struct _WCN_BHIE_TABLE {
    WCN_DMA         Segment[MHI_MAX_SEGMENTS];
    WCN_DMA         Vector;
    ULONG           Segments;
} WCN_BHIE_TABLE, *PWCN_BHIE_TABLE;

typedef struct _WCN_CHAN {
    ULONG           Id;
    WCN_RING        Ring;
    WCN_DMA         Buffers;        /* MHI_CHAN_ELEMENTS slots of MHI_CHAN_BUF_LEN */
    UCHAR           Slot[MHI_CHAN_ELEMENTS];  /* buffer slot behind each TRE */
    BOOLEAN         Running;
    BOOLEAN         Completed;
    ULONG           Ccs;            /* START command completion code */
} WCN_CHAN, *PWCN_CHAN;

typedef struct _WCN_MHI {
    ULONG           Bhi;
    ULONG           Bhie;
    ULONG           ChDb;
    ULONG           ErDb;
    WCN_DMA         ChanCtxt;
    WCN_DMA         EventCtxt;
    WCN_DMA         CmdCtxt;
    WCN_RING        Event[MHI_EVENT_RINGS];
    WCN_RING        Cmd;
    WCN_CHAN        Chan[2];        /* 0 = IPCR UL (20), 1 = IPCR DL (21) */
    WCN_BHIE_TABLE  Amss;
    WCN_BHIE_TABLE  Rddm;
    BOOLEAN         Ready;
    BOOLEAN         RddmSaved;
    BOOLEAN         WakeAsserted;
    BOOLEAN         MissionMode;
    BOOLEAN         SysErr;
    ULONG           Ee;
} WCN_MHI, *PWCN_MHI;

typedef struct _WCN_QRTR {
    BOOLEAN         WlfwFound;
    ULONG           WlfwNode;
    ULONG           WlfwPort;
} WCN_QRTR, *PWCN_QRTR;

/* ath12k qmi.h ATH12K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01 */
#define WCN_QMI_MAX_MEM_SEG         52
#define WCN_QMI_TX_MAX              6200
#define WCN_QMI_MAX_DATA            6144

typedef struct _WCN_QMI_MEM {
    ULONG           Size;
    ULONG           Type;
    WCN_DMA         Dma;
} WCN_QMI_MEM, *PWCN_QMI_MEM;

typedef struct _WCN_QMI {
    BOOLEAN         Started;
    BOOLEAN         Failed;
    BOOLEAN         MemRequestPending;
    BOOLEAN         MemReadyPending;
    BOOLEAN         MemDelayed;
    ULONG           Step;
    USHORT          TxnId;
    USHORT          Outstanding;        /* message id awaiting its response, 0 = none */
    ULONG           MemSegCount;
    WCN_QMI_MEM     Mem[WCN_QMI_MAX_MEM_SEG];
    WCN_DMA         M3;
    WCN_DMA         TmeLite;
    WCN_DMA         AuxUc;
    PUCHAR          DownloadData;
    SIZE_T          DownloadSize;
    SIZE_T          DownloadOffset;
    ULONG           DownloadSegment;
    ULONG           DownloadType;
    ULONG           DownloadNextStep;
    ULONG           BoardId;
    ULONG64         FirmwareCaps;
    ULONG64         CmemBase, CmemSize;
    ULONG           TxLength;
    USHORT          TxMsgId;
    UCHAR           Tx[WCN_QMI_TX_MAX];
} WCN_QMI, *PWCN_QMI;

typedef struct _WCN_CE_RING {
    WCN_DMA         Desc;
    WCN_DMA         Status;
    WCN_DMA         Buffers;
    ULONG           Entries;
    ULONG           EntrySize;
    ULONG           Head;
    ULONG           Tail;
    ULONG           StatusHead;
    ULONG           BufferIndex;
    BOOLEAN         Source;
    BOOLEAN         Active;
    BOOLEAN         SourceMmio;
} WCN_CE_RING, *PWCN_CE_RING;

#define WCN_MAX_RADIO_CAPS 8
/* Mode-specific firmware capabilities; band index0=2.4GHz, 1=5GHz. */
typedef struct _WCN_RADIO_CAP {
    ULONG Mode,Pdev,Phy,Bands,WidthCode[2],NssRatio;
    ULONG HeMcs[2],HePpe[2][10],EhtMcs[2][4],EhtPpe[2][10];
    UCHAR HeMac[2][6],HePhy[2][11],EhtMac[2][2],EhtPhy[2][9];
    BOOLEAN EhtValid;
} WCN_RADIO_CAP;
typedef struct _WCN_CE {
    WCN_CE_RING     Ring[WCN_CE_RING_COUNT];
    WCN_DMA         Rdp;
    BOOLEAN         Initialized;
    BOOLEAN         WmiServiceReadyExt2;
    BOOLEAN         WmiReady;
    BOOLEAN         WmiInitSent;
    BOOLEAN         HwModeSent;
    BOOLEAN         HttVersionSent, HttVersionReceived;
    UCHAR           HttMajor;
    ULONG           HttSequence;
    UCHAR           HtcState;
    UCHAR           EndpointWmi;
    UCHAR           EndpointHtt;
    WCN_RADIO_CAP RadioCaps[WCN_MAX_RADIO_CAPS];ULONG RadioCapCount;
    ULONG           HtCap, VhtCap;
    ULONG Services[16];UCHAR TwtEnable[2];LONGLONG TwtEnableAt[2];
    UCHAR           NumRfChains;
    UCHAR           PeerMetadataVersion;
    UCHAR           MacAddress[6];
    BOOLEAN         SourceFailed;
    ULONG           WmiCredits, HtcCreditSize, WmiSequence;
    ULONG           HwModeStatus, HwModeIndex;
    BOOLEAN         WmiCreditInitialized, HwModeValidated;
} WCN_CE, *PWCN_CE;

/* PEACH stage-5 data path resources (RX/TX runtime is a later stage). */
#define WCN_DP_MAX_RINGS 32
#define WCN_DP_SPT_PAGES 280 /* default ath12k profile: 4*64 TX + 24 RX */
#define WCN_DP_RX_PPT_BASE 256
#define WCN_DP_RX_CHUNKS 24 /* 4096 RX + 2*1024 monitor buffers, 256/chunk */

typedef struct _WCN_DP_RING {
    WCN_DMA Desc;
    ULONG Id, Entries, EntryDwords, RegBase, PointerReg, Head;
    BOOLEAN Source, Lmac, Active;
} WCN_DP_RING, *PWCN_DP_RING;

/* Hardware conversion returns a software-cookie pointer; keep its 16-byte stride. */
typedef struct _WCN_DP_COOKIE {
    ULONG64 Reserved,BufferAddress;
} WCN_DP_COOKIE, *PWCN_DP_COOKIE;
C_ASSERT(sizeof(WCN_DP_COOKIE)==16);
C_ASSERT(FIELD_OFFSET(WCN_DP_COOKIE,BufferAddress)==8);

typedef struct _WCN_DP {
    WCN_DMA Rdp, Wrp;
    WCN_DMA LinkBank[3];
    WCN_DMA Spt[WCN_DP_SPT_PAGES];
    WCN_DMA RxBuffer[WCN_DP_RX_CHUNKS];
    WCN_DP_RING Ring[WCN_DP_MAX_RINGS];
    WDFMEMORY CookieMemory;
    PWCN_DP_COOKIE Cookies;
    ULONG RingCount;
    ULONG HttRxStep;
    BOOLEAN HttRxReady;
    BOOLEAN Initialized, ReadyForWmi, Failed;
} WCN_DP, *PWCN_DP;

/* Stage 6 is bounded passive discovery only. No association or TX data. */
#define WCN_SCAN_MAX_RULES 64
#define WCN_SCAN_MAX_CHANNELS 64
#define WCN_SCAN_MAX_APS 128

typedef struct _WCN_SCAN_RULE {
    ULONG Start, End, MaxBw, Power, Gain, Flags;
} WCN_SCAN_RULE, *PWCN_SCAN_RULE;
typedef struct _WCN_SCAN_CHANNEL {
    ULONG Frequency, Power, Gain, Flags;
} WCN_SCAN_CHANNEL, *PWCN_SCAN_CHANNEL;
typedef struct _WCN_SCAN_AP {
    UCHAR Bssid[6];
    ULONG Frequency;
} WCN_SCAN_AP, *PWCN_SCAN_AP;
typedef struct _WCN_SCAN_TARGET {
    UCHAR Ssid[32],SsidLength,Bssid[6];BOOLEAN Active;
    ULONG IeLength,DwellTime,Flags;UCHAR Ies[768];
} WCN_SCAN_TARGET;
typedef struct _WCN_SCAN {
WCN_SCAN_RULE Rule[WCN_SCAN_MAX_RULES];
    WCN_SCAN_CHANNEL Channel[WCN_SCAN_MAX_CHANNELS];
    WCN_SCAN_AP Ap[WCN_SCAN_MAX_APS];
    ULONG Tx[512];
    ULONG State, RuleCount, Rules2g, ChannelCount, ApCount;
    ULONG MaxBw2g,MaxBw5g,PhyBitmap;
    ULONG CountryGeneration, ForeignChannels, ScanReason, ScanId;
    ULONG AbortAttempts;
BOOLEAN TxMaskSent, RxMaskSent, NssSent;
    UCHAR Alpha2[2];
    BOOLEAN CountrySent, CountryReady, ChannelListSent, VdevSubmitted;
    BOOLEAN ScanSent, ScanStarted, ScanCompleted, Passed, Failed, AbortSent, RuntimeScan;
    ULONG ScanVdev,RequestedCount,RequestedFrequency[WCN_SCAN_MAX_CHANNELS];
    WCN_SCAN_TARGET Target;
    LONGLONG StateAt, StartedAt, ScanAt, CompletedAt;
} WCN_SCAN, *PWCN_SCAN;

/* Firmware station state. Windows owns credentials and the IP stack. */
typedef struct _WCN_JOIN_AP {
    UCHAR Bssid[6], Rates[16], RateCount;
    ULONG Frequency, ChannelFlags, Power, Gain;
    USHORT Capability, BeaconInterval;
    LONG Rssi;
    BOOLEAN Valid, Pmf, H2eOnly, Qos, Ht, Vht, Sgi;
    UCHAR Nss, Width, HtRateCount, HtRates[16], HtCap[26], VhtCap[12], Edca[16];
    ULONG Auth, CenterFrequency, PhyMode, RateKbps;
    BOOLEAN He,Eht,Dfs;UCHAR HeLength,EhtLength,HeCap[64],EhtCap[96];
    USHORT HeMcs[2],Punctured;ULONG HeOps;
    UCHAR EhtMcs[2][3];
    USHORT VhtMcs;UCHAR Akm,FtMode;BOOLEAN TdlsProhibited,TdlsSwitchProhibited,Tdls,HasPmkid;UCHAR Pmkid[16];
} WCN_JOIN_AP, *PWCN_JOIN_AP;
typedef struct _WCN_BA {
    UCHAR Phase, Token, Attempts;BOOLEAN Active, Stop,Amsdu;
    USHORT Window, Ssn, Status;
    LONGLONG At;
} WCN_BA;
typedef struct _WCN_IGTK {UCHAR Key[16],Previous[16];ULONG64 Pn;BOOLEAN Valid,Known,PreviousKnown;} WCN_IGTK;
typedef struct _WCN_CIPHER_KEY {UCHAR Key[16],Previous[16];ULONG64 Floor;BOOLEAN Known,PreviousKnown,Installed;} WCN_CIPHER_KEY;
typedef struct _WCN_KEY_UPDATE {UCHAR Slot,Key[16],Rsc[6],Tid;BOOLEAN Delete,Sent,Ack,ReoSent,ResetPn;LONGLONG At;} WCN_KEY_UPDATE;
typedef struct _WCN_RUNTIME {
    BOOLEAN Ready,ReoPending,ReoDone;USHORT ReoCmd,ReoTid,ReoTag;
    ULONG TxCompleted,TxAcked,RuntimeErrors,RxDropped,ReoStatus,ReoOwner;ULONG64 ReoBase;
    ULONG RxPending[4096],RxPendingHead,RxPendingTail;UCHAR RxPosted[4096],TxUsed[WCN_TX_SLOTS];WCN_DMA TxDma;
} WCN_RUNTIME,*PWCN_RUNTIME;
typedef struct _WCN_JOIN {
    ULONG Vdev;UCHAR Mac[6],Infrastructure[6];BOOLEAN Created,Tdls;
    BOOLEAN Failed, Started, VdevStarted;
    BOOLEAN Authenticated, Associated, AssocConfirmed, PeerCreated, RuntimeReady;
    BOOLEAN MgmtBusy, PairwiseInstalled, GroupInstalled, Authorized;
    UCHAR Ssid[32], SsidLength,PreviousBssid[6];BOOLEAN Reassociate;
    PVOID PmfAlgorithm;WCN_IGTK Igtk[2];ULONG64 MgmtRxPn,MgmtTxPn;BOOLEAN PmfConfigured,H2e,SaQueryPending;UCHAR SaQueryId[2];
    WCN_JOIN_AP Ap;
    ULONG State, Attempts, Aid, Sequence, MgmtId, MgmtStatus, MacId,NssLimit;
    ULONG RxFrames,TxFrames;
    ULONG TxMcs,RxMcs;LONGLONG PeerSeenAt;
    ULONG BadEvents,PeerId,AstIndex,AstHash,ReoSetupTid;
    BOOLEAN PeerMapped, RoutingSet;
    WCN_BA Ba[16];
    BOOLEAN WmmSet,BeamformSet,AggregationSet,KeyHold;
    WCN_CIPHER_KEY Cipher[5];WCN_KEY_UPDATE KeyUpdate;
    UCHAR TwtState;ULONG Traffic;LONGLONG TrafficAt,TrafficBusyAt,TwtAt;
    UCHAR CsaState,CsaStep;BOOLEAN CsaAck,CsaBlock;WCN_JOIN_AP NextChannel;LONGLONG CsaAt,CsaDeadline;

    ULONG64 RxPn[5][17];
    UCHAR RxPnValid[5][17];
    ULONG Wire[512];
    WCN_DMA MgmtDma,ReoQueue;
    ULONG ReoReleaseStep;LONGLONG ReoReleaseAt;BOOLEAN Resetting,PeerDeleteConfirmed,PeerUnmapped,VdevStopConfirmed;
    LONGLONG StartedAt, StateAt, LastSendAt, MgmtAt,VdevAt;
} WCN_JOIN, *PWCN_JOIN;

typedef struct _WCN_DEVICE {
    PVOID Stage8,SecondaryStage8,RoleStage8,Hotspot,Tdls,P2p;
    BOOLEAN P2pClient;
    ULONG MgmtSequence,MgmtInUse[4];
    WDFDEVICE       Device;
    WDFINTERRUPT    Interrupt;
    WDFDMAENABLER   DmaEnabler;
    PUCHAR          Bar;
    ULONG           BarLength;
    PUCHAR          Config;
    PUCHAR          Dbi;
    PUCHAR          Parf;
    ULONG           ParfLength;
    ULONG           ConfigLength;
    ULONG           DbiLength;

    KSPIN_LOCK      WindowLock;
    ULONG           Window;
    BOOLEAN         MultiMsi;

    KEVENT          WorkEvent;
    PKTHREAD        Thread;
    volatile BOOLEAN Stop;

    PUCHAR          Firmware;
    SIZE_T          FirmwareSize;

    WCN_MHI         Mhi;
    WCN_QRTR        Qrtr;
    WCN_QMI         Qmi;
    WCN_CE          Ce;
    WCN_DP          Dp;
    WCN_SCAN        Scan;
    WCN_JOIN        Join,Secondary,P2pJoin;
    WCN_RUNTIME Runtime;
} WCN_DEVICE, *PWCN_DEVICE;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME (WCN_DEVICE, WcnGetDevice)
static __inline PWCN_JOIN WcnStation(PWCN_DEVICE D,ULONG Vdev)
{return Vdev==0?&D->Join:Vdev==2?&D->Secondary:Vdev==1&&D->P2pClient?&D->P2pJoin:NULL;}
static __inline UCHAR WcnTxOwner(ULONG Vdev){return (UCHAR)(Vdev==1?0x80:Vdev==2?0x40:0);}
static __inline ULONG WcnOwnerVdev(UCHAR Owner){return (Owner&0x80)?1u:(Owner&0x40)?2u:0u;}

/* TDLS owns its handshake/keys and per-peer queues, never an OS AP key. */
NTSTATUS WcnTdlsCreate(PWCN_DEVICE D);
VOID WcnTdlsDestroy(PWCN_DEVICE D);
VOID WcnTdlsPowerDown(PWCN_DEVICE D);
VOID WcnTdlsService(PWCN_DEVICE D);
BOOLEAN WcnTdlsSupported(PWCN_DEVICE D);
ULONG WcnTdlsCapabilities(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *Out);
NTSTATUS WcnTdlsAwake(PWCN_DEVICE D,PWCN_JOIN J);
NTSTATUS WcnTdlsPause(PWCN_DEVICE D,PWCN_JOIN J);
VOID WcnTdlsObserve(PWCN_JOIN J,const UCHAR *Ies,ULONG Length);
BOOLEAN WcnTdlsBusy(PWCN_DEVICE D,PWCN_JOIN J);
PWCN_JOIN WcnTdlsPeer(PWCN_DEVICE D,ULONG Vdev,const UCHAR Peer[6]);
NTSTATUS WcnTdlsStop(PWCN_DEVICE D,PWCN_JOIN J);
NTSTATUS WcnTdlsTx(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N);
BOOLEAN WcnTdlsTunnel(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N);
BOOLEAN WcnTdlsRx(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Buffer,ULONG N,ULONG Pad,BOOLEAN Error,const ULONG *Desc);
VOID WcnTdlsWmi(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N);
VOID WcnTdlsHtt(PWCN_DEVICE D,const UCHAR *P,ULONG N);
NTSTATUS WcnCmac(PVOID Algorithm,const UCHAR Key[16],const UCHAR *Data,ULONG Length,UCHAR Mac[16]);
VOID WcnStage7PeerReceive(PWCN_DEVICE D,PWCN_JOIN J,ULONG Event,const UCHAR *P,ULONG N);
VOID WcnStage7PeerHtt(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N);
NTSTATUS WcnStage7PeerQueues(PWCN_DEVICE D,PWCN_JOIN J);
NTSTATUS WcnStage7PublicAction(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR Peer[6],const UCHAR *Body,ULONG N);
ULONG WcnStage7RxReplay(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Buffer,const ULONG *Desc,BOOLEAN Error,ULONG Fc,const UCHAR *F);
VOID WcnStage8DirectEthernet(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *F,ULONG N);
/* Firmware feature gates are actual service bits, not chip-name assumptions. */
BOOLEAN WcnService(PWCN_DEVICE D,ULONG Service);
NTSTATUS WcnVdevParam(PWCN_DEVICE D,ULONG Vdev,ULONG Param,ULONG Value);
NTSTATUS WcnPeerParam(PWCN_DEVICE D,ULONG Vdev,const UCHAR Peer[6],ULONG Param,ULONG Value);
NTSTATUS WcnStationPower(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Enable);
NTSTATUS WcnStage8DeleteKey(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Group,UCHAR Index);
NTSTATUS WcnStationKeysService(PWCN_DEVICE D,PWCN_JOIN J);
VOID WcnStationKeyComplete(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N);
VOID WcnStationKeysClear(PWCN_JOIN J);
NTSTATUS WcnReoKeyUpdate(PWCN_DEVICE D,PWCN_JOIN J,ULONG Tid,BOOLEAN Enable);
BOOLEAN WcnStage7RxIdle(PWCN_DEVICE D);
NTSTATUS WcnAggregationConfigure(PWCN_DEVICE D,ULONG Vdev);
VOID WcnTwtService(PWCN_DEVICE D,PWCN_JOIN J);
NTSTATUS WcnTwtStop(PWCN_DEVICE D,PWCN_JOIN J);
NTSTATUS WcnStage7ChannelCommand(PWCN_DEVICE D,PWCN_JOIN J,BOOLEAN Up);
VOID WcnTwtEvent(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N);
BOOLEAN WcnChannelService(PWCN_DEVICE D,PWCN_JOIN J);
BOOLEAN WcnChannelEvent(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *P,ULONG N);
VOID WcnStage8ChannelChanged(PWCN_DEVICE D,PWCN_JOIN J);
VOID WcnStage8FtService(PWCN_DEVICE D,PWCN_JOIN J);
VOID WcnStage8FtReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Frame,ULONG Length);
ULONG WcnStage8FtIes(PWCN_DEVICE D,PWCN_JOIN J,UCHAR *Out);
VOID WcnStage8RoamEvent(PWCN_DEVICE D,ULONG Event,const UCHAR *P,ULONG N);
VOID WcnStage8ActionReceive(PWCN_DEVICE D,PWCN_JOIN J,const UCHAR *Body,ULONG Length);

/* driver.c */
VOID WcnLog (_In_ PWCN_DEVICE Dev, _In_z_ _Printf_format_string_ PCSTR Format, ...);
VOID WcnSleep (_In_ ULONG Milliseconds);
BOOLEAN WcnWait (_In_ PWCN_DEVICE Dev, _In_ ULONG Milliseconds);
NTSTATUS WcnReadFile (_In_ PCWSTR Path, _Outptr_ PUCHAR *Data, _Out_ SIZE_T *Size);
NTSTATUS WcnDmaAlloc (_In_ PWCN_DEVICE Dev, _In_ SIZE_T Length, _Out_ PWCN_DMA Dma);
NTSTATUS WcnDmaAllocAligned (_In_ PWCN_DEVICE Dev, _In_ SIZE_T Length, _In_ ULONG AlignmentMask, _Out_ PWCN_DMA Dma);
LONGLONG WcnNowMs (VOID);

/* pcie.c */
ULONG WcnRead (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset);
VOID WcnWrite (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset, _In_ ULONG Value);
ULONG WcnBarRead (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset);
VOID WcnBarWrite (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset, _In_ ULONG Value);
ULONG WcnCfgRead (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset);
VOID WcnCfgWrite (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset, _In_ ULONG Value);
ULONG WcnDbiRead (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset);
VOID WcnDbiWrite (_In_ PWCN_DEVICE Dev, _In_ ULONG Offset, _In_ ULONG Value);
NTSTATUS WcnPcieSetup (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnPcieConfigSid (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnSmmuSetup (VOID);
VOID WcnPcieSwReset (_In_ PWCN_DEVICE Dev, _In_ BOOLEAN PowerOn);
VOID WcnPcieMsiEnable (_In_ PWCN_DEVICE Dev, _In_ BOOLEAN Enable);

/* mhi.c */
NTSTATUS WcnMhiPrepare (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnMhiPowerUp (_In_ PWCN_DEVICE Dev);
VOID WcnMhiPowerDown (_In_ PWCN_DEVICE Dev);
VOID WcnMhiService (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnMhiStartChannels (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnMhiSend (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (HeaderLength) const VOID *Header, _In_ ULONG HeaderLength,
                     _In_reads_bytes_opt_ (PayloadLength) const VOID *Payload, _In_ ULONG PayloadLength);

/* qrtr.c */
VOID WcnQrtrStart (_In_ PWCN_DEVICE Dev);
VOID WcnQrtrReceive (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (Length) const UCHAR *Data, _In_ ULONG Length);
NTSTATUS WcnQrtrSendToService (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (Size) const VOID *Data, _In_ ULONG Size);

/* qmi.c */
VOID WcnQmiServiceArrive (_In_ PWCN_DEVICE Dev);
VOID WcnQmiReceive (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (Size) const UCHAR *Data, _In_ ULONG Size);

/* ce.c */
NTSTATUS WcnCeInitialize (_In_ PWCN_DEVICE Dev);
VOID WcnCeService (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnCeSendHtc (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (Length) const UCHAR *Data, _In_ ULONG Length);
VOID WcnWmiReceive (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (Length) const UCHAR *Data, _In_ ULONG Length);

/* dp.c: PEACH/QCC2072 hardware tables, not WCN7850 register offsets. */
NTSTATUS WcnDpInitialize (_In_ PWCN_DEVICE Dev);
NTSTATUS WcnDpPrepareWmi (_In_ PWCN_DEVICE Dev);
VOID WcnDpCleanup (_In_ PWCN_DEVICE Dev); /* only after endpoint DMA is stopped */

/* Stage 6, worker-thread serialized command submission and event parsing. */
NTSTATUS WcnWmiSend (_In_ PWCN_DEVICE Dev, _In_ ULONG Command, _In_reads_bytes_ (Length) const VOID *Body, _In_ ULONG Length);
VOID WcnStage6Service (_In_ PWCN_DEVICE Dev);
VOID WcnStage6Receive (_In_ PWCN_DEVICE Dev, _In_ ULONG Event, _In_reads_bytes_ (Length) const UCHAR *Payload, _In_ ULONG Length);

NTSTATUS WcnHttSend (_In_ PWCN_DEVICE Dev, _In_reads_bytes_ (Length) const VOID *Body, _In_ ULONG Length);
NTSTATUS WcnDpHttRxPrepare (_In_ PWCN_DEVICE Dev);

NTSTATUS WcnStage6Rescan(PWCN_DEVICE Dev,ULONG Vdev,const ULONG *Frequency,ULONG Count,const WCN_SCAN_TARGET *Target);
NTSTATUS WcnStage6CancelRescan(PWCN_DEVICE Dev);
VOID WcnStage7Cleanup(PWCN_DEVICE Dev);
VOID WcnStage7Service(PWCN_DEVICE Dev);
VOID WcnStage7Receive(PWCN_DEVICE Dev, ULONG Event, const UCHAR *Payload, ULONG Length);
VOID WcnStage7Fail(PWCN_DEVICE Dev,PWCN_JOIN J, PCSTR Reason);
NTSTATUS WcnStage7TeardownCommand(PWCN_DEVICE Dev,PWCN_JOIN J,ULONG Step);
NTSTATUS WcnStage7DpInit(PWCN_DEVICE Dev,PWCN_JOIN J);
VOID WcnStage7DpPoll(PWCN_DEVICE Dev);
/* The passive OS queue worker flushes its successful submissions once per batch. */
ULONG WcnMgmtAllocate(PWCN_DEVICE Dev,ULONG Owner);
NTSTATUS WcnStage7Tx(PWCN_DEVICE Dev,PWCN_JOIN J, const UCHAR *Frame, ULONG Length, BOOLEAN ClearEapol);
NTSTATUS WcnDataTx(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,BOOLEAN Eapol,ULONG Vdev,ULONG MacId,const UCHAR Local[6],const UCHAR Receiver[6],ULONG Ast,ULONG Hash,BOOLEAN LookupOverride);
VOID WcnStage7TxFlush(PWCN_DEVICE Dev);

VOID WcnStage7Htt(PWCN_DEVICE Dev,const UCHAR *Payload,ULONG Length);

#include "stage8-bridge.h"
#include "p2p.h"

BOOLEAN WcnStage7DataIdle(PWCN_DEVICE Dev,PWCN_JOIN J);

VOID WcnCapabilitiesEvent(PWCN_DEVICE Dev,ULONG Event,const UCHAR *Data,ULONG Length);
BOOLEAN WcnJoinCandidate(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,ULONG Frequency,LONG Rssi,const UCHAR *Ssid,ULONG SsidLength,ULONG Auth,BOOLEAN Pmf,UCHAR Akm,BOOLEAN Hidden,PWCN_JOIN_AP Out);

struct wmi_peer_assoc_complete_cmd;
const WCN_RADIO_CAP *WcnRadioCaps(PWCN_DEVICE Dev);
ULONG WcnMaximumRate(PWCN_DEVICE Dev);
BOOLEAN WcnPhyAdvancedSelect(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,PWCN_JOIN_AP Ap);
ULONG WcnPhyAdvancedRate(const WCN_JOIN_AP *Ap);
NTSTATUS WcnPhyBeamform(PWCN_DEVICE Dev,PWCN_JOIN_AP A,ULONG Vdev);
ULONG WcnPhyMode(const WCN_JOIN_AP *A);
ULONG WcnPhyRate(PWCN_JOIN_AP A);
ULONG WcnPhyAdvancedIes(PWCN_DEVICE Dev,PWCN_JOIN_AP A,UCHAR *Out);
VOID WcnPhyAdvancedPeer(PWCN_DEVICE Dev,PWCN_JOIN_AP A,struct wmi_peer_assoc_complete_cmd *Cmd);
ULONG WcnPhyAdvancedRates(PWCN_DEVICE Dev,PWCN_JOIN_AP A,ULONG *Out);
BOOLEAN WcnWidthAllowed(PWCN_DEVICE Dev,ULONG Center,ULONG Width);
BOOLEAN WcnPhyChannelChanged(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Frame,ULONG Length,ULONG BodyOffset);
BOOLEAN WcnPhySelect(PWCN_DEVICE Dev,const UCHAR *Frame,ULONG Length,PWCN_JOIN_AP Ap);
ULONG WcnPhyBuildIes(PWCN_DEVICE Dev,PWCN_JOIN_AP A,UCHAR *Out,BOOLEAN Ap);
ULONG WcnPhyOperationIes(PWCN_JOIN_AP A,UCHAR *Out);
NTSTATUS WcnPhyPeer(PWCN_DEVICE Dev,PWCN_JOIN_AP A,const UCHAR Peer[6],ULONG Vdev,ULONG Aid,ULONG Listen,ULONG *Wire);
NTSTATUS WcnPhyWmm(PWCN_DEVICE Dev,PWCN_JOIN_AP A,ULONG Vdev);
ULONG WcnPhyScore(const WCN_JOIN_AP *Ap);
VOID WcnPhyLimitNss(PWCN_DEVICE Dev,PWCN_JOIN J,ULONG Nss);
VOID WcnAggregationReceive(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Body,ULONG Length);
VOID WcnAggregationService(PWCN_DEVICE Dev,PWCN_JOIN J);
NTSTATUS WcnStage7ReoRelease(PWCN_DEVICE D,PWCN_JOIN J);
NTSTATUS WcnReoBaUpdate(PWCN_DEVICE Dev,ULONG Owner,ULONG64 Reo,ULONG Tid,ULONG Window,ULONG Ssn);
VOID WcnAggregationReceivePeer(PWCN_DEVICE D,PWCN_JOIN_AP Peer,WCN_BA *Ba,const UCHAR *B,ULONG N);
VOID WcnAggregationServicePeer(PWCN_DEVICE D,ULONG Vdev,PWCN_JOIN_AP Peer,WCN_BA *Ba,const UCHAR Mac[6],ULONG64 Reo,BOOLEAN Busy,ULONG Status);
NTSTATUS WcnStage7ActionSend(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Body,ULONG Length);
NTSTATUS WcnPmfInit(PWCN_DEVICE Dev,PWCN_JOIN J);
VOID WcnPmfClear(PWCN_DEVICE Dev,PWCN_JOIN J);
NTSTATUS WcnPmfInstall(PWCN_DEVICE Dev,PWCN_JOIN J,UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6]);
BOOLEAN WcnPmfReceive(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Frame,ULONG Length,ULONG *BodyOffset);
NTSTATUS WcnPmfInstallKey(WCN_IGTK Keys[2],UCHAR Index,const UCHAR Key[16],const UCHAR Rsc[6]);
BOOLEAN WcnPmfReceiveFrame(PVOID Algorithm,WCN_IGTK Keys[2],ULONG64 *RxPn,const UCHAR *Frame,ULONG Length,ULONG *BodyOffset);
NTSTATUS WcnPmfTxUnicast(UCHAR *Frame,ULONG *Length,ULONG Capacity,ULONG64 *Pn);
NTSTATUS WcnPmfTxGroup(PVOID Algorithm,WCN_IGTK Keys[2],UCHAR Index,UCHAR *Frame,ULONG *Length,ULONG Capacity);
NTSTATUS WcnStage7AuthSend(PWCN_DEVICE Dev,PWCN_JOIN J,const UCHAR *Body,ULONG Length);

VOID WcnStage7FailAll(PWCN_DEVICE Dev,PCSTR Reason);
NTSTATUS WcnStage7EnsureVdev(PWCN_DEVICE D,PWCN_JOIN J);
