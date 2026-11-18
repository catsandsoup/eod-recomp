// car_damage.cpp - car collision damage and dents (SLUS_209.10), readable C++ replacing the
// recompiled code. Stage R3 of the native renderer: these functions own the dent offsets the native
// renderer draws (docs/NATIVE_RENDERER.md "Stage R3").
//
//   0x17B918 ContactRecord(obj, id, impulse, point, t0, alt point, t2; f12 scale)
//            appends a 0x24-byte contact record from the pool 0x3CE3F0 (used 0x3CE3EC, size
//            0x3CE3E8) to the object's list (obj+0x190, 16 slots, count obj+0x1D0), then tail-calls
//            0x17B818(obj, impulse, point).
//   0x18CA98 CarApplyCollisionDamage(car, impulse, point, negate, other car, surface)
//            caller 0x189CD0 (after the collision response). Dents the bones of Bone.mvl nearest
//            the contact (the dent offsets car+0x4B8 that VisualCarSubmitModel 0x2189C8 uploads
//            through 0x34CA18 -> 0x35DD28 to VU1 qword 0x90 for Skin::Offset), detaches panels
//            past their distance (0x44AE68 table, 0x1E0DC8), raises damage and the wreck state.
//
// As src/render_queue.cpp and src/native_world.cpp: every replacement makes the same guest calls in
// the same order with the same return addresses, stack frame and arguments, the same memory writes
// and leaves the same $v0, so EOD_DIFF proves it against the recompiled original
// (runtime/core/diff_harness.h). Floats follow the EE FPU (fadd_ps2 ... as the recompiler).
//
// Car fields (car = a0):
//   +0x1DC car index          +0x260 controller (vtable +0x34 Impair(other), +0x3C IsWrecked())
//   +0x25C damage model M:     M+0x1C bone count, bones at M+0x20 + 0x34*i:
//                              [0] dent slot, [1..3] dent min, [4..6] dent max, [7..9] rest position,
//                              [10] full-dent length, [12] fixed (no dent); panel boxes at
//                              M+0x2150 + 0x18*bit (min xyz, max xyz)
//   +0x4B4 bone count          +0x4B8 dent offsets, 12 bytes per slot (x, y, z; the low byte of z is
//                              the dent shade 0x20..0x80 the offset program reads)
//   +0x13E8 panel states [8] x 0x18: [0] node, [1] bone, [3] f, [4] f, [5] state (2 loose, 3 off)
//   +0x1A00 wheels [4] x 0x54: [0] radius, +0x20 hub, +0x50 flags (0x80 tossed)
//   +0x1BE8 wreck state (0 intact, 1..4 damaged, 10/11/12 wrecked)   +0x1BEC damage 0..1
//   +0x1BF4 damage flash   +0x1BF8 cleared each call   +0x1C04 the side a wheel came off
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/limit_tracker.h"
#include "core/memory.h"

#include <cstdint>

extern "C" {
void f_0017B918_orig(void);
void f_0018CA98_orig(void);
void f_0017B818(void);  // ContactApply(obj, impulse, point)
void f_0018AAD0(void);  // CarCanTakeDamage(car)
void f_0018C6B8(void);  // CarDentBone(car, bone, impulse, -1, 0)
void f_0012F4F0(void);  // RngNextSignedFloat(4, n) -> 0..n-1
void f_001E0DC8(void);  // DetachedPanelPoolInit / spawn (car, bit, centre, min, max, t1, t2)
void f_001741C0(void);  // CareerIsFreeRoamActive
void f_001E06A8(void);  // CarSuspension(car index, out)
void f_0017D770(void);  // CarPointVelocity(car, out, point)
void f_001B3068(void);  // DetachedWheelSpawn(car index, wheel, matrix, velocity, position)
void f_00331E48(void);  // DebugPrintf
void f_001DA468(void);  // CarImpaired(car index, other)
void f_00202DA8(void);  // DamageCredit(f12 amount, car index, other)
void f_001D4B28(void);  // CarDamageShake(car, f12 amount)
}

