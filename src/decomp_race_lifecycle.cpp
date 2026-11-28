// decomp_race_lifecycle.cpp - Behaviorally exact C++ decompilation of SLUS_209.10's
// race-start phase/clock, countdown mode query, RNG seeding, crash-orbit pause,
// Eve event-loop predicate, and contest lifecycle/cleanup functions.
//
// Every function preserves exact PS2 EE integer/float semantics (fmul_ps2), guest stack
// frame layout, callee-saved registers, return addresses ($ra), and $v0/$f0 state so
// EOD_DIFF verifies 0 mismatches while keeping all netplay, session-start, and editor
// preview hooks in their original wrapper files.
//
// Full ledger & struct offsets: docs/DECOMP_RACE_LIFECYCLE.md.
#include "decomp_race_lifecycle.h"

#include "core/diff_harness.h"
#include "ee.h"

#include <cstdint>

extern "C" {
// ---- Recompiled originals (config/hle.txt `wrap` entries) ----
void f_00176698_orig(void);
void f_00176798_orig(void);
void f_00176918_orig(void);
void f_001D3FB8_orig(void);
void f_00120FB8_orig(void);
void f_00114358_orig(void);

// ---- Guest callees (called via public symbols so hooks/traces/nested diffs apply) ----
void f_00105E90(void);
void f_00105FF8(void);
void f_00106900(void);
void f_0010BBA0(void);
void f_00114020(void);  // Main race frame loop
void f_00115578(void);
void f_001155C8(void);
void f_00115758(void);
void f_00115B70(void);
void f_00116B30(void);  // Unregister overlay object
void f_00116B38(void);  // Register overlay object
void f_00116BB8(void);
void f_00116C08(void);
void f_0011F4A0(void);
void f_0011F4A8(void);
void f_00120740(void);  // Record completed contest results
void f_00120A80(void);
void f_00120AC8(void);
void f_00120C28(void);
void f_00120E78(void);  // Record quit contest results
void f_00120F30(void);
void f_0012D730(void);
void f_0012E6D0(void);
void f_0012E6E0(void);
void f_00132798(void);  // Unload resource archive ("overlays.res")
void f_00132890(void);  // Load resource archive ("overlays.res")
void f_0013D460(void);
void f_00173118(void);
void f_00174258(void);
void f_00175758(void);
void f_00176710(void);  // RequestPhysicsPause (sets 0x003CDAC8 = 1)
void f_00176798(void);  // RaceGetModeState (public symbol)
void f_001B04A0(void);
void f_001D4078(void);
void f_001D41B0(void);  // Register player viewport slot
void f_001D9B60(void);
void f_001D9C78(void);
void f_00200B10(void);
void f_00200DA8(void);
void f_00200DB8(void);
void f_002031F8(void);
void f_00203238(void);  // IsDemoOrReplayActive
void f_00211378(void);  // ViewGetActiveViewportCount
void f_0022DA58(void);
void f_00237760(void);
void f_002393A8(void);
void f_002393B8(void);
void f_002A11A8(void);
void f_002A11E8(void);
void f_002B7610(void);
void f_002CE120(void);
void f_002CE160(void);
void f_002CE1D0(void);
void f_002DE1F8(void);
void f_0032EDA0(void);
void f_00331E48(void);  // DebugPrintf
void f_00337718(void);  // TimerGetMs
void f_003A5FD0(void);  // memset
}

