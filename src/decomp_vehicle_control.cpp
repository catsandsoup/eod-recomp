// decomp_vehicle_control.cpp - readable, bit-exact C++ replacements for vehicle control,
// scalar damage, human driver update (turbo boost & weapon trigger), and car-to-car
// solved contact impulse dispatch (SLUS_209.10).
//
//   0x0018AA68  CarSetBrake(car, $f12 brake)
//               Writes car+0x1B74 (wrapped in src/hle_career.cpp).
//   0x0018AA70  CarSetThrottle(car, $f12 throttle)
//               Writes car+0x1B70 (wrapped in src/hle_career.cpp).
//   0x0018AAD0  CarCanTakeDamage(car) -> int32_t ($v0: 0 or 1)
//               Trailer bypass, contestant state check, free-roam / player gate
//               (wrapped in src/hle_career.cpp).
//   0x0018AD20  CarSetDamage(car, $f12 delta)
//               Adds delta to car+0x1BEC clamped to [0.0f, 1.0f]
//               (wrapped in src/game_patches.cpp).
//   0x0018F2A8  HumanDriverUpdate(driver)
//               Per-frame human driver update: steering/throttle/brake/gear,
//               turbo boost / wheelspin assist (0x003C4190 bits 0x1|0x2, 44.444447 m/s cap),
//               chicken weapon trigger (0x003C4190 bits 0x1|0x8 -> 0x17DE78),
//               force-feedback rumble, wrong-way & flip checks
//               (wrapped in src/game_patches.cpp).
//   0x00189648  CarApplySolvedContact(car, impulse, point, surface, other)
//               Solved collision impulse callback: computes car-to-car speed-ratio
//               scaling, transforms impulse & point into car-local space, and dispatches
//               to CarApplyCollisionDamage (0x0018CA98) above 741.6667 threshold
//               (wrapped in src/netplay_trace.cpp).
//
// Every replacement mirrors the exact EE FPU ops (fadd_ps2, fsub_ps2, fmul_ps2, fdiv_ps2,
// fsqrt_ps2, frsqrt_ps2, fmin_ps2, fmax_ps2), guest call order, return addresses, and
// stack frames so EOD_DIFF proves 0 mismatches against the recompiled original.

#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"

#include <cstdint>

extern "C" {
void f_0018AA68_orig(void);
void f_0018AA70_orig(void);
void f_0018AAD0_orig(void);
void f_0018AD20_orig(void);
void f_0018F2A8_orig(void);
void f_00189648_orig(void);

// Direct guest callees (go through any registered wrap hooks, matching recompiled code).
void f_0013B0D0(void);  // WrongWayCheck(contestant, vel_ptr)
void f_0013C2B0(void);  // HudUpdate(hud, owner)
void f_0013E490(void);  // CarFlippedNotify(owner, $f12=1.0f)
void f_001741C0(void);  // CareerIsFreeRoamActive()
void f_00176768(void);  // GameGetCurrent()
void f_00176798(void);  // GameGetMode()
void f_00179868(void);  // RaceIsFinished()
void f_00179888(void);  // RaceIsCountdown()
void f_0017B708(void);  // RigidBodyApplyImpulseAtOffset(car, impulse_ptr, point_ptr)
void f_0017DE78(void);  // WeaponRegistryQuery(owner, slot)
void f_0018A9B0(void);  // CarGetVelocityPtr(car)
void f_0018A9E0(void);  // CarRumblePre(car)
void f_0018A9F0(void);  // CarGetEngineRumble(car)
void f_0018AA20(void);  // CarSetWeaponButton(car, state)
void f_0018AA40(void);  // CarAutoShift(car)
void f_0018AA50(void);  // CarSetGear(car, gear)
void f_0018AA58(void);  // CarSetHandbrake(car, $f12)
void f_0018AA60(void);  // CarSetClutch(car, $f12)
void f_0018AA68(void);  // CarSetBrake(car, $f12)
void f_0018AA70(void);  // CarSetThrottle(car, $f12)
void f_0018AA80(void);  // CarSetSteering(car, $f12)
void f_0018AA90(void);  // CarPreContact(car, ...)
void f_0018CA98(void);  // CarApplyCollisionDamage(car, impulse, point, negate, other_idx, surface)
void f_0018F150(void);  // HumanDriverPreUpdate(driver)
void f_001D61D0(void);  // AnnounceEvent(id, param, owner)
void f_00203248(void);  // RumbleIsEnabled()
void f_00257BB8(void);  // AudioImpactNotify(type, car_idx, other_idx, $f12=imp_len)
void f_0039BB28(void);  // cosf($f12)
void f_0039BD68(void);  // sinf($f12)
void f_0039C750(void);  // fmodf($f12, $f13)
}

