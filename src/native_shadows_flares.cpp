// native_shadows_flares.cpp - projected shadows & sun/light flares (SLUS_209.10), readable C++
// replacing the recompiled code. Stage R4 Phase A1 of the native renderer; the address, struct,
// VU1 and GS state tables are in docs/NATIVE_RENDERER.md ("Stage R4: shadows, particles, sprites,
// flares and immediate geometry").
//
//   0x34C7D0 ShadowBegin(dir*, alpha)       copy shadow dir to 0x4C96A8, open shadow RT (0x35DA30)
//   0x34C798 ShadowEnd()                    close & composite shadow RT (0x35D9F0), unbind slots 0/1
//   0x34C208 ShadowSubmit(id, mat*, len)    sphere-cull extruded volume, emit front/back shadow batches
//   0x34C4B0 ShadowSubmitMasked(id, m, mask, len)  same with temporary layer-mask override (0x3DD634)
//   0x35D298 ShadowEmitBatch(batch, clip, matPkt)  forward with current pass *(0x4D7D14) to 0x35D170
//   0x35D170 ShadowEmitBatchPass(b, pass, clip, m) emit matrix REF + shadow batch DMA REF (ITOP)
//   0x304D50 FlareRender()                  update sun & point-light flares, flush 2D flare buckets
//   0x20F2D8 ViewportRenderFinish()         split-screen parity wrapper: lifts single-viewport flare gate
#include "core/config.h"
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
void f_0034C7D0_orig(void);
void f_0034C798_orig(void);
void f_0034C208_orig(void);
void f_0034C4B0_orig(void);
void f_0035D298_orig(void);
void f_0035D170_orig(void);
void f_00304D50_orig(void);
void f_0020F2D8_orig(void);

// Guest callees (called through public symbols so wrappers/traces/nested diffs still apply)
void f_0035DA30(void);  // GsShadowBegin(alpha) -> int opened
void f_0035D9F0(void);  // GsShadowEnd()
void f_003514F0(void);  // TextureBind(slot)
void f_0034F650(void);  // FrustumCullSphere(center*, clipFlag*; f12 radius) -> int visible
void f_0035D9C8(void);  // ShadowSetExtrude(f12 extrudeLen)
void f_0035CD28(void);  // MatrixPacketBuild(mat*) -> matPkt
void f_0034D060(void);  // ModelGetBatchList(modelId) -> { int count; uint32_t *batches; }*
void f_0035D148(void);  // ShadowSetPass(pass)
void f_0035D298(void);  // ShadowEmitBatch(batch, clipFlag, matPkt)
void f_0035D170(void);  // ShadowEmitBatchPass(batch, pass, clipFlag, matPkt)
void f_00366D30(void);  // PacketReserve(qwc) -> ptr
void f_00366CD0(void);  // PacketCommit(qwc)

void f_0011F9A0(void);  // FlareSuppressed() -> int (*(0x3C4BDC))
void f_00337718(void);  // TimerGetMs() -> uint32_t ms
void f_0034F760(void);  // RenderStatePush()
void f_0034F710(void);  // RenderStatePop()
void f_0034DFE0(void);  // RenderFlagClear(flag)
void f_0034DEC0(void);  // RenderFlagSet(flag)
void f_00304608(void);  // SunFlareUpdate(f12 dt)
void f_00304970(void);  // SunFlareEmit()
void f_00304818(void);  // PointLightFlareUpdate(light*, f12 dt)
void f_00304B18(void);  // PointLightFlareEmit(light*)
void f_0034D9A8(void);  // Begin2DTexture(texSlot)
void f_0035DF10(void);  // ImmediateBegin(numVerts) -> int ok
void f_00303CB8(void);  // FlareEmitQuad(pos*, rgb; f12 size, f13 intensity)
void f_00361BC8(void);  // ImmediateEnd()
void f_0034F888(void);  // TextureSlotEnd()

// ViewportRenderFinish (0x20F2D8) callees
void f_0034D8A0(void);  // SetViewport(x, y, w, h)
void f_0034D7D0(void);  // SetCamera(cam*)
void f_00212AC8(void);  // ViewRenderSetup(cam*, 0)
void f_00355420(void);  // TextureFrameTick()
void f_0034C9C0(void);  // RenderQueueBeginFrame()
void f_0020F0D8(void);  // ViewRenderWorld()
void f_0034C930(void);  // RenderQueueEndFrame()
void f_0034C860(void);  // RenderQueueFlush()
void f_002E7368(void);  // EffectsPostFlush(0)
void f_00304D50(void);  // FlareRender()
}

