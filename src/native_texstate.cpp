// native_texstate.cpp - the game's texture LOD and blend state setters as readable C++ (stage R5
// Phase A2, docs/NATIVE_RENDERER.md "Stage R5"), proven against the recompiled originals with
// EOD_DIFF, feeding the native renderer. RenderStateCommit 0x3620A8 turns this state into GS
// registers; the native renderer builds the same register values from what these functions hand
// over, never from the GS registers.
//
//   0x3652A8 TextureSetLodState(k_log2, max_level)  *0x4D8BAC = k_log2 << 4 (TEX1.K template, 1/16),
//            *0x4D8BB0 = k_log2, *0x4D8BBC = max_level (TEX1.MXL); dirty |= 0x400 on change.
//            Called by the texture manager (0x35F278 at 0x35F8D0, 0x35EE38 at 0x35EE94) with the
//            texture's log2 height (+0x1A) and its level count - 1 (+0x1C).
//   0x3654E0 BlendSelect(sel)                        *0x4D8B78 = sel; dirty |= 0x80000. 0x3620A8
//            writes ALPHA_1 = 0x48 (Cs, 0, As, Cd: add) for 0, 0x44 (Cs, Cd, As, Cd: mix) otherwise.
//   0x365308 LodBiasSet(f12 bias) -> old bias         *0x4D8BB4 = bias, *0x4D8BB8 = (int)(bias*16 -
//            1000) in both halves: the K delta the particle and sprite VU1 headers carry
//            (0x364068, 0x364328, 0x3645B8 put it in TOP+3.w / +0xB).
//   0x365578 GsStateReset (wrapped, original runs)  publishes the values it stored afterwards
//            (sel 1, K 0, MXL 1, bias 1.0).
// 0x360E18 (GS state init) is wrapped in src/native_fog.cpp and publishes these values as well.
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstring>

extern "C" {
void f_003652A8_orig(void);
void f_003654E0_orig(void);
void f_00365308_orig(void);
void f_00365578_orig(void);
}

namespace {

inline uint64_t reg64(int r) { return g_ee.r[r].ud[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline int64_t sext(uint32_t v) { return (int64_t)(int32_t)v; }

constexpr uint32_t kDirty = 0x4D8B74;       // dirty bits flushed by 0x3620A8
constexpr uint32_t kBlendSel = 0x4D8B78;    // 0x3620A8: ALPHA_1 0x48 when 0, else 0x44 (0x3623A0)
constexpr uint32_t kTex1K = 0x4D8BAC;       // TEX1.K template in 1/16 (0x3620A8: (K & 0xFFF) << 32)
constexpr uint32_t kTex1KLog2 = 0x4D8BB0;   // the k_log2 argument last stored
constexpr uint32_t kLodBiasK = 0x4D8BB8;    // (int)(bias * 16 - 1000), both halves
constexpr uint32_t kTex1Mxl = 0x4D8BBC;     // TEX1.MXL (0x3620A8: (MXL & 7) << 2)
constexpr uint32_t kTex1Dirty = 0x400;      // 0x3620A8 then UNPACKs the TEX1 A+D to VU1 qwords 20-22
constexpr uint32_t kBlendDirty = 0x80000;   // 0x3620A8 then writes ALPHA_1

void publish() {
    native::ee_tex_lod((int32_t)MEM_LW(kTex1K), MEM_LW(kTex1Mxl), MEM_LW(kLodBiasK));
    native::ee_blend_select(MEM_LW(kBlendSel));
}

// ---- 0x3652A8 TextureSetLodState(a0 k_log2, a1 max_level) ---------------------------------------
void texture_set_lod_state() {
    const uint64_t a0 = reg64(4), a1 = reg64(5);
    const uint32_t a0w = (uint32_t)a0;
    set(8, 0x4E0000);
    g_ee.r[6].ud[0] = a0;
    g_ee.r[7].ud[0] = a1;
    if ((uint64_t)sext(MEM_LW(kTex1KLog2)) != a0) {  // the original compares the whole register
        const uint32_t dirty = MEM_LW(kDirty) | kTex1Dirty;
        MEM_SW(kTex1K, a0w << 4);
        MEM_SW(kTex1KLog2, a0w);
        MEM_SW(kDirty, dirty);
        set(5, 0x4E0000);
    }
    set(4, 0x4E0000);
    set(3, 0x4E0000);
    const uint32_t old = MEM_LW(kTex1Mxl);
    g_ee.r[2].sd[0] = sext(old);
    if ((uint64_t)sext(old) != a1) {
        MEM_SW(kTex1Mxl, (uint32_t)a1);
        const uint32_t dirty = MEM_LW(kDirty) | kTex1Dirty;
        MEM_SW(kDirty, dirty);
        g_ee.r[2].sd[0] = sext(dirty);
    }
    if (!diff_in_original_side()) publish();
}

// ---- 0x3654E0 BlendSelect(a0 sel) -----------------------------------------------------------------
void blend_select() {
    const uint64_t a0 = reg64(4);
    set(6, 0x4E0000);
    g_ee.r[5].ud[0] = a0;
    const uint32_t old = MEM_LW(kBlendSel);
    g_ee.r[2].sd[0] = sext(old);
    set(4, 0x4E0000);
    if ((uint64_t)sext(old) != a0) {
        set(3, (int32_t)kBlendDirty);
        MEM_SW(kBlendSel, (uint32_t)a0);
        const uint32_t dirty = MEM_LW(kDirty) | kBlendDirty;
        MEM_SW(kDirty, dirty);
        g_ee.r[2].sd[0] = sext(dirty);
    }
    if (!diff_in_original_side()) publish();
}

}  // namespace

extern "C" void f_003652A8(void) { diff_run(0x003652A8, texture_set_lod_state, f_003652A8_orig); }
extern "C" void f_003654E0(void) { diff_run(0x003654E0, blend_select, f_003654E0_orig); }

// 0x365308 is a float function (cvt.w.s of bias * 16 - 1000): the original runs, the feed gets the
// value it stored (its rounding is the EE's own).
extern "C" void f_00365308(void) {
    f_00365308_orig();
    publish();
}

// 0x365578 resets the state block (sel 1, K 0, MXL 1, 0x365308(1.0), dirty 0x1FFFFF): the
// original runs, the feed gets what it stored.
extern "C" void f_00365578(void) {
    f_00365578_orig();
    publish();
}

namespace native_texstate {
void publish_init() { publish(); }  // after 0x360E18 (src/native_fog.cpp)
}
