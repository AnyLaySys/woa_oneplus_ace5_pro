/* SPDX-License-Identifier: BSD-3-Clause-Clear
 * PEACH CN regulatory discovery, firmware-rule-intersected passive channels,
 * one STA vdev, bounded passive scans, and beacon/probe-response decoding.
 * Wire formats: local ath12k/wmi.c,wmi.h and PEACH command TLV schema.
 * Legacy regulatory and DBS response fields also checked against Qualcomm
 * fw-api/wmi_unified.h. No credentials, association or data-TX is performed.
 */
#include "wcn7861.h"
#include "stage6-protocol.h"
#include "stage7-util.h"
#include "network-policy.h"

#define S6_PDEV 1u
#define S6_PDEV_SECOND 2u
#define S6_VDEV 0u
#define S6_SCAN_ID 0xa000u
#define S6_REQUESTOR 1u
#define S6_PASSIVE_FLAGS 0x10001u /* Linux passive + strict-passive; no CCXv4 promiscuous flag */

static VOID S6Fail (PWCN_DEVICE Dev, PCSTR Reason)
{
    if (!Dev->Scan.Failed) {
        Dev->Scan.Failed = TRUE;
        WcnLog (Dev, "stage6: FAIL state=%u reason=%s country-ready=%u channels=%u started=%u completed=%u APs=%u credits=%u",
                Dev->Scan.State, Reason, Dev->Scan.CountryReady, Dev->Scan.ChannelCount,
                Dev->Scan.ScanStarted, Dev->Scan.ScanCompleted, Dev->Scan.ApCount, Dev->Ce.WmiCredits);
    }
}

