// native_effects_imm.cpp - particles, effect sprites, immediate geometry, sphere-tested immediate
// geometry, and indexed immediate / cockpit quad (SLUS_209.10), readable C++ replacing the
// recompiled code. Stage R4 Phase A2 of the native renderer; specs & layouts in
// docs/NATIVE_RENDERER.md ("Stage R4: shadows, particles, sprites, flares and immediate geometry").
//
//   Particles (VU1 0x08E0, state 0x4D8798):
//     0x363B98 ParticleBegin(count, scale)
//     0x3639D0 ParticleAdd(col_rgb, col_alpha, size, x, y, z, angle)
//     0x363B68 ParticleEnd()
//     0x363FF8 ParticlePacketReserve(state*)
//
//   Simple effect sprites (VU1 0x01B8, state 0x4D8760):
//     0x363F48 SpriteBegin(count)
//     0x363DE8 SpriteAdd(r128 col, flip, size, x, y, z)
//     0x363F18 SpriteEnd()
//     0x364548 SpritePacketReserve(state*)
//
//   Atlas/grid effect sprites (VU1 0x05E0, state 0x4D8778):
//     0x363D78 AtlasSpriteBegin(count, gridSize)
//     0x363C18 AtlasSpriteAdd(r128 col, tile, size, x, y, z)
//     0x363D48 AtlasSpriteEnd()
//     0x3642B8 AtlasSpritePacketReserve(state*)  [RA 0x3642F4]
//
//   Immediate geometry (VU1 0x0270, state 0x3E5848):
//     0x35DF10 ImmediateBegin(numVerts)
//     0x361468 ImmediatePacketBegin(numVerts)
//     0x361708 ImmediateVertex(colour, strip, x, y, z, u, v)
//     0x361BC8 ImmediateEnd()
//     0x364D18 ImmediateVertexContinue(state*, colour, strip, x, y, z, u, v)
//
//   Sphere-tested pre-built immediate geometry (VU1 0x0270):
//     0x34EE70 SphereImmSubmit(obj*)
//     0x34EEB8 SphereImmSubmitTransformed(obj*, mat*)
//     0x361ED8 ImmStaticDraw(bufObj*)
//     0x361C30 ImmStaticEmitRef(bufObj*)
//
//   Indexed immediate triangles & cockpit non-queue quad (0x219C48 -> 0x34DB70 -> 0x35CAA0):
//     0x35CAA0 ImmediateDrawIndexedTris(verts*, numVerts, indices*, numIndices)
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstring>

