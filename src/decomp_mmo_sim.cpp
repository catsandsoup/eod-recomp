// decomp_mmo_sim.cpp - Behaviorally exact C++ decompilation of SLUS_209.10's
// authoritative simulation core inside WorldStepTick (0x00177718):
//
//   0x001779C0  EntityMatrixOrthonormalizeRoundRobin (98 lines, 0 direct callees)
//               Round-robin Gram-Schmidt orthonormalization of one active
//               dynamic entity's 3x3 orientation matrix (+0x64) per tick.
//   0x00177B40  SimSnapshotHistoryStep (84 lines, 1 callee: memset 0x003A5FD0)
//               Slides the 8-slot x 0x108-byte simulation snapshot ring at
//               0x0044A4C0 when the oldest entry exceeds 5.0s or time rewinds.
//   0x00177CA0  DynamicEntitiesPhysicsStep (362 lines, 1 direct callee: 0x001D57A8)
//               Main dynamic entity collision broadphase & static quadtree pass
//               over g_ActiveDynamicEntities (0x003CDBE8): collider pre-step,
//               dynamic-vs-dynamic spatial hash cells, and dual static track
//               quadtrees (0x004473FC / 0x00447400).
//   0x00178258  DynamicEntitiesPostStep (74 lines, 4 callees)
//               Post-physics integration, snapshot capture (0x001BF3F8) vs.
//               replay playback (0x001BEF40), and final transform sync.
//   0x001792E8  TrackRacePreStep (50 lines, 3 callees)
//               Applies pending pause/unpause requests (0x003CDAC8 -> 0x003CDAC4)
//               and mutes/pauses 5 channels via 0x00333398 on pause entry.
//   0x001793B8  TrackRaceMidStep (73 lines, 1 callee: 0x00189E80)
//               Drains the 20-slot deferred event/collision queue at 0x00447458
//               when 0x0044743C is non-zero.
//
// Every replacement mirrors exact PS2 EE FPU ops (fmul_ps2, fadd_ps2, fsub_ps2,
// frsqrt_ps2, fcvtw_ps2), guest call order, return addresses ($ra), stack frames,
// and $v0 exit values so EOD_DIFF proves 0 mismatches against the recompiled
// originals.
//
// Full ledger, struct maps & STAR handoff: docs/DECOMP_MMO_SIM.md.
#include "decomp_mmo_sim.h"

#include "core/diff_harness.h"
#include "ee.h"

#include <cstdint>

extern "C" {
// ---- Recompiled originals (config/hle.txt `wrap` entries) ----
void f_001779C0_orig(void);
void f_00177B40_orig(void);
void f_00177CA0_orig(void);
void f_00178258_orig(void);
void f_001792E8_orig(void);
void f_001793B8_orig(void);

// ---- Guest callees (invoked via public symbols so hooks/traces/nested diffs apply) ----
// SimSnapshotHistoryStep (0x00177B40):
void f_003A5FD0(void);  // memset(dst, val, size)

// DynamicEntitiesPhysicsStep (0x00177CA0):
void f_001D57A8(void);  // BroadphaseQueryCells(out_cells[4], &out_count, &pos_vec3)

// DynamicEntitiesPostStep (0x00178258):
void f_001BFA30(void);  // IsReplayOrOverrideActive()
void f_001ADDD0(void);  // PostPhysicsContactFlush()
void f_001BF3F8(void);  // SnapshotCaptureEntities(entity_array, entity_count)
void f_001BEF40(void);  // ReplayApplyEntities(entity_array, entity_count)

// TrackRacePreStep (0x001792E8):
void f_00203238(void);  // IsReplayPlaybackActive()
void f_001918D0(void);  // IsReplayAtEndLocked()
void f_00333398(void);  // PauseChannel(channel_idx)

// TrackRaceMidStep (0x001793B8):
void f_00189E80(void);  // ProcessDeferredEventSlot(slot_ptr)
}

