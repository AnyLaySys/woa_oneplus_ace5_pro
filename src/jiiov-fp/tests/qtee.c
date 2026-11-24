#include "fingerprint.h"
#include <stdlib.h>
#include <stdio.h>

enum { Success, VendorNull, Unavailable, InvalidNull, BridgeFailure, RootFailure, SpiFailure,
    BadHeader, BadObject, AliasObject, Yield, BadSleep, Pending, DeleteFailure, ReleaseFailure, CallbackLoop };

static int Scenario;
static PUCHAR Memory;
static ULONG Allocations, Frees, Bridges, Deletes, Clients, Spis, ReleasedClients, ReleasedSpis, Callbacks;
static BOOLEAN CallbackPending;
static ULONGLONG Clock;
static ULONG Uid = 16;
static ULONG MemorySize, Apps, ReleasedApps, LoadScenario, CommandMode, MemoryObject;
static BOOLEAN CommandPending, RegionPending;
static ULONG RegionMode, Regions, ReleasedRegions, CaptureMode, CaptureStep, SensorStarts, SensorStops;
static ULONG Interrupted, Resumed;
static BOOLEAN AuditFailure, JvActive;
static ULONG ConfigMode, ConfigStep;
static ULONG StorageObject, StorageMemory;
static ULONG FileObject, FileMemory;
static ULONG RawSpiMode, RawSpiOpened, RawSpiReads, RawSpiCloses;
static ULONG FeatureMode, FeatureCommands, FeatureCancelled;
static ULONG SessionMode, SessionSamples, SessionFeatures;
static ULONG PreflightMode, PreflightCommands, PreflightCancelled, PreflightPosted;
static ULONG KeymasterMode, KeymasterCommands;

PUCHAR MmAllocateContiguousMemorySpecifyCache(ULONG size, PHYSICAL_ADDRESS low,
    PHYSICAL_ADDRESS high, PHYSICAL_ADDRESS boundary, int cache) {
    assert(size >= 8192 && size <= JFP_JV_MAX_IMAGE + 237568 && !(size & 4095) &&
        low.QuadPart == 0 && high.QuadPart == INT64_MAX &&
        boundary.QuadPart == 0 && cache == MmNonCached && !Memory);
    Memory = aligned_alloc(4096, size);
    MemorySize = size;
    assert(Memory);
    Allocations++;
    return Memory;
}

VOID MmFreeContiguousMemory(PUCHAR memory) {
    assert(memory && memory == Memory && !CallbackPending && Deletes == Bridges);
    for (ULONG i = 0; i < MemorySize; i++) assert(!memory[i]);
    free(memory);
    Memory = NULL;
    Frees++;
}

PHYSICAL_ADDRESS MmGetPhysicalAddress(PUCHAR memory) {
    assert(memory >= Memory && memory < Memory + MemorySize);
    PHYSICAL_ADDRESS address;
    address.QuadPart = (LONGLONG)(uintptr_t)memory;
    return address;
}

ULONGLONG KeQueryInterruptTime(void) { return Clock; }

NTSTATUS KeDelayExecutionThread(int mode, BOOLEAN alertable, LARGE_INTEGER *delay) {
    assert(mode == KernelMode && !alertable && delay->QuadPart <= 0 && delay->QuadPart >= -5000000);
    Clock += (ULONGLONG)-delay->QuadPart;
    return STATUS_SUCCESS;
}

static VOID Complete(ULONGLONG *r, LONG status, ULONG type, LONG result) {
    r[0] = (ULONGLONG)(LONGLONG)status;
    r[1] = type;
    r[2] = (ULONGLONG)(LONGLONG)result;
    r[3] = 0;
}

static VOID RequestCallback(BOOLEAN sleep) {
    ULONG *out = (ULONG *)(Memory + MemorySize - 4096);
    out[0] = 0;
    out[1] = 0x80000000u;
    out[2] = sleep ? 2 : 1;
    out[3] = sleep ? 1 : 0;
    if (sleep) {
        out[4] = UINT32_MAX - 3;
        out[5] = 4;
    }
    CallbackPending = TRUE;
}