extern "C" {
void f_00363B98_orig(void);
void f_003639D0_orig(void);
void f_00363B68_orig(void);
void f_00363FF8_orig(void);
void f_00363F48_orig(void);
void f_00363DE8_orig(void);
void f_00363F18_orig(void);
void f_00364548_orig(void);
void f_00363D78_orig(void);
void f_00363C18_orig(void);
void f_00363D48_orig(void);
void f_003642B8_orig(void);
void f_0035DF10_orig(void);
void f_00361468_orig(void);
void f_00361708_orig(void);
void f_00361BC8_orig(void);
void f_00364D18_orig(void);
void f_0034EE70_orig(void);
void f_0034EEB8_orig(void);
void f_00361ED8_orig(void);
void f_00361C30_orig(void);
void f_0035CAA0_orig(void);

// Guest callees (invoked through public symbols so wrappers/traces/nested diffs stay active)
void f_0035DD28(void);  // DentOffsetWrite / VifUploadTable(flag, src, count)
void f_0035DDC8(void);  // MaterialPacketSet(pkt)
void f_0035CA98(void);  // Vu1SetParticleMicrocode()
void f_003620A8(void);  // GsStateFlush()
void f_00366D30(void);  // PacketReserve(qwc) -> ptr
void f_00366CD0(void);  // PacketCommit(qwc)
void f_00364068(void);  // ParticlePacketFlush(state*)
void f_00364328(void);  // AtlasSpritePacketFlush(state*)
void f_00364500(void);  // AtlasSpriteSetGrid(state*, gridSize)
void f_003645B8(void);  // SpritePacketFlush(state*)
void f_0035BA10(void);  // EmitCameraMatrixMaterial()
void f_003651D8(void);  // ImmediatePacketEnd(state*)
void f_0034E308(void);  // FrustumTestSphere(pos*, 0, 0; f12 radius)

// Public symbols for our own replaced functions (for nested calls)
void f_00363FF8(void);
void f_00364548(void);
void f_003642B8(void);
void f_0035DF10(void);
void f_00361468(void);
void f_00361708(void);
void f_00361BC8(void);
void f_00364D18(void);
void f_00361ED8(void);
void f_00361C30(void);
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
inline float fbits(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline uint32_t fu(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline void set_f(int r, uint32_t bits) { g_ee.fpr[r].u = bits; }
inline void call(void (*fn)(void), uint32_t ra) {
    set(31, (int32_t)ra);
    fn();
}

// Guest state addresses (SLUS_209.10)
constexpr uint32_t kParticleState = 0x4D8798;  // 0x4E0000 - 30824
constexpr uint32_t kAtlasState = 0x4D8778;     // 0x4E0000 - 30856
constexpr uint32_t kSpriteState = 0x4D8760;    // 0x4E0000 - 30880
constexpr uint32_t kImmState = 0x3E5848;       // 0x3E0000 + 0x5848
constexpr uint32_t kTexStateWord = 0x4D8BB8;   // 0x4E0000 - 29768

// Cockpit quad marker: set when 0x35CAA0 is invoked from 0x219C48 (RA 0x21A064)
bool g_cockpit_quad_scope = false;
// Native 2D marker (R7 A1b, KIND_HUD): 0x35CAA0 reached through 0x34DB70 (RA kept) from the rect
// leaf 0x11E730 (RA 0x11E834, under 0x11C278) or the frame sprite leaf 0x29E138 (RA 0x29E488);
// the text flush 0x29A2B8 calls 0x35DF10 directly (RA 0x29A368)
bool g_hud_scope = false;

// ---- Particles (0x4D8798, VU1 0x08E0) ---------------------------------------------------------

// 0x363FF8 ParticlePacketReserve(state*) -> int ok
void particle_packet_reserve() {
    const uint32_t state = reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    sw(state + 4, 0);
    const int32_t rem = lw(state + 12);
    const int32_t cap = (rem > 15) ? 16 : rem;
    sw(state + 12, (uint32_t)(rem - cap));
    sw(state + 8, (uint32_t)cap);

    set(4, cap + 7);
    call(f_00366D30, 0x364034);
    const uint32_t pkt = reg(2);
    sw(state + 20, pkt);
    int32_t ok = 0;
    if (pkt != 0) {
        sw(state + 24, pkt + 0x60u);
        ok = 1;
    }

    set(2, ok);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// 0x363B98 ParticleBegin(int32_t count, float scale) -> int active
void particle_begin() {
    const int32_t count = (int32_t)reg(4);
    const uint32_t scale_bits = g_ee.fpr[12].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x50;
    set(29, (int32_t)sp);

    set(4, 1);
    set(5, (int32_t)(kParticleState + 0x1Cu));
    set(6, 0x4F);
    call(f_0035DD28, 0x363BD0);

    set(4, (int32_t)reg(2));
    call(f_0035DDC8, 0x363BD8);

    call(f_0035CA98, 0x363BE0);
    call(f_003620A8, 0x363BE8);

    sw(kParticleState + 12, (uint32_t)count);
    sw(kParticleState + 16, scale_bits);

    if (!diff_in_original_side()) native::ee_particle_begin(fbits(scale_bits));

    set(4, (int32_t)kParticleState);
    call(f_00363FF8, 0x363BF8);
    const int32_t active = (int32_t)reg(2);
    sw(kParticleState, (uint32_t)active);
    if (!active && !diff_in_original_side()) native::ee_particle_end(false);

    set(2, active);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x50));
}

// 0x3639D0 ParticleAdd(int32_t col_rgb, int32_t col_alpha, float size, float x, float y, float z, float angle)
void particle_add() {
    const int32_t col_rgb = (int32_t)reg(4);
    const int32_t col_alpha = (int32_t)reg(5);
    const uint32_t size_u = g_ee.fpr[12].u;
    const uint32_t x_u = g_ee.fpr[13].u;
    const uint32_t y_u = g_ee.fpr[14].u;
    const uint32_t z_u = g_ee.fpr[15].u;
    const uint32_t angle_u = g_ee.fpr[16].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x70;
    set(29, (int32_t)sp);

    int32_t cnt = lw(kParticleState + 4);
    const int32_t cap = lw(kParticleState + 8);
    if (cnt == cap) {
        set(4, (int32_t)kParticleState);
        call(f_00364068, 0x363A30);
        set(4, (int32_t)kParticleState);
        call(f_00363FF8, 0x363A38);
        if (reg(2) == 0) {
            set(2, 0);
            set64(31, saved_ra);
            set(29, (int32_t)(sp + 0x70));
            return;
        }
        cnt = lw(kParticleState + 4);
    }

    // Exact PS2 angle -> table index (0..78) & fractional sub-step (0..15)
    float f = fmul_ps2(fbits(angle_u), fbits(0x3E22F983u));  // 1 / (2*pi)
    f = fmul_ps2(f, fbits(0x429C0000u));                     // 78.0f
    const int32_t idx = fcvtw_ps2(f);
    const float rem = fsub_ps2(f, (float)idx);
    const int32_t frac = fcvtw_ps2(fmul_ps2(rem, fbits(0x41800000u)));  // 16.0f

    const uint32_t w0 = (x_u & 0xFFFFFF00u) | (uint32_t)frac;
    const uint32_t w1 = (y_u & 0xFFFFFF00u) | (uint32_t)idx;
    const uint32_t w2 = (z_u & 0xFFFFFF00u) | (uint32_t)(col_rgb >> 1);
    const uint32_t w3 = (size_u & 0xFFFFFF00u) | (uint32_t)(col_alpha >> 1);

    const uint32_t dst = lwu(kParticleState + 24) + ((uint32_t)cnt << 4);
    sw(dst + 0, w0);
    sw(dst + 4, w1);
    sw(dst + 8, w2);
    sw(dst + 12, w3);
    sw(kParticleState + 4, (uint32_t)(cnt + 1));

    if (!diff_in_original_side()) {
        native::ee_particle_add(col_rgb, col_alpha, fbits(size_u), fbits(x_u), fbits(y_u), fbits(z_u), fbits(angle_u));
    }

    set(2, 1);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x70));
}