namespace {

// ---- Guest register & memory helpers ----
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline uint64_t reg64(int r) { return g_ee.r[r].ud[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline void set64(int r, uint64_t v) { g_ee.r[r].ud[0] = v; }
inline int8_t lb(uint32_t a) { return static_cast<int8_t>(MEM_LB(a)); }
inline int16_t lh(uint32_t a) { return static_cast<int16_t>(MEM_LH(a)); }
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

// ---- Verified Guest Addresses & Struct Offsets ----

// Shared simulation globals
constexpr uint32_t kGlobalWorldSimPaused       = 0x003CDAC4u;  // int32 nonzero when simulation paused
constexpr uint32_t kGlobalPauseRequest         = 0x003CDAC8u;  // int32 0=none, 1=pause, 2=unpause
constexpr uint32_t kGlobalSimTimeSeconds       = 0x003CDACCu;  // float current simulation clock (seconds)
constexpr uint32_t kGlobalActiveDynCount       = 0x003CDAE4u;  // int32 active dynamic entity count
constexpr uint32_t kGlobalActiveDynEntities    = 0x003CDBE8u;  // uint32[] active dynamic entity pointers

constexpr uint32_t kGlobalStaticQuadtreeMain   = 0x004473FCu;  // StaticQuadtree* primary track static collision
constexpr uint32_t kGlobalStaticQuadtreeAux    = 0x00447400u;  // StaticQuadtree* optional secondary collision
constexpr uint32_t kGlobalWorldEntityArrayPtr  = 0x00447404u;  // uint32* world entity pointer array
constexpr uint32_t kGlobalWorldEntityCount     = 0x00447408u;  // int32 total registered world entities
constexpr uint32_t kGlobalOrthonormalizeCursor = 0x00447428u;  // int32 round-robin index for matrix ortho
constexpr uint32_t kGlobalDeferredEventPending = 0x0044743Cu;  // int32 non-zero if deferred queue active
constexpr uint32_t kGlobalDeferredEventSlots   = 0x00447458u;  // uint32[20] deferred event pointers
constexpr uint32_t kGlobalSimSnapshotHistory   = 0x0044A4C0u;  // 8 x 0x108-byte snapshot history ring
constexpr uint32_t kGlobalSimSnapshotTailSlot  = 0x0044ABF8u;  // slot 7 base (0x0044A4C0 + 7 * 0x108)

// Snapshot history layout
constexpr uint32_t kSimSnapshotStride          = 0x108u;       // 264 bytes per history entry
constexpr uint32_t kSimSnapshotTimeOff         = 0x100u;       // float timestamp inside entry
constexpr int32_t  kSimSnapshotSlotCount       = 8;

// Entity & collider struct offsets
constexpr uint32_t kEntityCollidersArrayOff    = 0x10u;        // uint32[] inline collider pointers
constexpr uint32_t kEntityColliderCountOff     = 0x20u;        // int32 active collider count
constexpr uint32_t kEntityOrientMatrixOff      = 0x64u;        // float[9] 3x3 orientation matrix (rows X,Y,Z)
constexpr uint32_t kEntityWorldPosOff          = 0x88u;        // float[3] world position (x=+0, y=+4, z=+8)
constexpr uint32_t kEntityVtablePtrOff         = 0x98u;        // entity physics/sim vtable pointer

constexpr uint32_t kEntityVtPostPhysicsOff     = 0x1Cu;        // vtable+0x1C: PostPhysicsUpdate(ent)
constexpr uint32_t kEntityVtPostFinalizeOff    = 0x20u;        // vtable+0x20: PostStepFinalize(ent)
constexpr uint32_t kEntityVtIsDynamicOff       = 0x3Cu;        // vtable+0x3C: bool IsActiveDynamic(ent)
constexpr uint32_t kEntityVtPreTickOff         = 0x40u;        // vtable+0x40: bool IsDynamicPairFilter(ent)

constexpr uint32_t kColliderVtablePtrOff       = 0x1Cu;        // collider vtable pointer
constexpr uint32_t kColliderVtPreStepOff       = 0x0Cu;        // vtable+0x0C: Collider::PreStep(col)
constexpr uint32_t kColliderVtCollideWithOff   = 0x14u;        // vtable+0x14: Collider::CollideWith(col, other)

// Static quadtree struct offsets
constexpr uint32_t kQuadtreePropsBaseOff       = 0x08u;        // array of 0x108-byte static collision objects
constexpr uint32_t kQuadtreeIndexTableOff      = 0x0Cu;        // int16[] indirection indices into props array
constexpr uint32_t kQuadtreeNodesOff           = 0x10u;        // 8-byte nodes: int16 start, end, child_base
constexpr uint32_t kQuadtreePropStride         = 0x108u;       // 264 bytes per static prop entry
constexpr uint32_t kQuadtreePropColliderOff    = 0x3Cu;        // collider pointer inside static prop entry
constexpr int32_t  kQuadtreeRootMinCoord       = -40000000;    // 0xFD9DA600 (decimeters)
constexpr int32_t  kQuadtreeRootMaxCoord       =  40000000;    // 0x02625A00 (decimeters)

// EE float constants
constexpr uint32_t kFltOne                     = 0x3F800000u;  // 1.0f
constexpr uint32_t kFltFive                    = 0x40A00000u;  // 5.0f
constexpr uint32_t kFltTen                     = 0x41200000u;  // 10.0f

// ----------------------------------------------------------------------------
// 0x001779C0  EntityMatrixOrthonormalizeRoundRobin
// ----------------------------------------------------------------------------
// Advances a round-robin cursor across the world entity table, orthonormalizing
// one active dynamic entity's 3x3 orientation matrix (+0x64) per tick using
// exact EE FPU ops (Gram-Schmidt via cross products and frsqrt_ps2).
void decomp_001779C0(void) {
    int32_t next_cursor = 0;
    {
        Frame frame(0x60);

        const int32_t entity_count = lw(kGlobalWorldEntityCount);
        int32_t cursor = lw(kGlobalOrthonormalizeCursor);
        if (!(cursor < entity_count)) {
            sw(kGlobalOrthonormalizeCursor, 0u);
            cursor = 0;
        }

        const uint32_t entities = lwu(kGlobalWorldEntityArrayPtr);
        const uint32_t ent = lwu(entities + static_cast<uint32_t>(cursor << 2));
        if (ent != 0u) {
            const uint32_t vt = lwu(ent + kEntityVtablePtrOff);
            const uint32_t fn = lwu(vt + kEntityVtIsDynamicOff);
            set(2, static_cast<int32_t>(fn));
            set(4, static_cast<int32_t>(ent));
            call_at(fn, 0x00177A1Cu);

            if (reg(2) != 0u) {
                const uint32_t mat = ent + kEntityOrientMatrixOff;
                const float r0x = bf(lwu(mat + 0u));
                const float r0y = bf(lwu(mat + 4u));
                const float r0z = bf(lwu(mat + 8u));
                const float r2x = bf(lwu(mat + 24u));
                const float r2y = bf(lwu(mat + 28u));
                const float r2z = bf(lwu(mat + 32u));

                const float one = bf(kFltOne);

                // inv_len0 = 1 / |row0|
                float len0_sq = fmul_ps2(r0x, r0x);
                len0_sq = fadd_ps2(len0_sq, fmul_ps2(r0y, r0y));
                len0_sq = fadd_ps2(len0_sq, fmul_ps2(r0z, r0z));
                const float inv_len0 = frsqrt_ps2(one, len0_sq);

                const float n0z = fmul_ps2(r0z, inv_len0);

                // c = row2 x row0 (using unnormalized row0 to match native order)
                const float cz = fsub_ps2(fmul_ps2(r2x, r0y), fmul_ps2(r2y, r0x));
                const float cy = fsub_ps2(fmul_ps2(r2z, r0x), fmul_ps2(r2x, r0z));
                const float cx = fsub_ps2(fmul_ps2(r2y, r0z), fmul_ps2(r2z, r0y));

                // inv_lenc = 1 / |c|
                float lenc_sq = fmul_ps2(cx, cx);
                lenc_sq = fadd_ps2(lenc_sq, fmul_ps2(cy, cy));
                lenc_sq = fadd_ps2(lenc_sq, fmul_ps2(cz, cz));

                const float n0x = fmul_ps2(r0x, inv_len0);
                const float inv_lenc = frsqrt_ps2(one, lenc_sq);
                const float n0y = fmul_ps2(r0y, inv_len0);

                // Normalized row1 = c / |c|
                const float n1x = fmul_ps2(cx, inv_lenc);
                const float n1y = fmul_ps2(cy, inv_lenc);
                const float n1z = fmul_ps2(cz, inv_lenc);

                // Orthonormal row2 = row0_n x row1_n
                const float n2z = fsub_ps2(fmul_ps2(n0x, n1y), fmul_ps2(n0y, n1x));
                const float n2y = fsub_ps2(fmul_ps2(n0z, n1x), fmul_ps2(n0x, n1z));
                const float n2x = fsub_ps2(fmul_ps2(n0y, n1z), fmul_ps2(n0z, n1y));

                sw(mat + 0u,  fb(n0x));
                sw(mat + 4u,  fb(n0y));
                sw(mat + 8u,  fb(n0z));
                sw(mat + 12u, fb(n1x));
                sw(mat + 16u, fb(n1y));
                sw(mat + 20u, fb(n1z));
                sw(mat + 24u, fb(n2x));
                sw(mat + 28u, fb(n2y));
                sw(mat + 32u, fb(n2z));
            }
        }

        next_cursor = lw(kGlobalOrthonormalizeCursor) + 1;
        sw(kGlobalOrthonormalizeCursor, static_cast<uint32_t>(next_cursor));
    }
    set(2, next_cursor);
}

// ----------------------------------------------------------------------------
// 0x00177B40  SimSnapshotHistoryStep
// ----------------------------------------------------------------------------
// Maintains an 8-entry (0x108 bytes each) simulation snapshot ring at 0x0044A4C0.
// Shifts slots [1..7] -> [0..6] and zeroes slot 7 when slot 0 is older than 5.0s,
// when simulation time rewinds, or when force_flag != 0 and slot 7 is occupied.
void decomp_00177B40(void) {
    const uint32_t force_flag = reg(4);

    if (lb(kGlobalSimSnapshotHistory) == 0) {
        set(2, 0);
        return;
    }

    const float slot0_time = bf(lwu(kGlobalSimSnapshotHistory + kSimSnapshotTimeOff));
    const float sim_time = bf(lwu(kGlobalSimTimeSeconds));
    const float expiry_time = fadd_ps2(slot0_time, bf(kFltFive));

    bool should_shift = false;
    if (fcmp_lt(expiry_time, sim_time) || fcmp_lt(sim_time, slot0_time)) {
        should_shift = true;
    } else {
        if (force_flag == 0u) {
            set(2, 0x00450000);
            return;
        }
        if (lb(kGlobalSimSnapshotTailSlot) == 0) {
            set(2, 0x00450000);
            return;
        }
        should_shift = true;
    }

    if (should_shift) {
        for (int32_t i = 0; i < kSimSnapshotSlotCount - 1; ++i) {
            const uint32_t dst = kGlobalSimSnapshotHistory + static_cast<uint32_t>(i) * kSimSnapshotStride;
            const uint32_t src = dst + kSimSnapshotStride;
            for (uint32_t off = 0u; off < kSimSnapshotStride; off += 8u) {
                MEM_SD(dst + off, MEM_LD(src + off));
            }
        }
        // Tail-call memset(0x0044ABF8, 0, 0x108) preserving caller $ra.
        set(4, static_cast<int32_t>(kGlobalSimSnapshotTailSlot));
        set(5, 0);
        set(6, static_cast<int32_t>(kSimSnapshotStride));
        f_003A5FD0();
    }
}

// ----------------------------------------------------------------------------
// Helper: Static Track Quadtree Collision Traversal (shared by Pass 2B & 2C)
// ----------------------------------------------------------------------------
void collide_entity_against_quadtree(uint32_t ent, uint32_t qt, uint32_t call_ra) {
    const float ent_x = bf(lwu(ent + kEntityWorldPosOff));
    const float ent_z = bf(lwu(ent + kEntityWorldPosOff + 8u));
    const float scale10 = bf(kFltTen);
    const int32_t qz = fcvtw_ps2(fmul_ps2(ent_z, scale10));
    const int32_t qx = fcvtw_ps2(fmul_ps2(ent_x, scale10));

    int32_t min_x = kQuadtreeRootMinCoord;
    int32_t max_x = kQuadtreeRootMaxCoord;
    int32_t min_z = kQuadtreeRootMinCoord;
    int32_t max_z = kQuadtreeRootMaxCoord;
    int32_t node_idx = 0;

    for (;;) {
        const uint32_t node_addr = lwu(qt + kQuadtreeNodesOff) + static_cast<uint32_t>(node_idx << 3);
        const int32_t idx_start = lh(node_addr + 0u);
        const int32_t idx_end   = lh(node_addr + 2u);

        if (idx_start < idx_end) {
            int32_t ent_cols = lw(ent + kEntityColliderCountOff);
            for (int32_t k = idx_start; k < idx_end; ++k) {
                const uint32_t idx_table  = lwu(qt + kQuadtreeIndexTableOff);
                const uint32_t props_base = lwu(qt + kQuadtreePropsBaseOff);
                const int32_t prop_idx    = lh(idx_table + static_cast<uint32_t>(k << 1));
                const uint32_t prop_addr  = props_base + static_cast<uint32_t>(prop_idx * static_cast<int32_t>(kQuadtreePropStride));
                const uint32_t prop_col   = lwu(prop_addr + kQuadtreePropColliderOff);

                if (ent_cols > 0) {
                    for (int32_t ec = 0; ec < ent_cols; ++ec) {
                        const uint32_t ent_col = lwu(ent + kEntityCollidersArrayOff + static_cast<uint32_t>(ec << 2));
                        const uint32_t col_vt  = lwu(ent_col + kColliderVtablePtrOff);
                        const uint32_t fn      = lwu(col_vt + kColliderVtCollideWithOff);
                        set(4, static_cast<int32_t>(ent_col));
                        set(5, static_cast<int32_t>(prop_col));
                        set(2, static_cast<int32_t>(fn));
                        call_at(fn, call_ra);
                        ent_cols = lw(ent + kEntityColliderCountOff);
                    }
                }
            }
        }

        const int32_t child_base = lh(node_addr + 4u);
        if (child_base == 0) {
            break;
        }

        const int32_t mid_z = (min_z + max_z) >> 1;
        const int32_t mid_x = (min_x + max_x) >> 1;
        if (mid_z < qz) {
            min_z = mid_z;
            if (mid_x < qx) {
                min_x = mid_x;
                node_idx = child_base + 3;
            } else {
                max_x = mid_x;
                node_idx = child_base + 2;
            }
        } else {
            max_z = mid_z;
            if (mid_x < qx) {
                min_x = mid_x;
                node_idx = child_base + 1;
            } else {
                max_x = mid_x;
                node_idx = child_base;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// 0x00177CA0  DynamicEntitiesPhysicsStep
// ----------------------------------------------------------------------------
// Main dynamic entity collision step over g_ActiveDynamicEntities (0x003CDBE8):
//   Phase 1: Update every active entity's colliders via Collider::PreStep (+0x0C).
//   Phase 2: For each active entity with colliders:
//     (A) Broadphase grid query (0x001D57A8) -> dynamic-vs-dynamic & grid props.
//     (B) Primary static track quadtree (0x004473FC) traversal.
//     (C) Optional secondary static quadtree (0x00447400) traversal.
void decomp_00177CA0(void) {
    int32_t exit_v0 = 0;
    {
        Frame frame(0x120);
        const uint32_t sp = reg(29);

        // Phase 1: Per-entity collider pre-step (update world bounds/shapes).
        int32_t active_count = lw(kGlobalActiveDynCount);
        for (int32_t i = 0; i < active_count;) {
            const uint32_t ent = lwu(kGlobalActiveDynEntities + static_cast<uint32_t>(i << 2));
            int32_t num_cols = lw(ent + kEntityColliderCountOff);
            ++i;
            for (int32_t c = 0; c < num_cols; ++c) {
                const uint32_t col = (c >= 0 && c < num_cols)
                                         ? lwu(ent + kEntityCollidersArrayOff + static_cast<uint32_t>(c << 2))
                                         : 0u;
                const uint32_t col_vt = lwu(col + kColliderVtablePtrOff);
                const uint32_t fn = lwu(col_vt + kColliderVtPreStepOff);
                set(4, static_cast<int32_t>(col));
                set(2, static_cast<int32_t>(fn));
                call_at(fn, 0x00177D38u);
                num_cols = lw(ent + kEntityColliderCountOff);
            }
            active_count = lw(kGlobalActiveDynCount);
        }

        // Phase 2: Collision detection & response against broadphase grid & static quadtrees.
        active_count = lw(kGlobalActiveDynCount);
        if (active_count <= 0) {
            exit_v0 = active_count;
        } else {
            for (int32_t i = 0; i < active_count;) {
                const uint32_t ent = lwu(kGlobalActiveDynEntities + static_cast<uint32_t>(i << 2));
                const int32_t num_cols = lw(ent + kEntityColliderCountOff);
                ++i;
                if (num_cols > 0) {
                    const uint32_t ent_pos = ent + kEntityWorldPosOff;

                    // Pass 2A: Spatial hash broadphase query (output: cells[4] at sp+0, count at sp+0x10).
                    set(4, static_cast<int32_t>(sp));
                    set(5, static_cast<int32_t>(sp + 0x10u));
                    set(6, static_cast<int32_t>(ent_pos));
                    call(f_001D57A8, 0x00177DB8u);

                    int32_t cell_count = lw(sp + 0x10u);
                    for (int32_t ci = 0; ci < cell_count; ++ci) {
                        const uint32_t cell_ptr = lwu(sp + static_cast<uint32_t>(ci << 2));
                        uint32_t node = lwu(cell_ptr);
                        while (node != 0u) {
                            const uint32_t other = lwu(node);

                            // Pair deduplication: skip if ent >= other AND other is also dynamic.
                            bool skip_pair = false;
                            if (!(ent < other)) {
                                const uint32_t other_vt = lwu(other + kEntityVtablePtrOff);
                                const uint32_t fn = lwu(other_vt + kEntityVtPreTickOff);
                                set(4, static_cast<int32_t>(other));
                                set(2, static_cast<int32_t>(fn));
                                call_at(fn, 0x00177E04u);
                                if (reg(2) != 0u) {
                                    skip_pair = true;
                                }
                            }

                            if (!skip_pair) {
                                int32_t other_cols = lw(other + kEntityColliderCountOff);
                                for (int32_t oc = 0; oc < other_cols; ++oc) {
                                    const uint32_t other_col = (oc >= 0 && oc < other_cols)
                                                                   ? lwu(other + kEntityCollidersArrayOff + static_cast<uint32_t>(oc << 2))
                                                                   : 0u;
                                    int32_t ent_cols = lw(ent + kEntityColliderCountOff);
                                    if (ent_cols > 0) {
                                        for (int32_t ec = 0; ec < ent_cols; ++ec) {
                                            const uint32_t ent_col = lwu(ent + kEntityCollidersArrayOff + static_cast<uint32_t>(ec << 2));
                                            const uint32_t col_vt = lwu(ent_col + kColliderVtablePtrOff);
                                            const uint32_t fn = lwu(col_vt + kColliderVtCollideWithOff);
                                            set(4, static_cast<int32_t>(ent_col));
                                            set(5, static_cast<int32_t>(other_col));
                                            set(2, static_cast<int32_t>(fn));
                                            call_at(fn, 0x00177E74u);
                                            ent_cols = lw(ent + kEntityColliderCountOff);
                                        }
                                        other_cols = lw(other + kEntityColliderCountOff);
                                    }
                                }
                            }
                            node = lwu(node + 4u);
                        }
                        cell_count = lw(sp + 0x10u);
                    }

                    // Pass 2B: Primary static track collision quadtree.
                    const uint32_t main_qt = lwu(kGlobalStaticQuadtreeMain);
                    collide_entity_against_quadtree(ent, main_qt, 0x00177FC0u);

                    // Pass 2C: Optional secondary static collision quadtree.
                    const uint32_t aux_qt = lwu(kGlobalStaticQuadtreeAux);
                    if (aux_qt != 0u) {
                        collide_entity_against_quadtree(ent, aux_qt, 0x00178158u);
                    }
                }
                active_count = lw(kGlobalActiveDynCount);
            }
            exit_v0 = 0;
        }
    }
    set(2, exit_v0);
}

// ----------------------------------------------------------------------------
// 0x00178258  DynamicEntitiesPostStep
// ----------------------------------------------------------------------------
// Post-physics step for all active dynamic entities:
//   - Live simulation: flushes contacts (0x001ADDD0), invokes vtable+0x1C on each
//     active dynamic entity, and captures the post-physics snapshot (0x001BF3F8).
//   - Replay/override: applies replay entity state (0x001BEF40).
//   - Always finishes by invoking vtable+0x20 on each active dynamic entity.
void decomp_00178258(void) {
    int32_t exit_v0 = 0;
    {
        Frame frame(0x70);

        call(f_001BFA30, 0x0017827Cu);
        if (reg(2) == 0u) {
            call(f_001ADDD0, 0x0017828Cu);

            int32_t active_count = lw(kGlobalActiveDynCount);
            for (int32_t i = 0; i < active_count;) {
                const uint32_t ent = lwu(kGlobalActiveDynEntities + static_cast<uint32_t>(i << 2));
                const uint32_t vt = lwu(ent + kEntityVtablePtrOff);
                const uint32_t fn = lwu(vt + kEntityVtPostPhysicsOff);
                ++i;
                set(4, static_cast<int32_t>(ent));
                set(2, static_cast<int32_t>(vt));
                call_at(fn, 0x001782CCu);
                active_count = lw(kGlobalActiveDynCount);
            }

            set(4, static_cast<int32_t>(lwu(kGlobalWorldEntityArrayPtr)));
            set(5, lw(kGlobalWorldEntityCount));
            call(f_001BF3F8, 0x001782F8u);
        } else {
            set(4, static_cast<int32_t>(lwu(kGlobalWorldEntityArrayPtr)));
            set(5, lw(kGlobalWorldEntityCount));
            call(f_001BEF40, 0x00178314u);
        }

        int32_t active_count = lw(kGlobalActiveDynCount);
        if (active_count <= 0) {
            exit_v0 = active_count;
        } else {
            for (int32_t i = 0; i < active_count;) {
                const uint32_t ent = lwu(kGlobalActiveDynEntities + static_cast<uint32_t>(i << 2));
                const uint32_t vt = lwu(ent + kEntityVtablePtrOff);
                const uint32_t fn = lwu(vt + kEntityVtPostFinalizeOff);
                ++i;
                set(4, static_cast<int32_t>(ent));
                set(2, static_cast<int32_t>(vt));
                call_at(fn, 0x00178344u);
                active_count = lw(kGlobalActiveDynCount);
            }
            exit_v0 = 0;
        }
    }
    set(2, exit_v0);
}

// ----------------------------------------------------------------------------
// 0x001792E8  TrackRacePreStep
// ----------------------------------------------------------------------------
// Processes pending simulation pause/unpause requests (0x003CDAC8):
//   - If request == 2 (unpause) during locked replay end, cancels request.
//   - If request == 1 (pause), pauses 5 channels (0..4) on transition to paused.
//   - If request == 2 (unpause), clears g_WorldSimPaused (0x003CDAC4).
//   - Always clears g_PauseRequest (0x003CDAC8).
void decomp_001792E8(void) {
    uint64_t exit_v0 = 0u;
    {
        Frame frame(0x40);

        if (lw(kGlobalPauseRequest) == 2) {
            call(f_00203238, 0x00179314u);
            if (reg(2) != 0u) {
                call(f_001918D0, 0x00179324u);
                if (reg(2) != 0u) {
                    sw(kGlobalPauseRequest, 0u);
                }
            }
        }

        const int32_t req = lw(kGlobalPauseRequest);
        if (req == 1) {
            const int32_t was_paused = lw(kGlobalWorldSimPaused);
            if (was_paused == 0) {
                static constexpr uint32_t kPauseCallRa[5] = {
                    0x00179364u, 0x0017936Cu, 0x00179374u, 0x0017937Cu, 0x00179384u
                };
                for (int32_t ch = 0; ch < 5; ++ch) {
                    set(4, ch);
                    call(f_00333398, kPauseCallRa[ch]);
                }
                exit_v0 = reg64(2);
            } else {
                exit_v0 = static_cast<uint64_t>(static_cast<int64_t>(was_paused));
            }
            sw(kGlobalWorldSimPaused, static_cast<uint32_t>(req));
        } else if (req == 2) {
            sw(kGlobalWorldSimPaused, 0u);
            exit_v0 = 0x003D0000ull;
        } else {
            exit_v0 = 0x003D0000ull;
        }

        sw(kGlobalPauseRequest, 0u);
    }
    set64(2, exit_v0);
}

// ----------------------------------------------------------------------------
// 0x001793B8  TrackRaceMidStep
// ----------------------------------------------------------------------------
// Drains the 20-slot deferred event pointer queue at 0x00447458 via 0x00189E80
// whenever 0x0044743C is non-zero, then resets 0x0044743C to 0.
void decomp_001793B8(void) {
    uint64_t exit_v0 = 0u;
    {
        Frame frame(0x40);

        if (lw(kGlobalDeferredEventPending) != 0) {
            static constexpr uint32_t kSlotCallRa[10] = {
                0x001793FCu, 0x00179410u, 0x00179424u, 0x00179438u, 0x0017944Cu,
                0x00179460u, 0x00179474u, 0x00179488u, 0x0017949Cu, 0x001794B0u
            };
            set(2, 0x00440000);
            for (int32_t batch = 0; batch < 2; ++batch) {
                const uint32_t base = kGlobalDeferredEventSlots + static_cast<uint32_t>(batch * 40);
                for (int32_t s = 0; s < 10; ++s) {
                    const uint32_t slot_ptr = lwu(base + static_cast<uint32_t>(s << 2));
                    if (slot_ptr != 0u) {
                        set(4, static_cast<int32_t>(slot_ptr));
                        call(f_00189E80, kSlotCallRa[s]);
                    }
                }
            }
            sw(kGlobalDeferredEventPending, 0u);
            exit_v0 = reg64(2);
        }
    }
    set64(2, exit_v0);
}

}  // namespace

extern "C" {

void f_001779C0(void) { diff_run(0x001779C0u, decomp_001779C0, f_001779C0_orig); }
void f_00177B40(void) { diff_run(0x00177B40u, decomp_00177B40, f_00177B40_orig); }
void f_00177CA0(void) { diff_run(0x00177CA0u, decomp_00177CA0, f_00177CA0_orig); }
void f_00178258(void) { diff_run(0x00178258u, decomp_00178258, f_00178258_orig); }
void f_001792E8(void) { diff_run(0x001792E8u, decomp_001792E8, f_001792E8_orig); }
void f_001793B8(void) { diff_run(0x001793B8u, decomp_001793B8, f_001793B8_orig); }

}  // extern "C"
