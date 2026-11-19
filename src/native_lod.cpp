// native_lod.cpp - draw distance option and single-car LOD selection as readable C++ (stage R5
// Phase A3, P10; docs/NATIVE_RENDERER.md "Stage R5"), proven against the recompiled originals with
// EOD_DIFF. src/hle_lod.cpp applies the Detail=maximum policy after the faithful selection.
//
//   0x212808 DrawDistanceApplyOption(race)  *0x495D50 = 0, 0x212790 (clears 0x3D1AFC/00/04);
//            option "draw_distance" (0x4064D8, default 2) -> g_DrawDistanceFar *0x495D58 =
//            {0: 0.0, 1: 0.25, 2: 0.5} * 1700 + 300 = 300 / 725 / 1150; option "detail_level"
//            (0x4064E8) is read into 0x495D5C and then overwritten with 4; 0x495D60 = 1.0,
//            0x495D64 = 3, 0x495D68 = (race+0xC == 2); ProjectionSetPerspective 0x34E530(1.0, far,
//            0.6545 rad 0x3F278D37); 0x3D1B08 = 0x3D1B0C = 0 (split branch of 0x214B68 off).
//   0x21C6E0 VisualCarSelectLodIndex(car)  distance = |camera - car+0x28| (0x34FA98) stored at
//            +0x548, times ProjectionGetFovLodScale *0x3DD638 (0x34F620, set by 0x34E530) times
//            ViewGetLodDistanceFactor (0x211318: g_ViewCountLodFactors 0x4062A8 = {1, 1, 2, 2}
//            [views - 1]); index +0x54C = first level whose threshold (+0x220, stride 16) the
//            distance is below, else count - 1, clamped to [+0x550 (minimum), count - 1].
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"

#include <cstdint>
#include <cstring>

extern "C" {
void f_00212790(void);
void f_00137838(void);
void f_0034E530(void);
void f_0034FA98(void);
void f_0034F620(void);
void f_00211318(void);
void f_00212808_orig(void);
}