VOID JfpScmCall(ULONGLONG *r) {
    ULONG id = (ULONG)r[0];
    if (id == 1) {
        assert(Interrupted && r[1] == 0x224 && r[2] == (uintptr_t)Memory && r[6] == 0x445566);
        Resumed++;
        Clients++;
        ((ULONG *)Memory)[5] = 0x101;
        Complete(r, 0, 0, 0);
        return;
    }
    if (id == 0x42000601) {
        assert(r[1] == 1 && r[2] == 0x32000602 && !r[3] && !r[4] && !r[5]);
        Complete(r, 0, Scenario == Unavailable ? 0 : 1, 0);
        return;
    }
    if (id == 0x72000602 && !r[2]) {
        assert(r[1] == 0x224 && !r[3] && !r[4] && !r[5]);
        Complete(r, Scenario == InvalidNull ? 0 : Scenario == VendorNull ? (LONG)0x80000001u : -2, 0, 0);
        return;
    }
    if (id == 0x42000c1e) {
        assert(Memory && r[1] == 4 && r[2] == ((uintptr_t)Memory | 6) &&
            r[3] == r[2] && r[4] == (MemorySize | 0x200) && r[5] == 3);
        if (Scenario == BridgeFailure) Complete(r, -5, 0, 0);
        else { Bridges++; Complete(r, 0, 0, 0x300); }
        return;
    }
    if (id == 0x42000c1d) {
        assert(Memory && r[1] == 1 && r[2] == 0x300 && !Clients && !Spis && !Apps && !Regions && !CallbackPending);
        if (Scenario == DeleteFailure) Complete(r, -12, 0, 0);
        else { Deletes++; Complete(r, 0, 0, 0); }
        return;
    }
    if (id == 0x72000601) {
        assert(CallbackPending && r[1] == 0x22 && r[2] == (uintptr_t)(Memory + MemorySize - 4096) && r[3] == 4096);
        ULONG *out = (ULONG *)(Memory + MemorySize - 4096);
        if (RegionPending) {
            ULONGLONG physical, size;
            assert(!out[0] && out[8] && out[8] != MemoryObject);
            memcpy(&physical, out + 10, 8);
            memcpy(&size, out + 12, 8);
            assert(physical == (uintptr_t)(Memory + 69632) && size == 12288 && out[14] == 6);
            assert(Memory[69632] == 0x7f);
            ULONG mapping = out[8];
            memset(out, 0, 4096);
            out[0] = 0x10002;
            out[1] = 0xffff;
            out[2] = 2;
            out[3] = MemoryObject;
            out[4] = mapping;
            ((ULONG *)Memory)[7] = 0x404;
            Regions++;
            CallbackPending = RegionPending = FALSE;
            Complete(r, 0, 0, 0);
            return;
        }
        if (CommandPending) {
            assert((LONG)out[0] == (CommandMode == 2 || CommandMode == 3 ? -96 : 0));
            ULONG mapping = out[8];
            if (CommandMode == 1 || CommandMode == 4) {
                ULONGLONG physical, size;
                memcpy(&physical, out + 10, 8);
                memcpy(&size, out + 12, 8);
                assert(physical == (uintptr_t)(Memory + (RegionMode ? 4096 : MemorySize - 8192)) && size == (RegionMode ? 65536 : 4096) && out[14] == 6);
                assert(mapping != MemoryObject && (mapping & 0xff000000u) == 0x8a000000u);
            }
            memset(out, 0, 4096);
            out[0] = 0x10002;
            out[1] = 0xffff;
            out[2] = CommandMode == 4 ? UINT32_MAX : CommandMode == 1 ? 2 : 1;
            out[3] = MemoryObject;
            out[4] = mapping;
            ULONG *data = (ULONG *)(Memory + (RegionMode ? 4096 : MemorySize - 8192));
            data[2] = 0;
            if (!PreflightMode) data[15] = 0x5446;
            if (ConfigMode) {
                if (!ConfigStep) assert(data[1] == 0x834);
                else {
                    static const ULONG ids[] = {0, 0xa, 0xb, 0xc, 0x10, 0x3eb, 0x3ec, 0x3f1, 0x3f3, 0x3f4, 0x3f9};
                    static const ULONG sizes[] = {128, 32, 32, 32, 32, 4, 4, 4, 4, 16, 4};
                    assert(data[1] == 0x835 && data[6] == 576 && data[12] == ids[ConfigStep - 1]);
                    assert(data[142] == sizes[ConfigStep - 1]);
                    data[14] = data[12];
                    if (ConfigMode == 2 && ConfigStep == 2) data[2] = 0x100;
                }
                ConfigStep++;
            } else if (CaptureMode && JvActive) {
                if (CaptureStep == 0) {
                    assert(data[0] == 2 && data[1] == 0x834 && data[6] == 176);
                    assert(!memcmp((PUCHAR)data + 48, "24811", 6));
                    if (CaptureMode == 3) data[2] = 0xfc;
                } else if (CaptureStep == 1) {
                    assert(data[1] == 0x7d0 && data[6] == 448);
                    assert(data[7] == 0xf000);
                    memcpy((PUCHAR)data + 448, "sensor init failure\n", 21);
                    ((PUCHAR)data)[0xf1bf] = 0x5a;
                    if (CaptureMode == 4) data[2] = 0x13ba;
                } else if (PreflightMode) {
                    PreflightCommands++;
                    if (data[1] == 0x7d5) {
                        assert(PreflightCommands == 1 && data[6] == 128);
                        if (PreflightMode == 2) data[2] = 0x1389;
                        if (PreflightMode != 3) {
                            ULONGLONG challenge = 123;
                            memcpy((PUCHAR)data + 0x78, &challenge, sizeof(challenge));
                        }
                    } else if (data[1] == 0x7d6) {
                        assert(PreflightCommands == 2 && data[6] == 200);
                        for (ULONG i = 48; i < 200; i++) assert(!((PUCHAR)data)[i]);
                        data[2] = PreflightMode == 4 ? 0 : 0x69;
                    } else if (data[1] == 0x7e3) {
                        assert(data[6] == 48);
                        PreflightCancelled++;
                    } else {
                        assert(data[1] == 0x7da && data[6] == 120);
                        PreflightPosted++;
                    }
                } else if (SessionMode) {
                    if (data[1] == 0x7dc) {
                        assert(!SessionSamples && data[6] == 136);
                    } else if (data[1] == 0x7dd) {
                        assert(data[6] == 160);
                        SessionSamples++;
                    } else if (data[1] == 0x7d4) {
                        data[27] = SessionMode == 2 && SessionSamples == 1 ? 0x13ed : 0;
                    } else if (data[1] == 0x7d3) {
                        assert(!data[22] && !data[23]);
                        SessionFeatures++;
                        /* Stock success leaves the optional output unchanged. */
                    } else {
                        assert(data[1] == 0x7e3);
                        FeatureCancelled++;
                    }
                } else if (FeatureMode) {
                    FeatureCommands++;
                    if (data[1] == 0x7dc) {
                        assert(FeatureCommands == 1 && data[6] == 136);
                        if (FeatureMode == 2) data[2] = 0x1389;
                    } else if (data[1] == 0x7dd) {
                        assert(FeatureCommands == 2 && data[6] == 160);
                        if (FeatureMode == 9) data[2] = 0x1389;
                    } else if (data[1] == 0x7d4) {
                        assert(FeatureCommands == 3 && data[6] == 184);
                        data[27] = FeatureMode == 5 ? 0x13ed : 0;
                    } else if (data[1] == 0x7d3) {
                        assert(FeatureCommands == 4 && data[6] == 160 && data[12] == 1 && data[13] == 3);
                        assert(!data[22] && !data[23]);
                        if (FeatureMode == 3) { data[22] = 0x138b; data[23] = 7; }
                        if (FeatureMode == 4) data[2] = 0x138b;
                        if (FeatureMode == 8) data[2] = UINT32_MAX;
                    } else {
                        assert(data[1] == 0x7e3 && data[6] == 48);
                        FeatureCancelled++;
                        if (FeatureMode == 6) data[2] = 0x1389;
                    }
                } else if (CaptureStep == 2) {
                    assert(data[1] == 0x7d7 && data[6] == 144 && data[7] == 0xf000);
                } else {
                    assert(CaptureStep == 3 && data[1] == 0x7d4 && data[6] == 184);
                    data[27] = CaptureMode == 2 ? 0x13ed : 0;
                }
                assert(Memory[69632] == 0x7f);
                CaptureStep++;
            }
            CallbackPending = CommandPending = FALSE;
            Complete(r, 0, 0, CommandMode == 2 || CommandMode == 3 ? -96 : 0);
            return;
        }
        assert((LONG)out[0] == (Scenario == BadSleep ? -96 : 0));
        Callbacks++;
        if (Scenario == CallbackLoop) { RequestCallback(FALSE); Complete(r, 0, 3, 0); }
        else {
            CallbackPending = FALSE;
            if (Scenario == BadSleep) Complete(r, 0, 0, -96);
            else { Clients++; ((ULONG *)Memory)[5] = 0x101; Complete(r, 0, 0, 0); }
        }
        return;
    }
    assert(id == 0x72000602 && r[1] == 0x224 && r[2] == (uintptr_t)Memory &&
        r[3] == MemorySize - 4096 && r[4] == (uintptr_t)(Memory + MemorySize - 4096) && r[5] == 4096 && !CallbackPending);
    ULONG *in = (ULONG *)Memory;
    if (in[0] == 0x101 && !in[1] && in[2] == 0x1001 && in[8] == 87) {
        assert(CaptureMode && JvActive);
        in[5] = 0x505;
        Complete(r, 0, 0, 0);
    } else if (in[0] == 0x505 && !in[1]) {
        assert(in[2] == 0x201 && in[3] == 40 && in[4] == 4 && (in[10] == 0x7000 || in[10] == 10));
        if (in[10] == 0x7000) { StorageObject = in[5]; StorageMemory = in[7]; }
        else { FileObject = in[5]; FileMemory = in[7]; }
        assert(in[5] && in[7] && in[5] != in[7]);
        Complete(r, 0, 0, 0);
    } else if (in[0] == 0x505 && in[1] == 0xffff) {
        ULONG *out = (ULONG *)(Memory + MemorySize - 4096);
        out[0] = 0x10002;
        out[1] = 0xffff;
        out[2] = 4;
        out[3] = StorageObject;
        out[4] = StorageMemory;
        out[5] = FileObject;
        out[6] = FileMemory;
        StorageObject = StorageMemory = 0;
        FileObject = FileMemory = 0;
        Complete(r, 0, 0, 0);
    } else if (in[0] == 0x303 && in[1] == 2 && CaptureMode) {
        assert(!in[2]);
        RegionMode = 1;
        Complete(r, 0, 0, 0);
    } else if (in[0] == 0x303 && !in[1] && KeymasterMode) {
        if (KeymasterMode >= 8 && in[24] == 0x220f) {
            assert(KeymasterCommands == 1 && in[2] == 0x424 && in[4] == 77 && in[26] == 69);
            const UCHAR prefix[] = {0xa1, 0x18, 0x2b, 0x58, 0x40};
            assert(!memcmp(Memory + 104, prefix, sizeof(prefix)));
            for (ULONG i = 0; i < 32; i++) {
                assert(Memory[109 + i] == 0x5a && Memory[141 + i] == 0x6b);
            }
            ULONG responseOffset = in[13];
            memset(Memory + responseOffset, 0, 256);
            ((ULONG *)(Memory + responseOffset))[0] = KeymasterMode == 8 ? UINT32_MAX : 0;
            KeymasterCommands++;
            Complete(r, 0, 0, 0);
            return;
        }
        assert(in[2] == 0x424 && in[3] == 96 && in[4] == 8 && in[5] == 104 && in[6] == 256);
        assert(in[7] == 360 && !in[8] && in[9] == 360 && in[10] == 4);
        assert(in[11] == 368 && in[12] == 8 && in[13] == 376 && in[14] == 256);
        assert(!in[15] && !in[17] && !in[19] && !in[21]);
        BOOLEAN parameters = KeymasterMode == 7 || (KeymasterMode >= 8 && !KeymasterCommands);
        assert(in[24] == (parameters ? 0x220eu : 0x205u) &&
            in[25] == (parameters ? 0u : 2u) && in[90] == 1);
        assert((LONG)in[26] == -1 && (LONG)in[94] == -1);
        KeymasterCommands++;
        memset(Memory + 376, 0x5a, 256);
        ULONG *response = (ULONG *)(Memory + 376);
        response[0] = KeymasterMode == 2 ? (ULONG)-10020 : KeymasterMode == 6 ? UINT32_MAX : 0;
        response[1] = KeymasterMode == 3 ? 257 : 12;
        response[2] = 152;
        if (KeymasterMode == 4) in[14] = 8;
        if (KeymasterMode == 5) in[14] = 257;
        if (KeymasterMode >= 8 && parameters) {
            const UCHAR first[] = {0xa2, 0x18, 0x29, 0x58, 0x20};
            const UCHAR second[] = {0x18, 0x2a, 0x58, 0x20};
            response[1] = KeymasterMode == 10 ? 72 : 73;
            memcpy((PUCHAR)response + 8, first, sizeof(first));
            memset((PUCHAR)response + 13, 0x5a, 32);
            memcpy((PUCHAR)response + 45, second, sizeof(second));
            memset((PUCHAR)response + 49, 0x6b, 32);
        }
        Complete(r, 0, 0, 0);
    } else if (in[0] == 0x303 && !in[1]) {
        assert(CommandMode && in[2] == 0x424 && in[3] == 96 && in[4] == 64);
        assert(in[13] == 1272 && in[14] == 1024);
        assert(in[24] == (ConfigMode ? ConfigStep ? 576 : 176 : CaptureMode && JvActive ?
            CaptureStep == 0 ? 176 : CaptureStep == 1 ? 0xf1c0 : FeatureMode || SessionMode || PreflightMode ?
            ((ULONG *)(Memory + 4096))[6] + 0xf000 : CaptureStep == 2 ? 0xf090 : 0xf0b8 : 408));
        assert(in[15] && !in[17] && !in[19] && !in[21]);
        MemoryObject = in[15];
        ULONG *data = (ULONG *)(Memory + (RegionMode ? 4096 : MemorySize - 8192));
        if (!ConfigMode && !(CaptureMode && JvActive))
            assert(!data[0] && data[1] == UINT32_MAX && data[6] == 408 && data[101] == UINT32_MAX);
        assert(data[2] == UINT32_MAX);
        ULONG *out = (ULONG *)(Memory + MemorySize - 4096);
        memset(out, 0, 4096);
        out[1] = 0x80000000u;
        out[2] = 0;
        out[3] = 0x1110;
        out[4] = CommandMode == 3 ? 4096 : 40;
        out[5] = 20;
        out[6] = CommandMode == 2 ? 0x8a007777u : MemoryObject;
        CallbackPending = CommandPending = TRUE;
        Complete(r, 0, 3, 0);
    } else if (in[0] == 1) {
        assert(in[1] == 5 && in[2] == 0x1100 && !in[3] && !in[4] && !in[5]);
        if (Interrupted && !Resumed) { Complete(r, 1, 0, 0); r[6] = 0x445566; }
        else if (Scenario == RootFailure) Complete(r, 0, 0, 10);
        else if (Scenario == Pending) Complete(r, 2, 0, 0);
        else if (Scenario == BadObject) { in[5] = 0x80000001u; Complete(r, 0, 0, 0); }
        else if (Scenario == Yield || Scenario == BadSleep || Scenario == CallbackLoop) {
            RequestCallback(Scenario == BadSleep);
            Complete(r, 0, 3, 0);
        } else {
            Clients++;
            in[5] = 0x101;
            if (Scenario == BadHeader) in[2] = UINT32_MAX;
            Complete(r, 0, 0, 0);
        }
    } else if (in[1] == 0xffff) {
        assert(!in[2]);
        if (Scenario == ReleaseFailure) Complete(r, -12, 0, 0);
        else {
            if (in[0] == 0x101) { assert(Clients && !Spis && !Apps && !Regions); Clients--; ReleasedClients++; }
            else if (in[0] == 0x303) { assert(Apps && Spis); Apps--; ReleasedApps++; }
            else if (in[0] == 0x404) { assert(Regions && !Apps); Regions--; ReleasedRegions++; }
            else { assert(in[0] == 0x202 && Spis && !Apps); Spis--; ReleasedSpis++; }
            Complete(r, 0, 0, 0);
        }
    } else if (in[0] == 0x202 && RawSpiMode) {
        if (in[1] <= 1) {
            assert(in[2] == 1 && in[3] == 32 && in[4] == 4 && in[8] == 2);
            if (!in[1]) {
                assert(!RawSpiOpened);
                if (RawSpiMode == 2) { Complete(r, 0, 0, 10); return; }
                RawSpiOpened = 1;
            } else {
                assert(RawSpiOpened);
                RawSpiOpened = 0;
                RawSpiCloses++;
            }
            Complete(r, 0, 0, 0);
        } else {
            static const ULONG config[11] = {1, 1, 1, 2, 1, 8, 25000000, 0, 0, 0, 0};
            assert(RawSpiOpened && in[1] == 4 && in[2] == 0x23 && in[3] == 96 && in[4] == 4);
            assert(in[5] == 104 && in[6] == 44 && in[7] == 152 && in[8] == 4);
            assert(in[9] == 160 && in[10] == 8 && in[11] == 168 && in[12] == 4);
            assert(!memcmp(Memory + 104, config, sizeof(config)) && Memory[152] == 0x15);
            assert(Memory[153] == 2 + RawSpiReads);
            RawSpiReads++;
            ((ULONG *)(Memory + 160))[0] = 4;
            ((ULONG *)(Memory + 160))[1] = RawSpiMode == 3 ? 3 : 4;
            Memory[171] = Memory[153] == 2 ? 0x46 : 0x54;
            if (RawSpiMode == 4) in[11] = UINT32_MAX;
            Complete(r, 0, 0, RawSpiMode == 5 ? 10 : 0);
        }
    } else if (in[0] == 0x202 && RegionMode) {
        if (!in[1]) {
            assert(in[2] == 0x1101 && in[3] == 40 && in[5]);
            JvActive = in[4] == 27;
            assert(in[4] == (JvActive ? 27u : 28u));
            assert(!memcmp(Memory + 40, JvActive ? "/odm/vendor/firmware/uff_jv" : "/odm/vendor/firmware/uff_spi", in[4]));
            MemoryObject = in[5];
            ULONG *out = (ULONG *)(Memory + MemorySize - 4096);
            memset(out, 0, 4096);
            out[1] = 0x80000000u;
            out[3] = 0x1110;
            out[4] = 40;
            out[5] = 20;
            out[6] = MemoryObject;
            RegionPending = CallbackPending = TRUE;
            Complete(r, 0, 3, 0);
        } else {
            assert(in[1] == 2 && in[2] == 0x1011 && in[3] == 40 && in[4] == (JvActive ? 27u : 28u) && in[5] == 72 && in[6] == 4 && Regions);
            Apps++;
            in[7] = 0x303;
            Complete(r, 0, 0, 0);
        }
    } else if (in[0] == 0x202) {
        assert(LoadScenario && in[1] == 1 && in[2] == 0x1012 && in[3] == 48 &&
            in[4] == 8200 && in[5] == 8248 && in[6] == (KeymasterMode ? 11u : 7u) &&
            in[7] == (KeymasterMode ? 8264u : 8256u) && in[8] == 64 && !in[9]);
        assert(Memory[48] == 0x7f && !memcmp(Memory + 8248, KeymasterMode ? "keymaster64" : "uff_spi", in[6]));
        if (LoadScenario == 1) Complete(r, 0, 0, 16);
        else {
            Apps++;
            in[9] = 0x303;
            if (LoadScenario == 2) in[7] = UINT32_MAX;
            Complete(r, 0, 0, 0);
        }
    } else {
        assert(in[0] == 0x101 && in[1] == 0 && in[2] == 0x1001 && in[3] == 32 &&
            in[4] == 4 && in[8] == Uid && !in[5]);
        if (Scenario == SpiFailure) Complete(r, 0, 0, -96);
        else if (Scenario == AliasObject) { in[5] = 0x101; Complete(r, 0, 0, 0); }
        else { Spis++; in[5] = 0x202; Complete(r, 0, 0, 0); }
    }
}

