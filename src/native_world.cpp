// native_world.cpp - track world renderer, sky and water (SLUS_209.10), readable C++ replacing the
// recompiled code. Stage R2 of the native renderer; the address / structure / VU1 table is in
// docs/NATIVE_RENDERER.md ("Stage R2: track world, sky and water").
//
//   0x214B68 WorldFrameRender      one view: mlist visibility, world passes 0..4 around the queued
//                                  frame 0x22D7F0
//   0x3582B8 MlistVisibilityCalc   per sphere / LOD group / model visibility bytes in the scratchpad
//                                  (VU0 frustum test, microprogram 0x40)
//   0x358BD0 WorldRenderPass(pass) DMA REFs to the visible world batches of one pass
//   0x35DEE0 WorldPacketBegin(qwc) camera/matrix/material REFs (0x35BA10), then a packet (0x366D30)
//   0x35CBB8 PacketCommit(qwc)     closes the open packet (body of 0x366CD0)
//   0x223858 SkyDraw               sky dome parts at the camera through the draw queue (0x34BDA8)
//   0x223800 SkyDestroy            sky object destructor (vtable 0x408318 slot 2)
//   0x326168 WaterDrawIfPresent    0x325728(*(0x3DC134))
//   0x325728 WaterDraw(surface)    the water tiles through the immediate geometry API
//   0x325E40 WaterSimulate         car splashes on the water surface
//
// As src/render_queue.cpp: every replacement makes the same guest calls in the same order with the
// same return addresses and stack frame, the same memory writes and the same $v0, so EOD_DIFF can
// prove it against the recompiled original (runtime/core/diff_harness.h). The native renderer is
// fed from here (runtime/render/native_renderer.h): world leaves, the sun-facing quad, water.
#include "core/config.h"
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/limit_tracker.h"
#include "core/memory.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" {
void f_00214B68_orig(void);
void f_003582B8_orig(void);
void f_00358BD0_orig(void);
void f_0035DEE0_orig(void);
void f_0035CBB8_orig(void);
void f_00223858_orig(void);
void f_00223800_orig(void);
void f_00326168_orig(void);
void f_00325728_orig(void);
void f_00325E40_orig(void);
void f_00326188_orig(void);

// Called through their public symbols so wrappers (traces, diagnostics, nested diffs) still apply.
void f_00212DD0(void);  // WorldSunVisible() -> *(0x495D64) in one view, else 0
void f_002217A8(void);  // SunQuadAxes(out[6]): camera right / up
void f_0034C1F0(void);  // SunQuadUpload(axes) (0x35CBD8): material packet with the 4 corners
void f_00212E20(void);  // SplitScreen() -> *(0x3D1B0C)
void f_003582B8(void);
void f_00358BD0(void);
void f_0034DFE0(void);  // RenderFlagClear(flag)
void f_0034DEC0(void);  // RenderFlagSet(flag)
void f_0034E4D8(void);  // NearFarScale(f12)
void f_0032E638(void);  // world reflection object draw
void f_0034E518(void);  // NearFarRestore()
void f_002E7300(void);  // effects pass
void f_0022D7F0(void);  // queued object frame (draw queue)
void f_0030A580(void);  // projected shadows
void f_002283D0(void);  // immediate world geometry
void f_00359238(void);  // MlistClear(): *(0x4D6F10) = 0
void f_0033CA18(void);  // ProfileLabel(name)
void f_0033CA08(void);  // ProfileLabelClear()
void f_0033C578(void);  // DmaChannelsOpen(state)
void f_0033C620(void);  // DmaChannelsClose(state)
void f_001E0830(void);  // DmaWaitVif0(pcr*)
void f_0035CD28(void);  // MatrixPacketBuild(m) -> packet
void f_0035E018(void);  // SetMatrixPacket(packet)
void f_0034F760(void);  // RenderStatePush()
void f_0034F710(void);  // RenderStatePop()
void f_0034F8D0(void);  // TextureSlotBegin(slot)
void f_0034F888(void);  // TextureSlotEnd()
void f_0035DEE0(void);
void f_0035CBB8(void);
void f_00353470(void);  // TextureSlotNoteDistance(slot; f12 depth, f13 batch scale)
void f_0035E000(void);  // matrix packet cache = -1
void f_0035BA10(void);  // EmitCameraMatrixMaterial()
void f_00366D30(void);  // PacketReserve(qwc) -> pointer or 0
void f_0034FC68(void);  // Camera() -> 0x4CD2F8 (rows, then position at +0x24)
void f_0034F5E8(void);  // ProjectionFactors(out, near*, far*, out2*)
void f_00357E60(void);  // MlistGroupLod(group, camera qword; f12, f13) -> visibility byte
void f_002111D0(void);  // ViewFov(view) -> f0
void f_0039BB28(void);  // cosf(f12) -> f0
void f_0034BDA8(void);  // RenderQueueSubmitModel(model, matrix, material, layer mask)
void f_0034E8A8(void);  // SetColourA(0xRRGGBB)
void f_00223D00(void);  // display object base destructor
void f_00325728(void);
void f_0035DF10(void);  // ImmediateBegin(vertices) -> 0 when no packet space
void f_00326B80(void);  // WaterTileCorner(tile, out, i, j)
void f_0031E7E0(void);  // WaterVertexShade(tile, vertex, light)
void f_00361708(void);  // ImmediateVertex(f12..f16 xyz st, a0 colour, a1 strip) -> 0 accepted
void f_00361BC8(void);  // ImmediateEnd()
void f_00324928(void);  // WaterTileSkip(tile)
void f_002114B0(void);  // WaterLevelSpeed() -> f0
void f_0012F4F0(void);  // RngNextSignedFloat(a0, a1)
void f_00325910(void);  // WaterReset(surface)
void f_002E8768(void);  // CarByIndex(i)
void f_002E87B0(void);  // CarCount()
void f_0031BFA0(void);  // WaterCarContact(out, i) -> nonzero on contact
void f_0031C7E8(void);  // WaterTileContains(tile, point, id) -> nonzero
void f_0031CA30(void);  // WaterTilePush(f12 strength, tile, point)
void f_00325AD0(void);  // WaterStep(surface)
void f_0031C7E0(void);  // WaterTileStep(tile)
}

