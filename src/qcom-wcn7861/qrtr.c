/*
 * QRTR (IPC router) over the IPCR MHI channels, following net/qrtr
 * (af_qrtr.c, ns.c, mhi.c).
 *
 * Once channel 21 has receive buffers the firmware says HELLO. The host
 * answers the way the Linux name service does (ns.c ctrl_cmd_hello ->
 * say_hello), and the firmware then announces its services with NEW_SERVER.
 * WLFW traffic is handed to the QMI client in qmi.c.
 */
#include "wcn7861.h"

#define QRTR_PROTO_VER_1        1
#define QRTR_PROTO_VER_2        3

#define QRTR_TYPE_DATA          1
#define QRTR_TYPE_HELLO         2
#define QRTR_TYPE_BYE           3
#define QRTR_TYPE_NEW_SERVER    4
#define QRTR_TYPE_DEL_SERVER    5
#define QRTR_TYPE_DEL_CLIENT    6
#define QRTR_TYPE_RESUME_TX     7

#define QRTR_PORT_CTRL          0xFFFFFFFE
#define QRTR_PORT_CTRL_LEGACY   0xFFFF
#define QRTR_LOCAL_NODE         1           /* af_qrtr.c qrtr_local_nid */
#define QRTR_CLIENT_PORT        0x4000      /* first ephemeral port */

#define QMI_WLFW_SERVICE        0x45        /* ATH12K_QMI_WLFW_SERVICE_ID_V01 */

#pragma pack(push, 1)
typedef struct _QRTR_HDR_V1 {
    ULONG   Version;
    ULONG   Type;
    ULONG   SrcNode;
    ULONG   SrcPort;
    ULONG   ConfirmRx;
    ULONG   Size;
    ULONG   DstNode;
    ULONG   DstPort;
} QRTR_HDR_V1;

typedef struct _QRTR_HDR_V2 {
    UCHAR   Version;
    UCHAR   Type;
    UCHAR   Flags;
    UCHAR   OptLen;
    ULONG   Size;
    USHORT  SrcNode;
    USHORT  SrcPort;
    USHORT  DstNode;
    USHORT  DstPort;
} QRTR_HDR_V2;

/* qrtr_ctrl_pkt: server = service, instance, node, port; client = node, port */
typedef struct _QRTR_CTRL {
    ULONG   Cmd;
    ULONG   Arg[4];
} QRTR_CTRL;
#pragma pack(pop)

C_ASSERT (sizeof (QRTR_HDR_V1) == 32);
C_ASSERT (sizeof (QRTR_HDR_V2) == 16);
C_ASSERT (sizeof (QRTR_CTRL) == 20);

/* af_qrtr.c qrtr_node_enqueue: always a v1 header */
static
NTSTATUS
QrtrSend (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Type,
    _In_ ULONG SrcPort,
    _In_ ULONG DstNode,
    _In_ ULONG DstPort,
    _In_reads_bytes_ (Size) const VOID *Payload,
    _In_ ULONG Size
    )
{
    QRTR_HDR_V1 Hdr;
    NTSTATUS Status;

    Hdr.Version = QRTR_PROTO_VER_1;
    Hdr.Type = Type;
    Hdr.SrcNode = QRTR_LOCAL_NODE;
    Hdr.SrcPort = SrcPort;
    Hdr.ConfirmRx = 0;
    Hdr.Size = Size;
    Hdr.DstNode = DstNode;
    Hdr.DstPort = DstPort;
    Status = WcnMhiSend (Dev, &Hdr, sizeof (Hdr), Payload, Size);
    WcnLog (Dev, "qrtr tx: type %u %x:%x -> %x:%x size %u, status 0x%08x",
            Type, QRTR_LOCAL_NODE, SrcPort, DstNode, DstPort, Size, Status);
    return Status;
}

/* ns.c say_hello */
static
VOID
QrtrSayHello (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG Node
    )
{
    QRTR_CTRL Ctrl;

    RtlZeroMemory (&Ctrl, sizeof (Ctrl));
    Ctrl.Cmd = QRTR_TYPE_HELLO;
    QrtrSend (Dev, QRTR_TYPE_HELLO, QRTR_PORT_CTRL, Node, QRTR_PORT_CTRL, &Ctrl, sizeof (Ctrl));
}

/* af_qrtr.c qrtr_send_resume_tx */
static
VOID
QrtrResumeTx (
    _In_ PWCN_DEVICE Dev,
    _In_ ULONG LocalPort,
    _In_ ULONG RemoteNode,
    _In_ ULONG RemotePort
    )
{
    QRTR_CTRL Ctrl;

    RtlZeroMemory (&Ctrl, sizeof (Ctrl));
    Ctrl.Cmd = QRTR_TYPE_RESUME_TX;
    Ctrl.Arg[0] = QRTR_LOCAL_NODE;
    Ctrl.Arg[1] = LocalPort;
    QrtrSend (Dev, QRTR_TYPE_RESUME_TX, LocalPort, RemoteNode, RemotePort, &Ctrl, sizeof (Ctrl));
}

/* A QMI message from the client port to the WLFW service. */
NTSTATUS
WcnQrtrSendToService (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Size) const VOID *Data,
    _In_ ULONG Size
    )
{
    if (!Dev->Qrtr.WlfwFound) {
        return STATUS_DEVICE_NOT_READY;
    }
    return QrtrSend (Dev, QRTR_TYPE_DATA, QRTR_CLIENT_PORT, Dev->Qrtr.WlfwNode, Dev->Qrtr.WlfwPort, Data, Size);
}