static VOID S6Regulatory (PWCN_DEVICE Dev, const UCHAR *Data, ULONG Length, BOOLEAN Extended)
{
    PWCN_SCAN Scan = &Dev->Scan;
    const UCHAR *Fixed, *Array, *Rule;
    ULONG Size, ArraySize, Tag, MinSize, Index, N2, N5, Count, Offset, Value, Alpha, Phy, Status;
    Fixed = Jfind (Data, Length, Extended ? S6_WMI_TAG_REG_CHAN_LIST_CC_EXT_EVENT : S6_WMI_TAG_REG_CHAN_LIST_CC_EVENT, &Size);
    if (!Fixed || Size < 56) { return; }
    Status = Jle32 (Fixed); Phy = Jle32 (Fixed + 4); Alpha = Jle32 (Fixed + 8);

    if (!Scan->CountrySent || Phy != 0) return; /* pdev1 maps to firmware phy0 */
    if (Status) {
        WcnLog (Dev, "stage6: requested country/domain rejected status=%u (0=PASS 1=CURRENT_ALPHA2_NOT_FOUND 2=INIT_ALPHA2_NOT_FOUND 3=CHANGE_NOT_ALLOWED 4=NO_MEMORY 5=FAIL)", Status);
        S6Fail (Dev, "firmware rejected requested country/domain"); return;
    }
    if ((Alpha & 0xffff) != 0x4e43u) {
        if (Scan->CountryReady) S6Fail (Dev, "country changed away from requested country/domain");
        return; /* Do not silently use a board's default US or assume user country. */
    }

    N2 = Jle32 (Fixed + 48); N5 = Jle32 (Fixed + 52);
    if (N2 > WCN_SCAN_MAX_RULES || N5 > WCN_SCAN_MAX_RULES - N2 || N2 + N5 == 0) {
        S6Fail (Dev, "invalid/empty firmware regulatory rules"); return;
    }
    Count = N2 + N5;
    Array = Jfind (Data, Length, S6_WMI_TAG_ARRAY_STRUCT, &ArraySize);
    if (!Array || !Jtlvs (Array, ArraySize)) { S6Fail (Dev, "invalid regulatory rule array"); return; }
    Tag = Extended ? S6_WMI_TAG_REGULATORY_RULE_EXT_STRUCT : S6_WMI_TAG_REGULATORY_RULE_STRUCT;
    MinSize = Extended ? 16 : 12;
    /* Validate all required rules before changing the cached domain. */
    Offset = 0;
    for (Index = 0; Index < Count; Index++) {
        if (ArraySize - Offset < 4) { S6Fail (Dev, "truncated regulatory rules"); return; }
        Value = Jle32 (Array + Offset); Size = Value & 0xffff;
        if ((Value >> 16) != Tag || Size < MinSize || Size > ArraySize - Offset - 4) {
            S6Fail (Dev, "wrong regulatory rule tag/length"); return;
        }
        Rule = Array + Offset + 4; Value = Jle32 (Rule);
        if ((Value & 0xffff) >= (Value >> 16)) { S6Fail (Dev, "reversed regulatory frequency interval"); return; }
        Offset += Size + 4;
    }
    if (Scan->ScanSent) {
        /* A repeated identical country notification is not a domain change. */
        BOOLEAN Same = Count == Scan->RuleCount && N2 == Scan->Rules2g;
        Offset = 0;
        for (Index = 0; Index < Count && Same; Index++) {
            ULONG Freq, Bw, Flag;
            Size = Jle32 (Array + Offset) & 0xffff; Rule = Array + Offset + 4;
            Freq = Jle32 (Rule); Bw = Jle32 (Rule + 4); Flag = Jle32 (Rule + 8) & 0xffff;
            Same = Scan->Rule[Index].Start == (Freq & 0xffff) && Scan->Rule[Index].End == (Freq >> 16) &&
                   Scan->Rule[Index].MaxBw == (Bw & 0xffff) && Scan->Rule[Index].Power == ((Bw >> 16) & 255) &&
                   Scan->Rule[Index].Gain == (Bw >> 24) && Scan->Rule[Index].Flags == Flag;
            Offset += Size + 4;
        }
        if (!Same) S6Fail (Dev, "regulatory rules changed during scan");
        return;
    }
    Offset = 0;
    for (Index = 0; Index < Count; Index++) {
        Size = Jle32 (Array + Offset) & 0xffff; Rule = Array + Offset + 4;
        Value = Jle32 (Rule);
        Scan->Rule[Index].Start = Value & 0xffff; Scan->Rule[Index].End = Value >> 16;
        Value = Jle32 (Rule + 4);
        Scan->Rule[Index].MaxBw = Value & 0xffff;
        Scan->Rule[Index].Power = (Value >> 16) & 0xff;
        Scan->Rule[Index].Gain = Value >> 24;
        Scan->Rule[Index].Flags = Jle32 (Rule + 8) & 0xffff;

        Offset += Size + 4;
    }
    Scan->RuleCount = Count; Scan->Rules2g = N2; Scan->Alpha2[0] = (UCHAR)Alpha; Scan->Alpha2[1] = (UCHAR)(Alpha >> 8);
    Scan->MaxBw2g=Jle32(Fixed+36);Scan->MaxBw5g=Jle32(Fixed+44);Scan->PhyBitmap=Jle32(Fixed+28);

    Scan->CountryGeneration++; Scan->CountryReady = TRUE;

}