// 0x363B68 ParticleEnd()
void particle_end() {
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    set(4, (int32_t)kParticleState);
    call(f_00364068, 0x363B80);
    sw(kParticleState, 0);

    if (!diff_in_original_side()) native::ee_particle_end(true);

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// ---- Simple effect sprites (0x4D8760, VU1 0x01B8) ---------------------------------------------

// 0x364548 SpritePacketReserve(state*) -> int ok
void sprite_packet_reserve() {
    const uint32_t state = reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    sw(state + 4, 0);
    const int32_t rem = lw(state + 12);
    const int32_t cap = (rem > 32) ? 33 : rem;
    sw(state + 12, (uint32_t)(rem - cap));
    sw(state + 8, (uint32_t)cap);

    set(4, cap + 6);
    call(f_00366D30, 0x364584);
    const uint32_t pkt = reg(2);
    sw(state + 16, pkt);
    int32_t ok = 0;
    if (pkt != 0) {
        sw(state + 20, pkt + 0x50u);
        ok = 1;
    }

    set(2, ok);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// 0x363F48 SpriteBegin(int32_t count) -> int active
void sprite_begin() {
    const int32_t count = (int32_t)reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x40;
    set(29, (int32_t)sp);

    call(f_0035CA98, 0x363F6C);
    call(f_003620A8, 0x363F74);
    sw(kSpriteState + 12, (uint32_t)count);

    if (!diff_in_original_side()) native::ee_sprite_begin(0);

    set(4, (int32_t)kSpriteState);
    call(f_00364548, 0x363F80);
    const int32_t active = (int32_t)reg(2);
    sw(kSpriteState, (uint32_t)active);
    if (!active && !diff_in_original_side()) native::ee_sprite_end(false);

    set(2, active);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x40));
}

// 0x363DE8 SpriteAdd(r128 col, int32_t flip, float size, float x, float y, float z)
void sprite_add() {
    const uint32_t col[4] = {g_ee.r[4].uw[0], g_ee.r[4].uw[1], g_ee.r[4].uw[2], g_ee.r[4].uw[3]};
    const int32_t flip = (int32_t)reg(5);
    const uint32_t size_u = g_ee.fpr[12].u;
    const uint32_t x_u = g_ee.fpr[13].u;
    const uint32_t y_u = g_ee.fpr[14].u;
    const uint32_t z_u = g_ee.fpr[15].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x60;
    set(29, (int32_t)sp);

    int32_t cnt = lw(kSpriteState + 4);
    const int32_t cap = lw(kSpriteState + 8);
    if (cnt == cap) {
        set(4, (int32_t)kSpriteState);
        call(f_003645B8, 0x363E40);
        set(4, (int32_t)kSpriteState);
        call(f_00364548, 0x363E48);
        if (reg(2) == 0) {
            set(2, 0);
            set64(31, saved_ra);
            set(29, (int32_t)(sp + 0x60));
            return;
        }
        cnt = lw(kSpriteState + 4);
    }

    const uint32_t w0 = (x_u & 0xFFFFFF00u) | col[0] | (uint32_t)(flip & 1);
    const uint32_t w1 = (y_u & 0xFFFFFF00u) | col[1] | (uint32_t)((flip & 2) >> 1);
    const uint32_t w2 = (z_u & 0xFFFFFF00u) | col[2];
    const uint32_t w3 = (size_u & 0xFFFFFF00u) | col[3];

    const uint32_t dst = lwu(kSpriteState + 20) + ((uint32_t)cnt << 4);
    sw(dst + 0, w0);
    sw(dst + 4, w1);
    sw(dst + 8, w2);
    sw(dst + 12, w3);
    sw(kSpriteState + 4, (uint32_t)(cnt + 1));

    if (!diff_in_original_side()) {
        native::ee_sprite_add(col, flip, fbits(size_u), fbits(x_u), fbits(y_u), fbits(z_u));
    }

    // Match 0x363DE8's 128-bit r2 (upper 64 bits retain {w2, w3} from MEM_SQ(&R[2]))
    g_ee.r[2].uw[2] = w2;
    g_ee.r[2].uw[3] = w3;
    set(2, 1);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x60));
}