namespace {

// ---- guest register & memory helpers (same conventions as src/car_damage.cpp) ----
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
inline uint8_t lbu(uint32_t a) { return static_cast<uint8_t>(MEM_LB(a)); }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline float lf(uint32_t a) {
    FReg f;
    f.u = MEM_LW(a);
    return f.f;
}
inline uint32_t fb(float v) {
    FReg f;
    f.f = v;
    return f.u;
}
inline float bf(uint32_t u) {
    FReg f;
    f.u = u;
    return f.f;
}
inline void sf(uint32_t a, float v) { MEM_SW(a, fb(v)); }
inline float fneg(float v) { return bf(fb(v) ^ 0x80000000u); }
inline float fabs_ee(float v) { return bf(fb(v) & 0x7FFFFFFFu); }
// R5900 mula.s / madda.s / madd.s accumulator chain
inline float madd3(float a0, float b0, float a1, float b1, float a2, float b2) {
    return fadd_ps2(fadd_ps2(fmul_ps2(a0, b0), fmul_ps2(a1, b1)), fmul_ps2(a2, b2));
}
inline void call(void (*fn)(void), uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    fn();
}
inline void call_at(uint32_t fn, uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    ee_call(fn);
}
inline void call_vtable(uint32_t obj, uint32_t slot_offset, uint32_t ra) {
    const uint32_t vt = lwu(obj);
    call_at(lwu(vt + slot_offset), ra);
}

struct Frame {
    uint32_t caller_sp;
    uint32_t sp;
    uint64_t ra;
    explicit Frame(uint32_t size)
        : caller_sp(reg(29)), sp(caller_sp - size), ra(g_ee.r[31].ud[0]) {
        set(29, (int32_t)sp);
    }
    ~Frame() {
        set(29, (int32_t)caller_sp);
        g_ee.r[31].ud[0] = ra;
    }
};

// ---- shared globals & vehicle type FourCCs ----
constexpr uint32_t kCheatMask    = 0x003C4190u;
constexpr uint32_t kHudPtr       = 0x003CD170u;
constexpr uint32_t kSimFrame     = 0x003CDA8Cu;
constexpr uint32_t kSimTime      = 0x003CDACCu;
constexpr uint32_t kSpeedDisplay = 0x003CE550u;
constexpr uint32_t kBoostPitch   = 0x003FA3D0u;

constexpr uint32_t kTagCAR  = 0x00434152u;  // "CAR\0"
constexpr uint32_t kTagPCAR = 0x50434152u;  // "PCAR"
constexpr uint32_t kTagACAR = 0x41434152u;  // "ACAR"
constexpr uint32_t kTagNCAR = 0x4E434152u;  // "NCAR"
constexpr uint32_t kTagPACE = 0x50414345u;  // "PACE"
constexpr uint32_t kTagTRLR = 0x54524C52u;  // "TRLR"

inline bool is_car_tag(uint32_t tag, bool include_trailer) {
    if (tag == kTagCAR || tag == kTagPCAR || tag == kTagACAR ||
        tag == kTagNCAR || tag == kTagPACE)
        return true;
    return include_trailer && tag == kTagTRLR;
}

// ============================================================================
// 1. 0x0018AA68 — CarSetBrake(car, $f12 brake)
// ============================================================================
void car_set_brake() {
    sw(reg(4) + 0x1B74u, g_ee.fpr[12].u);
}

// ============================================================================
// 2. 0x0018AA70 — CarSetThrottle(car, $f12 throttle)
// ============================================================================
void car_set_throttle() {
    sw(reg(4) + 0x1B70u, g_ee.fpr[12].u);
}

// ============================================================================
// 3. 0x0018AAD0 — CarCanTakeDamage(car) -> int32_t ($v0: 0 or 1)
// ============================================================================
void car_can_take_damage() {
    const uint32_t car = reg(4);
    Frame frame(0x30u);

    int32_t result = 1;
    if (lwu(car + 0x0Cu) != kTagTRLR) {
        const uint32_t ctrl = lwu(car + 0x260u);
        set(4, (int32_t)ctrl);
        call_vtable(ctrl, 0x10u, 0x0018AB0Cu);
        const uint32_t contestant = reg(2);

        if (contestant != 0) {
            bool done = false;
            if (lw(contestant + 0x50u) == 1) {
                call(f_001741C0, 0x0018AB2Cu);
                if (reg(2) == 0) {
                    result = 0;
                    done = true;
                }
            }
            if (!done) {
                if (lw(contestant + 0x58u) != 0 && lwu(car + 0x1E0u) == 0x50u) {
                    call(f_00176798, 0x0018AB54u);
                    if (reg(2) == 2) {
                        result = 0;
                        done = true;
                    }
                }
            }
            if (!done) {
                result = (lw(contestant + 0x50u) != 3) ? 1 : 0;
            }
        }
    }

    set(2, result);
}

// ============================================================================
// 4. 0x0018AD20 — CarSetDamage(car, $f12 delta)
// ============================================================================
void car_set_damage() {
    const uint32_t car = reg(4);
    float dmg = fadd_ps2(lf(car + 0x1BECu), g_ee.fpr[12].f);
    dmg = fmin_ps2(dmg, bf(0x3F800000u));
    dmg = fmax_ps2(0.0f, dmg);
    sf(car + 0x1BECu, dmg);
}

// ============================================================================
// 5. 0x0018F2A8 — HumanDriverUpdate(driver)
// ============================================================================
void human_driver_update() {
    const uint32_t drv = reg(4);
    Frame stack_frame(0x210u);
    const uint32_t sp = stack_frame.sp;

    // Pre-update & current speed
    set(4, (int32_t)drv);
    call(f_0018F150, 0x0018F304u);

    set(4, (int32_t)lwu(drv + 0x38u));
    call(f_0018A9B0, 0x0018F30Cu);
    const uint32_t vel_ptr = reg(2);
    const float vx = lf(vel_ptr + 0x0u);
    const float vy = lf(vel_ptr + 0x4u);
    const float vz = lf(vel_ptr + 0x8u);
    const float speed = fsqrt_ps2(madd3(vx, vx, vy, vy, vz, vz));

    uint32_t car = lwu(drv + 0x38u);
    const float fwd_speed = madd3(
        lf(car + 0x104u), lf(car + 0x7Cu),
        lf(car + 0x108u), lf(car + 0x80u),
        lf(car + 0x10Cu), lf(car + 0x84u));
    sf(kSpeedDisplay, speed);

    // Poll input device (vtable +0x5C, +0x08) and update HUD
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x5Cu, 0x0018F374u);
    }
    sw(sp + 0x118u, sp + 0x114u);
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        car = lwu(drv + 0x38u);
        set(4, (int32_t)input);
        set(5, lw(car + 0x410u));
        call_vtable(input, 0x08u, 0x0018F3B0u);
    }
    set(4, lw(kHudPtr));
    set(5, lw(drv + 0x40u));
    call(f_0013C2B0, 0x0018F3BCu);

    // Steering: input->vtable+0x0C(|forward_speed|) -> CarSetSteering
    {
        car = lwu(drv + 0x38u);
        const uint32_t input = lwu(drv + 0x3Cu);
        const float cur_fwd = madd3(
            lf(car + 0x104u), lf(car + 0x7Cu),
            lf(car + 0x108u), lf(car + 0x80u),
            lf(car + 0x10Cu), lf(car + 0x84u));
        set(4, (int32_t)input);
        g_ee.fpr[12].f = fabs_ee(cur_fwd);
        call_vtable(input, 0x0Cu, 0x0018F404u);

        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].u = g_ee.fpr[0].u;
        call(f_0018AA80, 0x0018F410u);
    }

    // Throttle: suppressed if handbrake >= 0.25
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x1Cu, 0x0018F424u);
        if (g_ee.fpr[0].f < bf(0x3E800000u)) {
            const uint32_t in2 = lwu(drv + 0x3Cu);
            set(4, (int32_t)in2);
            call_vtable(in2, 0x10u, 0x0018F454u);
            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].u = g_ee.fpr[0].u;
            call(f_0018AA70, 0x0018F460u);
        } else {
            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].u = 0;
            call(f_0018AA70, 0x0018F474u);
        }
    }

    // Turbo boost cheat check (0x003C4190 bits 0x1 & 0x2 + L2/weapon button)
    bool turbo_active = false;
    {
        const uint32_t cheats = lwu(kCheatMask);
        if ((cheats & 1u) != 0 && (cheats & 2u) != 0) {
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            call_vtable(input, 0x2Cu, 0x0018F4ACu);
            turbo_active = (reg(2) != 0);
        }
    }

    float boost_strength;
    if (turbo_active) {
        boost_strength = bf(0x3FC00000u);  // 1.5f
    } else {
        call(f_00176768, 0x0018F4D0u);
        boost_strength = (lwu(reg(2) + 0x14u) & 0x10000000u) ? bf(0x3F800000u) : bf(0x3F000000u);
    }
    const float speed_cap = turbo_active ? bf(0x4231C71Du) : bf(0x408E38E4u);  // 44.444447f vs 4.4444447f

    // Forward boost / launch assist impulse
    car = lwu(drv + 0x38u);
    int32_t wheels_grounded = lw(car + 0x1BB8u);
    if ((int32_t)(lwu(kSimFrame) - lwu(drv + 0x10u)) >= 11) {
        if (turbo_active || bf(0x3F666666u) < lf(car + 0x1B84u)) {
            if (fwd_speed < speed_cap && wheels_grounded >= 3) {
                const float pitch = lf(kBoostPitch);
                float fx = fadd_ps2(lf(car + 0x7Cu), fmul_ps2(lf(car + 0x70u), pitch));
                float fy = fadd_ps2(lf(car + 0x80u), fmul_ps2(lf(car + 0x74u), pitch));
                float fz = fadd_ps2(lf(car + 0x84u), fmul_ps2(lf(car + 0x78u), pitch));

                const float kOne = bf(0x3F800000u);
                const float kNegOne = bf(0xBF800000u);
                float steer = fmin_ps2(lf(car + 0x1B80u), kOne);
                steer = fmax_ps2(kNegOne, steer);
                float yaw_damp = fmul_ps2(fmul_ps2(fneg(lf(car + 0x114u)), steer), bf(0xC1A00000u));  // -20.0f
                yaw_damp = fmax_ps2(0.0f, fmin_ps2(yaw_damp, kOne));
                const float fwd_scale = fsub_ps2(kOne, fmul_ps2(fmul_ps2(steer, steer), yaw_damp));
                fz = fmul_ps2(fz, fwd_scale);
                fx = fmul_ps2(fx, fwd_scale);
                fy = fmul_ps2(fy, fwd_scale);

                float lat_factor = fsub_ps2(kOne, fmul_ps2(lf(car + 0x1B64u), bf(0x3D666666u)));  // 0.05625f
                lat_factor = fmax_ps2(0.0f, fmin_ps2(lat_factor, kOne));
                lat_factor = fmul_ps2(lat_factor, bf(0xBDCCCCCDu));  // -0.1f
                const float steer_lat = fmul_ps2(fmul_ps2(steer, fabs_ee(steer)), lat_factor);

                fx = fadd_ps2(fx, fmul_ps2(lf(car + 0x64u), steer_lat));
                fy = fadd_ps2(fy, fmul_ps2(lf(car + 0x68u), steer_lat));
                fz = fadd_ps2(fz, fmul_ps2(lf(car + 0x6Cu), steer_lat));

                const float force = fmul_ps2(boost_strength, fmul_ps2(lf(car + 0x1B88u), bf(0x46EA6000u)));  // 30000.0f
                fz = fmul_ps2(fz, force);
                fx = fmul_ps2(fx, force);
                fy = fmul_ps2(fy, force);
                sf(sp + 0x00u, fx);
                sf(sp + 0x04u, fy);
                sf(sp + 0x08u, fz);

                const float kNegTwo = bf(0xC0000000u);
                sf(sp + 0x10u, fadd_ps2(lf(car + 0x88u), fmul_ps2(lf(car + 0x7Cu), kNegTwo)));
                sf(sp + 0x14u, fadd_ps2(lf(car + 0x8Cu), fmul_ps2(lf(car + 0x80u), kNegTwo)));
                sf(sp + 0x18u, fadd_ps2(lf(car + 0x90u), fmul_ps2(lf(car + 0x84u), kNegTwo)));

                if (lw(car + 0x1B9Cu) < 0) {
                    if (fabs_ee(fwd_speed) < bf(0x408E38E4u)) {
                        sf(sp + 0x00u, fneg(fx));
                        sf(sp + 0x04u, fneg(fy));
                        sf(sp + 0x08u, fneg(fz));
                    } else {
                        sf(sp + 0x00u, 0.0f);
                        sf(sp + 0x04u, 0.0f);
                        sf(sp + 0x08u, 0.0f);
                    }
                }
                set(4, (int32_t)lwu(drv + 0x38u));
                set(5, (int32_t)sp);
                set(6, (int32_t)(sp + 0x10u));
                call(f_0017B708, 0x0018F768u);

                car = lwu(drv + 0x38u);
                wheels_grounded = lw(car + 0x1BB8u);
            }
        }
    }

    // Reverse braking assist impulse
    if (wheels_grounded >= 3) {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x14u, 0x0018F790u);
        if (bf(0x3DCCCCCDu) < g_ee.fpr[0].f) {
            car = lwu(drv + 0x38u);
            const float pitch = lf(kBoostPitch);
            const float steer = lf(car + 0x1B80u);
            const float neg_force = fmul_ps2(boost_strength, bf(0xC53B8000u));  // -3000.0f
            float bx = fmul_ps2(fadd_ps2(lf(car + 0x7Cu), fmul_ps2(lf(car + 0x70u), pitch)), neg_force);
            float by = fmul_ps2(fadd_ps2(lf(car + 0x80u), fmul_ps2(lf(car + 0x74u), pitch)), neg_force);
            float bz = fmul_ps2(fadd_ps2(lf(car + 0x84u), fmul_ps2(lf(car + 0x78u), pitch)), neg_force);
            sf(sp + 0x00u, bx);
            sf(sp + 0x04u, by);
            sf(sp + 0x08u, bz);

            const uint32_t in2 = lwu(drv + 0x3Cu);
            set(4, (int32_t)in2);
            call_vtable(in2, 0x14u, 0x0018F838u);
            const float brake_val = g_ee.fpr[0].f;
            const float steer_scale = fsub_ps2(bf(0x3F800000u), fmul_ps2(steer, steer));
            bx = fmul_ps2(fmul_ps2(lf(sp + 0x00u), brake_val), steer_scale);
            by = fmul_ps2(fmul_ps2(lf(sp + 0x04u), brake_val), steer_scale);
            bz = fmul_ps2(fmul_ps2(lf(sp + 0x08u), brake_val), steer_scale);
            sf(sp + 0x00u, bx);
            sf(sp + 0x04u, by);
            sf(sp + 0x08u, bz);

            const float kNegTwo = bf(0xC0000000u);
            sf(sp + 0x20u, fadd_ps2(lf(car + 0x88u), fmul_ps2(lf(car + 0x7Cu), kNegTwo)));
            sf(sp + 0x24u, fadd_ps2(lf(car + 0x8Cu), fmul_ps2(lf(car + 0x80u), kNegTwo)));
            sf(sp + 0x28u, fadd_ps2(lf(car + 0x90u), fmul_ps2(lf(car + 0x84u), kNegTwo)));

            if (bf(0xC0638E39u) < fwd_speed) {  // -3.5555556f
                set(4, (int32_t)lwu(drv + 0x38u));
                set(5, (int32_t)sp);
                set(6, (int32_t)(sp + 0x20u));
                call(f_0017B708, 0x0018F8ECu);
            }
        }
    }

    // Brake
    if (lw(drv + 0x34u) == 3) {
        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].u = 0;
        call(f_0018AA68, 0x0018F930u);
    } else {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x14u, 0x0018F910u);
        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].u = g_ee.fpr[0].u;
        call(f_0018AA68, 0x0018F91Cu);
    }

    // Handbrake
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x1Cu, 0x0018F944u);
        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].u = g_ee.fpr[0].u;
        call(f_0018AA58, 0x0018F950u);
    }

    // Clutch / reverse / gear shifting
    float clutch;
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x18u, 0x0018F964u);
        clutch = g_ee.fpr[0].f;
    }
    uint32_t reverse_btn;
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x30u, 0x0018F978u);
        reverse_btn = reg(2);
    }

    if (reverse_btn != 0 || lw(drv + 0x34u) == 3) {
        set(4, (int32_t)lwu(drv + 0x38u));
        set(5, -1);
        call(f_0018AA50, 0x0018F99Cu);

        car = lwu(drv + 0x38u);
        const float cur_fwd = madd3(
            lf(car + 0x104u), lf(car + 0x7Cu),
            lf(car + 0x108u), lf(car + 0x80u),
            lf(car + 0x10Cu), lf(car + 0x84u));
        bool high_speed_brake = false;
        if (!(cur_fwd < bf(0x40D55555u))) {  // >= 6.6666665f
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            call_vtable(input, 0x10u, 0x0018F9F8u);
            if (!(bf(0x3F19999Au) < g_ee.fpr[0].f)) {  // <= 0.6f
                high_speed_brake = true;
            }
        }
        if (high_speed_brake) {
            clutch = 0.0f;
            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].f = bf(0x3F800000u);
            call(f_0018AA68, 0x0018FA54u);

            car = lwu(drv + 0x38u);
            if (lf(car + 0x1B60u) < bf(0x455AC000u)) {  // 3500.0f
                const uint32_t input = lwu(drv + 0x3Cu);
                set(4, (int32_t)input);
                call_vtable(input, 0x10u, 0x0018FA88u);
                const float thr = fmax_ps2(g_ee.fpr[0].f, bf(0x3F800000u));
                set(4, (int32_t)lwu(drv + 0x38u));
                g_ee.fpr[12].f = thr;
                call(f_0018AA70, 0x0018FA98u);
            }
        } else {
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            call_vtable(input, 0x10u, 0x0018FA2Cu);
            const float thr = fmax_ps2(g_ee.fpr[0].f, bf(0x3F800000u));
            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].f = thr;
            call(f_0018AA70, 0x0018FA98u);
        }
    } else if (lw(drv + 0x04u) == 0) {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x20u, 0x0018FB20u);
        set(4, (int32_t)lwu(drv + 0x38u));
        set(5, (int32_t)reg(2));
        call(f_0018AA50, 0x0018FB2Cu);

        const uint32_t in2 = lwu(drv + 0x3Cu);
        car = lwu(drv + 0x38u);
        set(4, (int32_t)in2);
        set(5, lw(car + 0x1B9Cu));
        call_vtable(in2, 0x24u, 0x0018FB44u);
    } else {
        car = lwu(drv + 0x38u);
        bool do_auto = (0.0f < lf(car + 0x1B5Cu)) || (lw(car + 0x1B9Cu) > 0);
        if (!do_auto) {
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            call_vtable(input, 0x10u, 0x0018FAE4u);
            do_auto = (bf(0x3F000000u) < g_ee.fpr[0].f);
        }
        if (do_auto) {
            set(4, (int32_t)lwu(drv + 0x38u));
            call(f_0018AA40, 0x0018FB08u);
        }
    }

    // Clutch output
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x1Cu, 0x0018FB60u);
        const float out_clutch = fmax_ps2(fsub_ps2(bf(0x3F800000u), g_ee.fpr[0].f), clutch);
        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].f = out_clutch;
        call(f_0018AA60, 0x0018FB74u);
    }

    // Weapon trigger vs countdown/race-end freeze
    uint32_t weapon_btn;
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x2Cu, 0x0018FB88u);
        weapon_btn = reg(2);
    }
    const float sim_time = lf(kSimTime);
    call(f_00179888, 0x0018FB94u);
    bool frozen = (reg(2) != 0);
    if (!frozen) {
        g_ee.fpr[12].f = bf(0x3F800000u);
        call(f_00179868, 0x0018FBA4u);
        frozen = (reg(2) != 0);
    }

    if (frozen) {
        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].f = bf(0x3F800000u);
        call(f_0018AA68, 0x0018FBB8u);

        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].u = 0;
        call(f_0018AA70, 0x0018FBC4u);

        set(4, (int32_t)lwu(drv + 0x38u));
        g_ee.fpr[12].u = 0;
        call(f_0018AA80, 0x0018FBD0u);

        if (lw(drv + 0x04u) == 0) {
            set(4, (int32_t)lwu(drv + 0x38u));
            set(5, 0);
            call(f_0018AA50, 0x0018FBE4u);

            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            set(5, 0);
            call_vtable(input, 0x24u, 0x0018FBF8u);
        }
        set(4, (int32_t)lwu(drv + 0x38u));
        set(5, 0);
        call(f_0018AA20, 0x0018FC04u);
    } else {
        set(4, (int32_t)lwu(drv + 0x38u));
        set(5, (int32_t)weapon_btn);
        call(f_0018AA20, 0x0018FC18u);

        const uint32_t cheats = lwu(kCheatMask);
        if ((cheats & 1u) != 0 && (cheats & 8u) != 0) {
            set(4, lw(drv + 0x40u));
            set(5, 0);
            call(f_0017DE78, 0x0018FC44u);
            const uint32_t launcher = reg(2);
            if (launcher != 0) {
                const uint32_t input = lwu(drv + 0x3Cu);
                set(4, (int32_t)input);
                call_vtable(input, 0x2Cu, 0x0018FC64u);
                const uint32_t btn = reg(2);
                sw(sp + 0x10u, btn);
                sw(sp + 0x14u, 0);
                sw(sp + 0x00u, btn);
                sw(sp + 0x04u, 0);

                car = lwu(drv + 0x38u);
                const uint32_t ctrl = lwu(car + 0x260u);
                set(4, (int32_t)ctrl);
                call_vtable(ctrl, 0x10u, 0x0018FC94u);
                if (lw(reg(2) + 0x8Cu) == 0) {
                    set(4, (int32_t)launcher);
                    set(5, (int32_t)lwu(drv + 0x38u));
                    set(6, (int32_t)sp);
                    call_vtable(launcher, 0x08u, 0x0018FCB8u);
                }
            }
        }
    }

    // If contestant is dead/wrecked (contestant+0x8C != 0), lock controls
    {
        car = lwu(drv + 0x38u);
        const uint32_t ctrl = lwu(car + 0x260u);
        set(4, (int32_t)ctrl);
        call_vtable(ctrl, 0x10u, 0x0018FCD0u);
        if (lw(reg(2) + 0x8Cu) != 0) {
            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].f = bf(0x3F800000u);
            call(f_0018AA68, 0x0018FCF0u);

            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].u = 0;
            call(f_0018AA70, 0x0018FCFCu);

            set(4, (int32_t)lwu(drv + 0x38u));
            g_ee.fpr[12].u = 0;
            call(f_0018AA80, 0x0018FD08u);
        }
    }

    // Impact force-feedback ring buffer & wheel rumble terms
    car = lwu(drv + 0x38u);
    const float ix = lf(drv + 0x14u);
    const float iy = lf(drv + 0x18u);
    const float iz = lf(drv + 0x1Cu);
    const float lat_imp = fneg(madd3(
        ix, lf(car + 0x64u), iy, lf(car + 0x68u), iz, lf(car + 0x6Cu)));
    float fwd_imp = fmul_ps2(fabs_ee(madd3(
        ix, lf(car + 0x7Cu), iy, lf(car + 0x80u), iz, lf(car + 0x84u))), bf(0x3ECCCCCDu));
    if (lat_imp < 0.0f) fwd_imp = fneg(fwd_imp);

    const int32_t frame = lw(kSimFrame);
    const int32_t slot = frame - (((frame < 0 ? frame + 3 : frame) >> 2) << 2);
    sf(drv + 0x24u + (uint32_t)(slot << 2),
       fmul_ps2(fadd_ps2(lat_imp, fwd_imp), bf(0x3DCCCCCDu)));

    const float h0 = lf(drv + 0x24u);
    const float h1 = lf(drv + 0x28u);
    const float h2 = lf(drv + 0x2Cu);
    const float h3 = lf(drv + 0x30u);
    const float h_sum = fadd_ps2(fadd_ps2(fadd_ps2(h0, h1), h2), h3);
    float h_abs = fadd_ps2(fadd_ps2(fadd_ps2(fabs_ee(h0), fabs_ee(h1)), fabs_ee(h2)), fabs_ee(h3));
    if (h_sum <= 0.0f) h_abs = fneg(h_abs);

    const float impact_ffb = fmul_ps2(fmul_ps2(h_abs, lf(drv + 0x20u)), bf(0x3C1374BCu));  // 0.009f
    float low_rumble = 0.0f;
    if ((lbu(car + 0x1A4Cu) & 8u) != 0 || (lbu(car + 0x1AA0u) & 8u) != 0) {
        float t = fmul_ps2(lf(car + 0x1B64u), bf(0x3CB851EBu));  // 0.0225f
        t = fmin_ps2(t, bf(0x3F800000u));
        low_rumble = fmax_ps2(low_rumble, t);
    }

    float susp_sum = fadd_ps2(lf(car + 0x1AF0u), lf(car + 0x1B44u));
    susp_sum = fmax_ps2(0.0f, fmin_ps2(susp_sum, bf(0x3F800000u)));
    float high_rumble = fadd_ps2(impact_ffb, fmul_ps2(susp_sum, bf(0x3E8F5C29u)));  // 0.28f

    if ((lbu(car + 0x1A50u) & 0x20u) != 0 || (lbu(car + 0x1AA4u) & 0x20u) != 0) {
        float t = fmul_ps2(lf(car + 0x1B64u), bf(0x3D666666u));  // 0.05625f
        t = fmin_ps2(bf(0x3F800000u), t);
        high_rumble = fadd_ps2(high_rumble, fmul_ps2(t, bf(0x3ECCCCCDu)));  // 0.4f
    }

    const float kQuarter = bf(0x3E800000u);
    set(4, (int32_t)lwu(drv + 0x38u));
    call(f_0018A9E0, 0x0018FEF8u);
    const uint32_t rumble_parity = ((uint32_t)(lw(kSimFrame) >> 1)) & 1u;

    call(f_00179888, 0x0018FF08u);
    bool do_rumble = (reg(2) == 0);
    if (do_rumble) {
        call(f_00203248, 0x0018FF18u);
        do_rumble = (reg(2) != 0);
    }
    if (do_rumble) {
        call(f_00179868, 0x0018FF28u);
        do_rumble = (reg(2) == 0);
    }

    if (do_rumble) {
        g_ee.fpr[12].f = lf(kSimTime);
        g_ee.fpr[13].f = bf(0x3F800000u);
        call(f_0039C750, 0x0018FF40u);

        float osc = fadd_ps2(fmul_ps2(low_rumble, kQuarter), fmul_ps2(high_rumble, kQuarter));
        if (rumble_parity != 0) osc = fneg(osc);

        const uint32_t in_vt = lwu(lwu(drv + 0x3Cu));
        set(4, (int32_t)lwu(drv + 0x38u));
        call(f_0018A9F0, 0x0018FF64u);

        set(4, (int32_t)lwu(drv + 0x3Cu));
        g_ee.fpr[12].f = 0.0f;
        g_ee.fpr[13].f = fmul_ps2(g_ee.fpr[0].f, kQuarter);
        g_ee.fpr[14].f = fadd_ps2(impact_ffb, osc);
        call_at(lwu(in_vt + 0x48u), 0x0018FF84u);

        // Steering-wheel lateral force feedback via small-angle rotation matrix
        car = lwu(drv + 0x38u);
        const uint32_t wheels_base = car + 0x1A00u;
        const float kNeg005 = bf(0xBD4CCCCDu);
        const float kOne = bf(0x3F800000u);
        const float wx = fmul_ps2(lf(car + 0x110u), kNeg005);
        const float wy = fmul_ps2(lf(car + 0x114u), kNeg005);
        const float wz = fmul_ps2(lf(car + 0x118u), kNeg005);
        sf(sp + 0x30u, wx);
        sf(sp + 0x34u, wy);
        sf(sp + 0x38u, wz);

        float spd_gain = fmul_ps2(lf(car + 0x1B64u), bf(0x3E666666u));  // 0.225f
        spd_gain = fmax_ps2(0.0f, fmin_ps2(spd_gain, kOne));

        const float ang = fsqrt_ps2(madd3(wx, wx, wy, wy, wz, wz));
        float m00, m01, m02, m10, m11, m12, m20, m21, m22;
        if (bf(0x38D1B717u) < ang) {  // 0.0001f
            const float inv = fdiv_ps2(kOne, ang);
            const float az = fmul_ps2(inv, wz);
            const float ay = fmul_ps2(inv, wy);
            const float ax = fmul_ps2(inv, wx);

            g_ee.fpr[12].f = ang;
            call(f_0039BD68, 0x0019002Cu);
            const float s = g_ee.fpr[0].f;

            g_ee.fpr[12].f = ang;
            call(f_0039BB28, 0x00190038u);
            const float c = g_ee.fpr[0].f;
            const float omc = fsub_ps2(kOne, c);
            car = lwu(drv + 0x38u);

            const float sz = fmul_ps2(s, az);
            const float sy = fmul_ps2(s, ay);
            const float sx = fmul_ps2(s, ax);
            const float tx = fmul_ps2(omc, ax);
            const float ty = fmul_ps2(omc, ay);
            const float tx_z = fmul_ps2(tx, az);
            const float tx_y = fmul_ps2(tx, ay);
            const float ty_z = fmul_ps2(ty, az);

            m00 = fadd_ps2(fmul_ps2(omc, fmul_ps2(ax, ax)), c);
            m01 = fadd_ps2(tx_y, sz);
            m02 = fsub_ps2(tx_z, sy);
            m10 = fsub_ps2(tx_y, sz);
            m11 = fadd_ps2(fmul_ps2(omc, fmul_ps2(ay, ay)), c);
            m12 = fadd_ps2(ty_z, sx);
            m20 = fadd_ps2(tx_z, sy);
            m21 = fsub_ps2(ty_z, sx);
            m22 = fadd_ps2(fmul_ps2(omc, fmul_ps2(az, az)), c);
        } else {
            m00 = kOne; m01 = 0.0f; m02 = 0.0f;
            m10 = 0.0f; m11 = kOne; m12 = 0.0f;
            m20 = 0.0f; m21 = 0.0f; m22 = kOne;
        }

        const float rx = lf(car + 0x64u);
        const float ry = lf(car + 0x68u);
        const float rz = lf(car + 0x6Cu);
        const float rtx = madd3(rx, m00, ry, m10, rz, m20);
        const float rty = madd3(rx, m01, ry, m11, rz, m21);
        const float rtz = madd3(rx, m02, ry, m12, rz, m22);

        const float slip_avg = fmul_ps2(
            fadd_ps2(lf(wheels_base + 0x48u), lf(wheels_base + 0x9Cu)), bf(0x3F000000u));
        const float grip_term = fmax_ps2(fsub_ps2(kOne, slip_avg), 0.0f);

        const float denom = fmax_ps2(lf(car + 0x1B64u), bf(0x3DCCCCCDu));
        float lat_ratio = fdiv_ps2(
            madd3(lf(car + 0x104u), rtx, lf(car + 0x108u), rty, lf(car + 0x10Cu), rtz), denom);
        lat_ratio = fmul_ps2(lat_ratio, bf(0x41200000u));  // 10.0f
        lat_ratio = fmax_ps2(bf(0xBF666666u), fmin_ps2(lat_ratio, bf(0x3F666666u)));  // [-0.9, 0.9]

        const float w_load = fmul_ps2(
            fadd_ps2(fmax_ps2(0.0f, lf(car + 0x1A0Cu)), fmax_ps2(0.0f, lf(car + 0x1A60u))),
            bf(0x419D7AF6u));  // 19.68504f
        const float load_clamped = fmin_ps2(kOne, w_load);

        float steer_gain = fmul_ps2(fabs_ee(lf(car + 0x1B80u)), bf(0x40800000u));  // 4.0f
        steer_gain = fmax_ps2(0.0f, fmin_ps2(steer_gain, kOne));

        {
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            g_ee.fpr[12].f = fmul_ps2(fmul_ps2(spd_gain, grip_term), load_clamped);
            g_ee.fpr[13].f = fmul_ps2(lat_ratio, steer_gain);
            g_ee.fpr[14].f = bf(0x3F666666u);  // 0.9f left in $f14 by 00190288, read by vtable+0x4C
            call_vtable(input, 0x4Cu, 0x001902D4u);
        }
        {
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            g_ee.fpr[12].f = low_rumble;
            g_ee.fpr[13].f = high_rumble;
            call_vtable(input, 0x50u, 0x001902ECu);
        }
        {
            const uint32_t input = lwu(drv + 0x3Cu);
            set(4, (int32_t)input);
            set(5, 1);
            call_vtable(input, 0x6Cu, 0x00190300u);
        }
    } else {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        set(5, 0);
        call_vtable(input, 0x6Cu, 0x00190318u);
    }

    // Input post-update, camera look-back flag, clear impact accumulator
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x3Cu, 0x0019032Cu);
    }
    {
        const uint32_t input = lwu(drv + 0x3Cu);
        set(4, (int32_t)input);
        call_vtable(input, 0x44u, 0x00190340u);
    }
    const uint32_t look_back = reg(2);
    car = lwu(drv + 0x38u);
    sw(drv + 0x14u, 0);
    sw(drv + 0x18u, 0);
    sw(drv + 0x1Cu, 0);
    sw(car + 0x1BA0u, look_back);

    // Wrong-way detection (> 2.2222223 m/s for > 4.0s)
    const uint32_t ctrl = lwu(car + 0x260u);
    set(4, (int32_t)ctrl);
    call_vtable(ctrl, 0x18u, 0x00190368u);

    set(4, (int32_t)reg(2));
    set(5, (int32_t)(lwu(drv + 0x38u) + 0x104u));
    call(f_0013B0D0, 0x00190378u);
    if (reg(2) == 0) {
        sf(drv + 0x44u, sim_time);
    } else {
        car = lwu(drv + 0x38u);
        if (bf(0x400E38E4u) < lf(car + 0x1B5Cu)) {  // 2.2222223f
            if (bf(0x40800000u) < fsub_ps2(sim_time, lf(drv + 0x44u))) {  // 4.0f
                set(4, (int32_t)ctrl);
                set(5, 4);
                call_vtable(ctrl, 0x24u, 0x001903D8u);

                set(4, 13);
                set(5, 4);
                set(6, lw(drv + 0x40u));
                call(f_001D61D0, 0x001903E8u);
            }
        } else {
            sf(drv + 0x44u, sim_time);
        }
    }

    // Controller update (vtable +0x2C) & periodic upside-down check
    car = lwu(drv + 0x38u);
    sw(sp + 0x00u, 0);
    sw(sp + 0x04u, 0);
    sw(sp + 0x110u, 0);
    {
        const uint32_t c2 = lwu(car + 0x260u);
        set(4, (int32_t)c2);
        set(5, (int32_t)sp);
        set(6, (int32_t)(sp + 0x110u));
        set(7, (int32_t)lwu(sp + 0x118u));
        call_vtable(c2, 0x2Cu, 0x00190420u);
    }
    const uint8_t frame_lo = lbu(kSimFrame);
    if (frame_lo != 0) {
        set(2, (int32_t)frame_lo);
    } else {
        car = lwu(drv + 0x38u);
        if (lf(car + 0x74u) < 0.0f) {
            set(4, lw(drv + 0x40u));
            g_ee.fpr[12].f = bf(0x3F800000u);
            call(f_0013E490, 0x00190454u);
        } else {
            set(2, (int32_t)car);
        }
    }
}