namespace {

inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline uint64_t reg64(int r) { return g_ee.r[r].ud[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline void set64(int r, uint64_t v) { g_ee.r[r].ud[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
inline int16_t lh(uint32_t a) { return (int16_t)MEM_LH(a); }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline float rf(uint32_t a) {
    uint32_t u = MEM_LW(a);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline uint32_t fu(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline void wf(uint32_t a, float f) { sw(a, fu(f)); }
inline float fbits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline void set_f(int r, uint32_t bits) { g_ee.fpr[r].u = bits; }
inline void call(void (*fn)(void), uint32_t ra) {
    set(31, (int32_t)ra);
    fn();
}

// Guest addresses (SLUS_209.10, docs/NATIVE_RENDERER.md Stage R4)
constexpr uint32_t kShadowDir = 0x4C96A8;        // float[3] shadow extrusion direction
constexpr uint32_t kShadowOpen = 0x4C96B4;       // int32_t shadow target active
constexpr uint32_t kModelTablePtr = 0x4C96C0;    // ModelDesc* (stride 0x14; +8 -> hdr)
constexpr uint32_t kLayerMask = 0x3DD634;        // uint32_t active layer bitmask
constexpr uint32_t kCurMatPkt = 0x4D7C48;        // uint32_t current matrix packet
constexpr uint32_t kLastMatPkt = 0x4D7C4C;       // uint32_t last emitted matrix packet
constexpr uint32_t kShadowPass = 0x4D7D14;       // int32_t shadow pass (1=front/+, 0=back/-)

constexpr uint32_t kFlareLastMs = 0x3DBF6C;      // uint32_t last TimerGetMs()
constexpr uint32_t kSunFlareEnabled = 0x3DBB40;  // int32_t sun flare enabled
constexpr uint32_t kPointLightCount = 0x3DBF68;  // int32_t point-light flare count
constexpr uint32_t kPointLightBase = 0x3DBB48;   // PointLightFlare[n] (stride 0x2C)
constexpr uint32_t kFlareBucketCount = 0x3D94F0; // int32_t flare bucket count
constexpr uint32_t kFlareBucketBase = 0x3D94F4;  // FlareBucket[n] (stride 0x308)

constexpr uint32_t kViewCount = 0x3D1A74;        // int32_t viewport count (1, 2, 4)
constexpr uint32_t kViewIndex = 0x3D1A78;        // int32_t active viewport index
constexpr uint32_t kScreenW = 0x3DC978;          // int32_t screen width
constexpr uint32_t kScreenH = 0x3DC97C;          // int32_t screen height

bool split_parity() {
    static const bool on = [] {
        if (const char *e = std::getenv("EOD_SPLIT_PARITY")) return *e && *e != '0';
        return g_config.split_screen_parity;
    }();
    return on;
}

bool diff_on() {
    static const bool on = [] {
        const char *e = std::getenv("EOD_DIFF");
        return e && *e && *e != '0';
    }();
    return on;
}

// ---- 0x34C7D0 ShadowBegin(dir*, alpha) ---------------------------------------------------------
void shadow_begin() {
    const uint32_t dir_ptr = reg(4);
    const int32_t alpha = (int32_t)reg(5);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    // Copy 3-float shadow direction into 0x4C96A8
    sw(kShadowDir + 0, lwu(dir_ptr + 0));
    sw(kShadowDir + 4, lwu(dir_ptr + 4));
    sw(kShadowDir + 8, lwu(dir_ptr + 8));

    // Snapshot main-viewport camera/projection BEFORE 0x35DA30 -> 0x363218 overwrites 0x4D7C20
    if (!diff_in_original_side()) native::ee_shadow_begin(alpha);

    set(4, alpha);
    call(f_0035DA30, 0x34C808);
    const uint32_t opened = reg(2);
    sw(kShadowOpen, opened);

    if (!diff_in_original_side()) native::ee_shadow_opened(opened != 0);

    set(2, (int32_t)opened);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// ---- 0x34C798 ShadowEnd() ----------------------------------------------------------------------
void shadow_end() {
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x10;
    set(29, (int32_t)sp);

    if (!diff_in_original_side()) native::ee_shadow_end();

    call(f_0035D9F0, 0x34C7A8);
    set(4, 0);
    call(f_003514F0, 0x34C7B0);
    set(4, 1);
    call(f_003514F0, 0x34C7B8);
    sw(kShadowOpen, 0);
    set(2, 0x4D0000);  // matches original lui v0, 0x4D before final sw

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x10));
}

// Shared helper for 0x34C208 and 0x34C4B0: transform model bounding sphere center, offset by
// shadowDir * (extrudeLen * 0.5f), and frustum-cull with radius + extrudeLen * 0.5f.
bool shadow_cull_sphere(uint32_t sp, uint32_t model_id, uint32_t mat_ptr, uint32_t extrude_bits) {
    const uint32_t table = lwu(kModelTablePtr);
    const uint32_t hdr = lwu(table + model_id * 0x14u + 8u);
    const uint32_t center_ptr = hdr + 0x1Cu;

    // Matrix 3x3 * center + translation (exact PS2 fmul_ps2 / fadd_ps2 order)
    const float cx = rf(center_ptr + 0);
    const float cy = rf(center_ptr + 4);
    const float cz = rf(center_ptr + 8);

    float wx = fadd_ps2(fadd_ps2(fmul_ps2(cx, rf(mat_ptr + 0)), fmul_ps2(cy, rf(mat_ptr + 12))),
                        fmul_ps2(cz, rf(mat_ptr + 24)));
    float wy = fadd_ps2(fadd_ps2(fmul_ps2(cx, rf(mat_ptr + 4)), fmul_ps2(cy, rf(mat_ptr + 16))),
                        fmul_ps2(cz, rf(mat_ptr + 28)));
    float wz = fadd_ps2(fadd_ps2(fmul_ps2(cx, rf(mat_ptr + 8)), fmul_ps2(cy, rf(mat_ptr + 20))),
                        fmul_ps2(cz, rf(mat_ptr + 32)));

    wx = fadd_ps2(wx, rf(mat_ptr + 36));
    wy = fadd_ps2(wy, rf(mat_ptr + 40));
    wz = fadd_ps2(wz, rf(mat_ptr + 44));

    const float half_len = fmul_ps2(fbits(extrude_bits), 0.5f);
    wx = fadd_ps2(wx, fmul_ps2(rf(kShadowDir + 0), half_len));
    wy = fadd_ps2(wy, fmul_ps2(rf(kShadowDir + 4), half_len));
    wz = fadd_ps2(wz, fmul_ps2(rf(kShadowDir + 8), half_len));

    wf(sp + 0, wx);
    wf(sp + 4, wy);
    wf(sp + 8, wz);
    sw(sp + 16, 1);  // clipFlag = 1

    const float radius = fadd_ps2(half_len, rf(hdr + 24));
    set(4, (int32_t)sp);
    set(5, (int32_t)(sp + 16));
    set_f(12, fu(radius));
    call(f_0034F650, 0x34C3D8);
    return reg(2) != 0;
}

// ---- 0x34C208 ShadowSubmit(modelId, mat*, f12 extrudeLen) --------------------------------------
void shadow_submit() {
    const uint32_t model_id = reg(4);
    const uint32_t mat_ptr = reg(5);
    const uint32_t extrude_bits = g_ee.fpr[12].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0xA0;
    set(29, (int32_t)sp);

    if (shadow_cull_sphere(sp, model_id, mat_ptr, extrude_bits)) {
        set_f(12, extrude_bits);
        call(f_0035D9C8, 0x34C3E8);

        set(4, (int32_t)mat_ptr);
        call(f_0035CD28, 0x34C3F0);
        const uint32_t mat_pkt = reg(2);

        const uint32_t clip_flag = lwu(sp + 16);
        set(4, (int32_t)model_id);
        call(f_0034D060, 0x34C400);
        const uint32_t batch_list = reg(2);
        int32_t count = lw(batch_list);

        for (int32_t i = 0; i < count; i++) {
            const uint32_t arr = lwu(batch_list + 4);
            const uint32_t batch = lwu(arr + (uint32_t)i * 4u);
            const uint32_t layer_bit = 1u << ((uint32_t)lh(batch + 24) & 31u);
            if ((lwu(kLayerMask) & layer_bit) == 0) continue;

            // Pass 1: front faces (additive)
            set(4, 1);
            call(f_0035D148, 0x34C448);
            set(4, (int32_t)batch);
            set(5, (int32_t)clip_flag);
            set(6, (int32_t)mat_pkt);
            call(f_0035D298, 0x34C458);

            // Pass 0: back faces (subtractive)
            set(4, 0);
            call(f_0035D148, 0x34C460);
            set(4, (int32_t)batch);
            set(5, (int32_t)clip_flag);
            set(6, (int32_t)mat_pkt);
            call(f_0035D298, 0x34C470);

            count = lw(batch_list);
        }
        if (count > 0) set(2, 0);  // matches original loop-exit slt v0, s1, a2 == 0
    }

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0xA0));
}

// ---- 0x34C4B0 ShadowSubmitMasked(modelId, mat*, layerMask, f12 extrudeLen) ---------------------
void shadow_submit_masked() {
    const uint32_t model_id = reg(4);
    const uint32_t mat_ptr = reg(5);
    const uint32_t mask = reg(6);
    const uint32_t extrude_bits = g_ee.fpr[12].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0xC0;
    set(29, (int32_t)sp);

    if (shadow_cull_sphere(sp, model_id, mat_ptr, extrude_bits)) {
        const uint32_t saved_mask = lwu(kLayerMask);
        sw(kLayerMask, mask);

        set_f(12, extrude_bits);
        call(f_0035D9C8, 0x34C6A8);

        set(4, (int32_t)mat_ptr);
        call(f_0035CD28, 0x34C6B0);
        const uint32_t mat_pkt = reg(2);

        const uint32_t clip_flag = lwu(sp + 16);
        set(4, (int32_t)model_id);
        call(f_0034D060, 0x34C6C0);
        const uint32_t batch_list = reg(2);
        int32_t count = lw(batch_list);

        for (int32_t i = 0; i < count; i++) {
            const uint32_t arr = lwu(batch_list + 4);
            const uint32_t batch = lwu(arr + (uint32_t)i * 4u);
            const uint32_t layer_bit = 1u << ((uint32_t)lh(batch + 24) & 31u);
            if ((lwu(kLayerMask) & layer_bit) == 0) continue;

            // Pass 1: front faces (additive)
            set(4, 1);
            call(f_0035D148, 0x34C704);
            set(4, (int32_t)batch);
            set(5, (int32_t)clip_flag);
            set(6, (int32_t)mat_pkt);
            call(f_0035D298, 0x34C714);

            // Pass 0: back faces (subtractive)
            set(4, 0);
            call(f_0035D148, 0x34C71C);
            set(4, (int32_t)batch);
            set(5, (int32_t)clip_flag);
            set(6, (int32_t)mat_pkt);
            call(f_0035D298, 0x34C72C);

            count = lw(batch_list);
        }

        sw(kLayerMask, saved_mask);
        if (count > 0) set(2, 0);  // matches original loop-exit slt v0, s1, a2 == 0
    }

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0xC0));
}

// ---- 0x35D298 ShadowEmitBatch(batch, clipFlag, matPkt) -----------------------------------------
void shadow_emit_batch() {
    const uint32_t batch = reg(4);
    const uint32_t clip_flag = reg(5);
    const uint32_t mat_pkt = reg(6);
    const uint32_t pass = lwu(kShadowPass);
    set(4, (int32_t)batch);
    set(5, (int32_t)pass);
    set(6, (int32_t)clip_flag);
    set(7, (int32_t)mat_pkt);
    f_0035D170();
}

// ---- 0x35D170 ShadowEmitBatchPass(batch, pass, clipFlag, matPkt) -------------------------------
void shadow_emit_batch_pass() {
    const uint32_t batch = reg(4);
    const uint32_t pass = reg(5);
    const uint32_t clip_flag = reg(6);
    const uint32_t mat_pkt = reg(7);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x50;
    set(29, (int32_t)sp);

    if (mat_pkt != 0xFFFFFFFFu) {
        sw(kCurMatPkt, mat_pkt);
        set(4, 1);
        call(f_00366D30, 0x35D1B4);
        const uint32_t pkt = reg(2);
        if (pkt != 0) {
            sw(pkt + 0, 0x30000009u);          // DMA REF qwc=9
            sw(pkt + 4, mat_pkt & 0x0FFFFFFFu);
            sw(pkt + 8, 0x01000101u);          // STCYCL 1,1
            sw(pkt + 12, 0x05000000u);         // STMOD 0
            set(4, 1);
            call(f_00366CD0, 0x35D1F4);
        }
        sw(kLastMatPkt, mat_pkt);
    }

    set(4, 1);
    call(f_00366D30, 0x35D204);
    const uint32_t ref_pkt = reg(2);
    if (ref_pkt != 0) {
        const uint32_t itop_val = ((clip_flag != 0 ? (pass | 0x200u) : pass) & 0x3FFu) | 0x04000000u;
        const uint32_t qwc = (uint32_t)(uint16_t)lh(batch + 52);
        const uint32_t data_addr = (batch + 0x38u) & 0x7FFFFFFFu;
        // 16-byte DMA REF qword: [0]=0x30000000|qwc, [4]=data_addr, [8]=ITOP, [12]=0x05000000 (STMOD 0)
        // Also populate R[2].ud[1] to match original mmi_pcpyld(2, 2, 6, 14) upper 64 bits.
        sw(ref_pkt + 0, 0x30000000u | qwc);
        sw(ref_pkt + 4, data_addr);
        sw(ref_pkt + 8, itop_val);
        sw(ref_pkt + 12, 0x05000000u);
        g_ee.r[2].ud[1] = (0x05000000ull << 32) | (uint64_t)itop_val;

        if (!diff_in_original_side()) {
            native::ee_shadow_batch(batch, pass, clip_flag, mat_pkt, ref_pkt);
        }

        set(4, 1);
        set64(31, saved_ra);
        set(29, (int32_t)(sp + 0x50));
        f_00366CD0();
        return;
    }

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x50));
}

