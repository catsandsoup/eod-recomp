// decomp_online_sim.cpp - Behaviorally exact C++ decompilation of SLUS_209.10's
// online simulation tick, active driver switcher, 6-byte human input replay recorder,
// and DetentionMaster participant role setup functions:
//
//   0x00177718  WorldStepTick / WorldSimulationStep (169 instructions)
//               Main per-tick simulation step boundary; netplay lockstep boundary
//               and core entry point for future 24/7 headless dedicated server.
//               (wrapped in src/netplay_trace.cpp)
//   0x00190A38  CarSetActiveDriver / DriverWrapperSetActive (26 instructions)
//               Switches vehicle active driver (car+0x1C1C wrapper) between pace-lap
//               autopilot (+0x08) and human/remote player (+0x0C) with lifecycle hooks.
//               (wrapped in src/netplay_trace.cpp)
//   0x00191548  HumanReplayRecordTick / ReplayRecordHumanTick (216 instructions)
//               Quantizes & packs per-tick human controller state into a 6-byte replay
//               record, and pauses/rewinds when replay playback reaches the last tick.
//               (wrapped in src/netplay_trace.cpp)
//   0x00165E10  DetentionMasterSetupRole / DetentionSetupParticipants (28 instructions)
//               Resets all Detention participant records (16-byte stride via 0x00165EF0)
//               and clears DetentionMaster scoring/role state words.
//               (wrapped in src/game_patches.cpp)
//
// Every replacement mirrors the exact PS2 EE FPU ops (fsub_ps2, fdiv_ps2, fmin_ps2,
// fmax_ps2, fmul_ps2, fadd_ps2, fcvtw_ps2), guest call order, return addresses ($ra),
// stack frames, and $v0 exit values so EOD_DIFF proves 0 mismatches against the
// recompiled originals.
//
// Full ledger, struct maps & MMO dedicated-server wave notes: docs/DECOMP_ONLINE_SIM.md.
#include "decomp_online_sim.h"

#include "core/diff_harness.h"
#include "ee.h"

#include <cstdint>