// 0x363F18 SpriteEnd()
void sprite_end() {
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    set(4, (int32_t)kSpriteState);
    call(f_003645B8, 0x363F30);
    sw(kSpriteState, 0);

    if (!diff_in_original_side()) native::ee_sprite_end(true);

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// ---- Atlas/grid effect sprites (0x4D8778, VU1 0x05E0) -----------------------------------------

// 0x3642B8 AtlasSpritePacketReserve(state*) -> int ok  (RA 0x3642F4 to 0x366D30)
void atlas_sprite_packet_reserve() {
    const uint32_t state = reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    sw(state + 4, 0);
    const int32_t rem = lw(state + 12);
    const int32_t cap = (rem > 32) ? 33 : rem;
    sw(state + 12, (uint32_t)(rem - cap));
    sw(state + 8, (uint32_t)cap);

    set(4, cap + 5);
    call(f_00366D30, 0x3642F4);
    const uint32_t pkt = reg(2);
    sw(state + 24, pkt);
    int32_t ok = 0;
    if (pkt != 0) {
        sw(state + 28, pkt + 0x40u);
        ok = 1;
    }

    set(2, ok);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// 0x363D78 AtlasSpriteBegin(int32_t count, int32_t gridSize) -> int active
void atlas_sprite_begin() {
    const int32_t count = (int32_t)reg(4);
    const int32_t grid_size = (int32_t)reg(5);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x50;
    set(29, (int32_t)sp);

    call(f_0035CA98, 0x363DA4);
    call(f_003620A8, 0x363DAC);

    set(4, (int32_t)kAtlasState);
    set(5, grid_size);
    call(f_00364500, 0x363DB8);
    sw(kAtlasState + 12, (uint32_t)count);

    if (!diff_in_original_side()) native::ee_sprite_begin(grid_size);

    set(4, (int32_t)kAtlasState);
    call(f_003642B8, 0x363DC4);
    const int32_t active = (int32_t)reg(2);
    sw(kAtlasState, (uint32_t)active);
    if (!active && !diff_in_original_side()) native::ee_sprite_end(false);

    set(2, active);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x50));
}

// 0x363C18 AtlasSpriteAdd(r128 col, int32_t tile, float size, float x, float y, float z)
void atlas_sprite_add() {
    const uint32_t col[4] = {g_ee.r[4].uw[0], g_ee.r[4].uw[1], g_ee.r[4].uw[2], g_ee.r[4].uw[3]};
    const int32_t tile = (int32_t)reg(5);
    const uint32_t size_u = g_ee.fpr[12].u;
    const uint32_t x_u = g_ee.fpr[13].u;
    const uint32_t y_u = g_ee.fpr[14].u;
    const uint32_t z_u = g_ee.fpr[15].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x60;
    set(29, (int32_t)sp);

    int32_t cnt = lw(kAtlasState + 4);
    const int32_t cap = lw(kAtlasState + 8);
    if (cnt == cap) {
        set(4, (int32_t)kAtlasState);
        call(f_00364328, 0x363C70);
        set(4, (int32_t)kAtlasState);
        call(f_003642B8, 0x363C78);
        if (reg(2) == 0) {
            set(2, 0);
            set64(31, saved_ra);
            set(29, (int32_t)(sp + 0x60));
            return;
        }
        cnt = lw(kAtlasState + 4);
    }

    const int32_t grid_shift = lw(kAtlasState + 16);
    const int32_t grid_mask = lw(kAtlasState + 20);
    const uint32_t tx = (uint32_t)(tile & grid_mask);
    const uint32_t ty = (uint32_t)(tile >> (grid_shift & 31));

    const uint32_t w0 = (x_u & 0xFFFFFF00u) | col[0] | tx;
    const uint32_t w1 = (y_u & 0xFFFFFF00u) | col[1];
    const uint32_t w2 = (z_u & 0xFFFFFF00u) | col[2] | ty;
    const uint32_t w3 = (size_u & 0xFFFFFF00u) | col[3];

    const uint32_t dst = lwu(kAtlasState + 28) + ((uint32_t)cnt << 4);
    sw(dst + 0, w0);
    sw(dst + 4, w1);
    sw(dst + 8, w2);
    sw(dst + 12, w3);
    sw(kAtlasState + 4, (uint32_t)(cnt + 1));

    if (!diff_in_original_side()) {
        native::ee_atlas_sprite_add(col, tile, grid_shift, grid_mask,
                                    fbits(size_u), fbits(x_u), fbits(y_u), fbits(z_u));
    }

    // Match 0x363C18's 128-bit r2 (upper 64 bits retain {w2, w3} from MEM_SQ(&R[2]))
    g_ee.r[2].uw[2] = w2;
    g_ee.r[2].uw[3] = w3;
    set(2, 1);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x60));
}