namespace {

// ---- Guest register & memory helpers ----
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline uint64_t reg64(int r) { return g_ee.r[r].ud[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline void set64(int r, uint64_t v) { g_ee.r[r].ud[0] = v; }
inline int32_t lw(uint32_t a) { return static_cast<int32_t>(MEM_LW(a)); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline float bf(uint32_t u) {
    FReg f;
    f.u = u;
    return f.f;
}
inline uint32_t fb(float v) {
    FReg f;
    f.f = v;
    return f.u;
}
inline void call(void (*fn)(void), uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    fn();
}

struct Frame {
    uint32_t caller_sp;
    uint64_t ra;
    explicit Frame(uint32_t size) : caller_sp(reg(29)), ra(reg64(31)) {
        set(29, static_cast<int32_t>(caller_sp - size));
    }
    ~Frame() {
        set(29, static_cast<int32_t>(caller_sp));
        set64(31, ra);
    }
    uint32_t sp() const { return reg(29); }
};

// ---- Verified Guest Addresses & Constants ----

// Race tick, clock & mode globals
constexpr uint32_t kGlobalRaceTick = 0x003CDA8C;         // int32 simulation frame counter
constexpr uint32_t kGlobalRaceClockSec = 0x003CDACC;     // float elapsed race seconds
constexpr uint32_t kGlobalRaceModeState = 0x004472C0;    // int32 (0 = inactive, 2 = active countdown/race)
constexpr uint32_t kFltNtscFramePeriod = 0x3C88AB86u;    // 0.016683351f (~1 / 59.94 s)

// Race RNG seeding globals & constants
constexpr uint32_t kGlobalRngSeedEntropy = 0x00447370;   // int32 running entropy accumulator
constexpr int32_t kRngSeedModulus = 0x7530;              // 30000
constexpr uint32_t kRngEntropyStep1 = 0x00009947u;       // 39239
constexpr uint32_t kRngEntropyStep2 = 0x0002F1CDu;       // 192973
constexpr uint32_t kFmtSeedDebug = 0x003F8F38;           // "seed: %d, %d"

// Crash orbit pause globals & camera controller offsets
constexpr uint32_t kGlobalPhysCamType = 0x0048CB90;      // int32 active camera view mode
constexpr uint32_t kGlobalCrashOrbitStartSec = 0x0048D280;  // float start time (seconds)
constexpr uint32_t kGlobalCrashCamCtrlPtr = 0x003CE9D8;  // ptr to camera controller
constexpr uint32_t kCrashCamOrbitActiveOff = 0x54;       // int32 (set to 1)
constexpr uint32_t kCrashCamOrbitTimerAOff = 0x58;       // int32 (cleared to 0)
constexpr uint32_t kCrashCamOrbitTimerBOff = 0x5C;       // int32 (cleared to 0)
constexpr uint32_t kFltMsToSec = 0x3A83126Fu;            // 0.001f

// Eve event loop globals & EveTracker vtable offset
constexpr uint32_t kGlobalEveMode = 0x004394F0;          // int32 (3 = Single Event)
constexpr int32_t kEveModeSingleEvent = 3;
constexpr uint32_t kGlobalEveTrackerPtr = 0x003C4BF8;    // EveTracker*
constexpr uint32_t kEveTrackerVtableHasMoreOff = 0x2C;   // int32 HasMoreEvents(EveTracker*)

// Contest lifecycle globals, tables & struct offsets (0x00114358)
constexpr uint32_t kSessionContestInfoOff = 0x2C;        // contest info/result descriptor
constexpr uint32_t kSessionTrackIdOff = 0x9A4;           // track/event ID (2468)
constexpr uint32_t kStrOverlaysRes = 0x003EDB20;         // "overlays.res"
constexpr uint32_t kOverlayObjectTable = 0x003EDB30;     // uint32_t[9] overlay object pointers
constexpr uint32_t kOverlayObjectCount = 9;
constexpr uint32_t kGlobalPlayerViewportBase = 0x003C4118;  // +0x04, +0x20, +0x3C, +0x58 (stride 0x1C)
constexpr uint32_t kPlayerViewportFirstOff = 0x04;
constexpr uint32_t kPlayerViewportStride = 0x1C;
constexpr uint32_t kGlobalRacePhaseIndex = 0x003C3CE8;   // int32 (0 or 0xB)
constexpr uint32_t kGlobalRacePhaseTable = 0x003C3CF8;   // stride 0x18, +0x04 = init callback
constexpr uint32_t kRacePhaseEntryStride = 0x18;
constexpr uint32_t kRacePhaseInitFnOff = 0x04;
constexpr uint32_t kGlobalContestExitFlag = 0x00437BA8;  // int32 contest loop exit flag
constexpr uint32_t kGlobalRaceQuitFlag = 0x00437BAC;     // int32 race quit flag
constexpr uint32_t kGlobalRaceStateFlag = 0x00437BB0;    // int32 race state flag

// ---- 1. 0x00176698: RaceSessionInitPhase(int32_t tick) ----
// Sets the native race simulation tick and computes the corresponding NTSC clock in seconds.
void decomp_00176698(void) {
    const int32_t tick = static_cast<int32_t>(reg(4));
    const float clock_sec = fmul_ps2(static_cast<float>(tick), bf(kFltNtscFramePeriod));

    sw(kGlobalRaceTick, static_cast<uint32_t>(tick));
    sw(kGlobalRaceClockSec, fb(clock_sec));

    // Match EE scratch $v0 residue (lui $v0, 0x3D) for diff_run equivalence.
    set(2, 0x003D0000);
}

// ---- 2. 0x00176798: int32_t RaceGetModeState(void) ----
// Returns g_RaceModeState (0 = inactive, 2 = active countdown/race).
void decomp_00176798(void) {
    set(2, lw(kGlobalRaceModeState));
}

// ---- 3. 0x00176918: RaceSessionSeedRng(int32_t* seed_pair) ----
// Seeds a 2-word race RNG pair using TimerGetMs() + running entropy modulo 30000,
// then tail-calls DebugPrintf("seed: %d, %d", seed0, seed1).
void decomp_00176918(void) {
    const uint32_t dst = reg(4);
    int32_t sum2 = 0;
    int32_t seed0_out = 0;
    int32_t seed1_out = 0;

    {
        Frame frame(0x40);

        call(f_00337718, 0x0017693C);
        const uint32_t ms1 = reg(2);
        uint32_t entropy = lwu(kGlobalRngSeedEntropy);
        const int32_t sum1 = static_cast<int32_t>(ms1 + entropy);
        const int32_t seed0 = sum1 % kRngSeedModulus;
        entropy += kRngEntropyStep1;
        sw(dst + 0, static_cast<uint32_t>(seed0));
        sw(kGlobalRngSeedEntropy, entropy);

        call(f_00337718, 0x00176960);
        const uint32_t ms2 = reg(2);
        entropy = lwu(kGlobalRngSeedEntropy);
        seed0_out = lw(dst + 0);
        sum2 = static_cast<int32_t>(ms2 + entropy);
        seed1_out = sum2 % kRngSeedModulus;
        entropy += kRngEntropyStep2;
        sw(dst + 4, static_cast<uint32_t>(seed1_out));
        sw(kGlobalRngSeedEntropy, entropy);
    }

    // Tail-call DebugPrintf("seed: %d, %d", seed0, seed1) with caller's $sp and $ra restored.
    set(2, sum2);
    set(4, static_cast<int32_t>(kFmtSeedDebug));
    set(5, seed0_out);
    set(6, seed1_out);
    f_00331E48();
}

// ---- 4. 0x001D3FB8: StartCrashOrbitPause(void) ----
// Requests a physics pause and arms the 3-second crash-orbit camera when not in demo/replay,
// running single-viewport, and using a qualifying chase camera view (4, 5, 6, or 0x15).
void decomp_001D3FB8(void) {
    Frame frame(0x20);

    call(f_00203238, 0x001D3FC8);
    if (reg64(2) != 0) return;

    call(f_00211378, 0x001D3FD8);
    if (reg64(2) != 1) return;

    const int32_t cam_mode = lw(kGlobalPhysCamType);
    const bool qualifying_cam =
        (cam_mode == 4) || (cam_mode == 6) || (cam_mode == 5) || (cam_mode == 0x15);
    if (!qualifying_cam) {
        // Match EE scratch $v0 residue (last comparison immediate 0x15) for diff_run.
        set(2, 0x15);
        return;
    }

    call(f_00176710, 0x001D4028);
    call(f_00337718, 0x001D4030);
    const int32_t now_ms = static_cast<int32_t>(reg(2));
    const float start_sec = fmul_ps2(static_cast<float>(now_ms), bf(kFltMsToSec));

    const uint32_t cam_ctrl = lwu(kGlobalCrashCamCtrlPtr);
    sw(cam_ctrl + kCrashCamOrbitActiveOff, 1u);
    sw(cam_ctrl + kCrashCamOrbitTimerAOff, 0u);
    sw(kGlobalCrashOrbitStartSec, fb(start_sec));
    sw(cam_ctrl + kCrashCamOrbitTimerBOff, 0u);

    // Match EE scratch $v0 residue (cam_ctrl pointer) for diff_run.
    set(2, static_cast<int32_t>(cam_ctrl));
}

// ---- 5. 0x00120FB8: int32_t EveHasMoreEvents(void) ----
// Returns 0 in Single Event mode (g_EveMode == 3), otherwise queries EveTracker::HasMoreEvents().
void decomp_00120FB8(void) {
    Frame frame(0x10);

    if (lw(kGlobalEveMode) == kEveModeSingleEvent) {
        set(2, 0);
        return;
    }

    const uint32_t tracker = lwu(kGlobalEveTrackerPtr);
    const uint32_t vtable = lwu(tracker);
    const uint32_t has_more_fn = lwu(vtable + kEveTrackerVtableHasMoreOff);
    set(4, static_cast<int32_t>(tracker));
    g_ee.r[31].ud[0] = 0x00120FEC;
    ee_call(has_more_fn);
}

// ---- 6. 0x00114358: RunRaceContest(session_ptr, abort_flag_ptr, completed_out_ptr) ----
// Runs one complete contest: pre-race resource/overlay setup, retry loop around the main
// race frame loop (0x00114020), post-race replay/scoring, and resource teardown.
void decomp_00114358(void) {
    const uint32_t session_ptr = reg(4);
    const uint32_t abort_flag_ptr = reg(5);
    const uint32_t completed_out_ptr = reg(6);
    Frame frame(0x2D0);

    // Pre-race initialization
    call(f_002DE1F8, 0x00114394);
    if (reg64(2) != 0) {
        call(f_0011F4A8, 0x001143A4);
        call(f_0011F4A0, 0x001143AC);
    }

    set(4, lw(session_ptr + kSessionTrackIdOff));
    call(f_00200B10, 0x001143C0);

    set(4, static_cast<int32_t>(kStrOverlaysRes));
    call(f_00132890, 0x001143CC);

    call(f_002A11E8, 0x001143D4);
    call(f_00105E90, 0x001143DC);
    call(f_00173118, 0x001143E4);
    call(f_00116C08, 0x001143EC);

    uint32_t overlays[kOverlayObjectCount];
    for (uint32_t i = 0; i < kOverlayObjectCount; ++i) {
        overlays[i] = lwu(kOverlayObjectTable + i * 4);
        sw(frame.sp() + i * 4, overlays[i]);
    }
    for (uint32_t i = 0; i < kOverlayObjectCount; ++i) {
        set(4, static_cast<int32_t>(overlays[i]));
        call(f_00116B38, 0x00114444u + i * 8u);
    }

    // Main contest retry loop
    for (;;) {
        sw(kGlobalRaceQuitFlag, 0u);
        sw(kGlobalRaceStateFlag, 0u);

        for (uint32_t slot = 0; slot < 4; ++slot) {
            const uint32_t vp =
                lwu(kGlobalPlayerViewportBase + kPlayerViewportFirstOff + slot * kPlayerViewportStride);
            set(4, static_cast<int32_t>(vp));
            set(5, static_cast<int32_t>(slot));
            set(6, 0);
            call(f_001D41B0, 0x001144A8u + slot * 0x10u);
        }

        call(f_001D4078, 0x001144E0);
        call(f_0012E6E0, 0x001144E8);
        call(f_002393A8, 0x001144F0);

        call(f_002B7610, 0x001144F8);
        sw(kGlobalRacePhaseIndex, (reg64(2) != 0) ? 0xBu : 0u);

        call(f_001155C8, 0x00114514);

        const int32_t phase_idx = lw(kGlobalRacePhaseIndex);
        const uint32_t phase_entry =
            kGlobalRacePhaseTable + static_cast<uint32_t>(phase_idx) * kRacePhaseEntryStride;
        const uint32_t phase_init_fn = lwu(phase_entry + kRacePhaseInitFnOff);
        if (phase_init_fn != 0) {
            g_ee.r[31].ud[0] = 0x00114540;
            ee_call(phase_init_fn);
        }

        // Main race frame loop
        call(f_00114020, 0x00114548);

        call(f_0022DA58, 0x00114550);
        if (reg64(2) != 0) {
            sw(kGlobalRaceQuitFlag, 1u);
        }

        call(f_00120AC8, 0x00114560);
        const bool retry = (reg64(2) == 0) && (lw(kGlobalRaceQuitFlag) == 0);

        call(f_002393B8, 0x00114578);
        call(f_0012E6D0, 0x00114580);
        call(f_00115758, 0x00114588);

        call(f_00176798, 0x00114590);
        const int32_t finish_code = (reg64(2) == 0) ? 3 : (retry ? 1 : 2);
        set(4, finish_code);
        call(f_00175758, 0x001145B0);

        call(f_001B04A0, 0x001145B8);
        call(f_002CE1D0, 0x001145C0);
        call(f_002CE160, 0x001145C8);
        call(f_0012D730, 0x001145D0);

        call(f_00106900, 0x001145D8);
        if (reg64(2) != 0) {
            call(f_002CE120, 0x001145E8);
            if (reg64(2) == 0) {
                call(f_002B7610, 0x001145F8);
                if (reg64(2) == 0) {
                    call(f_0032EDA0, 0x00114608);
                    if (reg64(2) == 0) {
                        set(4, 0);
                        call(f_002031F8, 0x00114618);
                        set(4, static_cast<int32_t>(session_ptr));
                        call(f_0010BBA0, 0x00114620);
                    }
                }
            }
        }

        if (!retry) break;

        call(f_00115578, 0x00114630);
        call(f_00200DA8, 0x00114638);
        IRQ_CHECK();
    }

    // Post-race resource teardown
    call(f_00174258, 0x00114648);
    call(f_00105FF8, 0x00114650);
    call(f_002A11A8, 0x00114658);

    for (uint32_t i = 0; i < kOverlayObjectCount; ++i) {
        set(4, static_cast<int32_t>(overlays[i]));
        call(f_00116B30, 0x00114660u + i * 8u);
    }

    call(f_00116BB8, 0x001146A8);
    call(f_00237760, 0x001146B0);

    set(4, static_cast<int32_t>(kStrOverlaysRes));
    call(f_00132798, 0x001146BC);

    // Contest completion flags & scoring handoff
    if (lw(abort_flag_ptr) != 0) {
        sw(abort_flag_ptr, 0u);
        sw(kGlobalContestExitFlag, 0u);
        sw(completed_out_ptr, 1u);
        call(f_00120F30, 0x001146DC);
    }

    call(f_00120A80, 0x001146E8);
    if (reg64(2) != 0) {
        sw(completed_out_ptr, 1u);
    }

    call(f_0022DA58, 0x001146FC);
    if (reg64(2) != 0) {
        sw(completed_out_ptr, 0u);
    }

    call(f_00120C28, 0x0011470C);
    if (reg64(2) != 0 && lw(completed_out_ptr) != 0) {
        const int32_t quit_flag = lw(kGlobalRaceQuitFlag);
        const uint32_t contest_info = session_ptr + kSessionContestInfoOff;
        const uint32_t result_buf = frame.sp() + 0x30;
        const uint32_t extra_buf = frame.sp() + 0x130;

        call(f_0013D460, 0x00114728);

        set(4, static_cast<int32_t>(result_buf));
        call(f_001D9B60, 0x00114730);
        if (reg64(2) == 0) {
            set(4, static_cast<int32_t>(result_buf));
            set(5, 0);
            set(6, 0x100);
            call(f_003A5FD0, 0x00114748);

            set(4, static_cast<int32_t>(result_buf));
            set(5, static_cast<int32_t>(contest_info));
            call(f_00115B70, 0x00114754);
        }

        set(4, static_cast<int32_t>(extra_buf));
        set(5, 0x14);
        call(f_001D9C78, 0x00114760);
        const uint32_t extra_ptr = (reg64(2) != 0) ? extra_buf : 0u;

        set(4, static_cast<int32_t>(result_buf));
        set(5, static_cast<int32_t>(contest_info));
        set(6, static_cast<int32_t>(extra_ptr));
        if (quit_flag != 0) {
            call(f_00120E78, 0x0011477C);
        } else {
            call(f_00120740, 0x00114790);
        }
    }

    call(f_00200DB8, 0x00114798);
}

}  // namespace

extern "C" {

void eod_race_00176698(void) { diff_run(0x00176698, decomp_00176698, f_00176698_orig); }
void eod_race_00176798(void) { diff_run(0x00176798, decomp_00176798, f_00176798_orig); }
void eod_race_00176918(void) { diff_run(0x00176918, decomp_00176918, f_00176918_orig); }
void eod_race_001D3FB8(void) { diff_run(0x001D3FB8, decomp_001D3FB8, f_001D3FB8_orig); }
void eod_race_00120FB8(void) { diff_run(0x00120FB8, decomp_00120FB8, f_00120FB8_orig); }
void eod_race_00114358(void) { diff_run(0x00114358, decomp_00114358, f_00114358_orig); }

}  // extern "C"
