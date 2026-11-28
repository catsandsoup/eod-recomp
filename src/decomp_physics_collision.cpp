// decomp_physics_collision.cpp - Behaviorally exact C++ decompilation of
// SLUS_209.10's vehicle recovery countdown, timed-damage feat / race director
// update, wheel ground query, and vehicle physics step + wheel sampler:
//
//   0x001B8370  CarUpdateOutOfBoundsRecovery (348 instructions)
//               Evaluates wrong-way, off-track, rollover, and stuck/<4-wheels
//               recovery timers, sets HUD countdown deadline, and triggers
//               auto-reset via 0x001B63E8 when enabled.
//               (wrapped in src/hle_recovery.cpp)
//   0x001B5840  TimedDamageFeatUpdate (523 instructions)
//               Per-tick race director & feat update: spectator cycling,
//               finish grace timer, inline Deadly Ambition timed-damage feat,
//               start countdown release, time-limit expiration, and standings.
//               (wrapped in src/game_patches.cpp)
//   0x00212500  WheelGroundQuery (64 instructions)
//               Queries static track SCOL ground (0x00227950) and dynamic/overlay
//               collision (0x00217240), keeping whichever surface is higher.
//               (wrapped in src/custom_map_physics.cpp)
//   0x001DE7B0  ChassisSweptBoxCollide / VehiclePhysicsStep (576 instructions)
//               Uploads VU0 physics program, runs per-car rigid-body/suspension
//               step (eod_vehicle_physics_step) pipelined with 4-wheel fore/aft
//               dual-probe ground sampling via WheelGroundQuery (0x00212500).
//               (wrapped in src/custom_map_physics.cpp)
//
// Every replacement mirrors the exact PS2 EE FPU ops (fadd_ps2, fsub_ps2,
// fmul_ps2, fmin_ps2, fmax_ps2, frsqrt_ps2, fcmp_*), guest call order, return
// addresses ($ra), stack frames, VU0 staging/collection, and $v0 exit values so
// EOD_DIFF proves 0 mismatches against the recompiled originals.
//
// Full ledger, struct maps & MMO dedicated-server notes:
// docs/DECOMP_PHYSICS_COLLISION.md.
#include "decomp_physics_collision.h"

#include "core/diff_harness.h"
#include "core/memory.h"
#include "ee.h"
#include "physics_transfer.h"

#include <cstdint>
#include <cstring>

extern "C" {
// ---- Recompiled originals (config/hle.txt `wrap` entries) ----
void f_001B8370_orig(void);
void f_001B5840_orig(void);
void f_00212500_orig(void);
void f_001DE7B0_orig(void);

// ---- Guest callees (invoked via public symbols so hooks/traces/nested diffs apply) ----
// CarUpdateOutOfBoundsRecovery (0x001B8370) callees:
void f_00210F98(void);  // RaceGetState
void f_001741C0(void);  // CareerIsFreeRoamActive
void f_00179888(void);  // IsRaceFinishedOrPostRace
void f_00179868(void);  // IsRaceCameraOverride
void f_001F0D78(void);  // IsPhysicsSubstepSuppressed
void f_0013B0D0(void);  // TrackSplineIsWrongWay(spline_state, car_forward_vec)
void f_001B63E8(void);  // CarTriggerAutoRecovery(slot_id, car)

// TimedDamageFeatUpdate (0x001B5840) callees:
void f_001BFA30(void);  // IsReplayOrOverrideActive
void f_001D3F98(void);  // CameraGetTargetSlot(viewport)
void f_001D4190(void);  // CameraSetTargetSlot(slot, flags)
void f_001766E8(void);  // IsSimPausedOrLocked
void f_001BAD10(void);  // DriverStatusPreUpdate
void f_002CE180(void);  // AudioOrCutsceneBlockingFinish
void f_00179920(void);  // GetLeaderDriverIndex(exclude_slot)
void f_00210F88(void);  // RaceSetState(state)
void f_001B7198(void);  // DriverStartRaceAtTime(driver, start_time)
void f_0018A950(void);  // RaceStartNotify(handle, mode)
void f_00201BB8(void);  // ReplayInitRaceHeader(header, count, flags)
void f_001767A8(void);  // GetEventRuleFlags
void f_001741D8(void);  // IsCountdownStartReady
void f_00257BB8(void);  // TriggerStartLights(a0, a1, a2, f12)
void f_001BB538(void);  // SetRaceBannerState(a0, a1)
void f_001BB708(void);  // ClearRaceBannerFlags
void f_0039BD40(void);  // isnanf(x)
void f_0039BC30(void);  // isinff(x)
void f_001B7E58(void);  // UpdateDriverStandings
void f_001BB8A0(void);  // AdvanceSinglePlayerBanner
void f_001BB848(void);  // IsDemoBannerReadyA
void f_001BB850(void);  // IsDemoBannerReadyB

// WheelGroundQuery (0x00212500) callees:
void f_00227950(void);  // ScolQueryStaticGround(pos, norm, mat, cache)
void f_00227830(void);  // ScolGetSectorDynamicMask(cache)
void f_00217220(void);  // GetGlobalDynamicCollisionMask
void f_00217240(void);  // QueryDynamicOverlayGround(mask, pos, norm, mat)

// ChassisSweptBoxCollide / VehiclePhysicsStep (0x001DE7B0) callees:
void f_001E0830(void);  // WaitForVif0MicroprogramUpload(scratch_ptr)
void f_00212500(void);  // WheelGroundQuery (public wrapper -> custom map hook)
}

