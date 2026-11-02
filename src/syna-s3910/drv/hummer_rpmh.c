/*
 * apps RSC ("RPMh") DRV-2 vote for the touch panel rails on hummer (SM8750).
 */
#include "hummer_rpmh.h"

#define HR_TCS_OFF        0xD00u
#define HR_TCS_STRIDE     0x2A0u
#define HR_ACTIVE_N       3u
#define HR_DRV            2u
#define HR_WRITE_ACK      (8u | (1u << 8) | (1u << 16))
#define HR_DB_MAGIC       0x0C0330DBu
#define HR_DB_ENTRY       24u
#define HR_DB_SIZE        0x20000u

typedef struct _HR_LAYOUT
{
    ULONG Wait;
    ULONG Control;
    ULONG Status;
    ULONG Enable;
    ULONG MsgId;
    ULONG Addr;
    ULONG Data;
    ULONG CmdStatus;
    ULONG CmdStride;
} HR_LAYOUT;

static const HR_LAYOUT HrV2 = { 0x10, 0x14, 0x18, 0x1C, 0x30, 0x34, 0x38, 0x3C, 20 };
static const HR_LAYOUT HrV3 = { 0x20, 0x24, 0x28, 0x2C, 0x34, 0x38, 0x3C, 0x40, 24 };

static const UCHAR HrSBase[2] = { 8u, 16u };
static const USHORT HrHBase[2] = { 136u, 144u };

static
__declspec(noinline)
ULONG
HrRd (
    _In_ PUCHAR Base,
    _In_ ULONG  Off
    )
{
    return READ_REGISTER_ULONG ((PULONG)(Base + Off));
}

static
__declspec(noinline)
VOID
HrWr (
    _In_ PUCHAR Base,
    _In_ ULONG  Off,
    _In_ ULONG  Value
    )
{
    WRITE_REGISTER_ULONG ((PULONG)(Base + Off), Value);
    KeMemoryBarrier ();
}

/*
 * All waits are hard bounded: this code runs during device start, so a wedged
 * RSC must fail the vote, never the boot. KeStallExecutionProcessor is used
 * instead of a thread sleep so it is safe regardless of IRQL.
 */
static
VOID
HrWaitUs (
    _In_ ULONG Microseconds
    )
{
    if (Microseconds > 1000) {
        Microseconds = 1000;
    }
    KeStallExecutionProcessor (Microseconds);
}

static
NTSTATUS
HrSync (
    _In_ PHRPMH Ctx,
    _In_ ULONG  Off,
    _In_ ULONG  Value
    )
{
    ULONG Index;
    ULONG Observed = 0;

    HrWr (Ctx->Rsc, Off, Value);
    for (Index = 0; Index < 20; Index++) {
        Observed = HrRd (Ctx->Rsc, Off);
        if (Observed == Value) {
            return STATUS_SUCCESS;
        }
        HrWaitUs (1000);
    }
    return STATUS_IO_DEVICE_ERROR;
}

static
USHORT
HrDb16 (
    _In_ PHRPMH Ctx,
    _In_ ULONG  Off
    )
{
    return (USHORT)(HrRd (Ctx->Db, Off & ~3u) >> (8u * (Off & 3u)));
}

static
BOOLEAN
HrWalk (
    _In_  PHRPMH Ctx,
    _In_  const ULONG Key[2],
    _In_  ULONG SBase,
    _In_  ULONG HBase,
    _Out_ PULONG Address
    )
{
    ULONG Matches = 0;
    BOOLEAN HaveAddr = FALSE;
    ULONG I;
    ULONG J;

    for (I = 0; I < 8; I++) {
        ULONG Sh = SBase + 16u * I;
        ULONG Entries;
        ULONG Count;

        if (HrDb16 (Ctx, Sh) == 0) {
            break;
        }
        Entries = HBase + HrDb16 (Ctx, Sh + 2);
        Count = HrDb16 (Ctx, Sh + 6);
        if ((Entries & 3u) != 0 || Entries + Count * HR_DB_ENTRY > HR_DB_SIZE) {
            continue;
        }
        for (J = 0; J < Count; J++) {
            ULONG E = Entries + HR_DB_ENTRY * J;
            ULONG K0 = HrRd (Ctx->Db, E);
            ULONG K1 = HrRd (Ctx->Db, E + 4);
            ULONG A;

            if (K0 != Key[0] || K1 != Key[1]) {
                continue;
            }
            A = HrRd (Ctx->Db, E + 16);
            Matches++;
            if ((A & 0xFu) == 0 && !HaveAddr) {
                *Address = A;
                HaveAddr = TRUE;
            }
            if (!HaveAddr) {
                *Address = A;
            }
        }
    }

    return Matches != 0;
}