// 0x363D48 AtlasSpriteEnd()
void atlas_sprite_end() {
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    set(4, (int32_t)kAtlasState);
    call(f_00364328, 0x363D60);
    sw(kAtlasState, 0);

    if (!diff_in_original_side()) native::ee_sprite_end(true);

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// ---- Immediate geometry (0x3E5848, VU1 0x0270) ------------------------------------------------

// Shared helper to build an immediate VIF chunk packet (exact match to 0x361468 / 0x361708 / 0x364D18).
int32_t imm_alloc_chunk(uint32_t state, int32_t cap, bool emit_stmask, uint32_t reserve_ra) {
    const int32_t words = cap * 7 + (emit_stmask ? 7 : 0) + 18;
    const int32_t qwc = (words >= 0 ? words : (words + 3)) >> 2;
    set(4, qwc);
    call(f_00366D30, reserve_ra);
    const uint32_t pkt = reg(2);
    if (pkt == 0) {
        sw(state + 48, 0);
        return 0;
    }

    // DMA CNT tag + VIF header
    sw(pkt + 0, 0x10000000u | (uint32_t)((qwc - 1) & 0xFFFF));
    sw(pkt + 4, 0);
    sw(pkt + 8, 0x01000202u);   // STCYCL 2,2
    sw(pkt + 12, 0x05000000u);  // STMOD 0
    sw(pkt + 16, 0x6102C0C6u);  // UNPACK S-16, 2 -> 0xC6 (tex state)
    sw(pkt + 20, lwu(kTexStateWord));
    sw(pkt + 24, 0x6001C000u);  // UNPACK S-32, 1 -> 0x00 (GIFtag NLOOP)
    sw(pkt + 28, (uint32_t)cap | 0x8000u);
    sw(state + 52, pkt + 28);
    sw(pkt + 32, 0x01000103u);  // STCYCL 1,3
    sw(pkt + 36, 0x6E00C001u | (((uint32_t)cap & 0xFFu) << 16));  // UNPACK V4-8 -> 0x01 (colours+ADC)

    uint32_t cur = pkt + 40;
    sw(state + 44, cur);
    cur += (uint32_t)cap * 4u;

    if (emit_stmask) {
        sw(cur + 0, 0x20000000u);   // STMASK
        sw(cur + 4, 0x40404040u);
        sw(cur + 8, 0x30000000u);   // STROW
        sw(cur + 12, 0);
        sw(cur + 16, 0);
        sw(cur + 20, 0);
        sw(cur + 24, 0x3F800000u);  // 1.0f
        cur += 28;
    }

    sw(cur + 0, 0x01000203u);  // STCYCL 2,3
    sw(cur + 4, 0x05000001u);  // STMOD 1
    const uint32_t vcnt = ((uint32_t)cap << 1) & 0xFEu;
    sw(cur + 8, 0x7800C002u | (vcnt << 16));  // UNPACK V3-32 (0x60000000 | 0x10000002 | 0x0800C000)
    cur += 12;
    sw(state + 48, cur);
    cur += (uint32_t)cap * 24u;

    sw(cur + 0, 0x04000200u);  // ITOP 0x200
    sw(cur + 4, 0x1400004Eu);  // MSCAL 0x4E (0x0270 >> 3)
    cur += 8;

    while (((cur - pkt) & 15u) != 0) {
        sw(cur, 0);
        cur += 4;
    }
    sw(state + 56, (cur - pkt) >> 4);
    return 1;
}

// 0x361468 ImmediatePacketBegin(int32_t numVerts) -> int ok
void immediate_packet_begin() {
    const int32_t num_verts = (int32_t)reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x40;
    set(29, (int32_t)sp);

    const int32_t cap = (num_verts > 60) ? 60 : num_verts;
    sw(kImmState + 28, (uint32_t)num_verts);
    sw(kImmState + 32, (uint32_t)num_verts);
    sw(kImmState + 40, (uint32_t)cap);
    sw(kImmState + 36, 0);

    const int32_t ok = imm_alloc_chunk(kImmState, cap, true, 0x3614CC);
    sw(kImmState + 24, (uint32_t)ok);
    sw(kImmState + 32, (uint32_t)(lw(kImmState + 32) - lw(kImmState + 40)));

    set(2, ok);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x40));
}