namespace {

// ---- Guest register & memory helpers ----
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline int32_t reg_s32(int r) { return static_cast<int32_t>(g_ee.r[r].uw[0]); }
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

// ---- Verified Guest Addresses, Constants & Struct Offsets ----

// CarUpdateOutOfBoundsRecovery (0x001B8370)
constexpr uint32_t kEventTypeFlags           = 0x00484FECu;  // 0x484FD8 + 0x14
constexpr uint32_t kRecoveryAutoResetEnabled = 0x00485098u;  // 0x480000 + 20632
constexpr uint32_t kRecoveryCheckEnabled     = 0x0048509Cu;  // 0x480000 + 20636
constexpr uint32_t kRaceModeControllerPtr    = 0x004850B4u;  // 0x480000 + 20660
constexpr uint32_t kRecoverySuppressedFlag   = 0x004852C4u;  // 0x480000 + 21188

constexpr uint32_t kCarMagicPCAR             = 0x50434152u;  // 'PCAR'
constexpr uint32_t kEventTypeSpecialCheck    = 0x00004214u;
constexpr uint32_t kEventTypeReversedLanes   = 0x00402112u;
constexpr uint32_t kEventFlagArenaNoWrongWay = 0x00020000u;
constexpr uint32_t kEventTypeSpecialOffTrack = 0x00042119u;

constexpr uint32_t kFloatNegOne              = 0xBF800000u;  // -1.0f
constexpr uint32_t kFloatWrongWaySpeedMin    = 0x400E38E4u;  // 2.2222223f (20/9 m/s)
constexpr uint32_t kFloatRolloverUpYMax      = 0x3DCCCCCDu;  // 0.1f
constexpr uint32_t kFloatStuckSpeedMax       = 0x3FAAAAABu;  // 1.3333334f (4/3 m/s)
constexpr uint32_t kFloatRecoveryGraceWindow = 0x3F666666u;  // 0.9f

// TimedDamageFeatUpdate (0x001B5840)
constexpr uint32_t kGlobalCheatFlags         = 0x003C4190u;  // 0x3C0000 + 16784
constexpr uint32_t kGlobalRaceTimeSec        = 0x003CDACCu;  // 0x3D0000 - 9524
constexpr uint32_t kGlobalCamCycleDelta      = 0x003CE910u;  // 0x3D0000 - 5872
constexpr uint32_t kGlobalNanSentinel        = 0x003F0FF4u;  // 0x3F0000 + 4084
constexpr uint32_t kFinishEligibleMaskBase   = 0x00484F58u;
constexpr uint32_t kDemoBannerResultFlag     = 0x00484FD0u;  // 0x480000 + 20432
constexpr uint32_t kSinglePlayerBannerCount  = 0x00485024u;  // 0x480000 + 20516
constexpr uint32_t kReplayHeaderBuf          = 0x00485030u;
constexpr uint32_t kCountdownStarted         = 0x00485094u;  // 0x480000 + 20628
constexpr uint32_t kFeatEnabledFlag          = 0x004850A0u;  // 0x480000 + 20640
constexpr uint32_t kRaceStartHookPtr         = 0x004850B8u;  // 0x480000 + 20664
constexpr uint32_t kDriverCount              = 0x0048522Cu;  // 0x480000 + 21036
constexpr uint32_t kDriverArrayBase          = 0x00485230u;
constexpr uint32_t kSelectedDriverIndex      = 0x00485280u;  // 0x480000 + 21120
constexpr uint32_t kHumanPlayerCount         = 0x004852A0u;  // 0x480000 + 21152
constexpr uint32_t kCountdownBannerActive    = 0x004852A4u;  // 0x480000 + 21156
constexpr uint32_t kSinglePlayerBannerDone   = 0x004852C0u;  // 0x480000 + 21184
constexpr uint32_t kDemoBannerCycleCounter   = 0x004852D0u;  // 0x4852C8 + 8
constexpr uint32_t kStandingsOrderBase       = 0x004852F0u;
constexpr uint32_t kCountdownDeadline        = 0x00485390u;  // 0x480000 + 21392
constexpr uint32_t kRaceTimeLimit            = 0x00485394u;  // 0x480000 + 21396
constexpr uint32_t kFinishGraceDeadline      = 0x00485398u;  // 0x480000 + 21400
constexpr uint32_t kFinishGraceActive        = 0x0048539Cu;  // 0x480000 + 21404
constexpr uint32_t kCountdownArmed           = 0x004853A0u;  // 0x480000 + 21408

constexpr uint32_t kFloatFinishGraceExtend   = 0x47C35000u;  // 100000.0f
constexpr uint32_t kFloatTimedDamagePerTick  = 0x39DAAC09u;  // 0.0004170838f
constexpr uint32_t kFloatOne                 = 0x3F800000u;  // 1.0f
constexpr uint32_t kFloatTwo                 = 0x40000000u;  // 2.0f
constexpr uint32_t kFloatFive                = 0x40A00000u;  // 5.0f
constexpr uint32_t kFloatSixty               = 0x42700000u;  // 60.0f

// ChassisSweptBoxCollide / VehiclePhysicsStep (0x001DE7B0)
constexpr uint32_t kDmacVif0Chcr             = 0x10008000u;
constexpr uint32_t kDmacVif0Qwc              = 0x10008020u;
constexpr uint32_t kDmacVif0Tadr             = 0x10008030u;
constexpr uint32_t kDmacStat                 = 0x1000E010u;
constexpr uint32_t kDmacEnabler              = 0x1000E020u;
constexpr uint32_t kVu0VehicleMicroprogAddr  = 0x003BC590u;

constexpr uint32_t kPhysicsScolCacheArrayPtr = 0x00494D18u;  // 0x490000 + 19736
constexpr uint32_t kPhysicsCarCount          = 0x00494D28u;  // 0x490000 + 19752
constexpr uint32_t kPhysicsBodyArrayPtr      = 0x00494D30u;  // 0x490000 + 19760
constexpr uint32_t kPhysicsWheelArrayPtr     = 0x00494D34u;  // 0x490000 + 19764
constexpr uint32_t kPhysicsParamArrayPtr     = 0x00494D38u;  // 0x490000 + 19768
constexpr uint32_t kPhysicsDisabledArrayPtr  = 0x00494D3Cu;  // 0x490000 + 19772

constexpr uint32_t kBodyStride               = 0x00B0u;
constexpr uint32_t kWheelStride              = 0x01F0u;
constexpr uint32_t kParamStride              = 0x0180u;
constexpr uint32_t kScolCacheStride          = 0x0014u;

constexpr uint32_t kFloatHalf                = 0x3F000000u;  //  0.5f
constexpr uint32_t kFloatNegHalf             = 0xBF000000u;  // -0.5f
constexpr uint32_t kFloatFallbackGroundY     = 0xC2C80000u;  // -100.0f

// ---------------------------------------------------------------------------
// 1. CarUpdateOutOfBoundsRecovery (0x001B8370)
// ---------------------------------------------------------------------------
inline void update_condition_timer(uint32_t sub_timer, bool active_now, uint32_t now_bits) {
    const uint32_t prev = lwu(sub_timer + 12u);
    const bool was_active = (prev != 0u);
    if (active_now != was_active) {
        const uint32_t next = prev ^ 1u;
        sw(sub_timer + 12u, next);
        if (next != 0u) {
            sw(sub_timer + 4u, now_bits);
        }
    }
}

inline bool eval_recovery_check_gate(uint32_t ra_state, uint32_t ra_roam, uint32_t ra_fin,
                                     uint32_t ra_cam, uint32_t ra_phys) {
    if (lwu(kRecoveryCheckEnabled) == 0u) return false;
    call(f_00210F98, ra_state);
    if (reg(2) != 1u) {
        call(f_001741C0, ra_roam);
        if (reg(2) == 0u) return false;
    }
    call(f_00179888, ra_fin);
    if (reg(2) != 0u) return false;
    call(f_00179868, ra_cam);
    if (reg(2) != 0u) return false;
    call(f_001F0D78, ra_phys);
    if (reg(2) != 0u) return false;
    return lwu(kRecoverySuppressedFlag) == 0u;
}

void car_update_out_of_bounds_recovery_impl(void) {
    Frame frame(0x80u);
    const uint32_t timer = reg(4);
    const uint32_t person = reg(5);
    const uint32_t now_bits = g_ee.fpr[12].u;
    const float now = bf(now_bits);

    // Clear per-tick recovery HUD warning & SELECT deadline before evaluating.
    sw(person + 192u, 0u);
    sw(person + 196u, kFloatNegOne);

    // Special event mode check (0x4214): ask mode controller if driver is eligible.
    if (lwu(kEventTypeFlags) == kEventTypeSpecialCheck) {
        const uint32_t ctrl = lwu(kRaceModeControllerPtr);
        const uint32_t vt = lwu(ctrl);
        const uint32_t fn = lwu(vt + 20u);
        set(4, static_cast<int32_t>(ctrl));
        set(5, lw(person + 8u));
        set(6, 0);
        set(2, static_cast<int32_t>(fn));
        call_at(fn, 0x001B83E0u);
        if (reg(2) == 0u) {
            return;  // $v0 == 0
        }
    }

    uint32_t car = lwu(person + 4u);
    if (lwu(car + 12u) != kCarMagicPCAR) {
        set(2, static_cast<int32_t>(kCarMagicPCAR));
        return;
    }

    // First gate evaluation: if disabled AND auto-reset is off, bail out early.
    const bool gate1 = eval_recovery_check_gate(0x001B8418u, 0x001B8428u, 0x001B8438u,
                                                0x001B8448u, 0x001B8458u);
    if (!gate1 && lwu(kRecoveryAutoResetEnabled) == 0u) {
        set(2, 0x00480000);
        return;
    }

    // Second gate evaluation (original re-checks before running wrong-way/off-track/rollover).
    const bool gate2 = eval_recovery_check_gate(0x001B8498u, 0x001B84A8u, 0x001B84B8u,
                                                0x001B84C8u, 0x001B84D8u);
    if (gate2) {
        uint32_t spline_state = lwu(person + 232u);
        car = lwu(person + 4u);
        const uint32_t person_state = person + 0x40u;
        const uint32_t event_flags = lwu(kEventTypeFlags);

        // ---- Condition 0 (timer+0x00): Wrong-way ----
        bool wrong_way = false;
        bool skip_wrong_way = false;
        if (event_flags == kEventTypeReversedLanes) {
            const uint32_t mode_val = lwu(person_state + 136u);
            if ((mode_val - 3u) < 2u) {
                skip_wrong_way = true;
            }
        }
        if (!skip_wrong_way && (event_flags & kEventFlagArenaNoWrongWay) != 0u &&
            lwu(person_state + 136u) == 2u) {
            skip_wrong_way = true;
        }
        if (!skip_wrong_way) {
            const uint32_t ctrl = lwu(kRaceModeControllerPtr);
            bool ctrl_forced = false;
            if (ctrl != 0u) {
                const uint32_t vt = lwu(ctrl);
                const uint32_t fn = lwu(vt + 48u);
                set(4, static_cast<int32_t>(ctrl));
                set(5, lw(person + 340u));
                set(2, static_cast<int32_t>(fn));
                call_at(fn, 0x001B857Cu);
                if (reg(2) != 0u) {
                    wrong_way = true;
                    ctrl_forced = true;
                }
            }
            if (!ctrl_forced) {
                set(4, static_cast<int32_t>(spline_state));
                set(5, static_cast<int32_t>(car + 0x104u));
                call(f_0013B0D0, 0x001B8598u);
                if (reg(2) != 0u) {
                    call(f_001741C0, 0x001B85A8u);
                    if (reg(2) != 0u) {
                        wrong_way = true;
                    } else {
                        const float abs_speed = bf(lwu(car + 7004u) & 0x7FFFFFFFu);
                        if (fcmp_lt(bf(kFloatWrongWaySpeedMin), abs_speed)) {
                            wrong_way = true;
                        }
                    }
                }
            }
            car = lwu(person + 4u);
            spline_state = lwu(person + 232u);
        }
        update_condition_timer(timer + 0x00u, wrong_way, now_bits);

        // ---- Condition 1 (timer+0x10): Off-track ----
        const uint32_t cur_event_flags = lwu(kEventTypeFlags);
        uint32_t off_track = 0u;
        if ((cur_event_flags & kEventFlagArenaNoWrongWay) != 0u &&
            lwu(person_state + 136u) == 2u) {
            off_track = 0u;
        } else if (cur_event_flags == kEventTypeSpecialOffTrack &&
                   lwu(person_state + 136u) == 0u) {
            off_track = 0u;
        } else {
            off_track = lwu(spline_state + 36u);
        }
        update_condition_timer(timer + 0x10u, off_track != 0u, now_bits);

        // ---- Condition 2 (timer+0x20): Rollover (chassis up.y < 0.1f) ----
        const bool upside_down = fcmp_lt(bf(lwu(car + 116u)), bf(kFloatRolloverUpYMax));
        update_condition_timer(timer + 0x20u, upside_down, now_bits);
    } else {
        car = lwu(person + 4u);
    }

    // ---- Condition 3 (timer+0x30): Stuck / fewer than 4 wheels grounded at low speed ----
    bool stuck = false;
    if (lw(car + 7096u) < 4) {
        if (fcmp_lt(bf(lwu(car + 7012u)), bf(kFloatStuckSpeedMax))) {
            stuck = true;
        }
    }
    update_condition_timer(timer + 0x30u, stuck, now_bits);

    // ---- Master recovery countdown (timer+0x40) ----
    bool any_triggered = false;
    if (lwu(timer + 76u) != 0u) {
        const float elapsed = fsub_ps2(now, bf(lwu(timer + 68u)));
        if (fcmp_lt(elapsed, bf(kFloatRecoveryGraceWindow))) {
            any_triggered = true;
        }
    }
    if (!any_triggered) {
        for (uint32_t node = lwu(timer + 80u); node != 0u; node = lwu(node)) {
            if (lwu(node + 12u) != 0u) {
                const float elapsed = fsub_ps2(now, bf(lwu(node + 4u)));
                if (fcmp_lt(bf(lwu(node + 8u)), elapsed)) {
                    any_triggered = true;
                    break;
                }
            }
        }
    }
    update_condition_timer(timer + 0x40u, any_triggered, now_bits);

    bool master_expired = false;
    if (lwu(timer + 76u) != 0u) {
        const float elapsed = fsub_ps2(now, bf(lwu(timer + 68u)));
        if (fcmp_lt(bf(lwu(timer + 72u)), elapsed)) {
            master_expired = true;
        }
    }

    if (master_expired && lwu(kRecoveryAutoResetEnabled) != 0u) {
        set(4, lw(person + 8u));
        set(5, lw(person + 4u));
        call(f_001B63E8, 0x001B8884u);
        return;  // $v0 preserved from f_001B63E8
    }

    if (any_triggered) {
        sw(person + 192u, 0xFFFFFFFFu);
        if (lwu(kRecoveryAutoResetEnabled) != 0u) {
            const float deadline = fadd_ps2(bf(lwu(timer + 68u)), bf(lwu(timer + 72u)));
            sw(person + 196u, fb(deadline));
        }
        set(2, -1);
    } else {
        set(2, 0x00480000);
    }
}

// ---------------------------------------------------------------------------
// 2. TimedDamageFeatUpdate (0x001B5840)
// ---------------------------------------------------------------------------
void timed_damage_feat_update_impl(void) {
    const uint32_t caller_sp = reg(29);
    const uint64_t caller_ra = reg64(31);
    set(29, static_cast<int32_t>(caller_sp - 0xB0u));

    auto restore_frame = [&]() {
        set(29, static_cast<int32_t>(caller_sp));
        set64(31, caller_ra);
    };

    // Phase 1: Spectator / camera target cycling
    const int32_t cam_delta = lw(kGlobalCamCycleDelta);
    sw(kGlobalCamCycleDelta, 0u);
    if (cam_delta != 0) {
        call(f_001BFA30, 0x001B5890u);
        if (reg(2) != 0u) {
            set(4, 0);
            call(f_001D3F98, 0x001B58A0u);
            int32_t next_slot = reg_s32(2) + cam_delta;
            const int32_t count = lw(kDriverCount);
            if (next_slot < 0) {
                next_slot = count - 1;
            } else if (next_slot >= count) {
                next_slot = 0;
            }
            // Tail call to f_001D4190(next_slot, 0)
            set(4, next_slot);
            set(5, 0);
            restore_frame();
            f_001D4190();
            return;
        }
        set(4, 0);
        call(f_001D3F98, 0x001B590Cu);
        const int32_t cur_slot = reg_s32(2);
        int32_t rank_idx = 0;
        if (cur_slot >= 0 && cur_slot < lw(kDriverCount)) {
            const uint32_t drv = lwu(kDriverArrayBase + static_cast<uint32_t>(cur_slot) * 4u);
            rank_idx = lw(drv + 68u) - 1;
        }
        rank_idx += (cam_delta > 0) ? 1 : -1;
        if (rank_idx < 0) {
            rank_idx = lw(kDriverCount) - 1;
        }
        if (rank_idx >= lw(kDriverCount)) {
            rank_idx = 0;
        }
        const int32_t target_slot = lw(kStandingsOrderBase + static_cast<uint32_t>(rank_idx) * 4u);
        set(4, target_slot);
        set(5, 0);
        call(f_001D4190, 0x001B598Cu);
    }

    // Phase 2: Replay / pause early exit
    call(f_001BFA30, 0x001B5994u);
    if (reg(2) != 0u) {
        restore_frame();
        return;
    }
    call(f_001766E8, 0x001B59A4u);
    if (reg(2) != 0u) {
        restore_frame();
        return;
    }

    // Phase 3: Active race finish grace timer & inline Deadly Ambition timed-damage feat
    call(f_001BAD10, 0x001B59B4u);
    call(f_00210F98, 0x001B59BCu);
    const uint32_t race_state = reg(2);
    const float race_time = bf(lwu(kGlobalRaceTimeSec));

    if (race_state == 1u) {
        call(f_002CE180, 0x001B59D8u);
        if (reg(2) == 0u) {
            set(4, -1);
            call(f_00179920, 0x001B59E8u);
            if (reg_s32(2) >= 0) {
                if (lwu(kFinishGraceActive) != 0u) {
                    if (fcmp_lt(bf(lwu(kFinishGraceDeadline)), race_time)) {
                        bool all_finished = true;
                        const int32_t count = lw(kDriverCount);
                        for (int32_t i = 0; i < count; ++i) {
                            const uint32_t off = static_cast<uint32_t>(i) * 4u;
                            const uint32_t drv = lwu(kDriverArrayBase + off);
                            if (lwu(drv + 60u) != 0u) continue;
                            if (lwu(kFinishEligibleMaskBase + off) == 0u) continue;
                            if (lwu(drv + 52u) == 0u) {
                                all_finished = false;
                                break;
                            }
                        }
                        if (all_finished) {
                            set(4, 3);
                            call(f_00210F88, 0x001B5AA8u);
                        } else {
                            const float extended = fadd_ps2(race_time, bf(kFloatFinishGraceExtend));
                            sw(kFinishGraceDeadline, fb(extended));
                        }
                    }
                } else {
                    set(4, -1);
                    call(f_00179920, 0x001B5AD4u);
                    const uint32_t leader_idx = reg(2);
                    const uint32_t leader = lwu(kDriverArrayBase + leader_idx * 4u);
                    if (lwu(leader + 52u) != 0u) {
                        sw(kFinishGraceActive, race_state);
                        const float extended = fadd_ps2(race_time, bf(kFloatFinishGraceExtend));
                        sw(kFinishGraceDeadline, fb(extended));
                        set(4, -1);
                        call(f_00179920, 0x001B5B1Cu);
                        const uint32_t ldrv = lwu(kDriverArrayBase + reg(2) * 4u);
                        const uint32_t lvt = lwu(ldrv);
                        const uint32_t lfn = lwu(lvt + 16u);
                        set(4, static_cast<int32_t>(ldrv));
                        set(2, static_cast<int32_t>(lfn));
                        call_at(lfn, 0x001B5B38u);
                        const int32_t min_lap = lw(reg(2)) - 1;
                        const int32_t count = lw(kDriverCount);
                        for (int32_t i = 0; i < count; ++i) {
                            const uint32_t off = static_cast<uint32_t>(i) * 4u;
                            const uint32_t drv = lwu(kDriverArrayBase + off);
                            const uint32_t dvt = lwu(drv);
                            const uint32_t dfn = lwu(dvt + 16u);
                            set(4, static_cast<int32_t>(drv));
                            set(2, static_cast<int32_t>(dfn));
                            call_at(dfn, 0x001B5B74u);
                            const int32_t lap = lw(reg(2));
                            sw(kFinishEligibleMaskBase + off, (lap < min_lap) ? 0u : 1u);
                        }
                    }
                }

                // Inline timed-damage feat (master bit 0x01 + Deadly Ambition bit 0x40)
                const uint32_t cheat_flags = lwu(kGlobalCheatFlags);
                if ((cheat_flags & 0x01u) != 0u && (cheat_flags & 0x40u) != 0u &&
                    lw(kHumanPlayerCount) != 1 && lwu(kFeatEnabledFlag) != 0u) {
                    const uint32_t sel_idx = lwu(kSelectedDriverIndex);
                    const uint32_t drv = lwu(kDriverArrayBase + sel_idx * 4u);
                    const uint32_t dvt = lwu(drv);
                    const uint32_t fn_immune = lwu(dvt + 60u);
                    set(4, static_cast<int32_t>(drv));
                    set(2, static_cast<int32_t>(fn_immune));
                    call_at(fn_immune, 0x001B5C24u);
                    if (reg(2) == 0u) {
                        const uint32_t car = lwu(drv + 4u);
                        float dmg = bf(lwu(car + 7148u));
                        dmg = fadd_ps2(dmg, bf(kFloatTimedDamagePerTick));
                        dmg = fmin_ps2(dmg, bf(kFloatOne));
                        dmg = fmax_ps2(0.0f, dmg);
                        sw(car + 7148u, fb(dmg));
                        if (fcmp_eq(dmg, bf(kFloatOne))) {
                            const uint32_t dvt2 = lwu(drv);
                            const uint32_t fn_wreck = lwu(dvt2 + 52u);
                            set(4, static_cast<int32_t>(drv));
                            set(5, -1);
                            set(2, static_cast<int32_t>(fn_wreck));
                            call_at(fn_wreck, 0x001B5C84u);
                        }
                    }
                }
            }
        }
    }

    // Phase 4: Race-start countdown & grid release
    if (lwu(kCountdownArmed) != 0u && fcmp_le(bf(lwu(kCountdownDeadline)), race_time)) {
        if (race_state == 0u) {
            set(4, 1);
            call(f_00210F88, 0x001B5CC4u);
            const int32_t count = lw(kDriverCount);
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t drv = lwu(kDriverArrayBase + static_cast<uint32_t>(i) * 4u);
                g_ee.fpr[12].u = lwu(kCountdownDeadline);
                set(4, static_cast<int32_t>(drv));
                call(f_001B7198, 0x001B5D00u);
            }
            const uint32_t start_hook = lwu(kRaceStartHookPtr);
            if (start_hook != 0u) {
                const uint32_t hvt = lwu(start_hook);
                const uint32_t hfn = lwu(hvt + 28u);
                set(4, static_cast<int32_t>(start_hook));
                set(2, static_cast<int32_t>(hfn));
                call_at(hfn, 0x001B5D40u);
                set(4, reg_s32(2));
                set(5, 2);
                call(f_0018A950, 0x001B5D4Cu);
            }
            if (lwu(kFeatEnabledFlag) != 0u) {
                set(4, static_cast<int32_t>(kReplayHeaderBuf));
                set(5, lw(kDriverCount));
                set(6, 0);
                call(f_00201BB8, 0x001B5D6Cu);
            }
        }
    } else if (race_state != 3u) {
        set(4, 0);
        call(f_00210F88, 0x001B5D88u);
        call(f_001767A8, 0x001B5D90u);
        if ((reg(2) & 0x400u) != 0u && lwu(kCountdownStarted) == 0u) {
            call(f_001741D8, 0x001B5DB0u);
            if (reg(2) != 0u) {
                g_ee.fpr[12].u = 0u;
                set(4, 0);
                set(5, -1);
                set(6, 0);
                call(f_00257BB8, 0x001B5DC8u);
                sw(kCountdownStarted, 1u);
                sw(kCountdownArmed, 1u);
                sw(kCountdownDeadline, fb(fadd_ps2(race_time, bf(kFloatTwo))));
                set(4, 1);
                set(5, 1);
                call(f_001BB538, 0x001B5DF8u);
                sw(kCountdownBannerActive, 1u);
                call(f_001BB708, 0x001B5E04u);
                const int32_t count = lw(kDriverCount);
                for (int32_t i = 0; i < count; ++i) {
                    const uint32_t drv = lwu(kDriverArrayBase + static_cast<uint32_t>(i) * 4u);
                    const uint32_t timer_block = drv + 0x100u;
                    call(f_001767A8, 0x001B5E30u);
                    uint32_t dur_bits = kFloatFive;
                    if ((reg(2) & 0x400u) != 0u) {
                        call(f_001741C0, 0x001B5E44u);
                        if (reg(2) != 0u) {
                            dur_bits = kFloatTwo;
                        }
                    }
                    sw(timer_block + 72u, dur_bits);
                }
            } else {
                sw(kCountdownArmed, 1u);
                sw(kCountdownDeadline, fb(fadd_ps2(race_time, bf(kFloatSixty))));
            }
        }
    }