// ============================================================================
// 6. 0x00189648 — CarApplySolvedContact(car, impulse, point, surface, other)
// ============================================================================
void car_apply_solved_contact() {
    const uint32_t car     = reg(4);
    const uint32_t impulse = reg(5);
    const uint32_t point   = reg(6);
    const uint32_t surface = reg(7);
    const uint32_t other   = reg(8);
    Frame frame(0x150u);
    const uint32_t sp = frame.sp;

    // Pre-contact hook (receives original a0..t0 intact)
    call(f_0018AA90, 0x00189690u);

    const float ix = lf(impulse + 0x0u);
    const float iy = lf(impulse + 0x4u);
    const float iz = lf(impulse + 0x8u);
    const float imp_len = fsqrt_ps2(madd3(ix, ix, iy, iy, iz, iz));

    const bool is_car_contact = (other != 0) && (surface == 0x6Du);
    float scale = bf(0x3F800000u);  // 1.0f
    int32_t other_car_idx = -1;
    int32_t exit_v0 = 0x70;  // last $v0 value on non-damage exit (for EOD_DIFF)

    if (surface == 0x6Du && other != 0) {
        if (is_car_tag(lwu(other + 0x0Cu), true)) {
            other_car_idx = lw(other + 0x1DCu);
            const int32_t self_idx = lw(car + 0x1DCu);
            if (self_idx < other_car_idx) {
                set(4, 10);
                set(5, self_idx);
                set(6, other_car_idx);
                g_ee.fpr[12].f = imp_len;
                call(f_00257BB8, 0x00189740u);
            }

            float nx = lf(impulse + 0x0u);
            float ny = lf(impulse + 0x4u);
            float nz = lf(impulse + 0x8u);
            if (bf(0x3DCCCCCDu) < imp_len) {  // 0.1f
                const float inv = frsqrt_ps2(bf(0x3F800000u), madd3(nx, nx, ny, ny, nz, nz));
                nz = fmul_ps2(nz, inv);
                nx = fmul_ps2(nx, inv);
                ny = fmul_ps2(ny, inv);
            }

            exit_v0 = (int32_t)(car + 0x104u);
            const float self_v = fabs_ee(madd3(
                nx, lf(car + 0x104u), ny, lf(car + 0x108u), nz, lf(car + 0x10Cu)));
            const float other_v = fabs_ee(madd3(
                nx, lf(other + 0x104u), ny, lf(other + 0x108u), nz, lf(other + 0x10Cu)));
            const float dv = fsub_ps2(other_v, self_v);

            if (bf(0x40D55555u) < dv && fmul_ps2(self_v, bf(0x3FC00000u)) < other_v) {
                const float denom = fmax_ps2(bf(0x3A83126Fu), self_v);  // 0.001f
                scale = fmin_ps2(bf(0x40A00000u), fdiv_ps2(other_v, denom));  // 5.0f
            } else if (dv < bf(0xC0D55555u) && fmul_ps2(other_v, bf(0x3FC00000u)) < self_v) {
                exit_v0 = (int32_t)(car + 0x34u);
                const float dx = fsub_ps2(lf(point + 0x0u), lf(car + 0x58u));
                const float dy = fsub_ps2(lf(point + 0x4u), lf(car + 0x5Cu));
                const float dz = fsub_ps2(lf(point + 0x8u), lf(car + 0x60u));
                const float denom = fmax_ps2(bf(0x3A83126Fu), self_v);
                scale = fmax_ps2(bf(0x3E4CCCCDu), fdiv_ps2(other_v, denom));  // 0.2f

                const float local_z = madd3(
                    dx, lf(car + 0x4Cu), dy, lf(car + 0x50u), dz, lf(car + 0x54u));
                if (local_z < bf(0xBF020C4Au)) {  // -0.508f
                    exit_v0 = (int32_t)(car + 0x4Cu);
                    const float imp_fwd = madd3(
                        lf(impulse + 0x0u), lf(car + 0x4Cu),
                        lf(impulse + 0x4u), lf(car + 0x50u),
                        lf(impulse + 0x8u), lf(car + 0x54u));
                    if (0.0f < imp_fwd) scale = 0.0f;
                }
            }
        } else {
            exit_v0 = (int32_t)kTagTRLR;
        }
    } else if (surface == 0x6Eu && other == 0) {
        set(4, 11);
        set(5, lw(car + 0x1DCu));
        set(6, 0);
        g_ee.fpr[12].f = imp_len;
        call(f_00257BB8, 0x001899CCu);
    } else if (surface == 0x70u && other != 0) {
        if (is_car_tag(lwu(other + 0x0Cu), false)) {
            other_car_idx = lw(other + 0x1DCu);
        }
    }

    float mult = lf(car + 0x4B0u);
    if (is_car_contact) {
        mult = fmul_ps2(mult, scale);
    } else if (surface == 0x70u) {
        mult = fmul_ps2(mult, bf(0x40E00000u));  // 7.0f
    } else {
        mult = fmul_ps2(fmul_ps2(mult, bf(0x3ECCCCCDu)), lf(car + 0x2F0u));  // 0.4f
    }

    const float sx = fmul_ps2(lf(impulse + 0x0u), mult);
    const float sy = fmul_ps2(lf(impulse + 0x4u), mult);
    const float sz = fmul_ps2(lf(impulse + 0x8u), mult);
    const float scaled_len = fmul_ps2(imp_len, mult);

    if (bf(0x44396AABu) < scaled_len) {  // 741.6667f
        const float m00 = lf(car + 0x64u), m01 = lf(car + 0x68u), m02 = lf(car + 0x6Cu);
        const float m10 = lf(car + 0x70u), m11 = lf(car + 0x74u), m12 = lf(car + 0x78u);
        const float m20 = lf(car + 0x7Cu), m21 = lf(car + 0x80u), m22 = lf(car + 0x84u);

        const float cx = lf(car + 0x28u);
        const float cy = lf(car + 0x2Cu);
        const float cz = lf(car + 0x30u);

        const float wx = fadd_ps2(madd3(cx, m00, cy, m10, cz, m20), lf(car + 0x88u));
        const float wy = fadd_ps2(madd3(cx, m01, cy, m11, cz, m21), lf(car + 0x8Cu));
        const float wz = fadd_ps2(madd3(cx, m02, cy, m12, cz, m22), lf(car + 0x90u));

        const float px = fsub_ps2(lf(point + 0x0u), wx);
        const float py = fsub_ps2(lf(point + 0x4u), wy);
        const float pz = fsub_ps2(lf(point + 0x8u), wz);

        // Local contact point at sp+0x50
        sf(sp + 0x50u, madd3(px, m00, py, m01, pz, m02));
        sf(sp + 0x54u, madd3(px, m10, py, m11, pz, m12));
        sf(sp + 0x58u, madd3(px, m20, py, m21, pz, m22));

        // Local scaled impulse at sp+0x60
        sf(sp + 0x60u, madd3(sx, m00, sy, m01, sz, m02));
        sf(sp + 0x64u, madd3(sx, m10, sy, m11, sz, m12));
        sf(sp + 0x68u, madd3(sx, m20, sy, m21, sz, m22));

        set(4, (int32_t)car);
        set(5, (int32_t)(sp + 0x60u));
        set(6, (int32_t)(sp + 0x50u));
        set(7, 0);
        set(8, other_car_idx);
        set(9, (int32_t)surface);
        call(f_0018CA98, 0x00189CD8u);
    } else {
        set(2, exit_v0);
    }
}

}  // namespace

// ---- diff_run-backed entry points called by existing wrappers ----
extern "C" void decomp_car_set_brake(void) {
    diff_run(0x0018AA68, car_set_brake, f_0018AA68_orig);
}
extern "C" void decomp_car_set_throttle(void) {
    diff_run(0x0018AA70, car_set_throttle, f_0018AA70_orig);
}
extern "C" void decomp_car_can_take_damage(void) {
    diff_run(0x0018AAD0, car_can_take_damage, f_0018AAD0_orig);
}
extern "C" void decomp_car_set_damage(void) {
    diff_run(0x0018AD20, car_set_damage, f_0018AD20_orig);
}
extern "C" void decomp_human_driver_update(void) {
    diff_run(0x0018F2A8, human_driver_update, f_0018F2A8_orig);
}
extern "C" void decomp_car_apply_solved_contact(void) {
    diff_run(0x00189648, car_apply_solved_contact, f_00189648_orig);
}