VOID
WcnQrtrStart (
    _In_ PWCN_DEVICE Dev
    )
{
    RtlZeroMemory (&Dev->Qrtr, sizeof (Dev->Qrtr));
    Dev->Qrtr.StartMs = WcnNowMs ();
    WcnLog (Dev, "qrtr: local node %u, client port 0x%x, waiting for the firmware HELLO", QRTR_LOCAL_NODE, QRTR_CLIENT_PORT);
}

/* af_qrtr.c qrtr_endpoint_post + the ns.c control handling the host needs */
VOID
WcnQrtrReceive (
    _In_ PWCN_DEVICE Dev,
    _In_reads_bytes_ (Length) const UCHAR *Data,
    _In_ ULONG Length
    )
{
    PWCN_QRTR Qrtr = &Dev->Qrtr;
    const QRTR_CTRL *Ctrl;
    const UCHAR *Payload;
    ULONG Version;
    ULONG Type;
    ULONG SrcNode;
    ULONG SrcPort;
    ULONG DstNode;
    ULONG DstPort;
    ULONG Size;
    ULONG HdrLength;
    BOOLEAN ConfirmRx;

    Qrtr->Received++;
    if (Length < sizeof (QRTR_HDR_V2) || (Length & 3) != 0) {
        WcnLog (Dev, "qrtr rx: %u byte packet ignored", Length);
        WcnLogHex (Dev, "qrtr rx:   ", Data, Length);
        return;
    }

    Version = Data[0];
    if (Version == QRTR_PROTO_VER_1 && Length >= sizeof (QRTR_HDR_V1)) {
        const QRTR_HDR_V1 *Hdr = (const QRTR_HDR_V1 *)Data;

        HdrLength = sizeof (QRTR_HDR_V1);
        Type = Hdr->Type;
        SrcNode = Hdr->SrcNode;
        SrcPort = Hdr->SrcPort;
        DstNode = Hdr->DstNode;
        DstPort = Hdr->DstPort;
        ConfirmRx = Hdr->ConfirmRx != 0;
        Size = Hdr->Size;
    } else if (Version == QRTR_PROTO_VER_2) {
        const QRTR_HDR_V2 *Hdr = (const QRTR_HDR_V2 *)Data;

        HdrLength = sizeof (QRTR_HDR_V2) + Hdr->OptLen;
        Type = Hdr->Type;
        SrcNode = Hdr->SrcNode;
        SrcPort = (Hdr->SrcPort == 0xFFFE) ? QRTR_PORT_CTRL : Hdr->SrcPort;
        DstNode = Hdr->DstNode;
        DstPort = (Hdr->DstPort == 0xFFFE) ? QRTR_PORT_CTRL : Hdr->DstPort;
        ConfirmRx = (Hdr->Flags & 1) != 0;
        Size = Hdr->Size;
    } else {
        WcnLog (Dev, "qrtr rx: unknown version %u, %u bytes", Version, Length);
        WcnLogHex (Dev, "qrtr rx:   ", Data, Length);
        return;
    }
    if (DstPort == QRTR_PORT_CTRL_LEGACY) {
        DstPort = QRTR_PORT_CTRL;
    }

    WcnLog (Dev, "qrtr rx: v%u type %u %x:%x -> %x:%x size %u%s",
            Version, Type, SrcNode, SrcPort, DstNode, DstPort, Size, ConfirmRx ? " confirm_rx" : "");
    if (Size == 0 || HdrLength + Size > Length) {
        WcnLog (Dev, "qrtr rx: size %u does not fit the %u byte packet", Size, Length);
        WcnLogHex (Dev, "qrtr rx:   ", Data, Length);
        return;
    }
    Payload = Data + HdrLength;
    Ctrl = (const QRTR_CTRL *)Payload;

    switch (Type) {
    case QRTR_TYPE_HELLO:
        Qrtr->HelloSeen = TRUE;
        Qrtr->RemoteNode = SrcNode;
        QrtrSayHello (Dev, SrcNode);
        break;

    case QRTR_TYPE_NEW_SERVER:
    case QRTR_TYPE_DEL_SERVER:
        if (Size < sizeof (QRTR_CTRL)) {
            break;
        }
        WcnLog (Dev, "qrtr: %s service 0x%x instance 0x%x (version %u, instance %u) at %x:%x",
                (Type == QRTR_TYPE_NEW_SERVER) ? "new server" : "del server",
                Ctrl->Arg[0], Ctrl->Arg[1], Ctrl->Arg[1] & 0xFF, Ctrl->Arg[1] >> 8, Ctrl->Arg[2], Ctrl->Arg[3]);
        if (Type == QRTR_TYPE_NEW_SERVER && Ctrl->Arg[0] == QMI_WLFW_SERVICE && !Qrtr->WlfwFound) {
            Qrtr->WlfwFound = TRUE;
            Qrtr->WlfwNode = Ctrl->Arg[2];
            Qrtr->WlfwPort = Ctrl->Arg[3];
            WcnQmiServiceArrive (Dev);
        }
        break;

    case QRTR_TYPE_DATA:
        if (DstPort == QRTR_CLIENT_PORT) {
            WcnQmiReceive (Dev, Payload, Size);
        } else {
            WcnLogHex (Dev, "qrtr rx:   ", Payload, Size);
        }
        if (ConfirmRx) {
            QrtrResumeTx (Dev, DstPort, SrcNode, SrcPort);
        }
        break;

    default:
        WcnLogHex (Dev, "qrtr rx:   ", Payload, Size);
        break;
    }
}