    // Phase 5: Optional race time-limit expiration
    const uint32_t limit_bits = lwu(kRaceTimeLimit);
    bool limit_finite = false;
    g_ee.fpr[12].u = limit_bits;
    call(f_0039BD40, 0x001B5EACu);
    if (reg(2) == 0u) {
        g_ee.fpr[12].u = limit_bits;
        call(f_0039BC30, 0x001B5EBCu);
        limit_finite = (reg(2) != 0u);
    }
    if (limit_finite && fcmp_le(bf(lwu(kRaceTimeLimit)), race_time)) {
        set(4, 3);
        call(f_00210F88, 0x001B5EE4u);
        sw(kRaceTimeLimit, lwu(kGlobalNanSentinel));
    }

    // Phase 6: Standings update, banner advancement, and per-driver per-tick clear
    call(f_001766E8, 0x001B5EFCu);
    if (reg(2) == 0u && race_state != 0u) {
        call(f_001B7E58, 0x001B5F14u);
    }

    const int32_t human_count = lw(kHumanPlayerCount);
    if (human_count == 1) {
        if (lwu(kSinglePlayerBannerDone) == 0u && lw(kSinglePlayerBannerCount) >= 2) {
            call(f_001BB8A0, 0x001B5F8Cu);
            sw(kSinglePlayerBannerDone, 0u);
        }
    } else {
        sw(kSinglePlayerBannerCount, 0u);
        if (human_count == 0) {
            const uint32_t sel_idx = lwu(kSelectedDriverIndex);
            const uint32_t drv = lwu(kDriverArrayBase + sel_idx * 4u);
            if (lwu(drv + 144u) == 0u) {
                call(f_001BB848, 0x001B5FA0u);
                if (reg(2) != 0u) {
                    call(f_001BB850, 0x001B5FB0u);
                    if (reg(2) != 0u) {
                        set(4, 1);
                        set(5, 0);
                        call(f_001BB538, 0x001B5FC0u);
                        call(f_001BB8A0, 0x001B5FC8u);
                        const int32_t banner_val = reg_s32(2) - 4;
                        sw(kDemoBannerResultFlag, (banner_val < 10) ? 1u : 0u);
                        sw(kDemoBannerCycleCounter, static_cast<uint32_t>(lw(kDemoBannerCycleCounter) + 1));
                    }
                }
            }
        }
    }