// 0x35DF10 ImmediateBegin(int32_t numVerts) -> int ok
void immediate_begin() {
    const int32_t num_verts = (int32_t)reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    call(f_0035BA10, 0x35DF24);

    // Start general immediate / cockpit stream if not already inside water or flare
    const bool track_imm = !diff_in_original_side() && !native::ee_in_water() && !native::ee_in_flare();
    const bool hud = g_hud_scope || (uint32_t)saved_ra == 0x29A368u;
    if (track_imm) native::ee_imm_begin(g_cockpit_quad_scope, hud);

    set(4, num_verts);
    call(f_00361468, 0x35DF2C);
    const int32_t ok = (int32_t)reg(2);
    if (!ok && track_imm) native::ee_imm_end(false);

    set(2, ok);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// Shared vertex write body for 0x361708 (ImmediateVertex) and 0x364D18 (ImmediateVertexContinue)
int32_t imm_vertex_body(uint32_t state, uint32_t colour, uint32_t strip,
                        uint32_t x_u, uint32_t y_u, uint32_t z_u, uint32_t u_u, uint32_t v_u,
                        uint32_t commit_ra, uint32_t reserve_ra, uint32_t cont1_ra, uint32_t cont2_ra,
                        bool feed_native, uint32_t producer_ra = 0) {
    if (lwu(state + 48) == 0) return 1;

    int32_t failed = 0;
    if (lw(state + 36) == lw(state + 40)) {
        set(4, lw(state + 56));
        call(f_00366CD0, commit_ra);
        const int32_t rem = lw(state + 32);
        const int32_t total = lw(state + 28);
        const bool is_first = (rem == total);
        const int32_t cap = (rem > 60) ? 60 : rem;
        sw(state + 36, 0);
        sw(state + 40, (uint32_t)cap);
        const int32_t ok = imm_alloc_chunk(state, cap, is_first, reserve_ra);
        failed = ok ^ 1;
        sw(state + 32, (uint32_t)(lw(state + 32) - lw(state + 40)));
    }
    if (failed) return failed;

    const int32_t idx = lw(state + 36);
    sw(state + 36, (uint32_t)(idx + 1));

    const uint32_t adc = (strip == 0 || idx < 2) ? 0x80000000u : 0u;
    uint32_t c = (colour >> 1) & 0x7F7F7F7Fu;
    c += (c & 0x40404040u) >> 6;
    const uint32_t r = (c >> 16) & 0xFFu;
    const uint32_t g = c & 0xFF00u;
    const uint32_t b = (c << 16) & 0xFF0000u;
    const uint32_t a = (c >> 24) & 0xFFu;
    const uint32_t packed_col = adc | b | g | r;

    sw(lwu(state + 44) + ((uint32_t)idx << 2), packed_col);
    const uint32_t vdst = lwu(state + 48) + (uint32_t)idx * 24u;
    sw(vdst + 0, x_u);
    sw(vdst + 4, y_u);
    sw(vdst + 8, z_u);
    sw(vdst + 12, u_u);
    sw(vdst + 16, v_u);
    sw(vdst + 20, a);

    if (feed_native && !diff_in_original_side()) {
        const float xyz[3] = {fbits(x_u), fbits(y_u), fbits(z_u)};
        const float st[2] = {fbits(u_u), fbits(v_u)};
        if (native::ee_in_water()) native::ee_water_vertex(xyz, st, colour, strip);
        else if (native::ee_in_flare()) native::ee_flare_vertex(xyz, st, colour, strip);
        else if (native::ee_in_imm()) {
            float age = -1.0f;
            switch (producer_ra) {
            // Both four-vertex paths in the long/short spark emitter 0x2FA5F8.
            // f31 is the game time and s0 points to its active 32-byte spark record.
            case 0x2FAC18: case 0x2FAC40: case 0x2FAC68: case 0x2FAC90:
            case 0x2FAEA8: case 0x2FAED0: case 0x2FAEF8: case 0x2FAF20:
                age = g_ee.fpr[31].f - rf(reg(16) + 24);
                break;
            }
            native::ee_imm_vertex(xyz, st, colour, strip, age);
        }
    }

    int32_t ret = 0;
    if (lw(state + 36) == lw(state + 40)) {
        const int32_t rem = lw(state + 32);
        if (rem > 0) {
            sw(state + 32, (uint32_t)(rem + 2));
            // Repeat previous vertex, then current vertex, into next chunk via 0x364D18
            const uint32_t prev_v = lwu(state + 16);
            set(4, (int32_t)state);
            set(5, lw(state + 20));
            set(6, (int32_t)(prev_v & 1u));
            set_f(12, lwu(state + 0));
            set_f(13, lwu(state + 4));
            set_f(14, lwu(state + 8));
            set_f(15, lwu(state + 12));
            set_f(16, prev_v);
            call(f_00364D18, cont1_ra);

            set(4, (int32_t)state);
            set(5, (int32_t)colour);
            set(6, (int32_t)strip);
            set_f(12, x_u);
            set_f(13, y_u);
            set_f(14, z_u);
            set_f(15, u_u);
            set_f(16, v_u);
            call(f_00364D18, cont2_ra);
            ret = (int32_t)reg(2);
        }
    }

    // Save current vertex for potential next-chunk strip continuation
    sw(state + 0, x_u);
    sw(state + 4, y_u);
    sw(state + 8, z_u);
    sw(state + 12, u_u);
    sw(state + 16, (v_u & 0xFFFFFFFEu) | strip);
    sw(state + 20, colour);
    return ret;
}

// 0x361708 ImmediateVertex(a0 colour, a1 strip, f12..f16 xyz uv) -> 0 ok, 1 fail
void immediate_vertex() {
    const uint32_t colour = reg(4);
    const uint32_t strip = reg(5);
    const uint32_t x_u = g_ee.fpr[12].u;
    const uint32_t y_u = g_ee.fpr[13].u;
    const uint32_t z_u = g_ee.fpr[14].u;
    const uint32_t u_u = g_ee.fpr[15].u;
    const uint32_t v_u = g_ee.fpr[16].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0xA0;
    set(29, (int32_t)sp);

    const int32_t ret = imm_vertex_body(kImmState, colour, strip, x_u, y_u, z_u, u_u, v_u,
                                        0x361784, 0x3617E4, 0x361B34, 0x361B58, true, (uint32_t)saved_ra);

    set(2, ret);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0xA0));
}