static BOOLEAN S6ChannelAdd (PWCN_DEVICE Dev, ULONG Frequency)
{
    PWCN_SCAN Scan = &Dev->Scan;
    ULONG Index, Start, End, MaxPower, Power = MAXULONG, Gain = MAXULONG, Flags = 0;
    BOOLEAN Matched = FALSE;
    PWCN_SCAN_RULE Rule;
    if (Scan->ChannelCount >= WCN_SCAN_MAX_CHANNELS) return FALSE;
    /* Intersect the firmware domain with ath12k's conservative world ranges.
     * 20 MHz channel edges must fit. No 6 GHz or channel14 in this version. */
    if (Frequency < 2500) { Start = 2402; End = 2482; MaxPower = 20; }
    else if (Frequency >= 5150 && Frequency <= 5350) { Start = 5140; End = 5360; MaxPower = 30; }
    else if (Frequency >= 5725 && Frequency <= 5850) { Start = 5715; End = 5860; MaxPower = 30; }
    else return FALSE;
    if (Frequency < Start + 10 || Frequency + 10 > End) return FALSE;
    for (Index = Frequency < 2500 ? 0 : Scan->Rules2g;
         Index < (Frequency < 2500 ? Scan->Rules2g : Scan->RuleCount); Index++) {
        Rule = &Scan->Rule[Index];
        if (Frequency < Rule->Start + 10 || Frequency + 10 > Rule->End || Rule->MaxBw < 20) continue;
        Matched = TRUE; Flags |= Rule->Flags;
        if (Rule->Power < Power) Power = Rule->Power;
        if (Rule->Gain < Gain) Gain = Rule->Gain;
    }
    if (!Matched || (Flags & (1u | (1u << 11)))) return FALSE;
    if (Power > MaxPower) Power = MaxPower;
    Scan->Channel[Scan->ChannelCount].Frequency = Frequency;
    Scan->Channel[Scan->ChannelCount].Power = Power;
    Scan->Channel[Scan->ChannelCount].Gain = Gain;
    Scan->Channel[Scan->ChannelCount].Flags = Flags;
    Scan->ChannelCount++;
    return TRUE;
}
static VOID S6ChannelsBuild (PWCN_DEVICE Dev)
{
    ULONG Channel;
    Dev->Scan.ChannelCount = 0;
    for (Channel = 1; Channel <= 13u; Channel++) S6ChannelAdd (Dev, 2407 + 5 * Channel);
    for (Channel = 36; Channel <= 64; Channel += 4) S6ChannelAdd (Dev, 5000 + 5 * Channel);
    for (Channel = 149; Channel <= 165; Channel += 4) S6ChannelAdd (Dev, 5000 + 5 * Channel);

}
static BOOLEAN S6FrequencyAllowed (PWCN_DEVICE Dev, ULONG Frequency)
{
    ULONG Index;
    for (Index = 0; Index < Dev->Scan.ChannelCount; Index++) if (Dev->Scan.Channel[Index].Frequency == Frequency) return TRUE;
    return FALSE;
}

static NTSTATUS S6CountrySend (PWCN_DEVICE Dev)
{
    ULONG Body[4] = { 0 };
    Body[0] = Jtlv (S6_WMI_TAG_SET_INIT_COUNTRY_CMD, 12);
    Body[1] = S6_PDEV; Body[2] = 0; Body[3] = 0x4e43u; /* Explicit CN, confirmed status0 rules required. */
    return WcnWmiSend (Dev, S6_WMI_SET_INIT_COUNTRY_CMDID, Body, sizeof (Body));
}
static NTSTATUS S6ChannelListSend (PWCN_DEVICE Dev)
{
    PWCN_SCAN Scan = &Dev->Scan; ULONG *B = Scan->Tx, *C, I, Power, Gain, Info;
    if (!Scan->ChannelCount || Scan->ChannelCount > WCN_SCAN_MAX_CHANNELS) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory (B, sizeof (Scan->Tx));
    B[0] = Jtlv (S6_WMI_TAG_SCAN_CHAN_LIST_CMD, 12);
    B[1] = Scan->ChannelCount; B[2] = 0; B[3] = S6_PDEV;
    B[4] = Jtlv (S6_WMI_TAG_ARRAY_STRUCT, Scan->ChannelCount * 28);
    for (I = 0; I < Scan->ChannelCount; I++) {
        C = B + 5 + I * 7;
        C[0] = Jtlv (S6_WMI_TAG_CHANNEL, 24);
        C[1] = C[2] = Scan->Channel[I].Frequency;
        Info = (1u << 9) | (1u << 11); /* AP disabled, HT; passive restriction follows the validated channel. */
        if(!WcnProbeAllowed(C[1],Scan->Channel[I].Flags))Info|=1u<<7;
        if (C[1] < 2500) Info |= 1; /* MODE_11G else MODE_11A */
        if (Scan->Channel[I].Flags & 8) Info |= 1u << 10;
        C[4] = Info;
        Power = Scan->Channel[I].Power * 2; if (Power > 127) Power = 127;
        Gain = Scan->Channel[I].Gain * 2; if (Gain > 127) Gain = 127;
        C[5] = (Power << 8) | (Power << 16); C[6] = Gain | (Power << 8);
    }
    return WcnWmiSend (Dev, S6_WMI_SCAN_CHAN_LIST_CMDID, B, 20 + Scan->ChannelCount * 28);
}
static NTSTATUS S6PdevMaskSend (PWCN_DEVICE Dev, BOOLEAN Rx)
{
    ULONG B[4], Chains = Dev->Ce.NumRfChains;
    if (!Chains || Chains > 2) return STATUS_DEVICE_CONFIGURATION_ERROR;
    B[0] = Jtlv (S6_WMI_TAG_PDEV_SET_PARAM_CMD, 12); B[1] = S6_PDEV;
    B[2] = Rx ? S6_WMI_PDEV_PARAM_RX_CHAIN_MASK : S6_WMI_PDEV_PARAM_TX_CHAIN_MASK;
    B[3] = (1u << Chains) - 1;
    return WcnWmiSend (Dev, S6_WMI_PDEV_SET_PARAM_CMDID, B, sizeof (B));
}
static NTSTATUS S6NssSend (PWCN_DEVICE Dev)
{return WcnVdevParam(Dev,S6_VDEV,S6_WMI_VDEV_PARAM_NSS,Dev->Ce.NumRfChains);}