    const int32_t final_count = lw(kDriverCount);
    if (final_count > 0) {
        for (int32_t i = 0; i < final_count; ++i) {
            const uint32_t drv = lwu(kDriverArrayBase + static_cast<uint32_t>(i) * 4u);
            sw(drv + 16u, 0u);
            if (lwu(drv + 152u) != 0u) {
                sw(drv + 156u, 0u);
            }
        }
        // Delay slot of final loop back-edge sets $v0 = kDriverArrayBase
        set(2, static_cast<int32_t>(kDriverArrayBase));
    } else {
        set(2, final_count);
    }

    restore_frame();
}

// ---------------------------------------------------------------------------
// 3. WheelGroundQuery (0x00212500)
// ---------------------------------------------------------------------------
void wheel_ground_query_impl(void) {
    Frame frame(0x80u);
    const uint32_t scol_cache = reg(4);
    const uint32_t pos_xyz = reg(5);
    const uint32_t normal_out = reg(6);
    const uint32_t material_out = reg(7);

    const uint16_t saved_tri_idx = static_cast<uint16_t>(MEM_LH(scol_cache));

    set(4, static_cast<int32_t>(pos_xyz));
    set(5, static_cast<int32_t>(normal_out));
    set(6, static_cast<int32_t>(material_out));
    set(7, static_cast<int32_t>(scol_cache));
    call(f_00227950, 0x00212548u);
    if (reg(2) == 0u) {
        MEM_SH(scol_cache, saved_tri_idx);
        set(2, 0);
        return;
    }

    set(4, static_cast<int32_t>(scol_cache));
    call(f_00227830, 0x00212558u);
    const uint32_t sector_mask = reg(2);

    call(f_00217220, 0x00212560u);
    const uint32_t combined_mask = sector_mask & reg(2);

    // Save primary SCOL ground hit in case dynamic/overlay query misses or is lower.
    const uint32_t saved_y_bits = lwu(pos_xyz + 4u);
    const uint32_t saved_mat = lwu(material_out);
    uint8_t saved_normal[12];
    ee_read_block(normal_out, saved_normal, sizeof(saved_normal));

    set(4, static_cast<int32_t>(combined_mask));
    set(5, static_cast<int32_t>(pos_xyz));
    set(6, static_cast<int32_t>(normal_out));
    set(7, static_cast<int32_t>(material_out));
    call(f_00217240, 0x00212594u);

    if (reg(2) != 0u && fcmp_lt(bf(saved_y_bits), bf(lwu(pos_xyz + 4u)))) {
        // Dynamic/overlay surface is strictly higher than primary ground: keep it.
        set(2, 1);
        return;
    }

    // Restore primary SCOL ground hit.
    sw(pos_xyz + 4u, saved_y_bits);
    ee_write_block(normal_out, saved_normal, sizeof(saved_normal));
    sw(material_out, saved_mat);
    set(2, 1);
}