namespace {

// ---- guest access (values as the EE leaves them: 32-bit results sign-extended) ----
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
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
inline void copy3(uint32_t dst, uint32_t src) {
    const uint32_t x = lwu(src), y = lwu(src + 4), z = lwu(src + 8);
    sw(dst, x), sw(dst + 4, y), sw(dst + 8, z);
}
inline float fneg(float v) { return bf(fb(v) ^ 0x80000000u); }
inline float fabs_ee(float v) { return bf(fb(v) & 0x7FFFFFFFu); }
// mula / madda / madd: a0*b0 + a1*b1 + a2*b2 through the accumulator
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
inline int32_t rng(int32_t n, uint32_t ra) {
    set(4, 4), set(5, n);
    call(f_0012F4F0, ra);
    return (int32_t)reg(2);
}

// constants (PS2 EE immediates, lui/ori in 0x18CA98)
constexpr uint32_t kDentReach = 0x3FE66666;      // 1.8: dent offset weight in the nearest-bone search
constexpr uint32_t kNearRadius2 = 0x40100000;    // 2.25: (1.5)^2 search radius
constexpr uint32_t kNearInit = 0x47C35000;       // 100000
constexpr uint32_t kOne = 0x3F800000;
constexpr uint32_t kSpreadMin = 0x3DCCCCCD;      // 0.1: distance^2 floor of the dent spread
constexpr uint32_t kSpread = 0x3E4CCCCD;         // 0.2: dent spread numerator
constexpr uint32_t kShadeScale = 0x43000000;     // 128: dent shade per full-dent length
constexpr uint32_t kLooseMin3 = 0x3C23D70A;      // 0.01
constexpr uint32_t kLooseMin4 = 0x3E4CCCCD;      // 0.2
constexpr uint32_t kHalfNeg = 0xBF000000;        // -0.5
constexpr uint32_t kEpsilon = 0x34000000;        // FLT_EPSILON
constexpr uint32_t kHardHit = 0x43960000;        // 300: impulse length that damages
constexpr uint32_t kCapCar = 0x3EB33333;         // 0.35: damage cap per hit by a car (surface 0x70)
constexpr uint32_t kCapOther = 0x3DCCCCCD;       // 0.1: by anything else
constexpr uint32_t kDamagePerImpulse = 0x36DFB23B;  // 6.6666666e-06
constexpr uint32_t kDamaged = 0x3F19999A;        // 0.6
constexpr uint32_t kHalf = 0x3F000000;           // 0.5
constexpr uint32_t kSmallHit = 0x3CF5C28F;       // 0.03
constexpr uint32_t kFreeRoamCap = 0x3F666666;    // 0.9
constexpr uint32_t kTossVel = 0x3E99999A;        // 0.3
constexpr uint32_t kMinDelta = 0x3A83126F;       // 0.001
constexpr uint32_t kFlash = 0x3DCCCCCD;          // 0.1
constexpr uint32_t kShakeFrom = 0x3CCCCCCD;      // 0.025
constexpr uint32_t kShakeGain = 0x40800000;      // 4
constexpr uint32_t kShakeMax = 0x3ECCCCCD;       // 0.4
constexpr uint32_t kSurfaceCar = 0x70;
constexpr uint32_t kPanelTable = 0x44AE68;       // 8 detachable panels x 0x40 (g_DetachablePanels)
constexpr uint32_t kCheatMask = 0x3C4190;
constexpr uint32_t kMsgToss = 0x3FA040, kMsgImpaired = 0x3FA068, kMsgReimpair = 0x3FA080;

// ---- 0x17B918 ----
constexpr uint32_t kContactSlots = 16;           // obj+0x190 .. obj+0x1CC (count obj+0x1D0)
uint32_t g_contacts_peak = 0;

void contact_record() {
    const uint32_t obj = reg(4), id = reg(5), impulse = reg(6), point = reg(7);
    const uint32_t t0 = reg(8), alt = reg(9), t2 = reg(10);
    const float scale = g_ee.fpr[12].f;
    const int32_t count = lw(obj + 0x1D0);
    int64_t v0 = 0;  // slti: count < 16 failed
    if (count < (int32_t)kContactSlots) {
        const int32_t used = lw(0x3CE3EC), size = lw(0x3CE3E8);
        uint32_t rec = 0;
        if (used < size) {
            rec = lwu(0x3CE3F0) + (uint32_t)used * 0x24;
            sw(0x3CE3EC, (uint32_t)(used + 1));
        } else {
            limit_note("contact records (0x3CE3F0)", (uint64_t)used + 1, (uint64_t)size, (uint64_t)size);
        }
        v0 = (int32_t)(rec + 4);
        if (rec) {
            sw(rec, id);
            sf(rec + 4, fmul_ps2(lf(impulse), scale));
            sf(rec + 8, fmul_ps2(lf(impulse + 4), scale));
            sf(rec + 12, fmul_ps2(lf(impulse + 8), scale));
            copy3(rec + 0x10, alt ? alt : point);
            sw(rec + 0x1C, t0);
            sw(rec + 0x20, t2);
            const int32_t n = lw(obj + 0x1D0);
            v0 = (int32_t)(obj + 4u * (uint32_t)n);
            sw(obj + 0x190 + 4u * (uint32_t)n, rec);
            sw(obj + 0x1D0, (uint32_t)(n + 1));
            if ((uint32_t)(n + 1) > g_contacts_peak) {
                g_contacts_peak = (uint32_t)(n + 1);
                limit_note("contacts per object (obj+0x190)", g_contacts_peak, kContactSlots, kContactSlots);
            }
        }
    } else {
        limit_note("contacts per object (obj+0x190)", kContactSlots + 1, kContactSlots, kContactSlots);
    }
    // tail call 0x17B818(obj, impulse, point): $ra is the caller's
    g_ee.r[2].sd[0] = v0;
    set(4, (int32_t)obj), set(5, (int32_t)impulse), set(6, (int32_t)point);
    f_0017B818();
}

// ---- 0x18CA98 ----
// stack frame 0x170 (callees see the original's $sp and the same data at the same offsets)
enum : uint32_t {
    kImpulse = 0x00,   // impulse, negated when a3 != 0 (to 0x18C6B8)
    kPoint = 0x10,     // contact point
    kNearest = 0x20,   // dented position of the nearest bone / scratch for the wheel toss
    kOld = 0x30,       // dent of the nearest bone before 0x18C6B8 / candidate position
    kDelta = 0x40,     // candidate - point
    kNormal = 0x50,    // -impulse, normalised
    kDentDelta = 0x60, // dent change of the nearest bone
    kCentre = 0x70,    // dented position of the nearest bone
    kBone = 0x80,      // dented position of bone i / panel box centre (to 0x1E0DC8)
    kBoneDelta = 0x90, // bone i - centre
    kSuspension = 0xA0,
    kFrame = 0x170,
};

struct Frame {
    uint32_t caller_sp;
    uint64_t ra;
    Frame() : caller_sp(reg(29)), ra(g_ee.r[31].ud[0]) { set(29, (int32_t)(caller_sp - kFrame)); }
    ~Frame() {
        set(29, (int32_t)caller_sp);
        g_ee.r[31].ud[0] = ra;
    }
    uint32_t sp() const { return caller_sp - kFrame; }
};

// Dents every free bone near the hit along the impulse: the nearest bone within 1.5 of the contact
// (dent offsets weighted 1.8) takes the impulse (0x18C6B8); every other bone moves along the
// normal by how far it lies behind the nearest bone's dent plane, falling off with 0.2/d^2 and
// clamped to its dent box. Returns the nearest bone entry (0 = none).
uint32_t dent_bones(uint32_t car, uint32_t sp) {
    const uint32_t model = lwu(car + 0x25C);
    const int32_t count = lw(car + 0x4B4);
    float best = bf(kNearInit);
    uint32_t nearest = 0;
    for (int32_t i = 1; i < count; i++) {
        const uint32_t bone = model + 0x20 + 0x34u * (uint32_t)i;
        if (lw(bone + 0x30) != 0) continue;
        const uint32_t dent = car + 0x4B8 + 12u * (uint32_t)i;
        const float reach = bf(kDentReach);
        const float x = fadd_ps2(lf(bone + 0x1C), fmul_ps2(lf(dent), reach));
        const float y = fadd_ps2(lf(bone + 0x20), fmul_ps2(lf(dent + 4), reach));
        const float z = fadd_ps2(lf(bone + 0x24), fmul_ps2(lf(dent + 8), reach));
        sf(sp + kOld, x), sf(sp + kOld + 4, y), sf(sp + kOld + 8, z);
        const float dx = fsub_ps2(x, lf(sp + kPoint)), dy = fsub_ps2(y, lf(sp + kPoint + 4)),
                    dz = fsub_ps2(z, lf(sp + kPoint + 8));
        sf(sp + kDelta, dx), sf(sp + kDelta + 4, dy), sf(sp + kDelta + 8, dz);
        const float d2 = madd3(dx, dx, dy, dy, dz, dz);
        if (fcmp_lt(d2, bf(kNearRadius2)) && fcmp_lt(d2, best)) {
            copy3(sp + kNearest, sp + kOld);
            best = d2;
            nearest = bone;
        }
    }
    if (!nearest) return 0;

    const uint32_t slot = lwu(nearest);
    copy3(sp + kOld, car + 0x4B8 + 12u * slot);
    set(4, (int32_t)car), set(5, (int32_t)slot), set(6, (int32_t)(sp + kImpulse)), set(7, -1), set(8, 0);
    call(f_0018C6B8, 0x18CCDC);

    const uint32_t dent = car + 0x4B8 + 12u * lwu(nearest);
    float nx = fneg(lf(sp + kImpulse)), ny = fneg(lf(sp + kImpulse + 4)), nz = fneg(lf(sp + kImpulse + 8));
    const float inv = frsqrt_ps2(bf(kOne), madd3(nx, nx, ny, ny, nz, nz));
    nz = fmul_ps2(nz, inv), nx = fmul_ps2(nx, inv), ny = fmul_ps2(ny, inv);
    sf(sp + kNormal, nx), sf(sp + kNormal + 4, ny), sf(sp + kNormal + 8, nz);
    const int32_t bones = lw(car + 0x4B4);
    const float ddx = fsub_ps2(lf(sp + kOld), lf(dent)), ddy = fsub_ps2(lf(sp + kOld + 4), lf(dent + 4)),
                ddz = fsub_ps2(lf(sp + kOld + 8), lf(dent + 8));
    sf(sp + kDentDelta, ddx), sf(sp + kDentDelta + 4, ddy), sf(sp + kDentDelta + 8, ddz);
    const float depth = fabs_ee(madd3(nx, ddx, ny, ddy, nz, ddz));
    const float cx = fadd_ps2(lf(nearest + 0x1C), lf(dent)), cy = fadd_ps2(lf(nearest + 0x20), lf(dent + 4)),
                cz = fadd_ps2(lf(nearest + 0x24), lf(dent + 8));
    sf(sp + kCentre, cx), sf(sp + kCentre + 4, cy), sf(sp + kCentre + 8, cz);

    for (int32_t i = 1; i < bones; i++) {
        const uint32_t bone = model + 0x20 + 0x34u * (uint32_t)i;
        const uint32_t d = car + 0x4B8 + 12u * lwu(bone);
        const float ox = lf(d), oy = lf(d + 4), oz = lf(d + 8);
        const float px = fadd_ps2(lf(bone + 0x1C), ox), py = fadd_ps2(lf(bone + 0x20), oy),
                    pz = fadd_ps2(lf(bone + 0x24), oz);
        sf(sp + kBone, px), sf(sp + kBone + 4, py), sf(sp + kBone + 8, pz);
        const float rx = fsub_ps2(px, cx), ry = fsub_ps2(py, cy), rz = fsub_ps2(pz, cz);
        sf(sp + kBoneDelta, rx), sf(sp + kBoneDelta + 4, ry), sf(sp + kBoneDelta + 8, rz);
        const float behind = fmin_ps2(depth, madd3(nx, rx, ny, ry, nz, rz));
        if (!fcmp_lt(0.0f, behind)) continue;
        const float r2 = fmax_ps2(madd3(rx, rx, ry, ry, rz, rz), bf(kSpreadMin));
        const float falloff = fmin_ps2(fdiv_ps2(bf(kSpread), r2), bf(kOne));
        int32_t shade = (int32_t)(fb(oz) & 0xFF) > 0x1F ? (int32_t)(fb(oz) & 0xFF) : 0x20;
        const float push = fmul_ps2(fneg(behind), falloff);
        const float z1 = fadd_ps2(oz, fmul_ps2(nz, push));
        const float x1 = fadd_ps2(ox, fmul_ps2(nx, push));
        const float y1 = fadd_ps2(oy, fmul_ps2(ny, push));
        sf(d + 8, z1), sf(d, x1), sf(d + 4, y1);
        const float x = fmax_ps2(lf(bone + 0x04), fmin_ps2(x1, lf(bone + 0x10)));
        sf(d, x);
        const float y = fmax_ps2(lf(bone + 0x08), fmin_ps2(y1, lf(bone + 0x14)));
        sf(d + 4, y);
        const float z = fmax_ps2(lf(bone + 0x0C), fmin_ps2(z1, lf(bone + 0x18)));
        sf(d + 8, z);
        // dent shade: |offset| / full-dent length * 128 in the low byte of z, 0x20..0x80
        const float len = fsqrt_ps2(madd3(x, x, y, y, z, z));
        const int32_t s = fcvtw_ps2(fmul_ps2(fdiv_ps2(len, lf(bone + 0x28)), bf(kShadeScale)));
        if (shade < s) shade = s;
        const uint32_t clamped = shade > 0x7F ? 0x80u : (uint32_t)shade;
        sw(d + 8, (fb(z) & 0xFFFFFF00u) | (clamped & 0xFF));
    }
    return nearest;
}

// Panels: a panel whose bone moved further than its distance comes loose (state 2) or off (3).
void update_panels(uint32_t car, uint32_t sp) {
    for (uint32_t i = 0; i < 8; i++) {
        const uint32_t st = car + 0x13E8 + 0x18 * i, panel = kPanelTable + 0x40 * i;
        int32_t state = lw(st + 0x14);
        const bool fresh = lwu(st) != 0 && lw(st + 4) > 0 && state == 0;
        if (!fresh && state != 2) continue;
        const float limit = lf(panel + 0x30);
        const uint32_t dent = car + 0x4B8 + 12u * lwu(st + 4);
        const float moved = madd3(lf(dent), lf(dent), lf(dent + 4), lf(dent + 4), lf(dent + 8), lf(dent + 8));
        if (!fcmp_lt(fmul_ps2(limit, limit), moved)) continue;
        const float dir = fabs_ee(madd3(lf(panel + 0xC), lf(panel + 0xC), lf(panel + 0x10), lf(panel + 0x10),
                                        lf(panel + 0x14), lf(panel + 0x14)));
        if (fcmp_le(dir, bf(kEpsilon))) {
            state = 3;  // no direction to hang from: comes off
            sw(st + 0x14, 3);
        } else if ((uint32_t)(lw(panel) - 2) < 2) {
            sw(st + 0x14, 2);  // types 2 and 3 only hang loose
            continue;
        } else {
            const int32_t r = rng(10, 0x18D0CC);
            if (lw(st + 0x14) == 2)
                state = r < 2 ? 3 : 2;
            else
                state = r < 7 ? 2 : r < 8 ? 3 : 1;
            sw(st + 0x14, (uint32_t)state);
            if (state == 2) {
                sf(st + 0xC, fmax_ps2(bf(kLooseMin3), lf(st + 0xC)));
                sf(st + 0x10, fmax_ps2(bf(kLooseMin4), lf(st + 0x10)));
                continue;
            }
        }
        if (state != 3) continue;
        // off: a detached panel phob from the panel's box (0x1E0DC8)
        const uint32_t bit = lwu(panel + 8);
        const uint32_t box = lwu(car + 0x25C) + bit * 0x18 + 0x2150;
        const float h = bf(kHalfNeg);
        const float bx = fadd_ps2(lf(box), lf(box + 0xC));
        sf(sp + kBone, bx);
        const float by = fadd_ps2(lf(box + 4), lf(box + 0x10));
        sf(sp + kBone + 4, by);
        const float bz = fadd_ps2(lf(box + 8), lf(box + 0x14));
        sf(sp + kBone, fmul_ps2(bx, h));
        sf(sp + kBone + 4, fmul_ps2(by, h));
        sf(sp + kBone + 8, fmul_ps2(bz, h));
        set(4, (int32_t)car), set(5, (int32_t)(1u << (bit & 31))), set(6, (int32_t)(sp + kBone));
        set(7, (int32_t)box), set(8, (int32_t)(box + 0xC)), set(9, lw(panel + 0x34)), set(10, (int32_t)(panel + 0x38));
        call(f_001E0DC8, 0x18D1D0);
    }
}

// A wheel comes off on the side of the hit (0x1B3068) with the car's velocity at its hub.
void toss_wheel(uint32_t car, uint32_t sp, uint32_t wheel_index) {
    const uint32_t wheel = car + 0x1A00 + 0x54 * wheel_index;
    set(4, lw(car + 0x1DC)), set(5, (int32_t)(sp + kSuspension));
    call(f_001E06A8, 0x18D4F0);
    const uint32_t susp = lwu(sp + kSuspension);
    // the suspension's per-wheel stiffness (susp+0x70, one lane per wheel) halves on this wheel
    MEM_LQ(susp + 0x70, &g_ee.r[2]);
    MEM_SQ(sp + 0x40, &g_ee.r[2]);
    const uint32_t q0 = lwu(sp + 0x40), q1 = lwu(sp + 0x44), q2 = lwu(sp + 0x48), q3 = lwu(sp + 0x4C);
    sw(sp + 0x30, q0), sw(sp + 0x34, q1), sw(sp + 0x38, q2);
    sw(sp + 0x24, q1), sw(sp + 0x2C, q3), sw(sp + 0x28, q2), sw(sp + 0x20, q0);
    const uint32_t lane = sp + kNearest + 4 * wheel_index;
    sf(lane, fmul_ps2(lf(lane), bf(kHalf)));
    copy3(sp + 0x30, sp + 0x20);
    sw(sp + 0x3C, lwu(sp + 0x2C));
    MEM_LQ(sp + 0x30, &g_ee.r[2]);
    MEM_SQ(susp + 0x70, &g_ee.r[2]);
    // car orientation (car+0x64, 9 words) and the hub pushed 0.3 along the car's up axis
    for (uint32_t k = 0; k < 0x24; k += 4) sw(sp + 0x20 + k, lwu(car + 0x64 + k));
    copy3(sp + 0x44, wheel + 0x20);
    const float t = bf(kTossVel);
    sf(sp + 0x44, fadd_ps2(lf(sp + 0x44), fmul_ps2(lf(sp + 0x2C), t)));
    const float u = fmul_ps2(lf(sp + 0x30), t), w = fmul_ps2(lf(sp + 0x34), t);
    sf(sp + 0x48, fadd_ps2(lf(sp + 0x48), u));
    sf(sp + 0x4C, fadd_ps2(lf(sp + 0x4C), w));
    g_ee.r[2].ud[0] = (uint64_t)lwu(wheel + 0x20) | (uint64_t)lwu(wheel + 0x24) << 32;  // ldl/ldr $v0
    set(4, (int32_t)car), set(5, (int32_t)(sp + kNormal)), set(6, (int32_t)(sp + 0x44));
    call(f_0017D770, 0x18D62C);
    const float radius = lf(wheel);
    sf(sp + 0x60, fadd_ps2(fmul_ps2(lf(sp + 0x20), radius), lf(car + 0x110)));
    sf(sp + 0x64, fadd_ps2(fmul_ps2(lf(sp + 0x24), radius), lf(car + 0x114)));
    sf(sp + 0x68, fadd_ps2(fmul_ps2(lf(sp + 0x28), radius), lf(car + 0x118)));
    set(4, lw(car + 0x1DC)), set(5, (int32_t)wheel_index), set(6, (int32_t)(sp + 0x20)), set(7, (int32_t)(sp + kNormal));
    set(8, (int32_t)(sp + 0x60));
    call(f_001B3068, 0x18D688);
    sw(wheel + 0x50, lwu(wheel + 0x50) | 0x80);
    set(4, (int32_t)kMsgToss), set(5, (int32_t)wheel_index), set(6, lw(car + 0x1DC));
    call(f_00331E48, 0x18D6A8);
}

inline int32_t controller_call(uint32_t car, uint32_t slot, uint32_t ra) {
    const uint32_t obj = lwu(car + 0x260);
    set(4, (int32_t)obj);
    call_at(lwu(lwu(obj) + slot), ra);
    return (int32_t)reg(2);
}

// the side of the car the hit came from: 0 / 1 = front left / right of x, 3 = behind (z <= 0)
inline uint32_t hit_side(uint32_t sp) {
    if (!fcmp_lt(0.0f, lf(sp + kPoint + 8))) return 3;
    return fcmp_lt(lf(sp + kPoint), 0.0f) ? 0u : 1u;
}

void apply_collision_damage() {
    Frame fr;
    const uint32_t sp = fr.sp();
    const uint32_t car = reg(4), impulse = reg(5), point = reg(6), negate = reg(7);
    const int32_t other = (int32_t)reg(8);
    const uint32_t surface = reg(9);

    call(f_0018AAD0, 0x18CAF0);
    if (g_ee.r[2].ud[0] == 0) return;

    copy3(sp + kImpulse, impulse);
    if (negate) {
        sf(sp + kImpulse, fneg(lf(sp + kImpulse)));
        sf(sp + kImpulse + 4, fneg(lf(sp + kImpulse + 4)));
        sf(sp + kImpulse + 8, fneg(lf(sp + kImpulse + 8)));
    }
    copy3(sp + kPoint, point);
    const float ix = lf(sp + kImpulse), iy = lf(sp + kImpulse + 4), iz = lf(sp + kImpulse + 8);
    const float strength = fsqrt_ps2(madd3(ix, ix, iy, iy, iz, iz));
    sw(sp + kNearest, 0), sw(sp + kNearest + 4, 0), sw(sp + kNearest + 8, 0);

    if (dent_bones(car, sp)) update_panels(car, sp);

    // damage and wreck state; $v0 as the original leaves it on each path
    const float before = lf(car + 0x1BEC);
    float now = before;
    bool impaired = false;
    int64_t v0 = 0;
    const int32_t state = lw(car + 0x1BE8);
    const float cap = surface == kSurfaceCar ? bf(kCapCar) : bf(kCapOther);
    auto impair = [&]() {
        set(4, (int32_t)kMsgImpaired), set(5, lw(car + 0x1DC));
        call(f_00331E48, 0x18D6BC);
        impaired = true;
        set(4, lw(car + 0x1DC)), set(5, other);
        call(f_001DA468, 0x18D6C8);
        v0 = g_ee.r[2].sd[0];
        now = lf(car + 0x1BEC);
    };
    if (state == 0) {
        v0 = kSurfaceCar;
        if (fcmp_lt(bf(kHardHit), strength)) {
            const float add = fmin_ps2(cap, fmul_ps2(strength, bf(kDamagePerImpulse)));
            const float d = fadd_ps2(before, add);
            sf(car + 0x1BEC, d);
            bool roll = fcmp_lt(bf(kDamaged), d);
            now = d;
            if (!roll && fcmp_lt(bf(kHalf), d) && fcmp_lt(bf(kSmallHit), add)) roll = true;
            if (roll) {
                const int32_t r = rng(2, 0x18D2B0);
                sw(car + 0x1BE8, r != 0 ? 3u : 2u);
                v0 = lw(car + 0x1C04);
                if (v0 < 0) {
                    v0 = rng(6, 0x18D2D8);
                    if (v0 == 0) {
                        v0 = hit_side(sp);
                        sw(car + 0x1C04, (uint32_t)v0);
                    }
                }
                now = lf(car + 0x1BEC);
            }
        }
    } else if ((uint32_t)(state - 1) < 4) {
        v0 = kSurfaceCar;
        if (fcmp_lt(bf(kHardHit), strength)) {
            const float d = fadd_ps2(before, fmin_ps2(cap, fmul_ps2(strength, bf(kDamagePerImpulse))));
            sf(car + 0x1BEC, d);
            // free roam: a hit by a car at this car keeps it at 0.9 the first time it passes 0.9
            if (surface == kSurfaceCar) {
                v0 = lw(car + 0x1DC);
                if (other == v0 && fcmp_lt(bf(kFreeRoamCap), d)) sf(car + 0x1BEC, fmax_ps2(bf(kFreeRoamCap), before));
            }
            now = lf(car + 0x1BEC);
            if (fcmp_lt(bf(kOne), now)) {
                set(4, 4);
                call(f_001741C0, 0x18D3F8);
                v0 = g_ee.r[2].sd[0];
                if (v0 != 0) {
                    now = bf(kFreeRoamCap);
                    sf(car + 0x1BEC, now);
                } else {
                    now = lf(car + 0x1BEC);
                }
                if (fcmp_lt(bf(kOne), now)) {
                    sf(car + 0x1BEC, bf(kOne));
                    const int32_t r = rng(3, 0x18D440);
                    sw(car + 0x1BE8, r == 0 ? 10u : r == 1 ? 11u : 12u);
                    if (rng(4, 0x18D478) == 0) {
                        const uint32_t side = hit_side(sp);
                        if (!(lwu(car + 0x1A00 + 0x54 * side + 0x50) & 0x80)) toss_wheel(car, sp, side);
                    }
                    impair();
                }
            }
        }
    } else if (state < 9) {
        v0 = 1;
    } else {
        v0 = controller_call(car, 0x3C, 0x18D6EC);
        if (v0 == 0) {
            set(4, (int32_t)kMsgReimpair), set(5, lw(car + 0x1DC));
            call(f_00331E48, 0x18D708);
            v0 = g_ee.r[2].sd[0];
            impaired = true;
        }
        now = lf(car + 0x1BEC);
    }

    const float delta = fsub_ps2(now, before);
    if (fcmp_lt(bf(kMinDelta), delta)) {
        sf(car + 0x1BF4, fmin_ps2(fadd_ps2(lf(car + 0x1BF4), fmul_ps2(delta, bf(kFlash))), bf(kHalf)));
        if (other >= 0 && other != lw(car + 0x1DC)) {
            set(4, lw(car + 0x1DC)), set(5, other);
            g_ee.fpr[12].f = delta;
            call(f_00202DA8, 0x18D778);
        }
        if (fcmp_le(bf(kShakeFrom), delta)) {
            set(4, (int32_t)car);
            g_ee.fpr[12].f = fmin_ps2(fmul_ps2(fsub_ps2(delta, bf(kShakeFrom)), bf(kShakeGain)), bf(kShakeMax));
            call(f_001D4B28, 0x18D7C0);
        }
        // cheat 0x101 (one-hit wrecks) outside free roam
        const uint32_t cheats = lwu(kCheatMask);
        v0 = 0;
        if ((cheats & 1) && (cheats & 0x100)) {
            set(4, 1);
            call(f_001741C0, 0x18D7EC);
            v0 = g_ee.r[2].sd[0];
            if (v0 == 0) {
                sf(car + 0x1BEC, bf(kOne));
                v0 = controller_call(car, 0x3C, 0x18D814);
                impaired = (v0 ^ 1) != 0;
            }
        }
    }
    g_ee.r[2].sd[0] = v0;
    if (impaired) {
        const uint32_t obj = lwu(car + 0x260);
        set(4, (int32_t)obj), set(5, other);
        call_at(lwu(lwu(obj) + 0x34), 0x18D834);
    }
    sw(car + 0x1BF8, 0);
}

}  // namespace

extern "C" void f_0017B918(void) { diff_run(0x0017B918, contact_record, f_0017B918_orig); }
// called by f_0018CA98 (src/game_patches.cpp) when the invincibility cheat lets the damage through
extern "C" void car_apply_collision_damage(void) { diff_run(0x0018CA98, apply_collision_damage, f_0018CA98_orig); }