static NTSTATUS S6VdevSend (PWCN_DEVICE Dev)
{
    ULONG *B = Dev->Scan.Tx, I, Chains = Dev->Ce.NumRfChains;
    if (!Chains || Chains > 2) return STATUS_DEVICE_CONFIGURATION_ERROR;
    RtlZeroMemory (B, sizeof (Dev->Scan.Tx));
    B[0] = Jtlv (S6_WMI_TAG_VDEV_CREATE_CMD, 44);
    B[1] = S6_VDEV; B[2] = 2; /* STA */ B[3] = 0;
    RtlCopyMemory (B + 4, Dev->Ce.MacAddress, 6);
    B[6] = 2; B[7] = S6_PDEV;
    /* stats-id-valid=0; no MLD or MBSSID capabilities claimed */
    B[12] = Jtlv (S6_WMI_TAG_ARRAY_STRUCT, 32);
    for (I = 0; I < 2; I++) {
        B[13 + I * 4] = Jtlv (S6_WMI_TAG_VDEV_TXRX_STREAMS, 12);
        B[14 + I * 4] = I; B[15 + I * 4] = Chains; B[16 + I * 4] = Chains;
    }
    return WcnWmiSend (Dev, S6_WMI_VDEV_CREATE_CMDID, B, 84);
}
static NTSTATUS S6ScanSend (PWCN_DEVICE Dev)
{
    PWCN_SCAN Scan = &Dev->Scan; ULONG *B = Scan->Tx, I, Offset, Count=Scan->RequestedCount?Scan->RequestedCount:Scan->ChannelCount;
    if (!Scan->ChannelCount || Scan->ChannelCount > WCN_SCAN_MAX_CHANNELS||Scan->Target.SsidLength>32||Count>WCN_SCAN_MAX_CHANNELS) return STATUS_INVALID_PARAMETER;
    /* Keep the full firmware-validated list for initial and runtime scans. */
    if(!Scan->ScanId)Scan->ScanId=S6_SCAN_ID;
    RtlZeroMemory (B, sizeof (Scan->Tx));
    /* Exact 160-byte Linux fixed layout; firmware's 172-byte schema accepts
     * the backwards-compatible shorter struct. Do not guess new fields. */
    B[0] = Jtlv (S6_WMI_TAG_START_SCAN_CMD, 156);
    B[1] = Scan->ScanId; B[2] = S6_REQUESTOR; B[3] = Scan->ScanVdev; B[4] = 1;
    B[5] = 0x1ff; B[6] = 50; B[7] = 150; B[8] = 50; B[9] = 500;
    B[10]=Scan->Target.Active?20u:0u; /* second probe after 20ms, capped by n_probes */
    B[13] = 20000; B[14] = 5; B[15] = Scan->Target.Active?(0x10000u|4u|8u|(Scan->Target.SsidLength?0u:2u)):S6_PASSIVE_FLAGS;
    B[17] = Count; B[18] = 1;B[19]=Scan->Target.Active&&Scan->Target.SsidLength?1u:0u;B[21]=Scan->Target.Active?2u:0u;
    B[36] = Scan->Target.Active?50u:0u;B[37] = 70; B[38] = 70; /* active/passive6g fields unused, no6g channels */
    Offset = 40;
    B[Offset++] = Jtlv (S6_WMI_TAG_ARRAY_UINT32, Count * 4);
    for (I = 0; I < Count; I++) B[Offset++] = Scan->RequestedCount?Scan->RequestedFrequency[I]:Scan->Channel[I].Frequency;
    B[Offset++] = Jtlv (S6_WMI_TAG_ARRAY_FIXED_STRUCT, B[19]*36);
    if(B[19]){B[Offset++]=Scan->Target.SsidLength;RtlCopyMemory(B+Offset,Scan->Target.Ssid,Scan->Target.SsidLength);Offset+=8;}
    B[Offset++] = Jtlv (S6_WMI_TAG_ARRAY_FIXED_STRUCT, 8);
    RtlCopyMemory(B+Offset,Scan->Target.Bssid,6);if(!Jle32((const UCHAR*)B+Offset*4)&&!Jle16((const UCHAR*)B+Offset*4+4)){B[Offset]=MAXULONG;B[Offset+1]=0xffff;}Offset+=2;
    B[Offset++] = Jtlv (S6_WMI_TAG_ARRAY_BYTE, 0); /* no probe-request IE */
    NTSTATUS St=WcnWmiSend(Dev,S6_WMI_START_SCAN_CMDID,B,Offset*4);
    RtlSecureZeroMemory(B,sizeof(Scan->Tx));if(NT_SUCCESS(St))RtlSecureZeroMemory(&Scan->Target,sizeof(Scan->Target));return St;
}
static NTSTATUS S6AbortSend (PWCN_DEVICE Dev)
{
    ULONG B[6] = { 0 };
    B[0] = Jtlv (S6_WMI_TAG_STOP_SCAN_CMD, 20);
    B[1] = S6_REQUESTOR; B[2] = Dev->Scan.ScanId?Dev->Scan.ScanId:S6_SCAN_ID; B[3] = 0; B[4] = Dev->Scan.ScanVdev; B[5] = S6_PDEV;
    return WcnWmiSend (Dev, S6_WMI_STOP_SCAN_CMDID, B, sizeof (B));
}