extern "C" {
// ---- Recompiled originals (config/hle.txt `wrap` entries) ----
void f_00177718_orig(void);
void f_00190A38_orig(void);
void f_00191548_orig(void);
void f_00165E10_orig(void);

// ---- Guest callees (invoked via public symbols so hooks/traces/nested diffs apply) ----
// WorldStepTick (0x00177718) callees:
void f_00335D08(void);  // ProfilerScopeBegin(tag, 0, 0)
void f_00335D00(void);  // ProfilerScopeEnd(tag, 0, 0)
void f_00175958(void);  // PreStepHookA
void f_001792E8(void);  // RaceTrackPreStep
void f_00177B40(void);  // SimSnapshotHistoryStep(force_flag)
void f_0021F140(void);  // InputManagerPreStep
void f_002CE130(void);  // AudioStepBegin
void f_001B5840(void);  // TimedDamageFeatUpdate
void f_00173120(void);  // EventModeRulesStep
void f_00202FB0(void);  // ReplayAndPadStep
void f_00268CD0(void);  // WorldPropsAndTriggersStep
void f_001BFA30(void);  // IsReplayOrOverrideActive
void f_001DA9F0(void);  // CameraPreSimStep
void f_001F0D78(void);  // IsPhysicsSubstepSuppressed
void f_001DA720(void);  // CameraAuxStep
void f_001E8AD8(void);  // ScriptedEventAndHudStep
void f_001380B0(void);  // ActiveHudStep
void f_00138170(void);  // ReplayHudStep
void f_001779C0(void);  // EntityMatrixOrthonormalizeRoundRobin
void f_001793B8(void);  // RaceTrackMidStep
void f_00180A00(void);  // HalfRateEffectsAndDebrisStep($f12 = 2/59.94s)
void f_00177CA0(void);  // DynamicEntitiesPhysicsStep
void f_00178258(void);  // DynamicEntitiesPostStep
void f_001B6070(void);  // PostStepFeatsAndStats
void f_002CE128(void);  // AudioStepEnd
void f_00176698(void);  // RaceSessionInitPhase(tick)
void f_0012D6A0(void);  // PostStepBookkeeping

// HumanReplayRecordTick (0x00191548) callees:
void f_00203248(void);  // IsReplayRecordingActive
void f_00203238(void);  // IsReplayPlaybackActive
void f_00176710(void);  // RequestPhysicsPause

// DetentionMasterSetupRole (0x00165E10) callee:
void f_00165EF0(void);  // DetentionParticipantReset(entry, keep_score_flag)
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
inline void call(void (*fn)(void), uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    fn();
}
inline void call_at(uint32_t fn, uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    ee_call(fn);
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
};

// ---- Verified Guest Addresses & Struct Offsets ----

// WorldStepTick (0x00177718) globals & entity offsets
constexpr uint32_t kGlobalRaceTick             = 0x003CDA8Cu;  // int32 simulation frame counter
constexpr uint32_t kGlobalWorldSimPaused       = 0x003CDAC4u;  // int32 nonzero when simulation paused
constexpr uint32_t kGlobalWorldProfileTag      = 0x003CDADCu;  // uint32 profiler tag handle
constexpr uint32_t kGlobalActiveDynCount       = 0x003CDAE4u;  // int32 active dynamic entity count
constexpr uint32_t kGlobalHalfRateParity       = 0x003CDAE8u;  // uint32 toggled ^= 1 each tick (30Hz phase)
constexpr uint32_t kGlobalActiveDynEntities    = 0x003CDBE8u;  // uint32[] active dynamic entity pointers
constexpr uint32_t kGlobalWorldPostStepFlag    = 0x003CE3ECu;  // uint32 cleared after DynamicEntitiesPostStep
constexpr uint32_t kGlobalWorldEntityArrayPtr  = 0x00447404u;  // uint32* pointer to world entity array
constexpr uint32_t kGlobalWorldEntityCount     = 0x00447408u;  // int32 total registered world entities
constexpr uint32_t kGlobalWorldCounterA        = 0x00447448u;  // uint32 current counter A
constexpr uint32_t kGlobalWorldCounterAPrev    = 0x0044744Cu;  // uint32 previous-tick snapshot of A
constexpr uint32_t kGlobalWorldCounterB        = 0x00447450u;  // uint32 current counter B
constexpr uint32_t kGlobalWorldCounterBPrev    = 0x00447454u;  // uint32 previous-tick snapshot of B
constexpr uint32_t kGlobalWorldStepScratchFlag = 0x004474A8u;  // uint32 cleared before subsystem updates
constexpr uint32_t kGlobalWorldCollScratchFlag = 0x004494B8u;  // uint32 cleared before RaceTrackMidStep
constexpr uint32_t kEntityVtablePtrOff         = 0x98u;        // entity+0x98 -> physics/sim vtable
constexpr uint32_t kEntityVtIsDynamicOff       = 0x3Cu;        // vtable+0x3C: bool IsActiveDynamic(entity)
constexpr uint32_t kEntityVtPreTickOff         = 0x40u;        // vtable+0x40: bool PreTick(entity)
constexpr uint32_t kFltHalfRateDt              = 0x3D08AB86u;  // 0.033366702f (2 / 59.94 s, 30Hz step)

// CarSetActiveDriver (0x00190A38) driver wrapper & driver vtable offsets
constexpr uint32_t kWrapperActiveDriverOff     = 0x10u;        // wrapper+0x10: active driver ptr (+0x08 auto / +0x0C human)
constexpr uint32_t kDriverVtOnActivateOff      = 0x2Cu;        // driver vtable+0x2C: OnActivate(driver)
constexpr uint32_t kDriverVtOnDeactivateOff    = 0x30u;        // driver vtable+0x30: OnDeactivate(driver)

// HumanReplayRecordTick (0x00191548) struct offsets, globals & quantization constants
constexpr uint32_t kReplayControllerOff        = 0x04u;        // replay_input+0x04: live input controller ptr
constexpr uint32_t kReplayCurrentRecordOff     = 0x08u;        // replay_input+0x08: ptr to current 6-byte record
constexpr uint32_t kReplayRecordedCountOff     = 0x0Cu;        // replay_input+0x0C: high-water recorded tick count
constexpr uint32_t kReplayBufferBaseOff        = 0x24u;        // replay_input+0x24: base of 6-byte record array
constexpr uint32_t kReplayMaxTicksOff          = 0x2Cu;        // replay_input+0x2C: buffer capacity in ticks
constexpr uint32_t kCarSteerSpeedParamOff      = 0x1B64u;      // car+0x1B64: float steer speed param passed to GetSteer
constexpr uint32_t kGlobalReplayRecordedTicks  = 0x00482560u;  // int32 global recorded tick count mirror
constexpr uint32_t kInputVtSteerOff            = 0x0Cu;        // float GetSteer(ctrl, $f12 speed_param)
constexpr uint32_t kInputVtThrottleOff         = 0x10u;        // float GetThrottle(ctrl)
constexpr uint32_t kInputVtBrakeOff            = 0x14u;        // float GetBrake(ctrl)
constexpr uint32_t kInputVtHandbrakeOff        = 0x18u;        // float GetHandbrake(ctrl)
constexpr uint32_t kInputVtClutchOff           = 0x1Cu;        // float GetClutch(ctrl)
constexpr uint32_t kInputVtGearOff             = 0x20u;        // int32 GetGear(ctrl)
constexpr uint32_t kInputVtBtnBit15Off         = 0x2Cu;        // bool  GetButtonBit15(ctrl)
constexpr uint32_t kInputVtBtnBit12Off         = 0x30u;        // bool  GetButtonBit12(ctrl)
constexpr uint32_t kInputVtBtnBit13Off         = 0x34u;        // bool  GetButtonBit13(ctrl)
constexpr uint32_t kInputVtBtnBit14Off         = 0x38u;        // bool  GetButtonBit14(ctrl)
constexpr uint32_t kFltZero                    = 0x00000000u;  // 0.0f
constexpr uint32_t kFltHalf                    = 0x3F000000u;  // 0.5f
constexpr uint32_t kFltOne                     = 0x3F800000u;  // 1.0f
constexpr uint32_t kFltSteerMin                = 0xC0400000u;  // -3.0f
constexpr uint32_t kFltSteerSpan               = 0x40C00000u;  // 6.0f
constexpr uint32_t kFltU16Max                  = 0x477FFF00u;  // 65535.0f
constexpr uint32_t kFltU8Max                   = 0x437F0000u;  // 255.0f

// DetentionMasterSetupRole (0x00165E10) struct offsets
constexpr uint32_t kDetentionParticipantsOff   = 0x04u;        // master+0x04: participant array base (stride 0x10)
constexpr uint32_t kDetentionCountOff          = 0x08u;        // master+0x08: int32 participant count
constexpr uint32_t kDetentionKeepFlagOff       = 0x0Cu;        // master+0x0C: int32 flag passed to 0x00165EF0
constexpr uint32_t kDetentionStateWord0Off     = 0x10u;        // master+0x10: cleared to 0
constexpr uint32_t kDetentionStateWord1Off     = 0x14u;        // master+0x14: cleared to 0
constexpr uint32_t kDetentionStateWord2Off     = 0x18u;        // master+0x18: cleared to 0
constexpr uint32_t kDetentionStateWord3Off     = 0x1Cu;        // master+0x1C: cleared to 0
constexpr uint32_t kDetentionEntryStride       = 0x10u;        // 16 bytes per Detention participant

// Quantizes a [0.0f, 1.0f] analog channel using exact EE FPU ops:
//   fcvtw_ps2(fadd_ps2(fmul_ps2(fmax_ps2(0.0f, fmin_ps2(v, 1.0f)), scale), 0.5f))
inline int32_t quantize_unit_ee(float v, float scale) {
    float clamped = fmin_ps2(v, bf(kFltOne));
    clamped = fmax_ps2(bf(kFltZero), clamped);
    return fcvtw_ps2(fadd_ps2(fmul_ps2(clamped, scale), bf(kFltHalf)));
}

// ----------------------------------------------------------------------------
// 0x00177718  WorldStepTick / WorldSimulationStep
// ----------------------------------------------------------------------------
void decomp_00177718(void) {
    {
        Frame frame(0xA0);

        set(4, lw(kGlobalWorldProfileTag));
        set(5, 0);
        set(6, 0);
        call(f_00335D08, 0x0017775Cu);

        call(f_00175958, 0x00177764u);
        call(f_001792E8, 0x0017776Cu);

        // Snapshot per-tick counters before advancing history/input.
        sw(kGlobalWorldCounterAPrev, lwu(kGlobalWorldCounterA));
        sw(kGlobalWorldCounterBPrev, lwu(kGlobalWorldCounterB));

        set(4, 0);
        call(f_00177B40, 0x00177794u);
        call(f_0021F140, 0x0017779Cu);

        const int32_t paused = lw(kGlobalWorldSimPaused);
        sw(kGlobalWorldStepScratchFlag, 0u);

        call(f_002CE130, 0x001777B0u);
        call(f_001B5840, 0x001777B8u);
        call(f_00173120, 0x001777C0u);
        call(f_00202FB0, 0x001777C8u);
        call(f_00268CD0, 0x001777D0u);
        call(f_001BFA30, 0x001777D8u);
        const uint32_t replay_or_override = reg(2);

        const uint32_t half_rate_parity = lwu(kGlobalHalfRateParity) ^ 1u;
        sw(kGlobalHalfRateParity, half_rate_parity);

        if (replay_or_override != 0u || paused == 0) {
            call(f_001DA9F0, 0x001777FCu);
            call(f_001F0D78, 0x00177804u);
            if (reg(2) == 0u) {
                call(f_001DA720, 0x00177814u);
            }

            call(f_001E8AD8, 0x0017781Cu);

            if (replay_or_override != 0u) {
                call(f_00138170, 0x0017784Cu);
            } else {
                call(f_001F0D78, 0x0017782Cu);
                if (reg(2) == 0u) {
                    call(f_001380B0, 0x0017783Cu);
                }
            }

            call(f_001779C0, 0x00177854u);
            sw(kGlobalWorldCollScratchFlag, 0u);
            call(f_001793B8, 0x00177860u);

            if (lwu(kGlobalHalfRateParity) != 0u) {
                g_ee.fpr[12].u = kFltHalfRateDt;
                call(f_00180A00, 0x00177880u);
            }

            // Build active dynamic entity list for this tick's physics and post-step passes.
            sw(kGlobalActiveDynCount, 0u);
            int32_t entity_count = lw(kGlobalWorldEntityCount);
            if (entity_count > 0) {
                uint32_t entities = lwu(kGlobalWorldEntityArrayPtr);
                for (int32_t i = 0; i < entity_count;) {
                    const uint32_t ent = lwu(entities + static_cast<uint32_t>(i << 2));
                    const uint32_t vt0 = lwu(ent + kEntityVtablePtrOff);
                    set(4, static_cast<int32_t>(ent));
                    call_at(lwu(vt0 + kEntityVtPreTickOff), 0x001778BCu);
                    if (reg(2) != 0u) {
                        const uint32_t vt1 = lwu(ent + kEntityVtablePtrOff);
                        set(4, static_cast<int32_t>(ent));
                        call_at(lwu(vt1 + kEntityVtIsDynamicOff), 0x001778D4u);
                        if (reg(2) != 0u) {
                            const int32_t active_idx = lw(kGlobalActiveDynCount);
                            sw(kGlobalActiveDynEntities + static_cast<uint32_t>(active_idx << 2), ent);
                            sw(kGlobalActiveDynCount, static_cast<uint32_t>(active_idx + 1));
                        }
                    }
                    entity_count = lw(kGlobalWorldEntityCount);
                    ++i;
                    if (i < entity_count) {
                        entities = lwu(kGlobalWorldEntityArrayPtr);
                    }
                }
            }

            if (replay_or_override == 0u) {
                call(f_00177CA0, 0x00177928u);
            }
            call(f_00178258, 0x00177930u);
            sw(kGlobalWorldPostStepFlag, 0u);
        } else {
            if (half_rate_parity != 0u) {
                g_ee.fpr[12].u = kFltHalfRateDt;
                call(f_00180A00, 0x00177954u);
            }
        }

        call(f_001B6070, 0x0017795Cu);
        call(f_002CE128, 0x00177964u);

        if (paused == 0) {
            set(4, lw(kGlobalRaceTick) + 1);
            call(f_00176698, 0x00177978u);
        }

        call(f_0012D6A0, 0x00177980u);
    }

    // Tail-call ProfilerScopeEnd(tag, 0, 0) with caller's $sp and $ra restored.
    set(4, lw(kGlobalWorldProfileTag));
    set(5, 0);
    set(6, 0);
    f_00335D00();
}

// ----------------------------------------------------------------------------
// 0x00190A38  CarSetActiveDriver / DriverWrapperSetActive
// ----------------------------------------------------------------------------
void decomp_00190A38(void) {
    Frame frame(0x30);

    const uint32_t wrapper = reg(4);
    const uint32_t new_driver = reg(5);
    const uint32_t prev_driver = lwu(wrapper + kWrapperActiveDriverOff);

    if (new_driver == prev_driver) {
        return;
    }

    if (prev_driver != 0u) {
        const uint32_t vt = lwu(prev_driver);
        const uint32_t fn = lwu(vt + kDriverVtOnDeactivateOff);
        set(2, static_cast<int32_t>(fn));
        set(4, static_cast<int32_t>(prev_driver));
        call_at(fn, 0x00190A74u);
    }

    sw(wrapper + kWrapperActiveDriverOff, new_driver);

    if (new_driver != 0u) {
        const uint32_t vt = lwu(new_driver);
        const uint32_t fn = lwu(vt + kDriverVtOnActivateOff);
        set(2, static_cast<int32_t>(fn));
        set(4, static_cast<int32_t>(new_driver));
        call_at(fn, 0x00190A8Cu);
    }
}

// ----------------------------------------------------------------------------
// 0x00191548  HumanReplayRecordTick / ReplayRecordHumanTick
// ----------------------------------------------------------------------------
void decomp_00191548(void) {
    bool tail_call_init_phase = false;

    {
        Frame frame(0x80);

        const uint32_t replay_input = reg(4);
        const uint32_t car = reg(5);

        const int32_t sim_tick = lw(kGlobalRaceTick);
        const int32_t max_ticks = lw(replay_input + kReplayMaxTicksOff);
        const uint32_t buf_base = lwu(replay_input + kReplayBufferBaseOff);
        const uint32_t steer_speed_bits = lwu(car + kCarSteerSpeedParamOff);

        const int32_t tick_idx = (sim_tick < max_ticks) ? sim_tick : (max_ticks - 1);
        const uint32_t rec_ptr = buf_base + static_cast<uint32_t>(tick_idx * 6);
        sw(replay_input + kReplayCurrentRecordOff, rec_ptr);

        call(f_00203248, 0x001915ACu);
        if (reg(2) != 0u) {
            const int32_t recorded_count = lw(replay_input + kReplayRecordedCountOff);
            if (tick_idx >= recorded_count) {
                const int32_t next_count = tick_idx + 1;
                sw(replay_input + kReplayRecordedCountOff, static_cast<uint32_t>(next_count));
                sw(kGlobalReplayRecordedTicks, static_cast<uint32_t>(next_count));
            }

            // 1) Steering: [-3.0f, +3.0f] -> uint16 [0..65535] at record+0x00
            uint32_t ctrl = lwu(replay_input + kReplayControllerOff);
            uint32_t rec_steer = lwu(replay_input + kReplayCurrentRecordOff);
            g_ee.fpr[12].u = steer_speed_bits;
            set(4, static_cast<int32_t>(ctrl));
            call_at(lwu(lwu(ctrl) + kInputVtSteerOff), 0x001915F8u);
            float steer_norm = fdiv_ps2(fsub_ps2(g_ee.fpr[0].f, bf(kFltSteerMin)), bf(kFltSteerSpan));
            const int32_t steer_q16 = quantize_unit_ee(steer_norm, bf(kFltU16Max));
            MEM_SH(rec_steer, static_cast<uint16_t>(steer_q16));

            // 2) Brake: [0.0f, 1.0f] -> uint8 [0..255] at record+0x03
            ctrl = lwu(replay_input + kReplayControllerOff);
            uint32_t rec = lwu(replay_input + kReplayCurrentRecordOff);
            set(4, static_cast<int32_t>(ctrl));
            call_at(lwu(lwu(ctrl) + kInputVtBrakeOff), 0x0019165Cu);
            MEM_SB(rec + 3u, static_cast<uint8_t>(quantize_unit_ee(g_ee.fpr[0].f, bf(kFltU8Max))));

            // 3) Throttle: [0.0f, 1.0f] -> uint8 [0..255] at record+0x02
            ctrl = lwu(replay_input + kReplayControllerOff);
            rec = lwu(replay_input + kReplayCurrentRecordOff);
            set(4, static_cast<int32_t>(ctrl));
            call_at(lwu(lwu(ctrl) + kInputVtThrottleOff), 0x00191690u);
            MEM_SB(rec + 2u, static_cast<uint8_t>(quantize_unit_ee(g_ee.fpr[0].f, bf(kFltU8Max))));

            // 4) Handbrake: upper 4 bits of quantized uint8 -> bits [3:0] of record+0x04
            ctrl = lwu(replay_input + kReplayControllerOff);
            rec = lwu(replay_input + kReplayCurrentRecordOff);
            set(4, static_cast<int32_t>(ctrl));
            call_at(lwu(lwu(ctrl) + kInputVtHandbrakeOff), 0x001916C4u);
            {
                const uint32_t q8 = static_cast<uint32_t>(quantize_unit_ee(g_ee.fpr[0].f, bf(kFltU8Max)));
                uint16_t flags = static_cast<uint16_t>(MEM_LH(rec + 4u));
                flags = static_cast<uint16_t>((flags & 0xFFF0u) | ((q8 & 0xFFu) >> 4));
                MEM_SH(rec + 4u, flags);
            }

            // 5) Clutch: upper 4 bits of quantized uint8 -> bits [7:4] of record+0x04
            ctrl = lwu(replay_input + kReplayControllerOff);
            rec = lwu(replay_input + kReplayCurrentRecordOff);
            set(4, static_cast<int32_t>(ctrl));
            call_at(lwu(lwu(ctrl) + kInputVtClutchOff), 0x0019170Cu);
            {
                const uint32_t q8 = static_cast<uint32_t>(quantize_unit_ee(g_ee.fpr[0].f, bf(kFltU8Max)));
                uint16_t flags = static_cast<uint16_t>(MEM_LH(rec + 4u));
                flags = static_cast<uint16_t>((flags & 0xFF0Fu) | (q8 & 0xF0u));
                MEM_SH(rec + 4u, flags);
            }

            // 6) Gear: (gear + 1) & 0xF -> bits [11:8] of record+0x04
            ctrl = lwu(replay_input + kReplayControllerOff);
            rec = lwu(replay_input + kReplayCurrentRecordOff);
            set(4, static_cast<int32_t>(ctrl));
            call_at(lwu(lwu(ctrl) + kInputVtGearOff), 0x00191750u);
            {
                const uint32_t gear_nibble = (static_cast<uint32_t>(reg(2) + 1u) & 0xFu) << 8;
                uint16_t flags = static_cast<uint16_t>(MEM_LH(rec + 4u));
                flags = static_cast<uint16_t>((flags & 0xF0FFu) | gear_nibble);
                MEM_SH(rec + 4u, flags);
            }

            // 7) Discrete button flags -> bits [12..15] of record+0x04
            auto pack_button_bit = [&](uint32_t vt_off, uint32_t ra, uint32_t shift, uint16_t clear_mask) {
                const uint32_t c = lwu(replay_input + kReplayControllerOff);
                const uint32_t r = lwu(replay_input + kReplayCurrentRecordOff);
                set(4, static_cast<int32_t>(c));
                call_at(lwu(lwu(c) + vt_off), ra);
                const uint16_t bit = static_cast<uint16_t>((reg64(2) != 0ull ? 1u : 0u) << shift);
                uint16_t flags = static_cast<uint16_t>(MEM_LH(r + 4u));
                flags = static_cast<uint16_t>((flags & clear_mask) | bit);
                MEM_SH(r + 4u, flags);
            };
            pack_button_bit(kInputVtBtnBit12Off, 0x00191780u, 12u, 0xEFFFu);
            pack_button_bit(kInputVtBtnBit13Off, 0x001917ACu, 13u, 0xDFFFu);
            pack_button_bit(kInputVtBtnBit14Off, 0x001917D8u, 14u, 0xBFFFu);
            pack_button_bit(kInputVtBtnBit15Off, 0x00191804u, 15u, 0x7FFFu);
        }

        // In replay playback mode, pause & resync phase once the final recorded tick is reached.
        call(f_00203238, 0x00191824u);
        if (reg(2) == 0u) {
            return;
        }

        const int32_t recorded_count = lw(replay_input + kReplayRecordedCountOff);
        if (tick_idx < recorded_count - 1) {
            set(2, 1);
            return;
        }

        call(f_00176710, 0x00191848u);
        tail_call_init_phase = true;
    }

    if (tail_call_init_phase) {
        set(4, lw(kGlobalRaceTick));
        f_00176698();
    }
}

// ----------------------------------------------------------------------------
// 0x00165E10  DetentionMasterSetupRole / DetentionSetupParticipants
// ----------------------------------------------------------------------------
void decomp_00165E10(void) {
    Frame frame(0x30);

    const uint32_t master = reg(4);
    int32_t count = lw(master + kDetentionCountOff);

    if (count > 0) {
        uint32_t participants = lwu(master + kDetentionParticipantsOff);
        for (int32_t i = 0; i < count;) {
            const uint32_t entry = participants + static_cast<uint32_t>(i) * kDetentionEntryStride;
            const int32_t keep_flag = lw(master + kDetentionKeepFlagOff);
            ++i;
            set(4, static_cast<int32_t>(entry));
            set(5, keep_flag);
            call(f_00165EF0, 0x00165E4Cu);

            count = lw(master + kDetentionCountOff);
            if (i < count) {
                participants = lwu(master + kDetentionParticipantsOff);
            }
        }
        set(2, 0);
    } else {
        set(2, count);
    }

    sw(master + kDetentionStateWord3Off, 0u);
    sw(master + kDetentionStateWord0Off, 0u);
    sw(master + kDetentionStateWord1Off, 0u);
    sw(master + kDetentionStateWord2Off, 0u);
}

}  // namespace

extern "C" void eod_online_sim_00177718(void) {
    diff_run(0x00177718u, decomp_00177718, f_00177718_orig);
}

extern "C" void eod_online_sim_00190A38(void) {
    diff_run(0x00190A38u, decomp_00190A38, f_00190A38_orig);
}

extern "C" void eod_online_sim_00191548(void) {
    diff_run(0x00191548u, decomp_00191548, f_00191548_orig);
}

extern "C" void eod_online_sim_00165E10(void) {
    diff_run(0x00165E10u, decomp_00165E10, f_00165E10_orig);
}