static
NTSTATUS
HrFind (
    _In_  PHRPMH Ctx,
    _In_  const CHAR *Res,
    _Out_ PULONG    Address
    )
{
    ULONG Key[2] = { 0, 0 };
    ULONG I;
    ULONG S;
    ULONG B;

    for (I = 0; I < 8 && Res[I] != 0; I++) {
        Key[I / 4] |= (ULONG)(UCHAR)Res[I] << (8u * (I % 4));
    }

    if (Ctx->SBase != 0) {
        if (HrWalk (Ctx, Key, Ctx->SBase, Ctx->HBase, Address)) return STATUS_SUCCESS;
    }

    for (S = 0; S < 2; S++) {
        for (B = 0; B < 2; B++) {
            if (HrWalk (Ctx, Key, HrSBase[S], HrHBase[B], Address)) {
                Ctx->SBase = HrSBase[S];
                Ctx->HBase = HrHBase[B];
                return STATUS_SUCCESS;
            }
        }
    }

    return STATUS_NOT_FOUND;
}

NTSTATUS
HrpmhInit (
    _Out_ PHRPMH Ctx,
    _In_  PUCHAR Rsc,
    _In_  PUCHAR CmdDb
    )
{
    ULONG Id;
    ULONG Config;
    ULONG NTcs;
    ULONG NCpt;
    ULONG DbMagic;

    if (Ctx == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory (Ctx, sizeof (*Ctx));
    Ctx->Rsc = Rsc;
    Ctx->Db = CmdDb;

    if (Rsc == NULL || CmdDb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Id = HrRd (Ctx->Rsc, 0);
    Config = HrRd (Ctx->Rsc, 0x0C);
    Ctx->Major = (Id >> 16) & 0xFFu;
    Ctx->Minor = (Id >> 8) & 0xFFu;
    DbMagic = HrRd (Ctx->Db, 4);

    NTcs = (Config >> (6u * HR_DRV)) & 0x3Fu;
    NCpt = (Config >> 27) & 0x1Fu;

    if (!(Ctx->Major == 3 || Ctx->Major == 2) || NTcs < HR_ACTIVE_N || NTcs > 16 ||
        NCpt < 3 || NCpt > 16) {
        return STATUS_REVISION_MISMATCH;
    }

    if (DbMagic != HR_DB_MAGIC) return STATUS_NOT_FOUND;

    Ctx->Initialized = TRUE;
    return STATUS_SUCCESS;
}

static
NTSTATUS
HrVote (
    _In_ PHRPMH Ctx,
    _In_ const CHAR *Resource,
    _In_ ULONG MilliVolts,
    _In_ ULONG ExpectedAddress
    )
{
    const HR_LAYOUT *L = Ctx->Major >= 3 ? &HrV3 : &HrV2;
    ULONG Address = 0;
    ULONG IrqEn;
    ULONG IrqSt;
    ULONG Chosen = 0xFFFFFFFFu;
    ULONG Pass;
    ULONG T;
    ULONG Base;
    ULONG Bit;
    ULONG Offsets[3];
    ULONG Values[3];
    ULONG C;
    ULONG CmdBefore;
    ULONG IrqAfter;
    BOOLEAN Done = FALSE;
    BOOLEAN Triggered = FALSE;
    BOOLEAN CleanupOk = TRUE;
    ULONG Status;
    NTSTATUS NtStatus;
    NTSTATUS CleanupStatus;
    ULONGLONG Deadline;

    NtStatus = HrFind (Ctx, Resource, &Address);
    if (!NT_SUCCESS (NtStatus)) {
        return NtStatus;
    }
    if (Address != ExpectedAddress) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    IrqEn = HrRd (Ctx->Rsc, HR_TCS_OFF + 0);
    IrqSt = HrRd (Ctx->Rsc, HR_TCS_OFF + 4);
    if (IrqEn == MAXULONG || IrqSt == MAXULONG) {
        return STATUS_IO_DEVICE_ERROR;
    }

    for (Pass = 0; Pass < 2 && Chosen == 0xFFFFFFFFu; Pass++) {
        for (T = 0; T < HR_ACTIVE_N; T++) {
            ULONG B = HR_TCS_OFF + T * HR_TCS_STRIDE;
            ULONG St = HrRd (Ctx->Rsc, B + L->Status);
            ULONG Ctl = HrRd (Ctx->Rsc, B + L->Control);
            ULONG En = HrRd (Ctx->Rsc, B + L->Enable);
            ULONG Wt = HrRd (Ctx->Rsc, B + L->Wait);

            if (St == MAXULONG) {
                continue;
            }
            if (Ctl != 0 || En != 0 || Wt != 0 || (IrqSt & (1u << T)) != 0) {
                continue;
            }
            if (Pass == 0 && (St == 0 || St == 0xFFFFFFFFu)) {
                continue;
            }
            Chosen = T;
            break;
        }
    }

    if (Chosen == 0xFFFFFFFFu) {
        return STATUS_DEVICE_BUSY;
    }

    Base = HR_TCS_OFF + Chosen * HR_TCS_STRIDE;
    Bit = 1u << Chosen;

    if (!NT_SUCCESS (HrSync (Ctx, HR_TCS_OFF + 0, IrqEn & ~Bit))) {
        NtStatus = STATUS_IO_DEVICE_ERROR;
        goto cleanup;
    }

    Offsets[0] = 0u;
    Offsets[1] = 8u;
    Offsets[2] = 4u;
    Values[0] = MilliVolts;
    Values[1] = 7u;
    Values[2] = 1u;

    for (C = 0; C < 3; C++) {
        ULONG Cmd = Base + C * L->CmdStride;
        if (!NT_SUCCESS (HrSync (Ctx, Cmd + L->MsgId, HR_WRITE_ACK)) ||
            !NT_SUCCESS (HrSync (Ctx, Cmd + L->Addr, Address + Offsets[C])) ||
            !NT_SUCCESS (HrSync (Ctx, Cmd + L->Data, Values[C]))) {
            NtStatus = STATUS_IO_DEVICE_ERROR;
            goto cleanup;
        }
    }

    if (!NT_SUCCESS (HrSync (Ctx, Base + L->Wait, 7)) ||
        !NT_SUCCESS (HrSync (Ctx, Base + L->Enable, 7)) ||
        !NT_SUCCESS (HrSync (Ctx, Base + L->Control, 0)) ||
        !NT_SUCCESS (HrSync (Ctx, Base + L->Control, 1u << 16))) {
        NtStatus = STATUS_IO_DEVICE_ERROR;
        goto cleanup;
    }

    for (C = 0; C < 3; C++) {
        CmdBefore = HrRd(Ctx->Rsc, Base + L->CmdStatus + C * L->CmdStride);
        if (CmdBefore == MAXULONG || (CmdBefore & 0x10100u) != 0) {
            NtStatus = STATUS_IO_DEVICE_ERROR;
            goto cleanup;
        }
    }

    Triggered = TRUE;
    Deadline = KeQueryInterruptTime() + 1000000ULL;
    HrWr (Ctx->Rsc, Base + L->Control, (1u << 16) | (1u << 24));

    for (T = 0; T < 40 && !Done; T++) {
        BOOLEAN All = TRUE;
        ULONG C2;

        HrWaitUs (1000);
        for (C2 = 0; C2 < 3; C2++) {
            ULONG Cs = HrRd (Ctx->Rsc, Base + L->CmdStatus + C2 * L->CmdStride);
            if (Cs == 0xFFFFFFFFu || (Cs & 0x10100u) != 0x10100u) {
                All = FALSE;
            }
        }
        Status = HrRd (Ctx->Rsc, Base + L->Status);
        Done = All && Status != 0 && Status != MAXULONG;
        if (KeQueryInterruptTime() >= Deadline) {
            break;
        }
    }

    NtStatus = Done ? STATUS_SUCCESS : STATUS_IO_TIMEOUT;

cleanup:
    CleanupStatus = HrSync (Ctx, Base + L->Control, 1u << 16);
    CleanupOk = NT_SUCCESS(CleanupStatus) && CleanupOk;
    CleanupStatus = HrSync (Ctx, Base + L->Control, 0);
    CleanupOk = NT_SUCCESS(CleanupStatus) && CleanupOk;
    if (!Triggered || Done) {
        CleanupStatus = HrSync (Ctx, Base + L->Enable, 0);
        CleanupOk = NT_SUCCESS(CleanupStatus) && CleanupOk;
        CleanupStatus = HrSync (Ctx, Base + L->Wait, 0);
        CleanupOk = NT_SUCCESS(CleanupStatus) && CleanupOk;
        if (Done) {
            HrWr (Ctx->Rsc, HR_TCS_OFF + 8, Bit);
            for (T = 0; T < 20; T++) {
                IrqAfter = HrRd(Ctx->Rsc, HR_TCS_OFF + 4);
                if (IrqAfter != MAXULONG && (IrqAfter & Bit) == 0) {
                    break;
                }
                HrWaitUs(1000);
            }
            CleanupOk = T < 20 && CleanupOk;
        }
        if (CleanupOk) {
            CleanupStatus = HrSync (Ctx, HR_TCS_OFF + 0, IrqEn);
            CleanupOk = NT_SUCCESS(CleanupStatus);
        }
    }
    if (!NT_SUCCESS(NtStatus) || !CleanupOk) {
        Ctx->Faulted = TRUE;
    }

    if (NT_SUCCESS(NtStatus) && CleanupOk) {
        return STATUS_SUCCESS;
    }

    return NT_SUCCESS(NtStatus) ? STATUS_IO_DEVICE_ERROR : NtStatus;
}

NTSTATUS
HrpmhVoteLdo (
    _In_ PHRPMH Ctx,
    _In_ ULONG  Id,
    _In_ ULONG  MilliVolts
    )
{
    static const CHAR *const Suffix[3] = { "_E0", "_E1", "" };
    CHAR Name[12];
    NTSTATUS Last;
    ULONG S;
    ULONG ExpectedAddress;

    if (Ctx == NULL || !Ctx->Initialized || Ctx->Faulted) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if ((Id != 4 || MilliVolts != 1800) && (Id != 16 || MilliVolts != 3104)) {
        return STATUS_INVALID_PARAMETER;
    }
    ExpectedAddress = Id == 4 ? 0x40800u : 0x41900u;
    Last = STATUS_NOT_FOUND;

    for (S = 0; S < 4; S++) {
        ULONG P = 0;
        const CHAR *Q;

        if (S < 3) {
            Name[P++] = 'L';
            if (Id >= 10) {
                Name[P++] = (CHAR)('0' + Id / 10u);
            }
            Name[P++] = (CHAR)('0' + Id % 10u);
            Name[P++] = 'B';
            Q = Suffix[S];
            while (*Q != 0) {
                Name[P++] = *Q++;
            }
        } else {
            Name[P++] = 'l';
            Name[P++] = 'd';
            Name[P++] = 'o';
            Name[P++] = 'b';
            if (Id >= 10) {
                Name[P++] = (CHAR)('0' + Id / 10u);
            }
            Name[P++] = (CHAR)('0' + Id % 10u);
        }
        Name[P] = 0;

        Last = HrVote (Ctx, Name, MilliVolts, ExpectedAddress);
        if (NT_SUCCESS (Last)) {
            return STATUS_SUCCESS;
        }
        if (Last != STATUS_NOT_FOUND) {
            return Last;
        }
    }

    return Last;
}