static VOID S6ScanEvent (PWCN_DEVICE Dev, const UCHAR *Data, ULONG Length)
{
    PWCN_SCAN S = &Dev->Scan; const UCHAR *F; ULONG Size, Type, Reason, Req, Id, Vdev;
    F = Jfind (Data, Length, S6_WMI_TAG_SCAN_EVENT, &Size);
    if (!F || Size < 24) { return; }
    Type = Jle32 (F); Reason = Jle32 (F + 4); Req = Jle32 (F + 12);
    Id = Jle32 (F + 16); Vdev = Jle32 (F + 20);
    if (!S->ScanSent || S->ScanCompleted || Id != (S->ScanId?S->ScanId:S6_SCAN_ID) || Req != S6_REQUESTOR || Vdev != S->ScanVdev) return;

    if (Type & 1) { S->ScanStarted = TRUE;  }
    if (Type & 8) S->ForeignChannels++;
    if (Type & 0x40) { S6Fail (Dev, "scan START_FAILED"); return; }
    if (Type & 2) {
        S->ScanCompleted = TRUE; S->ScanReason = Reason; S->CompletedAt = WcnNowMs ();
        if(S->RuntimeScan&&S->AbortSent&&Reason==1){S->RuntimeScan=FALSE;S->State=7;
            return;}
        if (!S->ScanStarted || Reason) { S6Fail (Dev, "scan completion without start or nonzero reason"); return; }
        S->State = 6;
    }
}
static VOID S6Management (PWCN_DEVICE Dev, const UCHAR *Data, ULONG Length)
{
    PWCN_SCAN S = &Dev->Scan; const UCHAR *Header, *Frame, *Ie, *Ssid = NULL;
    ULONG HdrSize, ArraySize, FrameSize, Frequency, I, N;
    USHORT Fc; LONG Rssi; PWCN_SCAN_AP Ap;
    if (!S->ScanSent || (S->Passed && !S->RuntimeScan) || S->Failed) { return; }
    Header = Jfind (Data, Length, S6_WMI_TAG_MGMT_RX_HDR, &HdrSize);
    Frame = Jfind (Data, Length, S6_WMI_TAG_ARRAY_BYTE, &ArraySize);
    if (!Header || HdrSize < 68 || !Frame) { return; }
    FrameSize = Jle32 (Header + 16);
    if (FrameSize < 36 || FrameSize > ArraySize || (Jle32 (Header + 20) & 0x29)) { return; }
    /* PEACH is a two-pdev DBS device: the firmware tags scan mgmt frames with
     * the receiving MAC's pdev. 5GHz-only runtime scans are served by the
     * second MAC (hardware evidence 2026-11-01: boot-scan frames pdev=1, every
     * runtime-rescan frame pdev=2; the old pdev==1 filter rejected them all).
     * Accept both own pdevs; any other pdev still counts as rejected. */
    if (Jle32 (Header + 60) != S6_PDEV && Jle32 (Header + 60) != S6_PDEV_SECOND) { return; }
    Fc = Jle16 (Frame);
    if ((Fc & 0xfc) != 0x80 && (Fc & 0xfc) != 0x50) { return; }
    if ((Frame[16] & 1) || (Frame[16] | Frame[17] | Frame[18] | Frame[19] | Frame[20] | Frame[21]) == 0) { return; }
    Frequency = Jle32 (Header + 64);
    if (!Frequency) {
        I = Jle32 (Header);
        Frequency = I == 14 ? 2484 : (I >= 1 && I <= 13 ? 2407 + I * 5 : 5000 + I * 5);
    }
    if (!S6FrequencyAllowed (Dev, Frequency)) { return; }
    for (I = 36; I < FrameSize; I += N + 2) {
        if (FrameSize - I < 2) { return; }
        Ie = Frame + I; N = Ie[1];
        if (N > FrameSize - I - 2) { return; }
        if (Ie[0] == 0) {
            if (N > 32 || Ssid) { return; }
            Ssid = Ie + 2;
        }
    }
    if (!Ssid) { return; }
    Rssi = (LONG)Jle32 (Header + 44);
    if (Rssi > 0 || Rssi < -127) Rssi = -127; /* unknown, never invent a noise floor */
    WcnStage8Observe (Dev, Frame, FrameSize, Frequency, Rssi);
    for (I = 0; I < S->ApCount; I++) {
        Ap = &S->Ap[I];
        if (Ap->Frequency == Frequency && RtlCompareMemory (Ap->Bssid, Frame + 16, 6) == 6) return;
    }
    if (S->ApCount == WCN_SCAN_MAX_APS) return;
    Ap = &S->Ap[S->ApCount++];
    RtlZeroMemory (Ap, sizeof (*Ap)); RtlCopyMemory (Ap->Bssid, Frame + 16, 6);
    Ap->Frequency = Frequency;
}