// ---------------------------------------------------------------------------
// 4. ChassisSweptBoxCollide / VehiclePhysicsStep (0x001DE7B0)
// ---------------------------------------------------------------------------
inline void sample_four_wheels_dual_probe(uint32_t body, uint32_t wheel_base, uint32_t scol_cache,
                                          uint32_t sp, uint32_t mat_a_addr, uint32_t mat_b_addr,
                                          uint32_t ra_probe_a, uint32_t ra_probe_b) {
    // Stack buffer addresses inside 0x1A0 frame matching original layout:
    //   pos_a  = sp + 0x30 (48)   norm_a = sp + 0x50 (80)
    //   pos_b  = sp + 0x40 (64)   norm_b = sp + 0x60 (96)
    const uint32_t pos_a_addr = sp + 0x30u;
    const uint32_t pos_b_addr = sp + 0x40u;
    const uint32_t norm_a_addr = sp + 0x50u;
    const uint32_t norm_b_addr = sp + 0x60u;

    // Chassis forward axis (body + 0x30) for +/- 0.5m dual ground probes.
    const float axis_x = bf(lwu(body + 0x30u));
    const float axis_y = bf(lwu(body + 0x34u));
    const float axis_z = bf(lwu(body + 0x38u));

    const float half = bf(kFloatHalf);
    const float neg_half = bf(kFloatNegHalf);
    const float one = bf(kFloatOne);

    for (uint32_t w = 0u; w < 4u; ++w) {
        const uint32_t wheel_slot = wheel_base + w * 0x10u;
        const float hub_x = bf(lwu(wheel_slot + 0x40u));
        const float hub_y = bf(lwu(wheel_slot + 0x44u));
        const float hub_z = bf(lwu(wheel_slot + 0x48u));

        // Probe A: hub + 0.5 * axis; Probe B: hub - 0.5 * axis
        sw(pos_a_addr + 0u, fb(fadd_ps2(hub_x, fmul_ps2(axis_x, half))));
        sw(pos_a_addr + 4u, fb(fadd_ps2(hub_y, fmul_ps2(axis_y, half))));
        sw(pos_a_addr + 8u, fb(fadd_ps2(hub_z, fmul_ps2(axis_z, half))));

        sw(pos_b_addr + 0u, fb(fadd_ps2(hub_x, fmul_ps2(axis_x, neg_half))));
        sw(pos_b_addr + 4u, fb(fadd_ps2(hub_y, fmul_ps2(axis_y, neg_half))));
        sw(pos_b_addr + 8u, fb(fadd_ps2(hub_z, fmul_ps2(axis_z, neg_half))));

        // Query Probe A
        set(4, static_cast<int32_t>(scol_cache));
        set(5, static_cast<int32_t>(pos_a_addr));
        set(6, static_cast<int32_t>(norm_a_addr));
        set(7, static_cast<int32_t>(mat_a_addr));
        call(f_00212500, ra_probe_a);
        if (reg(2) == 0u) {
            sw(norm_a_addr + 0u, 0u);
            sw(norm_a_addr + 4u, kFloatOne);
            sw(norm_a_addr + 8u, 0u);
            sw(pos_a_addr + 4u, kFloatFallbackGroundY);
        }

        // Query Probe B
        set(4, static_cast<int32_t>(scol_cache));
        set(5, static_cast<int32_t>(pos_b_addr));
        set(6, static_cast<int32_t>(norm_b_addr));
        set(7, static_cast<int32_t>(mat_b_addr));
        call(f_00212500, ra_probe_b);
        if (reg(2) == 0u) {
            sw(norm_b_addr + 0u, 0u);
            sw(norm_b_addr + 4u, kFloatOne);
            sw(norm_b_addr + 8u, 0u);
            sw(pos_b_addr + 4u, kFloatFallbackGroundY);
        }

        // Average & normalize the two surface normals
        const float nx = fadd_ps2(fmul_ps2(bf(lwu(norm_a_addr + 0u)), half),
                                  fmul_ps2(bf(lwu(norm_b_addr + 0u)), half));
        const float ny = fadd_ps2(fmul_ps2(bf(lwu(norm_a_addr + 4u)), half),
                                  fmul_ps2(bf(lwu(norm_b_addr + 4u)), half));
        const float nz = fadd_ps2(fmul_ps2(bf(lwu(norm_a_addr + 8u)), half),
                                  fmul_ps2(bf(lwu(norm_b_addr + 8u)), half));

        float acc = fmul_ps2(nx, nx);
        acc = fadd_ps2(acc, fmul_ps2(ny, ny));
        const float len2 = fadd_ps2(acc, fmul_ps2(nz, nz));
        const float inv_len = frsqrt_ps2(one, len2);

        const uint32_t unx = fb(fmul_ps2(nx, inv_len));
        const uint32_t uny = fb(fmul_ps2(ny, inv_len));
        const uint32_t unz = fb(fmul_ps2(nz, inv_len));

        const float pa_x = bf(lwu(pos_a_addr + 0u));
        const float pa_y = bf(lwu(pos_a_addr + 4u));
        const float pa_z = bf(lwu(pos_a_addr + 8u));
        const float pb_x = bf(lwu(pos_b_addr + 0u));
        const float pb_y = bf(lwu(pos_b_addr + 4u));
        const float pb_z = bf(lwu(pos_b_addr + 8u));

        // Average contact height (for normal.w) and average contact point
        const uint32_t avg_y_w = fb(fmul_ps2(fadd_ps2(pa_y, pb_y), half));
        const uint32_t avg_px = fb(fadd_ps2(fmul_ps2(pa_x, half), fmul_ps2(pb_x, half)));
        const uint32_t avg_py = fb(fadd_ps2(fmul_ps2(pa_y, half), fmul_ps2(pb_y, half)));
        const uint32_t avg_pz = fb(fadd_ps2(fmul_ps2(pa_z, half), fmul_ps2(pb_z, half)));

        // Store Probe B surface material into scol_cache[1 + w]
        sw(scol_cache + 4u + w * 4u, lwu(mat_b_addr));

        // Store normalized contact normal + height qword at wheel_slot + 0x110 (272)
        R128 norm_q{};
        norm_q.uw[0] = unx;
        norm_q.uw[1] = uny;
        norm_q.uw[2] = unz;
        norm_q.uw[3] = avg_y_w;
        MEM_SQ(wheel_slot + 272u, &norm_q);

        // Store contact point qword at wheel_slot + 0x150 (336)
        // Exact MMI pextlw + pcpyld packing: uw[3] is sign-extension of avg_pz.
        R128 pos_q{};
        pos_q.uw[0] = avg_px;
        pos_q.uw[1] = avg_py;
        pos_q.uw[2] = avg_pz;
        pos_q.uw[3] = (static_cast<int32_t>(avg_pz) < 0) ? 0xFFFFFFFFu : 0u;
        MEM_SQ(wheel_slot + 336u, &pos_q);

        // Leave final wheel's packed pos_q in $v0 (matches exit register on last iteration)
        g_ee.r[2] = pos_q;
    }
}