namespace {

// ---- guest access (values as the EE leaves them: 32-bit results sign-extended) ----
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline uint64_t reg64(int r) { return g_ee.r[r].ud[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline void set64(int r, uint64_t v) { g_ee.r[r].ud[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
inline uint32_t lhu(uint32_t a) { return MEM_LH(a) & 0xFFFF; }
inline uint32_t lbu(uint32_t a) { return MEM_LB(a) & 0xFF; }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline void sb(uint32_t a, uint32_t v) { MEM_SB(a, (uint8_t)v); }
inline void sd(uint32_t a, uint64_t v) { MEM_SD(a, v); }
inline void sq(uint32_t a, uint64_t lo, uint64_t hi) {
    R128 q;
    q.ud[0] = lo;
    q.ud[1] = hi;
    MEM_SQ(a, &q);
}
// sq of a value the original builds in $v0 (pcpyld v0 / lq v0): $v0 keeps all 128 bits
inline void sq_v0(uint32_t a, uint64_t lo, uint64_t hi) {
    g_ee.r[2].ud[0] = lo;
    g_ee.r[2].ud[1] = hi;
    MEM_SQ(a, &g_ee.r[2]);
}
inline void copy_q_v0(uint32_t dst, uint32_t src) {
    MEM_LQ(src, &g_ee.r[2]);
    MEM_SQ(dst, &g_ee.r[2]);
}
inline void copy_q(uint32_t dst, uint32_t src) {
    R128 q;
    MEM_LQ(src, &q);
    MEM_SQ(dst, &q);
}
// ldl/ldr + sdl/sdr pair: 8 bytes, any alignment
inline void copy8(uint32_t dst, uint32_t src) {
    for (uint32_t k = 0; k < 8; k++) sb(dst + k, lbu(src + k));
}
inline float lf(uint32_t a) {
    FReg f;
    f.u = MEM_LW(a);
    return f.f;
}
inline uint32_t fbits(float v) {
    FReg f;
    f.f = v;
    return f.u;
}
inline float bitsf(uint32_t u) {
    FReg f;
    f.u = u;
    return f.f;
}
inline void set_f(int f, uint32_t bits) { g_ee.fpr[f].u = bits; }
// a 32-bit word widened as the EE's lw / mfc1 do (pcpyld / pextlw keep the upper half)
inline uint64_t sx(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }
// pextlw + pcpyld of four words: (a, b, c, sign(c)) as one quadword
inline void sq3(uint32_t dst, uint32_t a, uint32_t b, uint32_t c) { sq(dst, (uint64_t)a | (uint64_t)b << 32, sx(c)); }

inline void call(void (*fn)(void), uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    fn();
}
inline void call_at(uint32_t fn, uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    ee_call(fn);
}

// The original's stack frame: callees see the same $sp.
struct Frame {
    uint32_t caller_sp;
    uint64_t ra;
    explicit Frame(uint32_t size) : caller_sp(reg(29)), ra(reg64(31)) { set(29, (int32_t)(caller_sp - size)); }
    ~Frame() {
        set(29, (int32_t)caller_sp);
        set64(31, ra);
    }
    uint32_t sp() const { return reg(29); }
};

// VU0 macro instructions the original issues (COP2), by instruction word.
enum : uint32_t {
    kVmulRow0 = 0x4BC3106A,     // vmul.xyz  vf1, vf2, vf3
    kVmulRow1 = 0x4BC4106A,     // vmul.xyz  vf1, vf2, vf4
    kVmulRow2 = 0x4BC5106A,     // vmul.xyz  vf1, vf2, vf5
    kVaddY = 0x4B010841,        // vaddy.x   vf1, vf1, vf1y
    kVaddZ = 0x4B010842,        // vaddz.x   vf1, vf1, vf1z
    kVmulZ = 0x4BC1105A,        // vmulz.xyz vf1, vf2, vf1z
    kVmoveVf20 = 0x4BF42B3C,    // vmove vf20, vf5
    kVmoveVf21 = 0x4BF5233C,    // vmove vf21, vf4
    kVmoveVf22 = 0x4BF61B3C,    // vmove vf22, vf3
    kVmoveVf23 = 0x4BF70B3C,    // vmove vf23, vf1
    kVmoveXyzVf2 = 0x4BC2133C,  // vmove.xyz vf2, vf2
    kVmoveWVf2 = 0x4A220B3C,    // vmove.w   vf2, vf1
    kVmoveVf24 = 0x4BF80B3C,    // vmove vf24, vf1
    kVmoveVf26 = 0x4BFA133C,    // vmove vf26, vf2
    kVmoveVf30 = 0x4BFE0B3C,    // vmove vf30, vf1   (sphere into the microprogram's input)
    kVcallms40 = 0x4A000238,    // vcallms 0x40      (frustum / distance test)
    kVmoveFromVf31 = 0x4BE1FB3C,// vmove vf1, vf31   (the microprogram's result)
};
inline void vu0(uint32_t w) { vu0_macro(w); }
inline void vf_load(int n, uint32_t a) { MEM_LQ(a, (R128 *)&VU0.vf[n]); }
inline void vf_store(uint32_t a, int n) { MEM_SQ(a, (const R128 *)&VU0.vf[n]); }

// The microprogram's clip flags as a visibility byte: outside -> 0, partly outside -> 0x81, in -> 1.
inline uint32_t visibility(uint32_t clip) { return (clip & 0xBD0) ? 0u : (clip & 0x42F) ? 0x81u : 1u; }

constexpr uint32_t kScratch = 0x70000000;       // visibility bytes
constexpr uint32_t kMatrixTable = 0x70001C00;   // 256 matrix packets (scratchpad)
constexpr uint32_t kMlist = 0x4D6F10;           // the mlist being drawn
constexpr uint32_t kModelDepth = 0x3DE6E8;      // per-entry depth (mip selection)
constexpr uint32_t kWater = 0x3DC134;           // the track's water surface (0 = none)

bool split_parity() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_SPLIT_PARITY");
        return e && *e ? *e != '0' : g_config.split_screen_parity;
    }();
    return on;
}

// ---------------------------------------------------------------------------------------------
// 0x214B68 WorldFrameRender
void world_frame_render() {
    Frame f(0x60);
    const uint32_t sp = f.sp();
    call(f_00212DD0, 0x214B80);
    sw(0x3D1B30, reg(2));
    set(4, (int32_t)sp);
    call(f_002217A8, 0x214B90);
    set(4, (int32_t)sp);
    call(f_0034C1F0, 0x214B98);
    call(f_00212E20, 0x214BA0);
    const bool split_view = reg64(2) != 0;
    // P8: every split view gets the single-view sequence (PS2 and Xbox 0x0A8390 skip passes 1, 2, 4
    // and use the coarse mlist there); EOD_SPLIT_PARITY=0 keeps the original.
    const bool split = split_view && !split_parity();
    const uint32_t graf = lwu(0x3D1C18);
    set(4, lw(graf + (split ? 8 : 4)));
    set_f(12, lwu(0x3D1B28));
    set_f(13, lwu(0x3D1B2C));
    set(6, 0);
    set(5, lw(0x3D1B30));
    set(7, 0);
    call(f_003582B8, 0x214BDC);
    set(4, 0);
    call(f_00358BD0, 0x214BE4);
    if (!split) {
        set(4, 8);
        call(f_0034DFE0, 0x214BF4);
        set(4, 4);
        call(f_0034DEC0, 0x214BFC);
        set_f(12, 0x3F8CCCCD);  // near and far x 1.1 for the reflection pass
        call(f_0034E4D8, 0x214C10);
        set(4, 1);
        call(f_00358BD0, 0x214C18);
        call(f_0032E638, 0x214C20);
        call(f_0034E518, 0x214C28);
        set(4, 8);
        call(f_0034DEC0, 0x214C30);
        set(4, 2);
        call(f_00358BD0, 0x214C38);
        set(4, 0);
        call(f_002E7300, 0x214C40);
    }
    call(f_0022D7F0, 0x214C48);
    set(4, (int32_t)sp);
    call(f_0034C1F0, 0x214C50);
    set(4, 3);
    call(f_00358BD0, 0x214C58);
    if (!split) {
        set(4, 0);
        call(f_0030A580, 0x214C68);
        call(f_002283D0, 0x214C70);
        set(4, 4);
        call(f_00358BD0, 0x214C78);
    }
    call(f_00359238, 0x214C80);
}

// ---------------------------------------------------------------------------------------------
// 0x3582B8 MlistVisibilityCalc(mlist, -, matrices, count; f12, f13 track LOD factors)
void mlist_visibility_calc() {
    const uint32_t mlist = reg(4), matrices = reg(6);
    const int32_t count = (int32_t)reg(7);
    const uint32_t lod_a = g_ee.fpr[12].u, lod_b = g_ee.fpr[13].u;
    Frame f(0x1C0);
    const uint32_t sp = f.sp();
    set(4, 0x4296B0);  // "mlist: calc"
    call(f_0033CA18, 0x358314);

    // VU0 microcode upload: VIF0 DMA chain at 0x3BD4D0 (D_PCR saved, CPCOND0 only, then restored)
    const uint32_t pcr = lwu(0x1000E020);
    sw(sp + 0x100, pcr);
    sw(0x1000E020, 0x10001);
    sw(0x10008030, 0x3BD4D0u & 0xEFFFFFFFu);  // VIF0 TADR
    sw(0x10008020, 0);                         // VIF0 QWC
    sw(0x1000E010, 1);                         // D_STAT: clear channel 0
    sw(0x10008000, 0x144);                     // VIF0 CHCR: chain, start
    set(4, (int32_t)(sp + 0x100));
    call(f_001E0830, 0x358388);

    // identity 3x4 on the stack -> matrix packet 0, the default of every table entry
    sw(sp + 0x20, 0x3F800000);
    sw(sp + 0x00, 0x3F800000);
    sw(sp + 0x04, 0);
    sw(sp + 0x08, 0);
    sw(sp + 0x0C, 0);
    sw(sp + 0x10, 0x3F800000);
    sw(sp + 0x14, 0);
    sw(sp + 0x18, 0);
    sw(sp + 0x1C, 0);
    sw(sp + 0x24, 0);
    sw(0x1000E020, lwu(sp + 0x100));
    sw(kMlist, mlist);
    sw(sp + 0x2C, 0);
    sw(sp + 0x28, 0);
    set(4, (int32_t)sp);
    call(f_0035CD28, 0x3583DC);
    const uint32_t identity = reg(2);
    sw(kMatrixTable, identity);
    int32_t n = 0;
    for (; n < count;) {  // caller matrices (3x4, 0x30 bytes) -> entries 1..count
        set(4, (int32_t)((uint32_t)n * 0x30 + matrices));
        call(f_0035CD28, 0x35840C);
        n++;
        sw(kMatrixTable + (uint32_t)n * 4, reg(2));
        if (n < count) IRQ_CHECK();
    }
    n = count;
    for (int32_t i = n; i < 0x100; i++) sw(kMatrixTable + (uint32_t)i * 4, identity);  // (entry `count` too)

    copy8(sp + 0x30, mlist + 0x10);  // n spheres, spheres
    call(f_0034FC68, 0x3584E8);
    const uint32_t cam = reg(2);
    // camera rows and position as quadwords (w = sign of z, as pextlw/pcpyld leave it)
    sq3(0x4D2EA0, lwu(cam + 0x0C), lwu(cam + 0x10), lwu(cam + 0x14));
    sq3(0x4D2E90, lwu(cam + 0x00), lwu(cam + 0x04), lwu(cam + 0x08));
    sq3(0x4D2EB0, lwu(cam + 0x18), lwu(cam + 0x1C), lwu(cam + 0x20));
    sq3(0x4D2EC0, lwu(cam + 0x24), lwu(cam + 0x28), lwu(cam + 0x2C));
    copy_q(sp + 0x40, 0x4D2E90);
    copy_q(sp + 0x50, 0x4D2EA0);
    copy_q(sp + 0x60, 0x4D2EB0);
    copy_q(sp + 0x70, 0x4D2EC0);
    vf_load(3, sp + 0x40);
    vf_load(4, sp + 0x50);
    vf_load(5, sp + 0x60);
    // transposed rotation: columns at sp+0x80 / 0x90 / 0xA0 (and the 3x3 at sp+0xC0, column major)
    const uint32_t r0x = lwu(sp + 0x40), r0y = lwu(sp + 0x44), r0z = lwu(sp + 0x48);
    const uint32_t r1x = lwu(sp + 0x50), r1y = lwu(sp + 0x54), r1z = lwu(sp + 0x58);
    const uint32_t r2x = lwu(sp + 0x60), r2y = lwu(sp + 0x64), r2z = lwu(sp + 0x68);
    vf_store(sp + 0xF0, 5);
    sw(sp + 0xC0, r0x);
    sw(sp + 0xC4, r1x);
    sw(sp + 0xCC, r0y);
    sw(sp + 0xD0, r1y);
    sw(sp + 0xDC, r1z);
    sw(sp + 0xD8, r0z);
    sq3(sp + 0x80, r0x, r1x, r2x);
    sq3(sp + 0x90, r0y, r1y, r2y);
    sw(sp + 0xC8, r2x);
    sq3(sp + 0xA0, r0z, r1z, r2z);
    sw(sp + 0xE0, r2z);
    sw(sp + 0xD4, r2y);
    // camera position . each row (VU0): -R c, the view translation
    vf_load(2, sp + 0x70);
    vu0(kVmulRow0);
    vu0(kVaddY);
    vu0(kVaddZ);
    vf_store(sp + 0xC0, 1);
    const uint32_t d0 = lwu(sp + 0xC0);
    vu0(kVmulRow1);
    vu0(kVaddY);
    vu0(kVaddZ);
    vf_store(sp + 0xC0, 1);
    const uint32_t d1 = lwu(sp + 0xC0);
    vu0(kVmulRow2);
    vu0(kVaddY);
    vu0(kVaddZ);
    vf_store(sp + 0xC0, 1);
    sq3(sp + 0xB0, d0, d1, lwu(sp + 0xC0));
    MEM_LQ(sp + 0xB0, (R128 *)&VU0.vf[2]);
    sw(sp + 0xC8, 0xBF800000);  // -1
    vf_load(1, sp + 0xC0);
    vu0(kVmulZ);
    vf_store(sp + 0xB0, 1);
    set(4, 0x4D2E80);
    set(5, 0x3DE6D8);
    set(6, 0x3DE6DC);
    set(7, (int32_t)(sp + 0x104));
    call(f_0034F5E8, 0x358714);
    // projection constants: vf24 = (1/x, 1/x, (f+n)/(f-n), .), vf26 = (0, 0, 1/y, f (1 - (f+n)/(f-n)))
    const float far = lf(0x3DE6DC), near = lf(0x3DE6D8), one = 1.0f;
    const float range = fsub_ps2(far, near);
    float ratio = fadd_ps2(far, near);
    const float inv_x = fdiv_ps2(one, lf(0x4D2E80));
    const float inv_y = fdiv_ps2(one, lf(sp + 0x104));
    vf_load(5, sp + 0x80);
    vf_load(4, sp + 0x90);
    vf_load(3, sp + 0xA0);
    vf_load(1, sp + 0xB0);
    ratio = fdiv_ps2(ratio, range);
    const float far_term = fmul_ps2(far, fsub_ps2(one, ratio));
    const uint64_t a0_lo = (uint64_t)fbits(inv_x) | (uint64_t)fbits(inv_x) << 32, a0_hi = sx(fbits(ratio));
    {
        R128 q;
        q.ud[0] = 0;
        q.ud[1] = sx(fbits(inv_y));
        std::memcpy(&VU0.vf[2], &q, 16);
    }
    vu0(kVmoveVf20);
    vu0(kVmoveVf21);
    vu0(kVmoveVf22);
    vu0(kVmoveVf23);
    sq_v0(sp + 0xC0, 0, sx(fbits(far_term)));
    vu0(kVmoveXyzVf2);
    sw(sp + 0xCC, lwu(sp + 0xC8));
    vf_load(1, sp + 0xC0);
    vu0(kVmoveWVf2);
    {
        R128 q;
        q.ud[0] = a0_lo;
        q.ud[1] = a0_hi;
        std::memcpy(&VU0.vf[1], &q, 16);
    }
    vu0(kVmoveVf24);
    vu0(kVmoveVf26);

    // spheres: entries 1..n
    sb(kScratch, 0x81);
    uint32_t idx = 1;
    for (int32_t i = 0; i < lw(sp + 0x30); i++) {
        const uint32_t rec = lwu(sp + 0x34) + (uint32_t)i * 16;
        vf_load(1, rec);
        vu0(kVmoveVf30);
        vu0(kVcallms40);
        copy_q_v0(0x4D6F00, rec + 32);
        vu0_sync();
        const uint32_t clip = vu0_cfc2(18);
        vu0(kVmoveFromVf31);
        vf_store(sp + 0x80, 1);
        sb(kScratch + idx, visibility(clip));
        idx++;
        if (i + 1 < lw(sp + 0x30)) IRQ_CHECK();
    }

    // LOD groups: 0x357E60 when the parent sphere is visible (the original unrolls by 4; the
    // return address follows the unrolled slot)
    copy8(sp + 0x80, mlist + 0x18);
    R128 campos;
    MEM_LQ(sp + 0x70, &campos);
    const int32_t groups = lw(sp + 0x80);
    if (groups > 0) {
        static const uint32_t kLead[3] = {0x3588CC, 0x358910, 0x358954};
        static const uint32_t kLoop[4] = {0x35899C, 0x3589D8, 0x358A14, 0x358A50};
        const int32_t lead = (4 - ((-groups) & 3)) & 3;
        uint32_t rec = lwu(sp + 0x84);
        for (int32_t k = 0; k < groups; k++, rec += 0x20, idx++) {
            uint32_t v = 0;
            if (lbu(kScratch + lhu(rec + 12)) != 0) {
                set(4, (int32_t)rec);
                std::memcpy(&g_ee.r[5], &campos, 16);
                set_f(12, lod_a);
                set_f(13, lod_b);
                call(f_00357E60, k < lead ? kLead[3 - lead + k] : kLoop[(k - lead) & 3]);
                v = reg(2);
            }
            sb(kScratch + idx, v);
            if (k >= lead && ((k - lead) & 3) == 3 && k + 1 < groups) IRQ_CHECK();
        }
    }

    // models: visible when the parent's byte (low 7 bits) is the model's required value
    copy8(sp + 0x90, mlist + 0x20);
    const int32_t models = lw(sp + 0x90);
    uint32_t rec = lwu(sp + 0x94);
    for (int32_t k = 0; k < models; k++, rec += 0x20, idx++) {
        if ((lbu(kScratch + lhu(rec + 16)) & 0x7F) == lhu(rec + 18)) {
            vf_load(1, rec);
            vu0(kVmoveVf30);
            vu0(kVcallms40);
            copy_q_v0(0x4D6F00, rec + 64);
            vu0_sync();
            const uint32_t clip = vu0_cfc2(18);
            vu0(kVmoveFromVf31);
            vf_store(sp + 0xA0, 1);
            sw(kModelDepth + idx * 4, lwu(sp + 0xA0));
            sb(kScratch + idx, visibility(clip));
        } else {
            sb(kScratch + idx, 0);
        }
        if (k + 1 < models) IRQ_CHECK();
    }

    // PS2 tables in the 16 KB scratchpad (guest layout, docs/PS2_LIMITS.md): visibility bytes below
    // the matrix table at 0x1C00, 256 matrix packets
    limit_note("mlist visibility bytes (scratchpad 0x70000000)", idx, 0x1C00, 0x1C00);
    limit_note("world matrix table (scratchpad 0x70001C00)", (uint64_t)(count > 0 ? count : 0) + 1, 0x100, 0x100);

    // the mlist's display objects (vtable +0xC)
    for (int32_t k = 0; k < lw(mlist + 0x88);) {
        const uint32_t obj = lwu(lwu(mlist + 0x90) + (uint32_t)k * 4);
        const uint32_t fn = lwu(lwu(obj) + 0xC);
        set(4, (int32_t)obj);
        set(5, (int32_t)mlist);
        k++;
        call_at(fn, 0x358B70);
        if (k < lw(mlist + 0x88)) IRQ_CHECK();
    }
    set(4, 0x4D2F00);
    call(f_0033C620, 0x358B8C);
    call(f_0033CA08, 0x358B94);
}

// ---------------------------------------------------------------------------------------------
// 0x358BD0 WorldRenderPass(pass)
void world_render_pass() {
    const int32_t pass = (int32_t)reg(4);
    Frame f(0x120);
    const uint32_t sp = f.sp();
    sw(sp + 0x60, 0);  // last matrix packet REF'd
    set(4, 0x4296C0);  // "mlist: flush"
    call(f_0033CA18, 0x358C10);
    set(4, 0x4D2F00);
    call(f_0033C578, 0x358C1C);
    const uint32_t mlist = lwu(kMlist);
    copy8(sp + 0x00, mlist + 0x28);                        // n batches, batches
    copy8(sp + 0x10, mlist + 0x38 + (uint32_t)pass * 16);  // first, last texture record
    copy8(sp + 0x18, mlist + 0x40 + (uint32_t)pass * 16);  // first, last display object
    copy8(sp + 0x20, mlist + 0x30);                        // n texture records, records
    for (uint32_t k = 0; k < 12; k++) sw(sp + 0x30 + k * 4, (k % 4) == 0 ? 0x3F800000u : 0u);  // identity 3x4
    set(4, (int32_t)(sp + 0x30));
    call(f_0035CD28, 0x358CB0);
    const uint32_t identity = reg(2);
    set(4, (int32_t)identity);
    call(f_0035E018, 0x358CBC);
    sw(kMatrixTable, identity);
    call(f_0034F760, 0x358CCC);
    native::ee_world_pass(pass, identity);

    int32_t i = lw(sp + 0x10);
    if (i < lw(sp + 0x14)) {
        sd(sp + 0x68, (uint64_t)(lhu(0x429A98) | 0x30000000u));  // DMA REF tag, qwc of a matrix packet
        do {
            const uint32_t record = lwu(sp + 0x24) + (uint32_t)(i * 0x1C);
            const int32_t first = lw(record + 0x14);
            const int32_t batches = lw(record + 0x18) - first;
            const uint32_t slot = lwu(record + 0x10);
            uint32_t batch = lwu(sp + 4) + (uint32_t)(first * 12);
            bool open = false;
            uint32_t out = 0, qwords = 0;
            sw(sp + 0x70, (uint32_t)(i + 1));
            for (int32_t j = 0; j < batches; j++) {
                copy8(sp + 0x30, batch);
                sw(sp + 0x38, lwu(batch + 8));
                const uint32_t index = lhu(sp + 0x30);
                const uint32_t vis = lbu(kScratch + index);
                batch += 12;
                if ((vis & 0x7F) != lhu(sp + 0x32)) continue;
                if (!open) {
                    set(4, (int32_t)slot);
                    open = true;
                    call(f_0034F8D0, 0x358E84);
                    set(4, batches * 2);
                    call(f_0035DEE0, 0x358E8C);
                    out = reg(2);
                    if (!out) {  // no packet space: the rest of this record is not drawn
                        open = false;
                        call(f_0034F888, 0x358EA0);
                        native::ee_world_record(slot, false);
                        break;
                    }
                }
                const uint32_t matrix = lwu(kMatrixTable + lhu(sp + 0x34) * 4);
                if (lwu(sp + 0x60) != matrix) {
                    qwords++;
                    sw(sp + 0x60, matrix);
                    sq_v0(out, MEM_LD(sp + 0x68) | (uint64_t)(matrix & 0x0FFFFFFFu) << 32, 0x0500000001000101ull);
                    out += 16;
                }
                const uint32_t part = lwu(sp + 0x38);
                const uint32_t itop = (vis >> 7) ? 0x280u : 0x80u;  // bit 7: VU1 clipping path
                qwords++;
                sq_v0(out, (uint64_t)(lhu(sp + 0x36) | 0x30000000u) | (uint64_t)((part + 0x38) & 0x7FFFFFFFu) << 32,
                   sx(itop | 0x04000000u) | 0x0500000000000000ull);
                native::ee_world_leaf(out, part, matrix, itop);
                set(4, (int32_t)slot);
                set_f(12, lwu(kModelDepth + index * 4));
                set_f(13, lwu(part + 0x20));
                call(f_00353470, 0x358E20);
                out += 16;
                IRQ_CHECK();
            }
            if (open) {
                set(4, (int32_t)qwords);
                call(f_0035CBB8, 0x358EB0);
                native::ee_world_record(slot, reg(2) != 0);
                call(f_0034F888, 0x358EB8);
            }
            i = lw(sp + 0x70);
            IRQ_CHECK();
        } while (i < lw(sp + 0x14));
    }
    call(f_0034F710, 0x358ED4);
    call(f_0035E000, 0x358EDC);
    call(f_0033CA08, 0x358EE4);
    for (int32_t k = lw(sp + 0x18); k < lw(sp + 0x1C);) {  // the pass's display objects (vtable +0x14)
        const uint32_t obj = lwu(lwu(mlist + 0x90) + (uint32_t)k * 4);
        const uint32_t fn = lwu(lwu(obj) + 0x14);
        set(4, (int32_t)obj);
        set(5, (int32_t)mlist);
        k++;
        call_at(fn, 0x358F10);
        IRQ_CHECK();
    }
    native::ee_world_pass_end();
    set(2, 0);
}

// 0x35DEE0 WorldPacketBegin(qwc)
void world_packet_begin() {
    const uint64_t qwc = reg64(4);
    Frame f(0x20);
    call(f_0035BA10, 0x35DEF4);
    set64(4, qwc);
    call(f_00366D30, 0x35DEFC);
}

// 0x35CBB8 PacketCommit(qwc) (0x366CD0): the open packet of manager *(0x3E5978) grows by qwc
// quadwords; returns the new fill (quadwords), or 0 (and keeps it) when the buffer has no room.
void packet_commit() {
    const int32_t qwc = (int32_t)reg(4);
    const uint32_t mgr = lwu(0x3E5978);
    const uint32_t buf = mgr + (uint32_t)(lw(mgr + 0x28) * 0x14);
    const int32_t used = lw(buf + 8);
    if (lw(buf + 0x10) < used + qwc + 1) {
        set(2, 0);
        return;
    }
    if (lwu(buf + 4) + (uint32_t)used * 16 == 0) {
        set(2, 0);
        return;
    }
    const int32_t now = lw(buf + 8) + qwc;
    sw(buf + 8, (uint32_t)now);
    set(2, now);
}

// ---------------------------------------------------------------------------------------------
// 0x223858 SkyDraw(sky): the dome follows the camera; type 1 draws the parts inside the view cone
void sky_draw() {
    const uint32_t sky = reg(4);
    Frame f(0xC0);
    const uint32_t sp = f.sp();
    sw(sky + 4, 0);
    if (lw(sky + 0x3C) == 0 || lw(sky + 8) == 0) {
        set(2, 0);
        return;
    }
    call(f_0034FC68, 0x2238B4);
    const uint32_t cam = reg(2);
    copy8(sky + 0x30, cam + 0x24);
    sw(sky + 0x38, lwu(cam + 0x2C));
    uint32_t parts = lwu(sky + 0x3C);
    int64_t mask;
    if (lw(sky + 8) == 1) {
        set(4, -1);
        call(f_002111D0, 0x2238F0);
        const float cone = fmul_ps2(bitsf(g_ee.fpr[0].u), bitsf(0x3F200000));  // 0.625 x fov
        sw(sp, sky + 0xC);
        mask = 1;
        uint32_t centre = parts + 0x2C;
        for (uint32_t k = 0; k < 0x20; k += 2, centre += 0x20) {
            for (uint32_t h = 0; h < 2; h++) {
                const uint32_t c = centre + h * 0x10;
                set_f(12, fbits(fadd_ps2(lf(c + 0xC), cone)));
                // camera forward (cam +0x18) . part centre: mula / madda / madd
                float acc = fmul_ps2(lf(cam + 0x18), lf(c));
                acc = fadd_ps2(acc, fmul_ps2(lf(cam + 0x1C), lf(c + 4)));
                const float dot = fadd_ps2(acc, fmul_ps2(lf(cam + 0x20), lf(c + 8)));
                call(f_0039BB28, h ? 0x2239A4 : 0x22394C);
                if (fcmp_lt(bitsf(g_ee.fpr[0].u), dot)) {
                    mask |= (int64_t)(int32_t)(1u << (k + h));
                    sw(sky + 4, lwu(sky + 4) + 1);
                }
            }
            IRQ_CHECK();
        }
        parts = lwu(sky + 0x3C);
    } else {
        mask = -1;
        sw(sky + 4, 0x20);
        sw(sp, sky + 0xC);
    }
    set(4, lw(parts + 4));
    set(5, lw(sp));
    set64(7, (uint64_t)mask);
    set(6, -1);
    call(f_0034BDA8, 0x223A14);
}

// 0x223800 SkyDestroy(sky, flags)
void sky_destroy() {
    const uint32_t sky = reg(4);
    const uint64_t flags = reg64(5);
    {
        Frame f(0x30);
        sw(sky, 0x408318);
        set(4, -1);
        call(f_0034E8A8, 0x223830);
    }
    set(4, (int32_t)sky);
    set64(5, flags);
    f_00223D00();  // tail call
}

// ---------------------------------------------------------------------------------------------
// 0x326168 WaterDrawIfPresent
void water_draw_if_present() {
    static const bool log = std::getenv("EOD_NATIVE_WATER_LOG") != nullptr;
    static uint32_t calls = 0;
    if (log && (calls++ % 300) == 0) {
        const uint32_t w = lwu(kWater);
        uint32_t tiles = 0;
        if (w)
            for (uint32_t t = lwu(w + 0x40); t && tiles < 100000; t = lwu(t + 0x1B8)) tiles++;
        const uint32_t ml = lwu(lwu(0x3D1C18) + 4);
        std::fprintf(stderr, "[world] views %d mlist %08x objects %d first vtables %08x %08x\n", lw(0x439858), ml,
                     ml ? lw(ml + 0x88) : -1, ml && lw(ml + 0x88) > 0 ? lwu(lwu(lwu(ml + 0x90))) : 0,
                     ml && lw(ml + 0x88) > 1 ? lwu(lwu(lwu(ml + 0x90) + 4)) : 0);
        std::fprintf(stderr, "[world] water surface %08x visible %d slot %d tiles %u lists %d pos %g %g %g\n", w,
                     w ? lw(w + 0x38) : -1, w ? lw(w + 0x3C) : -1, tiles, w ? lw(w + 0x50) : -1, w ? lf(w + 0x24) : 0.0,
                     w ? lf(w + 0x28) : 0.0, w ? lf(w + 0x2C) : 0.0);
    }
    set(2, 0x3E0000);  // lui of the load
    set(4, lw(kWater));
    if (reg(4) != 0) f_00325728();  // tail call
}

// 0x325728 WaterDraw(surface): 4 vertices per tile (corners (0|8, 0|8)), strips of 2 triangles
void water_draw() {
    const uint32_t surface = reg(4);
    Frame f(0x80);
    const uint32_t sp = f.sp();
    if (lw(surface + 0x38) == 0) {
        set(2, 0);
        return;
    }
    call(f_0034F760, 0x32575C);
    set(4, lw(surface + 0x3C));
    call(f_0034F8D0, 0x325764);
    set(4, 0x3000);
    native::ee_water_begin(surface);
    call(f_0035DF10, 0x32576C);
    if (reg(2) == 0) native::ee_water_end(false);
    if (reg(2) != 0) {
        uint32_t tile = lwu(surface + 0x40);
        if (tile) {
            const uint32_t bounds = 0x423788;
            while (tile) {
                if (fcmp_lt(lf(tile + 0x1A4), lf(bounds)) || fcmp_lt(lf(tile + 0x1A8), lf(bounds + 4)) ||
                    fcmp_lt(lf(tile + 0x1AC), lf(bounds + 8)) || fcmp_lt(lf(tile + 0x1B0), lf(bounds + 12))) {
                    set(4, (int32_t)tile);
                    call(f_00324928, 0x3257EC);
                    tile = lwu(tile + 0x1B8);
                    continue;
                }
                static const uint32_t kCornerRa[2] = {0x325820, 0x325880}, kShadeRa[2] = {0x325830, 0x325890},
                                      kVertexRa[2] = {0x325854, 0x3258B4};
                bool full = false;
                for (uint32_t k = 0; k < 4 && !full; k++) {
                    set(4, (int32_t)tile);
                    set(5, (int32_t)sp);
                    set(6, (k & 1) ? 8 : 0);
                    set(7, (k & 2) ? 8 : 0);
                    call(f_00326B80, kCornerRa[k & 1]);
                    set(4, (int32_t)tile);
                    set(5, (int32_t)sp);
                    set(6, 0x4AEFA8);
                    call(f_0031E7E0, kShadeRa[k & 1]);
                    const uint32_t strip = k >= 2 ? 1u : 0u;  // ADC on the first two vertices
                    set(5, (int32_t)strip);
                    set_f(12, lwu(sp + 0x00));
                    set_f(13, lwu(sp + 0x04));
                    set_f(14, lwu(sp + 0x08));
                    set_f(15, lwu(sp + 0x18));
                    set_f(16, lwu(sp + 0x1C));
                    set(4, lw(sp + 0x10));
                    call(f_00361708, kVertexRa[k & 1]);
                    if (reg(2) != 0) {
                        full = true;  // no packet space: this tile ends here
                    } else {
                        native::ee_water_vertex(lwu(sp + 0x10), strip, sp);  // xyz +0, st +0x18
                    }
                }
                tile = lwu(tile + 0x1B8);
                IRQ_CHECK();
            }
        }
        call(f_00361BC8, 0x3258E0);
        native::ee_water_end(true);
    }
    call(f_0034F888, 0x3258E8);
    call(f_0034F710, 0x3258F0);
}

// 0x325E40 WaterSimulate: cars faster than 2.2222223 splash the tile they touch
void water_simulate() {
    Frame f(0xF0);
    const uint32_t sp = f.sp();
    if (lw(kWater) == 0) {
        set(2, 0);
        return;
    }
    call(f_002114B0, 0x325E90);
    if (fcmp_le(bitsf(g_ee.fpr[0].u & 0x7FFFFFFFu), bitsf(0x34000000))) {  // |speed| <= FLT_EPSILON
        set(2, 1);
        return;
    }
    set(4, 0);
    set(5, 0xB4);
    call(f_0012F4F0, 0x325EC8);
    if (reg64(2) == 0) {
        set(4, lw(kWater));
        call(f_00325910, 0x325ED8);
    }
    const float one = 1.0f, zero = 0.0f;
    int32_t car = 0;
    for (;;) {
        call(f_002E87B0, 0x3260BC);
        if (car >= (int32_t)reg(2)) break;
        set(4, car);
        call(f_002E8768, 0x325EF8);
        const uint32_t c = reg(2);
        const float limit = bitsf(0x400E38E4);  // 2.2222223
        if (fcmp_le(lf(c + 0x4C), limit)) {
            car++;
            continue;
        }
        const float strength = fmul_ps2(fsub_ps2(lf(c + 0x4C), limit), bitsf(0x3DB851EC));  // x 0.09
        sw(sp + 0, 0);
        sw(sp + 4, 0);
        sw(sp + 8, 0);
        set(4, (int32_t)sp);
        set(5, car);
        call(f_0031BFA0, 0x325F48);
        if (reg64(2) == 0) {
            car++;
            continue;
        }
        uint32_t surface = lwu(kWater);
        uint32_t tile = lwu(surface + 0x40);
        const int32_t id = (int16_t)lhu(c + 0x30);
        bool found = false;
        for (; tile; tile = lwu(tile + 0x1B8)) {
            set(4, (int32_t)tile);
            set(5, (int32_t)sp);
            set(6, id);
            call(f_0031C7E8, 0x325F74);
            if (reg64(2) != 0) {
                found = true;
                break;
            }
            IRQ_CHECK();
        }
        if (!found) {
            car++;
            continue;
        }
        const float push = bitsf(fbits(fmin_ps2(one, strength)) ^ 0x80000000u);
        surface = lwu(kWater);
        if (lw(surface + 0x38) != 0) {
            for (tile = lwu(surface + 0x40); tile; tile = lwu(tile + 0x1B8)) {
                set(4, (int32_t)tile);
                set(5, (int32_t)sp);
                set(6, id);
                call(f_0031C7E8, 0x325FDC);
                if (reg64(2) != 0) {
                    set(4, (int32_t)tile);
                    set(5, (int32_t)sp);
                    set_f(12, fbits(push));
                    call(f_0031CA30, 0x326004);
                    break;
                }
                IRQ_CHECK();
            }
        }
        // up x contact normal (the original computes it into its frame and does not use it)
        sw(sp + 0x20, 0);
        sw(sp + 0x24, fbits(one));
        sw(sp + 0x28, 0);
        const uint32_t n = c + 0x34;
        float acc = fmul_ps2(lf(n + 4), zero);
        sw(sp + 0x10, fbits(fsub_ps2(acc, fmul_ps2(lf(n + 8), lf(sp + 0x24)))));
        acc = fmul_ps2(lf(n + 8), lf(sp + 0x20));
        sw(sp + 0x14, fbits(fsub_ps2(acc, fmul_ps2(lf(c + 0x34), zero))));
        acc = fmul_ps2(lf(c + 0x34), lf(sp + 0x24));
        sw(sp + 0x18, fbits(fsub_ps2(acc, fmul_ps2(lf(n + 4), lf(sp + 0x20)))));
        car++;
        IRQ_CHECK();
    }
    const uint32_t surface = lwu(kWater);
    if (lw(surface + 0x38) == 0) {
        set(2, 0);
        return;
    }
    set(4, (int32_t)surface);
    call(f_00325AD0, 0x3260DC);
    const int32_t lists = lw(surface + 0x50);
    for (int32_t list = lists - 1; list >= 0; list--) {
        for (uint32_t tile = lwu(surface + 0x40 + (uint32_t)list * 4); tile; tile = lwu(tile + 0x1B8)) {
            set(4, (int32_t)tile);
            call(f_0031C7E0, 0x326108);
            IRQ_CHECK();
        }
        IRQ_CHECK();
    }
    set(2, lists > 0 ? -4 : lists);  // the loop's last sll (-1 << 2), or the list count
}

}  // namespace

extern "C" void f_00214B68(void) { diff_run(0x00214B68, world_frame_render, f_00214B68_orig); }
// 0x3582B8 is reached through src/hle_draw_distance.cpp's wrapper (EOD_CULL_TRACE, frame log).
extern "C" void (*eod_mlist_calc_impl)(void);
extern "C" void eod_world_003582B8(void) { diff_run(0x003582B8, mlist_visibility_calc, f_003582B8_orig); }
static const bool g_mlist_registered = (eod_mlist_calc_impl = eod_world_003582B8, true);
extern "C" void f_00358BD0(void) { diff_run(0x00358BD0, world_render_pass, f_00358BD0_orig); }
extern "C" void f_0035DEE0(void) { diff_run(0x0035DEE0, world_packet_begin, f_0035DEE0_orig); }
extern "C" void f_0035CBB8(void) { diff_run(0x0035CBB8, packet_commit, f_0035CBB8_orig); }
extern "C" void f_00223858(void) { diff_run(0x00223858, sky_draw, f_00223858_orig); }
extern "C" void f_00223800(void) { diff_run(0x00223800, sky_destroy, f_00223800_orig); }
// EOD_R2_REPLAY_SKY_DESTROY=<n>: SkyDestroy (0x223800) runs only when a track unloads, so each
// frame it is replayed n times on a scratch object below the stack (delete flag 0) with the guest
// registers and the colour it resets (0x4D7C90) put back afterwards; EOD_DIFF checks the replays.
void replay_sky_destroy() {
    static const int n = [] {
        const char *e = std::getenv("EOD_R2_REPLAY_SKY_DESTROY");
        return e ? std::atoi(e) : 0;
    }();
    if (n <= 0) return;
    const EEState saved = g_ee;
    R128 colour;
    MEM_LQ(0x4D7C90, &colour);
    const uint32_t obj = (reg(29) - 0x800) & ~0xFu;
    for (int k = 0; k < n; k++) {
        for (uint32_t o = 0; o < 0x40; o += 4) sw(obj + o, 0);
        set(29, (int32_t)(obj - 0x100));
        set(4, (int32_t)obj);
        set(5, k & 2);  // delete flag clear (bit 0)
        set64(31, 0x326168);
        f_00223800();
    }
    MEM_SQ(0x4D7C90, &colour);
    g_ee = saved;
}

// EOD_R2_REPLAY_WATER=<n> (verification runs only): WaterDraw runs only while the surface is in view
// (+0x38), which a scripted race seldom gives; after the frame's own call it is run n more times
// with +0x38 forced on and put back (the extra draws reach the GS like normal ones).
void replay_water() {
    static const int n = [] {
        const char *e = std::getenv("EOD_R2_REPLAY_WATER");
        return e ? std::atoi(e) : 0;
    }();
    const uint32_t w = lwu(kWater);
    if (n <= 0 || !w) return;
    const EEState saved = g_ee;
    const uint32_t visible = lwu(w + 0x38);
    sw(w + 0x38, 1);
    for (int k = 0; k < n; k++) {
        set(4, (int32_t)w);
        set64(31, 0x326168);
        f_00325728();
    }
    sw(w + 0x38, visible);
    g_ee = saved;
}

extern "C" void f_00326168(void) {
    replay_sky_destroy();
    diff_run(0x00326168, water_draw_if_present, f_00326168_orig);
    replay_water();
}
extern "C" void f_00325728(void) { diff_run(0x00325728, water_draw, f_00325728_orig); }
extern "C" void f_00325E40(void) { diff_run(0x00325E40, water_simulate, f_00325E40_orig); }

// 0x326188 WaterCreate(pos, axis a, axis b), from the Water display object (subtype 28, only
// Afton's track.grf has one): the original creates the surface only when *(0x439858) (views) < 2,
// so split screen has no water. With SplitScreenParity (P8) it is created as for one view.
extern "C" void f_00326188(void) {
    const int32_t views = lw(0x439858);
    const bool lift = split_parity() && views >= 2;
    if (lift) sw(0x439858, 1);
    f_00326188_orig();
    if (lift) sw(0x439858, (uint32_t)views);
}