VOID WcnStage6Receive (PWCN_DEVICE Dev, ULONG Event, const UCHAR *Payload, ULONG Length)
{
    const UCHAR *Fixed; ULONG Size;
    if (Event != S6_WMI_PDEV_SET_HW_MODE_RESP_EVENTID && Event != S6_WMI_REG_CHAN_LIST_CC_EVENTID &&
        Event != S6_WMI_REG_CHAN_LIST_CC_EXT_EVENTID && Event != S6_WMI_SCAN_EVENTID && Event != S6_WMI_MGMT_RX_EVENTID) return;
if (!Jtlvs (Payload, Length)) { return; }
    if (Event == S6_WMI_PDEV_SET_HW_MODE_RESP_EVENTID) {
        Fixed = Jfind (Payload, Length, S6_WMI_TAG_PDEV_SET_HW_MODE_RESPONSE_EVENT, &Size);
        if (!Fixed || Size < 16 || !Dev->Ce.HwModeSent || Jle32 (Fixed) != 0) return;
        Dev->Ce.HwModeStatus = Jle32 (Fixed + 4); Dev->Ce.HwModeIndex = Jle32 (Fixed + 8);
        Dev->Ce.HwModeValidated = TRUE;

    } else if (Event == S6_WMI_REG_CHAN_LIST_CC_EVENTID || Event == S6_WMI_REG_CHAN_LIST_CC_EXT_EVENTID) {
        S6Regulatory (Dev, Payload, Length, Event == S6_WMI_REG_CHAN_LIST_CC_EXT_EVENTID);
    } else if (Event == S6_WMI_SCAN_EVENTID) S6ScanEvent (Dev, Payload, Length);
    else {
        S6Management (Dev, Payload, Length);
    }
}