void chassis_swept_box_collide_impl(void) {
    Frame frame(0x1A0u);
    const uint32_t sp = reg(29);

    // Upload VU0 vehicle physics microprogram via VIF0 DMA channel 0
    const uint32_t saved_enabler = lwu(kDmacEnabler);
    sw(kDmacEnabler, 0x00010001u);
    sw(kDmacVif0Tadr, kVu0VehicleMicroprogAddr & 0xEFFFFFFFu);
    sw(kDmacVif0Qwc, 0u);
    sw(kDmacStat, 1u);
    sw(kDmacVif0Chcr, 0x00000144u);
    set(4, static_cast<int32_t>(sp + 0x90u));
    call(f_001E0830, 0x001DE864u);
    sw(kDmacEnabler, saved_enabler);

    uint32_t body = lwu(kPhysicsBodyArrayPtr);
    uint32_t wheel = lwu(kPhysicsWheelArrayPtr);
    uint32_t param = lwu(kPhysicsParamArrayPtr);
    uint32_t scol = lwu(kPhysicsScolCacheArrayPtr);
    int32_t count = lw(kPhysicsCarCount);

    for (int32_t i = 0; i < count; ++i) {
        // Stage body (11 qwords), wheels (3 + 7*4 = 31 qwords), and params (2*12 = 24 qwords) to VU0
        VU0.vi[1] = 0u;
        eod_physics_stage(body, 11u, 1);
        eod_physics_interlock();

        VU0.vi[1] = 11u;
        eod_physics_stage(wheel, 3u, 0);
        for (uint32_t b = 0u; b < 7u; ++b) {
            eod_physics_stage(wheel + 0x30u + b * 0x40u, 4u, 0);
        }
        eod_physics_interlock();

        VU0.vi[1] = 42u;
        eod_physics_stage(param, 12u, 1);
        eod_physics_stage(param + 0xC0u, 12u, 1);
        eod_physics_interlock();

        VU0.vi[1] = 0u;
        VU0.vi[2] = 11u;
        VU0.vi[3] = 42u;
        eod_physics_interlock();
        eod_vehicle_physics_step();

        // Software-pipelined 4-wheel ground sampling for car (i - 1)
        if (i > 0) {
            sample_four_wheels_dual_probe(body - kBodyStride, wheel - kWheelStride,
                                          scol - kScolCacheStride, sp,
                                          sp + 0x94u, sp + 0x98u,
                                          0x001DEB74u, 0x001DEBA4u);
        }

        // Collect solved body & wheel state back from VU0 unless vehicle is disabled
        eod_physics_interlock();
        const uint32_t disabled_base = lwu(kPhysicsDisabledArrayPtr);
        if (lwu(disabled_base + static_cast<uint32_t>(i) * 4u) == 0u) {
            eod_physics_interlock();
            VU0.vi[1] = 0u;
            eod_physics_collect(body, 11u, 1);
            eod_physics_interlock();

            eod_physics_interlock();
            VU0.vi[1] = 11u;
            eod_physics_collect(wheel, 3u, 0);
            for (uint32_t b = 0u; b < 7u; ++b) {
                eod_physics_collect(wheel + 0x30u + b * 0x40u, 4u, 0);
            }
            eod_physics_interlock();
        }

        body += kBodyStride;
        wheel += kWheelStride;
        param += kParamStride;
        scol += kScolCacheStride;
        count = lw(kPhysicsCarCount);
    }

    // Epilogue: 4-wheel ground sampling for the last car (count - 1)
    const int32_t final_count = lw(kPhysicsCarCount);
    if (final_count > 0) {
        sample_four_wheels_dual_probe(body - kBodyStride, wheel - kWheelStride,
                                      scol - kScolCacheStride, sp,
                                      sp + 0x9Cu, sp + 0xA0u,
                                      0x001DEF20u, 0x001DEF4Cu);
    } else {
        set(2, final_count);
    }
}

}  // namespace

extern "C" {

void eod_physics_001B8370(void) {
    diff_run(0x001B8370u, car_update_out_of_bounds_recovery_impl, f_001B8370_orig);
}

void eod_physics_001B5840(void) {
    diff_run(0x001B5840u, timed_damage_feat_update_impl, f_001B5840_orig);
}

void eod_physics_00212500(void) {
    diff_run(0x00212500u, wheel_ground_query_impl, f_00212500_orig);
}

void eod_physics_001DE7B0(void) {
    diff_run(0x001DE7B0u, chassis_swept_box_collide_impl, f_001DE7B0_orig);
}

}  // extern "C"