// ---- 0x304D50 FlareRender() --------------------------------------------------------------------
void flare_render() {
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x130;
    set(29, (int32_t)sp);

    call(f_0011F9A0, 0x304D74);
    if (reg(2) != 0) {
        set64(31, saved_ra);
        set(29, (int32_t)(sp + 0x130));
        return;
    }

    call(f_00337718, 0x304D84);  // TimerGetMs
    const uint32_t now_ms = reg(2);
    const int32_t prev_ms = lw(kFlareLastMs);
    sw(kFlareLastMs, now_ms);
    int32_t diff_ms = (int32_t)(now_ms - (uint32_t)prev_ms);
    if (diff_ms < 1) diff_ms = 1;
    if (diff_ms > 100) diff_ms = 100;
    const float dt = fmul_ps2((float)diff_ms, fbits(0x3A83126Fu));  // 0.001f

    call(f_0034F760, 0x304DD0);  // RenderStatePush
    set(4, 4);
    call(f_0034DFE0, 0x304DD8);  // RenderFlagClear(4)
    set(4, 8);
    call(f_0034DFE0, 0x304DE0);  // RenderFlagClear(8)
    set(4, 2);
    call(f_0034DFE0, 0x304DE8);  // RenderFlagClear(2)
    set(4, 0x80);
    call(f_0034DEC0, 0x304DF0);  // RenderFlagSet(0x80)

    if (lw(kSunFlareEnabled) != 0) {
        set_f(12, fu(dt));
        call(f_00304608, 0x304E08);
        call(f_00304970, 0x304E10);
    }

    for (int32_t i = 0; i < lw(kPointLightCount); i++) {
        const uint32_t light = kPointLightBase + (uint32_t)i * 0x2Cu;
        set(4, (int32_t)light);
        set_f(12, fu(dt));
        call(f_00304818, 0x304E44);
        set(4, (int32_t)light);
        call(f_00304B18, 0x304E4C);
    }

    for (int32_t b = 0; b < lw(kFlareBucketCount); b++) {
        const uint32_t bucket = kFlareBucketBase + (uint32_t)b * 0x308u;
        const int32_t count = lw(bucket);
        if (count <= 0) continue;

        const uint32_t tex_slot = lwu(bucket + 772);
        set(4, (int32_t)tex_slot);
        call(f_0034D9A8, 0x304EA0);

        if (!diff_in_original_side()) native::ee_flare_begin(tex_slot);

        set(4, lw(bucket) << 2);
        call(f_0035DF10, 0x304EAC);
        const bool ok = reg(2) != 0;
        if (!ok) {
            if (!diff_in_original_side()) native::ee_flare_end(false);
        } else {
            for (int32_t j = 0; j < lw(bucket); j++) {
                const uint32_t entry = bucket + 4u + (uint32_t)j * 0x18u;
                set(4, (int32_t)entry);
                set(5, lw(entry + 20));
                set_f(12, lwu(entry + 12));
                set_f(13, lwu(entry + 16));
                call(f_00303CB8, 0x304EE8);
            }
            call(f_00361BC8, 0x304F0C);
            if (!diff_in_original_side()) native::ee_flare_end(true);
        }

        call(f_0034F888, 0x304F14);
        sw(bucket, 0);
    }

    call(f_0034F710, 0x304F30);  // RenderStatePop

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x130));
}