VOID WcnStage6Service (PWCN_DEVICE Dev)
{
    PWCN_SCAN S = &Dev->Scan; LONGLONG Now = WcnNowMs (); NTSTATUS Status;
    if (!Dev->Ce.WmiReady || Dev->Mhi.SysErr || Dev->Mhi.RddmSaved) return;
    if (S->Failed) {
        if (S->ScanSent && !S->ScanCompleted && !S->AbortSent && S->AbortAttempts++ < 20) {
            Status = S6AbortSend (Dev); if (NT_SUCCESS (Status)) { S->AbortSent = TRUE; WcnLog (Dev, "stage6: scan abort sent after failure"); }
        }
        return;
    }
    if ((S->Passed && !S->RuntimeScan) || S->State == 7) return;
    if (!S->StartedAt) {
        S->StartedAt = Now; S->StateAt = Now;

    }
    if (Now - S->StartedAt > 45000) { S6Fail (Dev, "overall 45s deadline"); return; }
    switch (S->State) {
    case 0:
        if (!Dev->Ce.HttVersionReceived || !Dev->Ce.HwModeValidated) {
            if (Now - S->StateAt > 5000) S6Fail (Dev, "HTT/DBS response deadline");
            return;
        }
        if (Dev->Ce.HttMajor != 3 || Dev->Ce.HwModeStatus || Dev->Ce.HwModeIndex != 1) {
            S6Fail (Dev, "HTT or DBS prerequisite rejected"); return;
        }
        Status = S6CountrySend (Dev);
        if (Status == STATUS_DEVICE_BUSY) return;
        if (!NT_SUCCESS (Status)) { S6Fail (Dev, "country command transport error"); return; }
        S->CountrySent = TRUE; S->State = 1; S->StateAt = Now;

        break;
    case 1:
        if (!S->CountryReady) {
            if (Now - S->StateAt > 8000) S6Fail (Dev, "country confirmation deadline; not using rejected/default US rules");
            return;
        }
        S6ChannelsBuild (Dev);
        if (!S->ChannelCount) { S6Fail (Dev, "no permitted world/firmware channels"); return; }
        S->State = 2; S->StateAt = Now;
        break;
    case 2:
        Status = S6ChannelListSend (Dev);
        if (Status == STATUS_DEVICE_BUSY) return;
        if (!NT_SUCCESS (Status)) { S6Fail (Dev, "channel-list transport error"); return; }
        S->ChannelListSent = TRUE; S->State = 3; S->StateAt = Now;

        break;
    case 3:
        if (Now - S->StateAt < 200) return;
        Status = WcnDpHttRxPrepare (Dev);
        if (Status == STATUS_DEVICE_BUSY || Status == STATUS_PENDING) return;
        if (!NT_SUCCESS (Status)) { S6Fail (Dev, "HTT RX resource setup transport error"); return; }
        if (!S->TxMaskSent || !S->RxMaskSent) {
            BOOLEAN Rx = S->TxMaskSent;
            Status = S6PdevMaskSend (Dev, Rx);
            if (Status == STATUS_DEVICE_BUSY) return;
            if (!NT_SUCCESS (Status)) { S6Fail (Dev, "pdev antenna-chain transport error"); return; }
            if (Rx) S->RxMaskSent = TRUE; else S->TxMaskSent = TRUE;

            return;
        }
        Status = S6VdevSend (Dev);
        if (Status == STATUS_DEVICE_BUSY) return;
        if (!NT_SUCCESS (Status)) { S6Fail (Dev, "STA vdev-create transport error"); return; }
        Dev->Join.Created=TRUE;S->VdevSubmitted = TRUE; S->State = 4; S->StateAt = Now;

        break;
    case 4:
        if (Now - S->StateAt < 200) return;
        if (!S->NssSent) {
            Status = S6NssSend (Dev);
            if (Status == STATUS_DEVICE_BUSY) return;
            if (!NT_SUCCESS (Status)) { S6Fail (Dev, "STA NSS transport error"); return; }
            S->NssSent = TRUE;

            return;
        }
        Status = S6ScanSend (Dev);
        if (Status == STATUS_DEVICE_BUSY) return;
        if (!NT_SUCCESS (Status)) { S6Fail (Dev, "scan-start transport error"); return; }
        S->ScanSent = TRUE; S->ScanAt = Now; S->State = 5; S->StateAt = Now;

        break;
    case 5:
        if (!S->ScanStarted && Now - S->ScanAt > 5000) S6Fail (Dev, "scan STARTED event deadline");
        else if (Now - S->ScanAt > 25000) S6Fail (Dev, "scan completion deadline");
        break;
    case 6:
        if (Now - S->CompletedAt < 300) return; /* drain final CE management events */
        if(S->RuntimeScan){
            if(!S->CountryReady||!S->VdevSubmitted||!S->ScanStarted||!S->ScanCompleted||S->ScanReason||!S->ForeignChannels){S6Fail(Dev,"runtime scan evidence inconsistent");return;}
            S->RuntimeScan=FALSE;S->State=7;return;
        }
        if (S->CountryReady && S->ChannelListSent && Dev->Dp.HttRxReady && S->TxMaskSent && S->RxMaskSent &&
            S->NssSent && S->VdevSubmitted && S->ScanStarted &&
            S->ScanCompleted && S->ScanReason == 0 && S->ApCount && S->ForeignChannels) {
            S->Passed = TRUE;
        }
        S->State = 7;
        break;
    default:
        S6Fail (Dev, "invalid state");
        break;
    }
}