NTSTATUS JfpQteeTrace(JFP_TEE *tee, ULONG phase, const ULONGLONG *registers, NTSTATUS result) {
    (void)phase; (void)registers; (void)result;
    return AuditFailure && tee->Pending ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
NTSTATUS JfpQteeTaLog(JFP_TEE *tee, const UCHAR *data, ULONG size) {
    assert(tee->Info.Command == 0x7d0 && size == 20 && !memcmp(data, "sensor init failure\n", size));
    return CaptureMode == 4 ? STATUS_IO_DEVICE_ERROR : STATUS_SUCCESS;
}
NTSTATUS JfpQteeStorage(JFP_TEE *tee, BOOLEAN normal) { (void)tee; (void)normal; return STATUS_SUCCESS; }
VOID JfpQteeStorageClose(JFP_TEE *tee) { (void)tee; }
NTSTATUS JfpQteeStoragePrepare(VOID) { return STATUS_SUCCESS; }
NTSTATUS JfpSensorClockOn(JFP_DEVICE *device) { (void)device; return STATUS_SUCCESS; }
NTSTATUS JfpSensorOn(JFP_DEVICE *device) { (void)device; SensorStarts++; return CaptureMode || RawSpiMode ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED; }
NTSTATUS JfpSensorOff(JFP_DEVICE *device) { (void)device; SensorStops++; return STATUS_SUCCESS; }

int main(void) {
    for (Scenario = Success; Scenario <= CallbackLoop; Scenario++) {
        JFP_DEVICE device = {0};
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        CallbackPending = FALSE;
        Clock = 0;
        assert(JfpQteeProbe(&device, 0) == STATUS_INVALID_PARAMETER && !Allocations);
        NTSTATUS status = JfpQteeProbe(&device, Uid);
        if (Scenario == Success || Scenario == VendorNull || Scenario == Yield) {
            assert(status == STATUS_SUCCESS && device.Tee.Info.Client == 0x101 && device.Tee.Info.Service == 0x202);
            assert(ReleasedClients == 1 && ReleasedSpis == 1);
        } else assert(!NT_SUCCESS(status));
        if (Scenario == Unavailable || Scenario == InvalidNull) assert(!Allocations && !Frees);
        else if (Scenario == Pending || Scenario == CallbackLoop) {
            assert(device.Tee.Pending && device.Tee.Info.Pinned && !Frees && !Deletes);
            assert(JfpQteeProbe(&device, Uid) == STATUS_INVALID_DEVICE_STATE && !Frees);
            free(Memory);
            Memory = NULL;
        } else if (Scenario == DeleteFailure || Scenario == ReleaseFailure) {
            assert(device.Tee.Info.Pinned && !Frees);
            int original = Scenario;
            Scenario = Success;
            assert(JfpQteeRelease(&device) == STATUS_SUCCESS);
            Scenario = original;
            assert(!Memory && Frees == Allocations);
        } else assert(!Memory && Frees == Allocations && Deletes == Bridges && !Clients && !Spis);
        if (Scenario == BadHeader || Scenario == BadObject || Scenario == AliasObject || Scenario == InvalidNull)
            assert(status == STATUS_DEVICE_PROTOCOL_ERROR);
        if (Scenario == BadSleep) assert(Callbacks == 1 && !Clock);
        if (Scenario == CallbackLoop) assert(Callbacks == 256);
    }
    Scenario = Success;
    for (Uid = 122; Uid <= 2033; Uid += 1911) {
        JFP_DEVICE device = {0};
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        CallbackPending = FALSE;
        assert(JfpQteeProbe(&device, Uid) == STATUS_SUCCESS && device.Tee.Info.Uid == Uid);
        assert(!device.Tee.Memory && !Clients && !Spis && Allocations == Frees && Bridges == Deletes);
    }
    UCHAR image[8200] = {0x7f, 'E', 'L', 'F', 2, 1};
    image[18] = 183;
    image[32] = 64;
    image[54] = 56;
    image[56] = 1;
    Uid = 122;
    for (LoadScenario = 1; LoadScenario <= 3; LoadScenario++) {
        JFP_DEVICE device = {0};
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = 0;
        NTSTATUS status = JfpQteeLoadProbe(&device, image, sizeof(image));
        assert(!Memory && !Apps && !Clients && !Spis && Allocations == Frees && Bridges == Deletes);
        if (LoadScenario == 1) assert(status == STATUS_NOT_SUPPORTED && !device.Tee.Info.App && !ReleasedApps);
        else {
            assert(device.Tee.Info.App == 0x303 && ReleasedApps == 1);
            assert(status == (LoadScenario == 2 ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_SUCCESS));
        }
        image[54] = 55;
        assert(JfpQteeLoadProbe(&device, image, sizeof(image)) == STATUS_INVALID_PARAMETER && !Memory);
        image[54] = 56;
        memset(image + 72, 0xff, 8);
        assert(JfpQteeLoadProbe(&device, image, sizeof(image)) == STATUS_INVALID_PARAMETER && !Memory);
        memset(image + 72, 0, 8);
    }
    LoadScenario = 3;
    for (CommandMode = 1; CommandMode <= 4; CommandMode++) {
        JFP_DEVICE device = {0};
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = 0;
        NTSTATUS status = JfpQteeSpiProbe(&device, image, sizeof(image), 1);
        if (CommandMode == 1) {
            assert(status == STATUS_SUCCESS && device.Tee.Info.Maps == 1 && device.Tee.Info.Releases == 2);
            assert(!Memory && !device.Tee.Info.LiveObjects && !device.Tee.Info.Pinned && device.Tee.Info.Data[15] == 0x5446);
        } else if (CommandMode == 2 || CommandMode == 3) {
            assert(status == STATUS_NOT_SUPPORTED && !device.Tee.Info.Maps && !Memory && !device.Tee.Info.LiveObjects);
        } else {
            assert(status == STATUS_DEVICE_PROTOCOL_ERROR && device.Tee.Info.Pinned && device.Tee.Info.LiveObjects == 2);
            free(Memory);
            Memory = NULL;
        }
    }
    CommandMode = 0;
    LoadScenario = 3;
    for (KeymasterMode = 1; KeymasterMode <= 10; KeymasterMode++) {
        Allocations = Frees = Bridges = Deletes = Clients = Spis = Apps = ReleasedApps = 0;
        KeymasterCommands = SensorStarts = SensorStops = 0;
        JFP_DEVICE keymaster = {0};
        NTSTATUS status = JfpQteeKeymasterProbe(&keymaster, image, sizeof(image), KeymasterMode >= 8 ? 9 : KeymasterMode == 7 ? 8 : 7);
        assert(status == (KeymasterMode == 1 || KeymasterMode == 7 || KeymasterMode == 9 ? STATUS_SUCCESS : KeymasterMode == 2 || KeymasterMode == 6 || KeymasterMode == 8 ?
            STATUS_IO_DEVICE_ERROR : STATUS_DEVICE_PROTOCOL_ERROR));
        assert(KeymasterCommands == (KeymasterMode == 8 ? 2u : KeymasterMode == 9 ? 3u : 1u) && !SensorStarts && !SensorStops);
        assert(!Memory && !Clients && !Spis && !Apps && Allocations == Frees && Bridges == Deletes);
        assert(!keymaster.Tee.Info.Pinned && !keymaster.Tee.Info.LiveObjects);
        for (ULONG i = 3; i < RTL_NUMBER_OF(keymaster.Tee.Info.Response); i++) assert(!keymaster.Tee.Info.Response[i]);
        if (KeymasterMode != 7)
            for (ULONG i = 0; i < RTL_NUMBER_OF(keymaster.Tee.Info.Data); i++) assert(!keymaster.Tee.Info.Data[i]);
    }
    KeymasterMode = 0;
    RegionMode = 1;
    Regions = ReleasedRegions = 0;
    Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
    Apps = ReleasedApps = 0;
    JFP_DEVICE regionDevice = {0};
    assert(JfpQteeJvProbe(&regionDevice, image, sizeof(image)) == STATUS_SUCCESS);
    assert(!Memory && !Regions && ReleasedRegions == 1 && ReleasedApps == 1);
    assert(regionDevice.Tee.Info.Region == 0x404 && regionDevice.Tee.Info.App == 0x303 && regionDevice.Tee.Info.Maps == 1);
    for (CaptureMode = 1; CaptureMode <= 4; CaptureMode++) {
        CaptureStep = SensorStarts = SensorStops = 0;
        RegionMode = 1;
        JvActive = FALSE;
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = Regions = ReleasedRegions = 0;
        CommandMode = 1;
        JFP_DEVICE captureDevice = {0};
        NTSTATUS status = JfpQteeCaptureProbe(&captureDevice, image, sizeof(image), image, sizeof(image));
        assert(status == (CaptureMode == 1 ? STATUS_SUCCESS : CaptureMode == 2 ? STATUS_DEVICE_DATA_ERROR : STATUS_IO_DEVICE_ERROR));
        assert(!Memory && !Clients && !Spis && !Apps && !Regions && captureDevice.Tee.Info.Pinned == 0);
        assert(Allocations == 2 && Frees == 2 && SensorStarts == 1 && SensorStops == 1);
        assert(CaptureStep == (CaptureMode == 3 ? 1 : CaptureMode == 4 ? 2 : 4));
        assert(captureDevice.Tee.Info.Stage == (CaptureMode == 1 ? 11 : 7));
        if (CaptureMode == 4) {
            assert(captureDevice.Tee.Info.CommandStatus == 0x13ba);
            assert(captureDevice.Tee.Info.TaLogSize == 20 && captureDevice.Tee.Info.TaLogStatus == STATUS_IO_DEVICE_ERROR);
        }
    }
    CaptureMode = RegionMode = CommandMode = 1;
    CaptureStep = SensorStarts = SensorStops = 0;
    JvActive = FALSE;
    Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
    Apps = ReleasedApps = Regions = ReleasedRegions = 0;
    JFP_DEVICE initDevice = {0};
    assert(JfpQteeInitProbe(&initDevice, image, sizeof(image), image, sizeof(image)) == STATUS_SUCCESS);
    assert(CaptureStep == 2 && initDevice.Tee.Info.Command == 0x7d0 && !initDevice.Tee.Info.InitResult);
    assert(initDevice.Tee.Info.Flow[5] == STATUS_INVALID_DEVICE_STATE && !Memory && !Clients && !Spis && !Apps && !Regions);
    assert(!initDevice.Tee.Info.LiveObjects && !initDevice.Tee.Info.Pinned && Allocations == Frees && SensorStops == 1);
    CaptureMode = RegionMode = CommandMode = LoadScenario = 0;
    for (ConfigMode = 1; ConfigMode <= 2; ConfigMode++) {
        ConfigStep = 0;
        RegionMode = CommandMode = 1;
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = Regions = ReleasedRegions = 0;
        JFP_DEVICE configDevice = {0};
        NTSTATUS status = JfpQteeConfigProbe(&configDevice, image, sizeof(image));
        assert(status == (ConfigMode == 1 ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR));
        assert(ConfigStep == JFP_CONFIG_COUNT + 1 && !Memory && !Clients && !Spis && !Apps && !Regions);
        assert(Allocations == Frees && Bridges == Deletes && !configDevice.Tee.Info.Pinned);
        assert(configDevice.Tee.Info.Config[1].Result == (ConfigMode == 1 ? 0 : 0x100));
        assert(configDevice.Tee.Info.Config[JFP_CONFIG_COUNT - 1].Data[0] == 0x3f9);
    }
    ConfigMode = RegionMode = CommandMode = 0;
    Uid = 16;
    for (ULONG fail = 0; fail < 2; fail++) {
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = Regions = ReleasedRegions = Resumed = 0;
        Interrupted = 1;
        AuditFailure = fail;
        JFP_DEVICE interruptedDevice = {0};
        NTSTATUS status = JfpQteeProbe(&interruptedDevice, Uid);
        if (fail) {
            assert(status == STATUS_IO_DEVICE_ERROR && interruptedDevice.Tee.Pending && interruptedDevice.Tee.Info.Pinned && !Frees);
            AuditFailure = FALSE;
            assert(JfpQteeContinue(&interruptedDevice) == STATUS_SUCCESS);
        } else assert(status == STATUS_SUCCESS && Resumed == 1);
        assert(!Memory && !Clients && !Spis && Allocations == Frees);
    }
    Interrupted = 0;
    for (RawSpiMode = 1; RawSpiMode <= 5; RawSpiMode++) {
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        RawSpiOpened = RawSpiReads = RawSpiCloses = SensorStarts = SensorStops = 0;
        JFP_DEVICE rawDevice = {0};
        NTSTATUS status = JfpQteeSpiServiceProbe(&rawDevice);
        assert(status == (RawSpiMode == 1 ? STATUS_SUCCESS : RawSpiMode == 3 ? STATUS_DEVICE_DATA_ERROR :
            RawSpiMode == 4 ? STATUS_DEVICE_PROTOCOL_ERROR : STATUS_NOT_SUPPORTED));
        assert(!Memory && !Clients && !Spis && !RawSpiOpened && Allocations == Frees && Bridges == Deletes);
        assert(SensorStarts == 1 && SensorStops == 1);
        assert(RawSpiCloses == (RawSpiMode == 2 ? 0 : 1));
        if (RawSpiMode == 1) assert(rawDevice.Tee.Info.Data[0] == 0x5446 && RawSpiReads == 2);
        else assert(RawSpiReads == (RawSpiMode == 2 ? 0 : 1));
    }
    RawSpiMode = 0;
    for (FeatureMode = 1; FeatureMode <= 9; FeatureMode++) {
        CaptureMode = RegionMode = CommandMode = 1;
        CaptureStep = SensorStarts = SensorStops = FeatureCommands = FeatureCancelled = 0;
        JvActive = FALSE;
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = Regions = ReleasedRegions = 0;
        Uid = 122;
        JFP_DEVICE featureDevice = {0};
        NTSTATUS status = JfpQteeFeatureProbe(&featureDevice, image, sizeof(image), image, sizeof(image));
        assert(status == (FeatureMode == 1 || FeatureMode == 7 ? STATUS_SUCCESS : FeatureMode == 3 || FeatureMode == 5 ?
            STATUS_DEVICE_DATA_ERROR : STATUS_IO_DEVICE_ERROR));
        assert(!Memory && !Clients && !Spis && !Apps && !Regions && Allocations == Frees && Bridges == Deletes);
        assert(!featureDevice.Tee.Info.Pinned && SensorStarts == 1 && SensorStops == 1);
        assert(FeatureCancelled == (FeatureMode == 2 ? 0 : 1));
        if (FeatureMode == 1 || FeatureMode == 7) assert(!featureDevice.Tee.Info.AuthStartResult &&
            !featureDevice.Tee.Info.CaptureResult && !featureDevice.Tee.Info.FeatureCommandResult &&
            !featureDevice.Tee.Info.FeatureResult && !featureDevice.Tee.Info.CancelResult);
        if (FeatureMode == 2 || FeatureMode == 5 || FeatureMode == 9)
            assert(featureDevice.Tee.Info.FeatureResult == UINT32_MAX);
        if (FeatureMode == 3) assert(featureDevice.Tee.Info.FeatureValue == 7);
        if (FeatureMode == 8) assert(featureDevice.Tee.Info.FeatureCommandResult == UINT32_MAX);
    }
    FeatureMode = 0;
    for (SessionMode = 1; SessionMode <= 2; SessionMode++) {
        CaptureMode = RegionMode = CommandMode = 1;
        CaptureStep = SensorStarts = SensorStops = SessionSamples = SessionFeatures = FeatureCancelled = 0;
        JvActive = FALSE;
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = Regions = ReleasedRegions = 0;
        JFP_DEVICE session = {0};
        assert(JfpQteeSessionSample(&session) == STATUS_INVALID_DEVICE_STATE);
        assert(JfpQteeSessionBegin(&session, image, sizeof(image), image, sizeof(image)) == STATUS_SUCCESS);
        assert(session.Tee.Info.SessionActive && session.Tee.Info.Pinned && session.Tee.VerifyStarted);
        ULONG allocations = Allocations, calls = session.Tee.Info.Calls;
        assert(JfpQteeSessionBegin(&session, image, sizeof(image), image, sizeof(image)) == STATUS_INVALID_DEVICE_STATE);
        assert(Allocations == allocations && session.Tee.Info.Calls == calls);
        assert(JfpQteeSessionSample(&session) == (SessionMode == 1 ? STATUS_SUCCESS : STATUS_DEVICE_DATA_ERROR));
        assert(session.Tee.Info.SampleCount == 1 && session.Tee.Info.SessionActive);
        assert(JfpQteeSessionSample(&session) == STATUS_SUCCESS);
        assert(session.Tee.Info.SampleCount == 2 && Allocations == allocations && SensorStarts == 1);
        assert(SessionSamples == 2 && SessionFeatures == (SessionMode == 1 ? 2u : 1u));
        assert(JfpQteeSessionEnd(&session) == STATUS_SUCCESS && FeatureCancelled == 1);
        assert(!session.Tee.Info.SessionActive && !session.Tee.Info.Pinned && !session.Tee.VerifyStarted);
        assert(!Memory && !Clients && !Spis && !Apps && !Regions && Allocations == Frees && Bridges == Deletes);
        assert(JfpQteeSessionSample(&session) == STATUS_INVALID_DEVICE_STATE);
        assert(SensorStarts == 1 && SensorStops == 1);
    }
    SessionMode = 0;
    for (PreflightMode = 1; PreflightMode <= 4; PreflightMode++) {
        CaptureMode = RegionMode = CommandMode = 1;
        CaptureStep = SensorStarts = SensorStops = PreflightCommands = PreflightCancelled = PreflightPosted = 0;
        JvActive = FALSE;
        Allocations = Frees = Bridges = Deletes = Clients = Spis = ReleasedClients = ReleasedSpis = Callbacks = 0;
        Apps = ReleasedApps = Regions = ReleasedRegions = 0;
        JFP_DEVICE preflightDevice = {0};
        NTSTATUS status = JfpQteeEnrollPreflight(&preflightDevice, image, sizeof(image), image, sizeof(image));
        assert(status == (PreflightMode == 4 ? STATUS_SUCCESS : PreflightMode == 3 ?
            STATUS_DEVICE_PROTOCOL_ERROR : STATUS_IO_DEVICE_ERROR));
        assert(PreflightCancelled == 1 && PreflightPosted == 1);
        assert(PreflightCommands == (PreflightMode == 2 || PreflightMode == 3 ? 3u : 4u));
        assert(!preflightDevice.Tee.Info.SampleCount && !preflightDevice.Tee.Info.Pinned &&
            !preflightDevice.Tee.Info.LiveObjects && !preflightDevice.Tee.Info.SessionActive);
        if (PreflightMode == 2 || PreflightMode == 3) assert(preflightDevice.Tee.Info.MissingAuthResult == UINT32_MAX);
        else assert(preflightDevice.Tee.Info.MissingAuthResult == (PreflightMode == 4 ? 0 : 0x69));
        assert(!Memory && !Clients && !Spis && !Apps && !Regions && Allocations == Frees && Bridges == Deletes);
        assert(SensorStarts == 1 && SensorStops == 1);
    }
    puts("QTEE transport, rejected services, SPI service ABI, callback bounds and cleanup checks passed");
    return 0;
}