// 0x364D18 ImmediateVertexContinue(a0 state*, a1 colour, a2 strip, f12..f16 xyz uv) -> 0 ok, 1 fail
void immediate_vertex_continue() {
    const uint32_t state = reg(4);
    const uint32_t colour = reg(5);
    const uint32_t strip = reg(6);
    const uint32_t x_u = g_ee.fpr[12].u;
    const uint32_t y_u = g_ee.fpr[13].u;
    const uint32_t z_u = g_ee.fpr[14].u;
    const uint32_t u_u = g_ee.fpr[15].u;
    const uint32_t v_u = g_ee.fpr[16].u;
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0xA0;
    set(29, (int32_t)sp);

    const int32_t ret = imm_vertex_body(state, colour, strip, x_u, y_u, z_u, u_u, v_u,
                                        0x364D90, 0x364DF0, 0x365144, 0x365168, false);

    set(2, ret);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0xA0));
}

// 0x361BC8 ImmediateEnd()
void immediate_end() {
    set(4, (int32_t)kImmState);
    f_003651D8();
    if (!diff_in_original_side() && native::ee_in_imm()) native::ee_imm_end(true);
}

// ---- Sphere-tested pre-built immediate geometry (0x34EE70, 0x34EEB8, 0x361ED8, 0x361C30) ------

// 0x361C30 ImmStaticEmitRef(bufObj*)
void imm_static_emit_ref() {
    const uint32_t obj = reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    set(4, 1);
    call(f_00366D30, 0x361C48);
    const uint32_t pkt = reg(2);
    if (pkt != 0) {
        const uint32_t qwc = (uint32_t)(int32_t)lh(obj + 16);
        sw(pkt + 0, 0x30000000u | qwc);
        sw(pkt + 4, lwu(obj + 0));
        sw(pkt + 8, 0x04000200u);  // ITOP 0x200
        sw(pkt + 12, 0);

        if (!diff_in_original_side()) native::ee_sphere_imm(obj, pkt);

        set(4, 1);
        set64(31, saved_ra);
        set(29, (int32_t)(sp + 0x20));
        f_00366CD0();
        return;
    }

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// 0x361ED8 ImmStaticDraw(bufObj*)
void imm_static_draw() {
    const uint32_t obj = reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    call(f_003620A8, 0x361EEC);

    set(4, (int32_t)obj);
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
    f_00361C30();
}

// 0x34EE70 SphereImmSubmit(obj*)
void sphere_imm_submit() {
    const uint32_t obj = reg(4);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x20;
    set(29, (int32_t)sp);

    set(4, (int32_t)(obj + 4));
    set(5, 0);
    set(6, 0);
    set_f(12, lwu(obj + 16));
    call(f_0034E308, 0x34EE94);

    if (reg(2) != 0) {
        set(4, lw(obj));
        set64(31, saved_ra);
        set(29, (int32_t)(sp + 0x20));
        f_00361ED8();
        return;
    }

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x20));
}

// 0x34EEB8 SphereImmSubmitTransformed(obj*, mat*)
void sphere_imm_submit_transformed() {
    const uint32_t obj = reg(4);
    const uint32_t mat = reg(5);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x30;
    set(29, (int32_t)sp);

    const float vx = rf(obj + 4);
    const float vy = rf(obj + 8);
    const float vz = rf(obj + 12);

    // Exact PS2 FACC evaluation order (gen/rec_098.c:3180-3225)
    float acc = fmul_ps2(vx, rf(mat + 0));
    acc = fadd_ps2(acc, fmul_ps2(vy, rf(mat + 12)));
    const float rx = fadd_ps2(acc, fmul_ps2(vz, rf(mat + 24)));

    acc = fmul_ps2(vx, rf(mat + 4));
    acc = fadd_ps2(acc, fmul_ps2(vy, rf(mat + 16)));
    const float ry = fadd_ps2(acc, fmul_ps2(vz, rf(mat + 28)));

    acc = fmul_ps2(vx, rf(mat + 8));
    acc = fadd_ps2(acc, fmul_ps2(vy, rf(mat + 20)));
    const float rz = fadd_ps2(acc, fmul_ps2(vz, rf(mat + 32)));

    sw(sp + 0, fu(fadd_ps2(rx, rf(mat + 36))));
    sw(sp + 4, fu(fadd_ps2(ry, rf(mat + 40))));
    sw(sp + 8, fu(fadd_ps2(rz, rf(mat + 44))));

    set(4, (int32_t)sp);
    set(5, 0);
    set(6, 0);
    set_f(12, lwu(obj + 16));
    call(f_0034E308, 0x34EFF0);

    if (reg(2) != 0) {
        set(4, lw(obj));
        call(f_00361ED8, 0x34F000);
    }

    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x30));
}