namespace {

inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline float bits_f(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
inline uint32_t f_bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float rf(uint32_t a) { return bits_f(MEM_LW(a)); }
inline void call(void (*fn)(void), uint32_t ra) {
    set(31, (int32_t)ra);
    fn();
}

constexpr uint32_t kDrawDistanceFar = 0x495D58;   // g_DrawDistanceFar (float)
constexpr uint32_t kOptDrawDistance = 0x4064D8;   // "draw_distance"
constexpr uint32_t kOptDetailLevel = 0x4064E8;    // "detail_level"
constexpr uint32_t kSplitGate = 0x3D1B0C;         // 0x214B68 split-screen world branch

// ---- 0x212808 DrawDistanceApplyOption(a0 race) ---------------------------------------------------
void draw_distance_apply_option() {
    const uint32_t race = reg(4);
    const uint32_t sp = reg(29) - 0x50;
    set(29, (int32_t)sp);
    MEM_SD(sp + 48, g_ee.r[18].ud[0]);
    MEM_SD(sp + 32, g_ee.r[17].ud[0]);
    MEM_SD(sp + 16, g_ee.r[16].ud[0]);
    MEM_SD(sp + 64, g_ee.r[31].ud[0]);
    sw(0x495D50, 0);
    call(f_00212790, 0x212834);
    sw(sp, 2);  // the option's default when it is not set
    set(4, 1), set(5, (int32_t)kOptDrawDistance), set(6, (int32_t)sp);
    call(f_00137838, 0x21284C);
    const int32_t option = lw(sp);
    float step = 0.0f;  // option 0: 300
    if (option == 1) step = 0.25f;       // 725
    else if (option == 2) step = 0.5f;   // 1150
    sw(kDrawDistanceFar, f_bits(step));
    set(4, 1), set(5, (int32_t)kOptDetailLevel), set(6, 0x495D5C);
    call(f_00137838, 0x2128C4);
    const float far = fadd_ps2(fmul_ps2(rf(kDrawDistanceFar), 1700.0f), 300.0f);
    sw(0x495D5C, 4);              // the detail option is not used: always 4
    sw(0x495D60, 0x3F800000u);    // 1.0
    sw(kDrawDistanceFar, f_bits(far));
    sw(0x495D64, 3);
    sw(0x495D68, lw(race + 12) == 2 ? 1u : 0u);
    g_ee.fpr[12].u = 0x3F800000u;   // near 1.0
    g_ee.fpr[13].u = f_bits(far);   // far
    g_ee.fpr[14].u = 0x3F278D37u;   // fov 0.6545 rad
    call(f_0034E530, 0x21294C);
    g_ee.r[31].ud[0] = MEM_LD(sp + 64);
    g_ee.r[18].ud[0] = MEM_LD(sp + 48);
    g_ee.r[17].ud[0] = MEM_LD(sp + 32);
    g_ee.r[16].ud[0] = MEM_LD(sp + 16);
    sw(0x3D1B08, 0);
    sw(kSplitGate, 0);
    set(29, (int32_t)(sp + 0x50));
    set(2, 0x3D0000);  // $v0 as the original leaves it
}

}  // namespace

// ---- 0x21C6E0 VisualCarSelectLodIndex(a0 car) (wrapped in src/hle_lod.cpp) ------------------------
void eod_visual_car_select_lod_index() {
    const uint32_t car = reg(4);
    const uint32_t sp = reg(29) - 0x30;
    set(29, (int32_t)sp);
    MEM_SD(sp, g_ee.r[16].ud[0]);
    sw(sp + 32, g_ee.fpr[20].u);
    MEM_SD(sp + 16, g_ee.r[31].ud[0]);
    set(4, (int32_t)(car + 0x28));
    call(f_0034FA98, 0x21C6FC);  // CameraCalcDistanceToPoint
    sw(car + 0x548, g_ee.fpr[0].u);
    call(f_0034F620, 0x21C704);  // ProjectionGetFovLodScale (*0x3DD638)
    const float fov_scale = g_ee.fpr[0].f;
    call(f_00211318, 0x21C70C);  // ViewGetLodDistanceFactor (0x4062A8[views - 1])
    const uint32_t count_ptr = (uint32_t)lw(car + 0x224);
    const float distance = fmul_ps2(rf(car + 0x548), fmul_ps2(fov_scale, g_ee.fpr[0].f));
    sw(car + 0x54C, (uint32_t)(lw(count_ptr) - 1));
    sw(car + 0x548, f_bits(distance));
    int32_t count = lw(count_ptr);
    if (count > 0) {
        const uint32_t thresholds = (uint32_t)lw(car + 0x220);
        if (fcmp_lt(distance, rf(thresholds))) {
            sw(car + 0x54C, 0);
        } else {
            for (int32_t i = 1; i < count; i++) {
                if (fcmp_lt(rf(car + 0x548), rf(thresholds + (uint32_t)i * 16))) {
                    sw(car + 0x54C, (uint32_t)i);
                    break;
                }
                count = lw(count_ptr);
            }
        }
    }
    const int32_t minimum = lw(car + 0x550);
    int32_t index = lw(car + 0x54C);
    if (index < minimum) {
        sw(car + 0x54C, (uint32_t)minimum);
        index = minimum;
    }
    count = lw(count_ptr);
    if (index < count) {
        set(2, 1);
    } else {
        set(2, count - 1);
        sw(car + 0x54C, (uint32_t)(count - 1));
    }
    g_ee.r[31].ud[0] = MEM_LD(sp + 16);
    g_ee.r[16].ud[0] = MEM_LD(sp);
    g_ee.fpr[20].u = MEM_LW(sp + 32);
    set(29, (int32_t)(sp + 0x30));
}

extern "C" void f_00212808(void) { diff_run(0x00212808, draw_distance_apply_option, f_00212808_orig); }
