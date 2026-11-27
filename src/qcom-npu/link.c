#include "npu.h"

#define SMEM_CANARY 0xa5a5u
// qcom,smem is ordered inbound, outbound in both OEM and mainline SMP2P.
#define SMP2P_IN 94u
#define SMP2P_OUT 432u
#define SMP2P_SIZE 0x154u
#define SMP2P_HEADER 20u
#define GLINK_DESC 478u
#define GLINK_TX 479u
#define GLINK_RX 480u
#define VERSION 0u
#define VERSION_ACK 1u
#define OPEN 2u
#define CLOSE 3u
#define OPEN_ACK 4u
#define INTENT 5u
#define RX_DONE 6u
#define INTENT_REQ 7u
#define INTENT_REQ_ACK 8u
#define TX_DATA 9u
#define CLOSE_ACK 11u
#define TX_CONT 12u
#define READ_NOTIF 13u
#define RX_REUSE 14u

static USHORT Read16(PUCHAR base, ULONG offset) {
    return READ_REGISTER_USHORT((PUSHORT)(base + offset));
}
static VOID Write16(PUCHAR base, ULONG offset, USHORT value) {
    WRITE_REGISTER_USHORT((PUSHORT)(base + offset), value);
}
static VOID Kick(NPU *n, ULONG signal) {
    KeMemoryBarrier();
    NpuWrite(n->Ipcc, 12, (6u << 16) | signal); // IPCC_CLIENT_CDSP
}
static PUCHAR FindPartition(PUCHAR base, ULONG bytes, ULONG cacheline, ULONG item, PULONG size) {
    ULONG free = NpuRead(base, 12);
    if (free < 32 || free > bytes) return NULL;
    for (ULONG at = 32; at <= free - 16;) {
        PUCHAR p = base + at;
        ULONG length = NpuRead(p, 4);
        USHORT padding = Read16(p, 8), header = Read16(p, 10);
        if (Read16(p, 0) != SMEM_CANARY || padding > length ||
            header > free - at - 16 || length > free - at - 16 - header) return NULL;
        if (Read16(p, 2) == item) {
            *size = length - padding;
            return p + 16 + header;
        }
        at += 16 + header + length;
    }
    // Cached SMEM entries grow backwards; their data precedes their headers.
    ULONG cached = NpuRead(base, 16);
    if (cached < free || cached > bytes || !cacheline) return NULL;
    ULONG headerSize = (16 + cacheline - 1) & ~(cacheline - 1u);
    if (headerSize > bytes) return NULL;
    for (ULONG at = bytes - headerSize; at > cached;) {
        PUCHAR p = base + at;
        ULONG length = NpuRead(p, 4);
        USHORT padding = Read16(p, 8);
        if (Read16(p, 0) != SMEM_CANARY || padding > length || length > at - cached) return NULL;
        if (Read16(p, 2) == item) {
            *size = length - padding;
            return p - length;
        }
        if (length > at || headerSize > at - length) return NULL;
        at -= length + headerSize;
    }
    return NULL;
}
static PUCHAR Find(NPU *n, ULONG item, PULONG size) {
    return FindPartition(n->Smem + n->Partition, n->PartitionSize, n->Cacheline, item, size);
}
static PUCHAR Allocate(NPU *n, ULONG item, ULONG length, PULONG size) {
    // TCSR hardware spinlock owner 1 is APSS, matching the platform SMEM ABI.
    BOOLEAN locked = FALSE;
    for (ULONG i = 0; i < 10000; i++) {
        NpuWrite(n->Mutex, 0, 1);
        if (NpuRead(n->Mutex, 0) == 1) { locked = TRUE; break; }
        KeStallExecutionProcessor(1);
    }
    if (!locked) return NULL;
    PUCHAR p = Find(n, item, size);
    if (!p) {
        ULONG at = NpuRead(n->Smem, n->Partition + 12);
        ULONG end = NpuRead(n->Smem, n->Partition + 16), aligned = (length + 7) & ~7u;
        if (at >= 32 && end <= n->PartitionSize && at <= end && end - at >= 16 && aligned <= end - at - 16) {
            p = n->Smem + n->Partition + at;
            for (ULONG i = 0; i < aligned; i += 4) NpuWrite(p, 16 + i, 0);
            Write16(p, 0, SMEM_CANARY);
            Write16(p, 2, (USHORT)item);
            NpuWrite(p, 4, aligned);
            Write16(p, 8, (USHORT)(aligned - length));
            Write16(p, 10, 0);
            NpuWrite(p, 12, 0);
            KeMemoryBarrier();
            NpuWrite(n->Smem, n->Partition + 12, at + 16 + aligned);
            p += 16;
            *size = length;
        }
    }
    KeMemoryBarrier();
    NpuWrite(n->Mutex, 0, 0);
    return p;
}
static BOOLEAN Send(NPU *n, const VOID *data, ULONG length) {
    if (!n->Desc || !n->Tx || n->TxSize < 32) return FALSE;
    ULONG head = NpuRead(n->Desc, 4), tail = NpuRead(n->Desc, 0);
    ULONG padded = (length + 7) & ~7u;
    if (head >= n->TxSize || tail >= n->TxSize || ((head | tail) & 7)) return FALSE;
    ULONG available = tail <= head ? n->TxSize - head + tail : tail - head;
    if (padded > n->TxSize - 16 || available < padded + 16) return FALSE;
    for (ULONG i = 0; i < padded; i += 4) {
        ULONG word = 0;
        if (i < length) RtlCopyMemory(&word, (const UCHAR *)data + i, min(4u, length - i));
        NpuWrite(n->Tx, (head + i) % n->TxSize, word);
    }
    KeMemoryBarrier();
    NpuWrite(n->Desc, 4, (head + padded) % n->TxSize);
    Kick(n, 0);
    return TRUE;
}
static BOOLEAN Command(NPU *n, USHORT command, USHORT channel, ULONG value) {
    ULONG packet[] = { command | ((ULONG)channel << 16), value };
    return Send(n, packet, sizeof(packet));
}
static VOID ReadFifo(NPU *n, ULONG offset, VOID *data, ULONG length) {
    for (ULONG i = 0; i < length; i += 4) {
        ULONG word = NpuRead(n->Rx, (offset + i) % n->RxSize);
        RtlCopyMemory((PUCHAR)data + i, &word, min(4u, length - i));
    }
}
static ULONG DspState(NPU *n) {
    if (!n->In) n->In = Find(n, SMP2P_IN, &n->InSize);
    if (!n->In || n->InSize < SMP2P_HEADER || NpuRead(n->In, 0) != 0x504d5324 ||
        Read16(n->In, 8) != 5 || Read16(n->In, 10) != 0) return 0;
    // Match the peer's v1/v2 header before looking at named state entries.
    ULONG version = NpuRead(n->In, 4);
    if ((version & 255u) < 1 || (version & 255u) > 2) return 0;
    ULONG negotiated = (version & 255u) | (version & NpuRead(n->Out, 4) & 0x100u);
    if (n->Online && NpuRead(n->Out, 4) != negotiated) {
        NpuWrite(n->Out, 4, negotiated);
        Kick(n, 2);
    }
    ULONG flags = NpuRead(n->Out, 16);
    ULONG ack = (NpuRead(n->In, 16) & 1u) << 1;
    if (n->Online && (flags & 2u) != ack && (version & 0x100u)) {
        NpuWrite(n->Out, 16, (flags & ~2u) | ack);
        Kick(n, 2);
    }
    ULONG entries = min((ULONG)Read16(n->In, 14), 16u);
    for (ULONG i = 0; i < entries && SMP2P_HEADER + i * 20 + 20 <= n->InSize; i++) {
        CHAR name[16];
        for (ULONG j = 0; j < 16; j += 4) {
            ULONG word = NpuRead(n->In, SMP2P_HEADER + i * 20 + j);
            RtlCopyMemory(name + j, &word, 4);
        }
        if (!memcmp(name, "slave-kernel\0", 13)) return NpuRead(n->In, SMP2P_HEADER + i * 20 + 16);
    }
    return 0;
}
ULONG NpuSocId(NPU *n) {
    ULONG toc = NPU_SMEM_SIZE - PAGE_SIZE;
    if (!n->Smem || NpuRead(n->Smem, toc) != 0x434f5424) return 0;
    ULONG count = NpuRead(n->Smem, toc + 8);
    if (!count || count > (PAGE_SIZE - 32) / 48) return 0;
    for (ULONG i = 0; i < count; i++) {
        ULONG entry = toc + 32 + i * 48;
        if (Read16(n->Smem, entry + 12) != 0xfffe || Read16(n->Smem, entry + 14) != 0xfffe) continue;
        ULONG offset = NpuRead(n->Smem, entry), bytes = NpuRead(n->Smem, entry + 4);
        ULONG cacheline = NpuRead(n->Smem, entry + 16), size = 0;
        if (!offset || offset >= toc || bytes < 32 || bytes > toc - offset ||
            !cacheline || cacheline > PAGE_SIZE || (cacheline & (cacheline - 1u)) ||
            NpuRead(n->Smem, offset) != 0x54525024) return 0;
        PUCHAR info = FindPartition(n->Smem + offset, bytes, cacheline, 137, &size);
        return info && size >= 12 ? NpuRead(info, 4) : 0;
    }
    return 0;
}
NTSTATUS NpuLinkStart(NPU *n) {
    ULONG toc = NPU_SMEM_SIZE - PAGE_SIZE;
    if (NpuRead(n->Smem, toc) != 0x434f5424) return STATUS_DEVICE_NOT_READY;
    ULONG count = NpuRead(n->Smem, toc + 8);
    if (!count || count > (PAGE_SIZE - 32) / 48) return STATUS_DEVICE_CONFIGURATION_ERROR;
    for (ULONG i = 0; i < count; i++) {
        ULONG entry = toc + 32 + i * 48;
        USHORT host0 = Read16(n->Smem, entry + 12), host1 = Read16(n->Smem, entry + 14);
        if ((host0 == 0 && host1 == 5) || (host0 == 5 && host1 == 0)) {
            n->Partition = NpuRead(n->Smem, entry);
            n->PartitionSize = NpuRead(n->Smem, entry + 4);
            n->Cacheline = NpuRead(n->Smem, entry + 16);
            break;
        }
    }
    if (!n->Partition || n->Partition >= toc || n->PartitionSize > toc - n->Partition ||
        n->PartitionSize < 32 || NpuRead(n->Smem, n->Partition) != 0x54525024 ||
        n->Cacheline < 8 || n->Cacheline > PAGE_SIZE || (n->Cacheline & (n->Cacheline - 1u)))
        return STATUS_DEVICE_NOT_READY;
    ULONG size = 0;
    n->Out = Allocate(n, SMP2P_OUT, SMP2P_SIZE, &size);
    if (!n->Out || size < SMP2P_SIZE) return STATUS_DEVICE_CONFIGURATION_ERROR;
    n->In = Find(n, SMP2P_IN, &n->InSize);
    ULONG peerVersion = n->In && n->InSize >= SMP2P_HEADER ? NpuRead(n->In, 4) & 255u : 0;
    if (peerVersion > 2) return STATUS_DEVICE_PROTOCOL_ERROR;
    // Reinitialize the APSS-owned item, including one registered stop entry.
    // SMP2P's packed header is 20 bytes; entries begin immediately after flags.
    {
        for (ULONG i = 0; i < SMP2P_SIZE; i += 4) NpuWrite(n->Out, i, 0);
        NpuWrite(n->Out, 0, 0x504d5324);
        Write16(n->Out, 8, 0);
        Write16(n->Out, 10, 5);
        Write16(n->Out, 12, 16);
        Write16(n->Out, 14, 1);
        static const CHAR name[16] = "master-kernel";
        for (ULONG i = 0; i < 16; i += 4) {
            ULONG word;
            RtlCopyMemory(&word, name + i, 4);
            NpuWrite(n->Out, SMP2P_HEADER + i, word);
        }
        KeMemoryBarrier();
        NpuWrite(n->Out, 4, (peerVersion ? peerVersion : 2u) | (1u << 8));
    }
    Kick(n, 2);
    n->Desc = Allocate(n, GLINK_DESC, 32, &size);
    if (!n->Desc || size < 16) return STATUS_DEVICE_CONFIGURATION_ERROR;
    n->Tx = Allocate(n, GLINK_TX, 0x4000, &n->TxSize);
    if (!n->Tx || n->TxSize < 32 || (n->TxSize & 7)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    n->Online = TRUE;
    n->Closed = FALSE;
    n->LocalIid = 1;
    Command(n, VERSION, 1, 1);
    return STATUS_SUCCESS;
}
VOID NpuLinkPoll(NPU *n) {
    if (!n->Online) return;
    n->State = DspState(n);
    if (n->State & 1u) {
        n->Online = FALSE;
        n->Closed = TRUE;
        n->LastStatus = STATUS_DEVICE_HARDWARE_ERROR;
        return;
    }
    if (!n->Rx) {
        n->Rx = Find(n, GLINK_RX, &n->RxSize);
        if (!n->Rx || n->RxSize < 32 || (n->RxSize & 7)) { n->Rx = NULL; return; }
    }
    if (n->PendingCommand) {
        if (!Command(n, (USHORT)n->PendingCommand, NPU_LOCAL_CID, n->PendingIid)) return;
        n->PendingCommand = 0;
    }
    if (n->VersionReady && !n->Opening && !n->Closed) {
        static const CHAR name[] = "fastrpcglink-apps-dsp";
        UCHAR packet[32] = { 0 };
        ULONG head[] = { OPEN | (NPU_LOCAL_CID << 16), sizeof(name) };
        RtlCopyMemory(packet, head, 8);
        RtlCopyMemory(packet + 8, name, sizeof(name));
        n->Opening = Send(n, packet, sizeof(packet));
    }
    if (n->Rcid && !n->Advertised) {
        ULONG intent[] = { INTENT | (NPU_LOCAL_CID << 16), 1, NPU_RESPONSE_SIZE, n->LocalIid };
        n->Advertised = Send(n, intent, sizeof(intent));
    }
    for (ULONG message = 0; message < 128; message++) {
        ULONG head = NpuRead(n->Desc, 12), tail = NpuRead(n->Desc, 8);
        if (head >= n->RxSize || tail >= n->RxSize || ((head | tail) & 7)) goto corrupt;
        ULONG available = head < tail ? n->RxSize - tail + head : head - tail;
        if (available < 8) return;
        ULONG h[4] = { 0 };
        ReadFifo(n, tail, h, 8);
        USHORT cmd = (USHORT)h[0], cid = (USHORT)(h[0] >> 16);
        ULONG value = h[1], size = 8;
        if (cmd == OPEN) size += ((value & 0xffff) + 7) & ~7u;
        else if (cmd == INTENT) {
            if (value > (n->RxSize - 8) / 8) goto corrupt;
            size += value * 8;
        } else if (cmd == TX_DATA || cmd == TX_CONT) {
            if (available < 16) return;
            ReadFifo(n, tail + 8, h + 2, 8);
            if (h[2] > n->RxSize - 16) goto corrupt;
            size = (16 + h[2] + 7) & ~7u;
        }
        if (size > n->RxSize - 8) goto corrupt;
        if (available < size) return;
        // Leave an unacknowledged command in the FIFO until TX has capacity.
        if (cmd == VERSION) {
            if (!Command(n, VERSION_ACK, 1, value & 1u)) return;
        } else if (cmd == VERSION_ACK) {
            if (cid != 1 || value != 1) goto corrupt;
            n->VersionReady = TRUE;
        } else if (cmd == OPEN) {
            CHAR name[32] = { 0 };
            if ((value & 0xffff) < sizeof(name)) {
                ReadFifo(n, tail + 8, name, value & 0xffff);
                if (!strcmp(name, "fastrpcglink-apps-dsp")) {
                    if (!cid || !Command(n, OPEN_ACK, cid, 0)) return;
                    n->Rcid = cid;
                }
            }
        } else if (cmd == OPEN_ACK && cid == NPU_LOCAL_CID) n->Acked = TRUE;
        else if (cmd == CLOSE_ACK && cid == NPU_LOCAL_CID) {
            n->Acked = FALSE;
            n->Opening = FALSE;
            n->Closed = TRUE;
        } else if (cmd == CLOSE) {
            if (!Command(n, CLOSE_ACK, cid, 0)) return;
            if (cid == n->Rcid) {
                n->Closed = TRUE;
                n->Rcid = 0;
                n->Acked = FALSE;
            }
        } else if (cid == n->Rcid && n->Rcid) {
            if (cmd == INTENT) {
                n->IntentRequested = FALSE;
                for (ULONG i = 0; i < value; i++) {
                    ULONG pair[2];
                    ReadFifo(n, tail + 8 + i * 8, pair, 8);
                    ULONG j;
                    for (j = 0; j < n->IntentCount && n->Iid[j] != pair[1]; j++);
                    if (j == NPU_INTENTS) continue;
                    if (j == n->IntentCount) n->IntentCount++;
                    n->Iid[j] = pair[1];
                    n->IntentSize[j] = pair[0];
                    n->IntentFree[j] = TRUE;
                }
            } else if (cmd == RX_DONE || cmd == RX_REUSE) {
                for (ULONG i = 0; i < n->IntentCount; i++) {
                    if (n->Iid[i] != value) continue;
                    if (cmd == RX_REUSE) n->IntentFree[i] = TRUE;
                    else {
                        n->IntentCount--;
                        n->Iid[i] = n->Iid[n->IntentCount];
                        n->IntentSize[i] = n->IntentSize[n->IntentCount];
                        n->IntentFree[i] = n->IntentFree[n->IntentCount];
                    }
                    break;
                }
            } else if (cmd == INTENT_REQ_ACK) n->IntentRequested = FALSE;
            else if (cmd == INTENT_REQ) {
                // One request at a time, with a single reusable receive intent.
                if (!Command(n, INTENT_REQ_ACK, NPU_LOCAL_CID, value <= NPU_RESPONSE_SIZE && n->Advertised)) return;
            } else if (cmd == TX_DATA || cmd == TX_CONT) {
                if (value != n->LocalIid || cmd != TX_DATA || h[3] || h[2] < 12 || h[2] > NPU_RESPONSE_SIZE) goto corrupt;
                UCHAR response[NPU_RESPONSE_SIZE];
                ReadFifo(n, tail + 16, response, h[2]);
                ULONGLONG context;
                ULONG result;
                RtlCopyMemory(&context, response, 8);
                RtlCopyMemory(&result, response + 8, 4);
                NpuRpcReply(n, context, result);
                n->PendingCommand = RX_REUSE;
                n->PendingIid = value;
            }
        }
        KeMemoryBarrier();
        NpuWrite(n->Desc, 8, (tail + size) % n->RxSize);
        if (cmd == READ_NOTIF) Kick(n, 0);
        if (n->PendingCommand) {
            if (!Command(n, (USHORT)n->PendingCommand, NPU_LOCAL_CID, n->PendingIid)) return;
            n->PendingCommand = 0;
        }
    }
    return;
corrupt:
    n->Online = FALSE;
    n->Closed = TRUE;
    n->LastStatus = STATUS_DEVICE_PROTOCOL_ERROR;
}
BOOLEAN NpuLinkSend(NPU *n, const NPU_MESSAGE *message) {
    if (!n->Online || !n->Acked || !n->Rcid || n->Closed) return FALSE;
    ULONG i;
    for (i = 0; i < n->IntentCount && !(n->IntentFree[i] && n->IntentSize[i] >= sizeof(*message)); i++);
    if (i == n->IntentCount) {
        if (!n->IntentRequested) n->IntentRequested = Command(n, INTENT_REQ, NPU_LOCAL_CID, sizeof(*message));
        return FALSE;
    }
    UCHAR packet[16 + sizeof(*message)];
    ULONG header[] = { TX_DATA | (NPU_LOCAL_CID << 16), n->Iid[i], sizeof(*message), 0 };
    RtlCopyMemory(packet, header, 16);
    RtlCopyMemory(packet + 16, message, sizeof(*message));
    if (!Send(n, packet, sizeof(packet))) return FALSE;
    n->IntentFree[i] = FALSE;
    return TRUE;
}
VOID NpuLinkClose(NPU *n) {
    if (n->Online && n->Opening && !n->Closed) {
        Command(n, CLOSE, NPU_LOCAL_CID, 0);
        for (ULONG i = 0; i < 100 && !n->Closed; i++) { NpuLinkPoll(n); NpuSleep(); }
    }
    n->Online = FALSE;
}