/* Reuse the firmware-created STA vdev and confirmed CN table. The stage6
 * baseline stays valid; a new scan has its own ID and completion evidence. */
NTSTATUS WcnStage6Rescan(PWCN_DEVICE Dev,ULONG Vdev,const ULONG *Frequency,ULONG Count,const WCN_SCAN_TARGET *Target)
{
    PWCN_SCAN S=&Dev->Scan;
    if(!Dev->Ce.WmiReady||Dev->Mhi.SysErr||Dev->Mhi.RddmSaved||S->Failed||
       !S->Passed||!S->CountryReady||!S->ChannelListSent||!S->VdevSubmitted||!S->NssSent)return STATUS_DEVICE_NOT_READY;
    if(S->RuntimeScan||S->State!=7)return STATUS_DEVICE_BUSY;
    /* Do not take the radio off-channel in the middle of auth/association or
     * the Windows four-way handshake. Pending OS scans are bounded/deferred. */
    if((Dev->Join.Started&&!Dev->Join.Authorized)||(Dev->Secondary.Started&&!Dev->Secondary.Authorized))return STATUS_DEVICE_BUSY;
    if(Count>WCN_SCAN_MAX_CHANNELS||(Count&&!Frequency)||!Target||Target->SsidLength>32||(Vdev!=0&&Vdev!=2)||(Vdev==2&&!Count)||(Target->Active&&!Count))return STATUS_INVALID_PARAMETER;
    for(ULONG I=0;I<Count;I++){ULONG K;for(K=0;K<S->ChannelCount;K++)if(S->Channel[K].Frequency==Frequency[I])break;if(K==S->ChannelCount||(Target->Active&&!WcnProbeAllowed(Frequency[I],S->Channel[K].Flags)))return STATUS_INVALID_PARAMETER;}
    S->Target=*Target;S->ScanVdev=Vdev;S->RequestedCount=Count;if(Count)RtlCopyMemory(S->RequestedFrequency,Frequency,Count*sizeof(ULONG));
    S->ScanId=S6_SCAN_ID|((S->ScanId+1)&0xfff);
    S->RuntimeScan=TRUE;S->ScanSent=S->ScanStarted=S->ScanCompleted=S->AbortSent=FALSE;
    S->ScanReason=S->ForeignChannels=S->ApCount=0;
    S->AbortAttempts=0;S->ScanAt=S->CompletedAt=0;
    S->StartedAt=S->StateAt=WcnNowMs();S->State=4;

    return STATUS_SUCCESS;
}

NTSTATUS WcnStage6CancelRescan(PWCN_DEVICE Dev)
{
    PWCN_SCAN S=&Dev->Scan;NTSTATUS St;
    if(!S->RuntimeScan)return STATUS_SUCCESS;
    if(!S->ScanSent){RtlSecureZeroMemory(&S->Target,sizeof(S->Target));S->RuntimeScan=FALSE;S->ScanCompleted=TRUE;S->ScanReason=1;S->State=7;return STATUS_SUCCESS;}
    if(S->ScanCompleted||S->AbortSent)return STATUS_SUCCESS;
    St=S6AbortSend(Dev);if(NT_SUCCESS(St)){S->AbortSent=TRUE;}
    return St;
}
