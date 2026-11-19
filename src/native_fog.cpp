// native_fog.cpp - the game's fog state setters as readable C++ (stage R5, docs/NATIVE_RENDERER.md
// "Stage R5"), proven against the recompiled originals with EOD_DIFF, feeding the native renderer.
//
//   0x33ECB8 FogConfigureRange(start, end, density)  jump thunk to 0x3653F8
//   0x3653F8 GsFogWriteTables(start, end, density)   *0x4D8B84/88/8C = f12/f13/f14, dirty |= 0x800000
//   0x365428 FogSetColour(r, g, b)                   FOGCOL state *0x4D8B80, dirty |= 0x10000
//   0x365540 RenderFlagsCommit(flags)                *0x4D8BC8 (bit 3 = GS PRIM.FGE in 0x3620A8)
//   0x360E18 GsStateInit (wrapped, original runs)    publishes its fog constants afterwards
//
// The fog values reach Plume only from these functions; nothing is read back from GS registers.
// The range is dead state in the retail game (no reader in SLUS_209.10, and the matched Xbox
// consumer at 0x13DD5E is unreachable), and no call site sets the FGE bit, so the PS2 frame has no
// fog. The renderer keeps the GS fog stage for any path that sets it (runtime/render/native_shaders.hlsl).
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstring>

extern "C" {
void f_0033ECB8_orig(void);
void f_003653F8_orig(void);
void f_00365428_orig(void);
void f_00365540_orig(void);
void f_00360E18_orig(void);
}

namespace {

inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline float fpr(int r) {
    float f;
    std::memcpy(&f, &g_ee.fpr[r].u, 4);
    return f;
}

constexpr uint32_t kDirty = 0x4D8B74;      // dirty bits flushed by 0x3620A8
constexpr uint32_t kFogColour = 0x4D8B80;  // FOGCOL (GS A+D register 0x3D, 0x3620A8 at 0x3623CC)
constexpr uint32_t kFogStart = 0x4D8B84;
constexpr uint32_t kFogEnd = 0x4D8B88;
constexpr uint32_t kFogDensity = 0x4D8B8C;
constexpr uint32_t kRenderFlags = 0x4D8BC8;
constexpr uint32_t kFogRangeDirty = 0x800000;   // 0x3653F8 (no reader tests it)
constexpr uint32_t kFogColourDirty = 0x10000;   // 0x365428; 0x3620A8 emits FOGCOL when set

// ---- 0x3653F8 GsFogWriteTables(f12 start, f13 end, f14 density) --------------------------------
void fog_write_tables() {
    const uint32_t start = g_ee.fpr[12].u, end = g_ee.fpr[13].u, density = g_ee.fpr[14].u;
    const uint32_t dirty = MEM_LW(kDirty) | kFogRangeDirty;
    MEM_SW(kFogDensity, density);
    MEM_SW(kDirty, dirty);
    MEM_SW(kFogStart, start);
    MEM_SW(kFogEnd, end);
    if (!diff_in_original_side()) native::ee_fog_range(fpr(12), fpr(13), fpr(14));
    // registers as the original leaves them (lui/addiu/or)
    set(2, (int32_t)dirty);
    set(3, (int32_t)kFogColour);
    set(4, (int32_t)kFogRangeDirty);
    set(5, 0x4E0000);
}

// ---- 0x33ECB8 FogConfigureRange: `j 0x3653F8` (same arguments, same return) ---------------------
void fog_configure_range() { fog_write_tables(); }

// ---- 0x365428 FogSetColour(r, g, b) -----------------------------------------------------------
void fog_set_colour() {
    const uint32_t rgb = (reg(4) & 0xFF) | (reg(5) & 0xFF) << 8 | (reg(6) & 0xFF) << 16;
    const uint32_t old = MEM_LW(kFogColour);
    set(2, (int32_t)old);
    if (rgb != old) {
        const uint32_t dirty = MEM_LW(kDirty) | kFogColourDirty;
        MEM_SW(kFogColour, rgb);
        MEM_SW(kDirty, dirty);
        set(2, (int32_t)dirty);
        set(3, (int32_t)kFogColourDirty);
    }
    if (!diff_in_original_side()) native::ee_fog_colour(rgb);
    g_ee.r[4].ud[0] = 0x4E0000;
    g_ee.r[5].sd[0] = (int32_t)rgb;
    g_ee.r[6].ud[0] = (uint64_t)(reg(6) & 0xFF) << 16;
    g_ee.r[7].ud[0] = 0x4E0000;
}

// ---- 0x365540 RenderFlagsCommit(flags): 0x34F028 passes *0x4CD398 (bit 3 cleared when *0x4CD3A0 == 1)
void render_flags_commit() {
    const uint32_t flags = reg(4);
    MEM_SW(kRenderFlags, flags);
    if (!diff_in_original_side()) native::ee_render_flags(flags);
    set(2, 0x4E0000);
}

}  // namespace

extern "C" void f_003653F8(void) { diff_run(0x003653F8, fog_write_tables, f_003653F8_orig); }
extern "C" void f_0033ECB8(void) { diff_run(0x0033ECB8, fog_configure_range, f_0033ECB8_orig); }
extern "C" void f_00365428(void) { diff_run(0x00365428, fog_set_colour, f_00365428_orig); }
extern "C" void f_00365540(void) { diff_run(0x00365540, render_flags_commit, f_00365540_orig); }

// 0x360E18 initialises the GS state block, fog included (0x360E90..0x360EA0): colour 0xFF, start
// 200.0 (0x43480000), end 1000.0 (0x447A0000), 0.5 (0x3F000000). The original runs; the feed
// gets the values it stored.
extern "C" void f_00360E18(void) {
    f_00360E18_orig();
    native::ee_fog_colour(MEM_LW(kFogColour));
    uint32_t u[3] = {MEM_LW(kFogStart), MEM_LW(kFogEnd), MEM_LW(kFogDensity)};
    float f[3];
    std::memcpy(f, u, sizeof f);
    native::ee_fog_range(f[0], f[1], f[2]);
}