// ---- Indexed immediate triangles & cockpit quad (0x35CAA0 <- 0x34DB70 <- 0x219C48) ------------

// 0x35CAA0 ImmediateDrawIndexedTris(verts*, numVerts, indices*, numIndices)
void immediate_draw_indexed_tris() {
    const uint32_t verts = reg(4);
    const uint32_t indices = reg(6);
    const int32_t num_indices = (int32_t)reg(7);
    const uint64_t saved_ra = reg64(31);
    const uint32_t sp = reg(29) - 0x70;
    set(29, (int32_t)sp);

    const bool prev_cockpit = g_cockpit_quad_scope, prev_hud = g_hud_scope;
    if ((uint32_t)saved_ra == 0x21A064u) g_cockpit_quad_scope = true;
    if ((uint32_t)saved_ra == 0x11E834u || (uint32_t)saved_ra == 0x29E488u) g_hud_scope = true;

    set(4, num_indices);
    call(f_0035DF10, 0x35CAD4);
    if (reg(2) != 0) {
        for (int32_t i = 0; i < num_indices; i++) {
            const int32_t vi = (int32_t)lh(indices + (uint32_t)i * 2u);
            const uint32_t v = verts + ((uint32_t)vi << 5);
            set(4, lw(v + 16));
            set(5, (i % 3 == 2) ? 1 : 0);
            set_f(12, lwu(v + 0));
            set_f(13, lwu(v + 4));
            set_f(14, lwu(v + 8));
            set_f(15, lwu(v + 24));
            set_f(16, lwu(v + 28));
            call(f_00361708, 0x35CB38);
            if (reg(2) == 1) break;
        }
        g_cockpit_quad_scope = prev_cockpit;
        g_hud_scope = prev_hud;
        set64(31, saved_ra);
        set(29, (int32_t)(sp + 0x70));
        f_00361BC8();
        return;
    }

    g_cockpit_quad_scope = prev_cockpit;
        g_hud_scope = prev_hud;
    set64(31, saved_ra);
    set(29, (int32_t)(sp + 0x70));
}

}  // namespace

extern "C" void f_00363FF8(void) { diff_run(0x00363FF8, particle_packet_reserve, f_00363FF8_orig); }
extern "C" void f_00363B98(void) { diff_run(0x00363B98, particle_begin, f_00363B98_orig); }
extern "C" void f_003639D0(void) { diff_run(0x003639D0, particle_add, f_003639D0_orig); }
extern "C" void f_00363B68(void) { diff_run(0x00363B68, particle_end, f_00363B68_orig); }

extern "C" void f_00364548(void) { diff_run(0x00364548, sprite_packet_reserve, f_00364548_orig); }
extern "C" void f_00363F48(void) { diff_run(0x00363F48, sprite_begin, f_00363F48_orig); }
extern "C" void f_00363DE8(void) { diff_run(0x00363DE8, sprite_add, f_00363DE8_orig); }
extern "C" void f_00363F18(void) { diff_run(0x00363F18, sprite_end, f_00363F18_orig); }

extern "C" void f_003642B8(void) { diff_run(0x003642B8, atlas_sprite_packet_reserve, f_003642B8_orig); }
extern "C" void f_00363D78(void) { diff_run(0x00363D78, atlas_sprite_begin, f_00363D78_orig); }
extern "C" void f_00363C18(void) { diff_run(0x00363C18, atlas_sprite_add, f_00363C18_orig); }
extern "C" void f_00363D48(void) { diff_run(0x00363D48, atlas_sprite_end, f_00363D48_orig); }

extern "C" void f_0035DF10(void) { diff_run(0x0035DF10, immediate_begin, f_0035DF10_orig); }
extern "C" void f_00361468(void) { diff_run(0x00361468, immediate_packet_begin, f_00361468_orig); }
extern "C" void f_00361708(void) { diff_run(0x00361708, immediate_vertex, f_00361708_orig); }
extern "C" void f_00361BC8(void) { diff_run(0x00361BC8, immediate_end, f_00361BC8_orig); }
extern "C" void f_00364D18(void) { diff_run(0x00364D18, immediate_vertex_continue, f_00364D18_orig); }

extern "C" void f_0034EE70(void) { diff_run(0x0034EE70, sphere_imm_submit, f_0034EE70_orig); }
extern "C" void f_0034EEB8(void) { diff_run(0x0034EEB8, sphere_imm_submit_transformed, f_0034EEB8_orig); }
extern "C" void f_00361ED8(void) { diff_run(0x00361ED8, imm_static_draw, f_00361ED8_orig); }
extern "C" void f_00361C30(void) { diff_run(0x00361C30, imm_static_emit_ref, f_00361C30_orig); }

extern "C" void f_0035CAA0(void) { diff_run(0x0035CAA0, immediate_draw_indexed_tris, f_0035CAA0_orig); }