// ---- 0x20F2D8 ViewportRenderFinish() (P8 split-screen flare parity) ----------------------------
// Original skips 0x304D50 when *(0x3D1A74) (viewport count) != 1. With SplitScreenParity (and not
// under EOD_DIFF), we also run 0x304D50 for each split-screen viewport.
void viewport_render_finish_parity() {
    const int32_t views = lw(kViewCount);
    const bool lift = split_parity() && !diff_on() && views >= 2;
    if (!lift) {
        f_0020F2D8_orig();
        return;
    }
    // Run the exact original function; since views >= 2, it skips 0x304D50, so we invoke 0x304D50
    // right after its flush if we want flares in split-screen. Better: run the exact body with the
    // single-viewport check lifted so 0x304D50 runs BEFORE the post-flare display objects (0x495CAC)
    // and BEFORE RenderFlagClear(2)!
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x70;
    set(29, (int32_t)sp);

    const uint32_t view_arr = lwu(0x495C48);
    const int32_t view_idx = lw(kViewIndex);
    const uint32_t cam = view_arr + (uint32_t)view_idx * 0xE4u + 4u;
    sw(0x495CA8, lwu(cam + 48));

    call(f_00337718, 0x20F330);
    const uint32_t t0 = reg(2);

    const int32_t nviews = lw(kViewCount);
    const int32_t rows = (nviews < 2) ? 1 : 2;
    const int32_t cols = (nviews + rows - 1) / rows;
    const int32_t vw = lw(kScreenW) / cols;
    const int32_t vh = lw(kScreenH) / rows;
    const int32_t row = view_idx / cols;
    const int32_t col = view_idx % cols;
    if (nviews != 0) {
        set(4, vw * col);
        set(5, vh * row);
        set(6, vw);
        set(7, vh);
        call(f_0034D8A0, 0x20F398);
    }

    set(4, (int32_t)cam);
    call(f_0034D7D0, 0x20F3A0);
    set(4, (int32_t)cam);
    set(5, 0);
    call(f_00212AC8, 0x20F3AC);
    call(f_00355420, 0x20F3B4);
    call(f_0034C9C0, 0x20F3BC);
    call(f_0020F0D8, 0x20F3C4);
    call(f_0034C930, 0x20F3CC);
    call(f_0034C860, 0x20F3D4);
    set(4, 0);
    call(f_002E7368, 0x20F3DC);

    // P8: always run FlareRender (0x304D50), lifting the PS2 single-viewport check
    call(f_00304D50, 0x20F3F0);

    for (int32_t i = 0; i < lw(0x495CAC); i++) {
        const uint32_t list = lwu(0x495CB4);
        const uint32_t obj = lwu(list + (uint32_t)i * 4u);
        const uint32_t vtbl = lwu(obj);
        const uint32_t fn = lwu(vtbl + 28);
        set(4, (int32_t)obj);
        set(31, 0x20F42C);
        ee_call(fn);
    }

    call(f_00337718, 0x20F454);
    const uint32_t dt_ms = reg(2) - t0;
    sw(0x495C6C, lwu(0x495C6C) + dt_ms);
    sw(0x495C68, lwu(0x495C68) + 1u);
    set(4, 2);
    call(f_0034DFE0, 0x20F478);

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x70));
}

}  // namespace

extern "C" void eod_r4_0034C208(void) { diff_run(0x0034C208, shadow_submit, f_0034C208_orig); }
extern "C" void eod_r4_0034C4B0(void) { diff_run(0x0034C4B0, shadow_submit_masked, f_0034C4B0_orig); }
extern "C" void eod_r4_0035D298(void) { diff_run(0x0035D298, shadow_emit_batch, f_0035D298_orig); }

extern "C" void f_0034C7D0(void) { diff_run(0x0034C7D0, shadow_begin, f_0034C7D0_orig); }
extern "C" void f_0034C798(void) { diff_run(0x0034C798, shadow_end, f_0034C798_orig); }
extern "C" void f_0035D170(void) { diff_run(0x0035D170, shadow_emit_batch_pass, f_0035D170_orig); }
extern "C" void f_00304D50(void) { diff_run(0x00304D50, flare_render, f_00304D50_orig); }
extern "C" void f_0020F2D8(void) { viewport_render_finish_parity(); }
