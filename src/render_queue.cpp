// render_queue.cpp - EE 3D draw queue (SLUS_209.10), readable C++ replacing the recompiled code.
// See docs/EE_RENDER_SUBMISSION.md for the structures and the evidence.
//
// Every 3D model the game draws in a frame (except projected shadows) goes through this queue:
//
//   0x34C9C0 RenderQueueBeginFrame     open bucket set 1, empty the record pool
//   0x34BDA8 RenderQueueSubmitModel    (model, matrix, material packet, layer mask): sphere cull,
//                                      matrix packet, then 0x34B140
//   0x34BF90 RenderQueueSubmitModelKey (model, matrix, key): every batch into one bucket, no material
//   0x34CC98 RenderQueueSubmitStatic   (model): world-space model, identity matrix (handle 0)
//   0x34B140 RenderQueueAppendBatches  one 20-byte record per batch whose layer bit is set
//   0x34C930 RenderQueueFlushLayers0to8
//   0x34C860 RenderQueueFlushLayer9    identity matrix, layer 9, then reset
//   0x34AF98 RenderQueueFlush(layer, set): walk the layer's texture keys in order and emit each
//                                      bucket's records (matrix / material / clip flag / batch DMA)
//
// A guest record (pool *(0x4C96C4), count *(0x4C96C8), capacity 0xC00):
//   +0x00 batch, +0x04 next in bucket, +0x08 clip flag (0x365288: ITOP bit 0x200),
//   +0x0C matrix packet (0x35CD28 result; 0 = identity), +0x10 material packet or -1.
// Buckets: 0x4C96D0 + 0x800*set + 4*(key+1); the key is the batch's texture slot (table
// *(0x4D2D30), 0x44 bytes per slot), so each bucket is one texture.
//
// Live native rendering extends each bucket with host records after the guest pool fills. Original
// and EOD_DIFF use the guest layout and its original limit so the recompiled code can compare state.
#include "core/config.h"
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "remaster/scene_bridge.h"
#include "render/native_renderer.h"
#include "render/scene_tap.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
void f_0034BDA8_orig(void);
void f_0034B140_orig(void);
void f_0034BF90_orig(void);
void f_0034CC98_orig(void);
void f_0034C9C0_orig(void);
void f_0034C930_orig(void);
void f_0034C860_orig(void);
void f_0034AF98_orig(void);
void f_0034C828_orig(void);
void f_0034C8C0_orig(void);
void f_0034D1F8_orig(void);
void f_0034D088_orig(void);

// Called through their public symbols so wrappers (traces, LOD, nested diff checks) still apply.
void f_0034E308(void);  // FrustumTestSphere(centre*, clip_out*, depth_out*; f12 radius) -> visible
void f_0035CD28(void);  // MatrixPacketBuild(m) -> packet (VU1 TOPS+0..6) or 0
void f_0034B140(void);
void f_0034AF98(void);
void f_0034D060(void);  // ModelBatchList(model) -> header {count, batch[]}
void f_0034F7B0(void);  // RenderFlagTest(mask) -> (*(0x4CD39C) & mask) != 0
void f_00353470(void);  // TextureSlotNoteDistance(slot; f12 depth, f13 batch scale)
void f_0035DF40(void);  // BatchTakesMaterial(batch) -> batch type (+0x1C) is 3 or 5
void f_00354C00(void);  // LayerKeys(layer, list_out*, count_out*)
void f_00354BB8(void);  // LayerKeysImmediate(list_out*, count_out*)
void f_0035A4D0(void);  // special-layer bracket (1 open, 0 close)
void f_0034F988(void);  // LayerKeyBegin(key): texture state for the bucket
void f_0034F888(void);  // LayerKeyEnd
void f_0035E018(void);  // SetMatrixPacket(packet; 0 = identity)
void f_0035DDC8(void);  // SetMaterialPacket(packet; 0 = default)
void f_00365288(void);  // SetClipFlag(flag)
void f_0035CB88(void);  // EmitBatch(batch): VU1 DMA for one batch
}

namespace {

// Queue state (guest globals)
constexpr uint32_t kModelTable = 0x4C96C0;     // -> 0x14-byte entries; +8 header, +0x10 key array
constexpr uint32_t kPool = 0x4C96C4;           // -> records
constexpr uint32_t kPoolCount = 0x4C96C8;
constexpr uint32_t kBuckets = 0x4C96D0;        // two sets of 0x800 bytes
constexpr uint32_t kSetOpen = 0x4CA6D0;        // 1 while a frame is being queued
constexpr uint32_t kFrameActive = 0x4CA6D4;
constexpr uint32_t kCachedMatrix = 0x4CA6D8;   // last matrix packet sent by a flush
constexpr uint32_t kCachedMaterial = 0x4CA6DC; // last material packet sent by a flush
constexpr uint32_t kLayerMask = 0x3DD634;      // batch layer bits accepted by 0x34B140
constexpr uint32_t kSpecialKey = 0x3DD680;     // extra bucket for material batches (-2 = none)
constexpr uint32_t kLegacyRecordLimit = 0xC00 - 1;  // EE rejects an append when count + 1 reaches 0xC00.
constexpr uint32_t kRecordSize = 0x14;
constexpr uint32_t kBucketSetBytes = 0x800;
constexpr uint32_t kBucketsPerSet = kBucketSetBytes / sizeof(uint32_t);

// Guest register access. Values are written the way the EE leaves them: 32-bit results
// sign-extended into the low doubleword.
inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline uint64_t reg64(int r) { return g_ee.r[r].ud[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline void set64(int r, uint64_t v) { g_ee.r[r].ud[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
inline float lf(uint32_t a) {
    FReg f;
    f.u = MEM_LW(a);
    return f.f;
}
inline void sf(uint32_t a, float v) {
    FReg f;
    f.f = v;
    MEM_SW(a, f.u);
}
inline void set_f(int f, uint32_t bits) { g_ee.fpr[f].u = bits; }

struct DrawRecord {
    uint32_t batch, clip, matrix, material;
};

struct OverflowRecord {
    DrawRecord draw;
    uint32_t next;  // one-based index in its own bucket set; 0 ends the chain
};

struct OverflowQueue {
    std::array<uint32_t, kBucketsPerSet> heads{};
    std::vector<OverflowRecord> records;
    unsigned pending_buckets = 0;
    unsigned low_use_resets = 0;

    void append(uint32_t bucket, DrawRecord draw) {
        uint32_t &head = heads[bucket];
        records.push_back({draw, head});
        if (head == 0) ++pending_buckets;
        head = static_cast<uint32_t>(records.size());
    }

    void clear_bucket(uint32_t bucket) {
        heads[bucket] = 0;
        --pending_buckets;
    }

    void reset() {
        if (pending_buckets) heads.fill(0);
        pending_buckets = 0;
        // Reuse storage in a consistently dense scene. Release a sustained
        // high-water surplus once the view or immediate queue is quiet again.
        if (records.capacity() && records.size() <= records.capacity() / 4) {
            if (++low_use_resets == 64) {
                std::vector<OverflowRecord>().swap(records);
                low_use_resets = 0;
            }
        } else {
            low_use_resets = 0;
        }
        records.clear();
    }
};

// The EE thread owns both sets. Set 0 may flush immediately while set 1 still holds layer 9.
std::array<OverflowQueue, 2> g_overflow;

bool adaptive_enabled() {
    // A watched outer call can run the generated queue, and diff snapshots restore only guest state.
    static const bool diff_off = [] {
        const char *diff = std::getenv("EOD_DIFF");
        return !diff || !*diff;
    }();
    return diff_off && native::enabled();
}

OverflowQueue *overflow_queue(uint32_t set_offset) {
    const uint32_t set = set_offset / kBucketSetBytes;
    return set < g_overflow.size() && adaptive_enabled() ? &g_overflow[set] : nullptr;
}

void reset_adaptive_queues() {
    if (!adaptive_enabled()) return;
    for (auto &queue : g_overflow) queue.reset();
    // A pool count reset also invalidates guest links left behind by an interrupted view.
    std::memset(g_ram + kBuckets, 0, kBucketSetBytes * g_overflow.size());
}

void release_adaptive_queues() {
    if (!adaptive_enabled()) return;
    g_overflow = {};
    std::memset(g_ram + kBuckets, 0, kBucketSetBytes * g_overflow.size());
    sw(kPoolCount, 0);
    sw(kSetOpen, 0);
    sw(kFrameActive, 0);
}

// Calls a guest function as the original's jal at `ra` does.
inline void call(void (*fn)(void), uint32_t ra) {
    g_ee.r[31].ud[0] = ra;
    fn();
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

uint32_t model_header(int32_t model) { return lwu(lwu(kModelTable) + (uint32_t)(model * 0x14) + 8); }

// Bounding sphere centre (header +0x1C) transformed by the 3x4 matrix at m (rows 0..2, then the
// translation at +0x24), as the EE computes it (multiply-accumulate in this order).
void transform_centre(uint32_t header, uint32_t m, uint32_t out) {
    const float x = lf(header + 0x1C), y = lf(header + 0x20), z = lf(header + 0x24);
    auto row = [&](uint32_t c) {
        return fadd_ps2(fadd_ps2(fmul_ps2(x, lf(m + c)), fmul_ps2(y, lf(m + c + 12))), fmul_ps2(z, lf(m + c + 24)));
    };
    const float tx = row(0), ty = row(4), tz = row(8);
    sf(out, fadd_ps2(tx, lf(m + 0x24)));
    sf(out + 4, fadd_ps2(ty, lf(m + 0x28)));
    sf(out + 8, fadd_ps2(tz, lf(m + 0x2C)));
}

// FrustumTestSphere(centre, clip_out, depth_out), radius in f12.
bool sphere_visible(uint32_t centre, uint32_t clip_out, uint32_t depth_out, uint32_t radius_bits, uint32_t ra) {
    set(4, (int32_t)centre);
    set(5, (int32_t)clip_out);
    set(6, (int32_t)depth_out);
    set_f(12, radius_bits);
    call(f_0034E308, ra);
    return reg64(2) != 0;
}

// ---------------------------------------------------------------------------------------------
// 0x34BDA8 RenderQueueSubmitModel(model, const float m[12], material, layer_mask)
void submit_model() {
    const uint64_t model = reg64(4), material = reg64(6);
    const uint32_t m = reg(5), layer_mask = reg(7);
    if ((int64_t)model == -1) {
        set(2, -1);
        return;
    }
    Frame f(0x80);
    const uint32_t centre = f.sp(), clip = f.sp() + 0x10, depth = f.sp() + 0x14;
    const uint32_t header = model_header((int32_t)model);
    const uint32_t saved_mask = lwu(kLayerMask);
    sw(kLayerMask, layer_mask);
    transform_centre(header, m, centre);
    if (g_config.remaster_engine && m) {
        float mat12[12];
        for (int i = 0; i < 12; ++i) mat12[i] = lf(m + static_cast<uint32_t>(i) * 4u);
        remaster::ee_submit_entity(static_cast<uint32_t>(model), mat12, static_cast<uint32_t>(material),
                                   false, layer_mask);
    }
    if (sphere_visible(centre, clip, depth, lwu(header + 0x18), 0x34BF2C)) {
        set(4, (int32_t)m);
        call(f_0035CD28, 0x34BF3C);
        const uint64_t matrix_packet = reg64(2);
        set(7, lw(clip));
        set64(4, model);
        set_f(12, lwu(depth));
        set64(5, matrix_packet);
        set64(6, material);
        call(f_0034B140, 0x34BF54);
    }
    sw(kLayerMask, saved_mask);
}

// ---------------------------------------------------------------------------------------------
// 0x34B140 RenderQueueAppendBatches(model, matrix_packet, material, clip; f12 depth)
void queue_record(uint32_t batch, uint32_t bucket, uint32_t set_offset, uint32_t matrix,
                  uint32_t clip, uint64_t material, uint32_t material_ra) {
    const uint32_t n = lwu(kPoolCount);
    auto material_packet = [&] {
        if (material != 0xFFFFFFFFFFFFFFFFull) {
            set(4, (int32_t)batch);
            call(f_0035DF40, material_ra);
            if (reg64(2) != 0) return (uint32_t)material;
        }
        return 0xFFFFFFFFu;
    };
    if (n < kLegacyRecordLimit) {
        const uint32_t rec = lwu(kPool) + (uint32_t)n * kRecordSize;
        sw(kPoolCount, n + 1);
        sw(rec + 0x00, batch);
        sw(rec + 0x08, clip);
        sw(rec + 0x0C, matrix);
        sw(rec + 0x10, material_packet());
        const uint32_t head = kBuckets + set_offset + bucket * 4;
        sw(rec + 0x04, lwu(head));
        sw(head, rec);
    } else if (OverflowQueue *queue = overflow_queue(set_offset); queue && bucket < kBucketsPerSet) {
        queue->append(bucket, {batch, clip, matrix, material_packet()});
        sw(kPoolCount, n + 1);
    }
}

void note_distance(int32_t slot, uint32_t depth_bits, uint32_t batch, uint32_t ra) {
    set(4, slot);
    set_f(13, lwu(batch + 0x20));
    set_f(12, depth_bits);
    call(f_00353470, ra);
}

void append_batches() {
    const uint64_t model = reg64(4);
    const uint32_t matrix = reg(5), clip = reg(7);
    const uint64_t material = reg64(6);
    const uint32_t depth = g_ee.fpr[12].u;
    Frame f(0xD0);

    set64(4, model);
    call(f_0034D060, 0x34B18C);
    const uint32_t header = reg(2);

    if (lw(kSetOpen) == 0 && lw(kFrameActive) == 0) {  // a draw outside a frame starts clean
        if (auto *queue = overflow_queue(0); queue && queue->pending_buckets) {
            queue->reset();
            std::memset(g_ram + kBuckets, 0, kBucketSetBytes);
        }
        sw(kCachedMatrix, 0xFFFFFFFFu);
        sw(kCachedMaterial, 0xFFFFFFFFu);
        sw(kPoolCount, 0);
    }
    // Material batches also go to the special key's bucket when render flag 0x400 is set.
    bool duplicate = false;
    set(4, 0x400);
    call(f_0034F7B0, 0x34B1D8);
    if (reg64(2) != 0) duplicate = lw(kSpecialKey) != -2;

    const int32_t open = lw(kSetOpen);
    const uint32_t set_offset = (uint32_t)open << 11;
    const bool flush_now = ((int64_t)open ^ 1) != 0;  // no frame open: draw at once

    for (int32_t i = 0; i < lw(header); i++) {
        const uint32_t batch = lwu(lwu(header + 4) + (uint32_t)i * 4);
        const uint32_t layer_bit = 1u << ((uint32_t)(int16_t)MEM_LH(batch + 0x18) & 31);
        if (lwu(kLayerMask) & layer_bit) {
            const uint32_t keys = lwu(lwu(kModelTable) + (uint32_t)((int32_t)model * 0x14) + 0x10);
            const int32_t key = lw(keys + (uint32_t)i * 4);
            note_distance(key, depth, batch, 0x34B284);
            queue_record(batch, (uint32_t)key + 1, set_offset, matrix, clip, material, 0x34B2CC);
            if (duplicate) {
                set(4, (int32_t)batch);
                call(f_0035DF40, 0x34B320);
                if (reg64(2) != 0) {
                    queue_record(batch, lwu(kSpecialKey) + 1, set_offset, matrix, clip, material, 0x34B380);
                    note_distance(lw(kSpecialKey), depth, batch, 0x34B3CC);
                }
            }
        }
        IRQ_CHECK();
    }
    if (flush_now) {
        set(4, -1);
        set(5, 0);
        call(f_0034AF98, 0x34B400);
    }
    set(2, 0);
}

// ---------------------------------------------------------------------------------------------
// 0x34BF90 RenderQueueSubmitModelKey(model, const float m[12], key): all batches into bucket
// key + 1, no layer test, no material.
void submit_model_key() {
    const uint64_t model = reg64(4);
    const uint32_t m = reg(5), key = reg(6);
    Frame f(0x60);
    const uint32_t centre = f.sp(), clip = f.sp() + 0x10, depth = f.sp() + 0x14;
    const uint32_t header = model_header((int32_t)model);
    transform_centre(header, m, centre);
    if (!sphere_visible(centre, clip, depth, lwu(header + 0x18), 0x34C0F4)) return;
    set(4, (int32_t)m);
    call(f_0035CD28, 0x34C104);
    const uint32_t matrix_packet = reg(2);

    for (int32_t i = 0; i < lw(header); i++) {
        const uint32_t batch = lwu(lwu(header + 4) + (uint32_t)i * 4);
        queue_record(batch, key + 1, lwu(kSetOpen) << 11, matrix_packet, lwu(clip),
                     0xFFFFFFFFFFFFFFFFull, 0);
        IRQ_CHECK();
    }
    const int32_t open = lw(kSetOpen);
    if (open != 0) {
        set(2, open);
        return;
    }
    set(4, -1);
    set(5, 0);
    call(f_0034AF98, 0x34C1D8);
}

// ---------------------------------------------------------------------------------------------
// 0x34CC98 RenderQueueSubmitStatic(model): sphere already in world space, identity matrix.
void submit_static() {
    const uint64_t model = reg64(4);
    if ((int64_t)model == -1) {
        set(2, -1);
        return;
    }
    Frame f(0x30);
    const uint32_t header = model_header((int32_t)model);
    const bool visible = sphere_visible(header + 0x1C, f.sp(), f.sp() | 4, lwu(header + 0x18), 0x34CCDC);
    set_f(12, lwu(f.sp() + 4));  // depth
    if (!visible) return;
    set(7, lw(f.sp()));
    set64(4, model);
    set(5, 0);
    set(6, -1);
    call(f_0034B140, 0x34CCFC);
}

// ---------------------------------------------------------------------------------------------
// 0x34C9C0 RenderQueueBeginFrame
void begin_frame() {
    native::ee_frame_begin();
    scene_tap::begin_view();
    reset_adaptive_queues();
    sw(kSetOpen, 1);
    sw(kCachedMaterial, 0xFFFFFFFFu);
    sw(kFrameActive, 1);
    sw(kPoolCount, 0);
    sw(kCachedMatrix, 0xFFFFFFFFu);
    set(2, 0x4D0000);
}

// 0x34C930 RenderQueueFlushLayers0to8
void flush_layers_0_to_8() {
    Frame f(0x20);
    for (int32_t layer = 0; layer <= 8; layer++) {
        set(4, layer);
        set(5, 1);
        call(f_0034AF98, 0x34C94C + 12 * (uint32_t)layer);
    }
    sw(kSetOpen, 0);
}

// 0x34C860 RenderQueueFlushLayer9: drawn with the identity matrix, then the queue is reset.
void flush_layer_9() {
    Frame f(0x20);
    set(4, 0);
    call(f_0035E018, 0x34C878);
    sw(kCachedMatrix, 0);
    set(4, 9);
    set(5, 1);
    call(f_0034AF98, 0x34C888);
    reset_adaptive_queues();
    sw(kCachedMatrix, 0xFFFFFFFFu);
    sw(kFrameActive, 0);
    sw(kCachedMaterial, 0xFFFFFFFFu);
    sw(kPoolCount, 0);
    set(2, -1);
}

// ---------------------------------------------------------------------------------------------
void emit_record(DrawRecord record, int32_t key, int32_t layer) {
    if (record.matrix != lwu(kCachedMatrix)) {
        set(4, (int32_t)record.matrix);
        call(f_0035E018, 0x34B07C);
        sw(kCachedMatrix, record.matrix);
    }
    if (record.material != 0xFFFFFFFFu && record.material != lwu(kCachedMaterial)) {
        set(4, (int32_t)record.material);
        call(f_0035DDC8, 0x34B0A8);
        sw(kCachedMaterial, record.material);
    }
    scene_tap::object(record.batch, record.clip, record.matrix, record.material, key, layer);
    native::ee_object(record.batch);
    set(4, (int32_t)record.clip);
    call(f_00365288, 0x34B0B4);
    set(4, (int32_t)record.batch);
    call(f_0035CB88, 0x34B0BC);
    native::ee_object_end();
    IRQ_CHECK();
}

// 0x34AF98 RenderQueueFlush(layer, set): for each texture key of the layer, in the layer's order,
// emit the bucket's records (most recently queued first) and empty the bucket.
void flush() {
    const int64_t layer = (int64_t)reg64(4);
    const uint32_t set_offset = reg(5) << 11;
    auto *overflow = overflow_queue(set_offset);
    Frame f(0xB0);
    const uint32_t list_out = f.sp(), count_out = f.sp() | 4;
    sw(count_out, 0);
    if (layer < 0) {
        set(4, (int32_t)list_out);
        set(5, (int32_t)count_out);
        call(f_00354BB8, 0x34AFF0);
    } else {
        set(5, (int32_t)list_out);
        set(6, (int32_t)count_out);
        call(f_00354C00, 0x34AFDC);
    }
    int32_t keys = lw(count_out);
    for (int32_t i = 0; i < keys; i++) {
        const int32_t key = lw(lwu(list_out) + (uint32_t)i * 4);
        const uint32_t bucket = (uint32_t)key + 1;
        const uint32_t head = set_offset + kBuckets + bucket * 4;
        const uint32_t extra = overflow && bucket < kBucketsPerSet ? overflow->heads[bucket] : 0;
        if (extra || lwu(head) != 0) {
            if (key == lw(kSpecialKey)) {
                set(4, 1);
                call(f_0035A4D0, 0x34B04C);
            }
            set(4, key);
            call(f_0034F988, 0x34B054);
            // Overflow contains only later submissions to this key. Both chains prepend records.
            for (uint32_t link = extra; link != 0;) {
                const OverflowRecord record = overflow->records[link - 1];
                emit_record(record.draw, key, (int32_t)layer);
                link = record.next;
            }
            for (uint32_t rec = lwu(head); rec != 0; rec = lwu(rec + 4)) {
                emit_record({lwu(rec), lwu(rec + 0x08), lwu(rec + 0x0C), lwu(rec + 0x10)},
                            key, (int32_t)layer);
            }
            call(f_0034F888, 0x34B0D0);
            const bool special = key == lw(kSpecialKey);
            if (extra) overflow->clear_bucket(bucket);
            sw(head, 0);
            if (special) {
                set(4, 0);
                call(f_0035A4D0, 0x34B0E4);
            }
            keys = lw(count_out);
        }
        IRQ_CHECK();
    }
    set(4, 1);
    call(f_00365288, 0x34B110);
    if (overflow && set_offset == 0 && !overflow->pending_buckets) overflow->reset();
}

}  // namespace

// Entry points. render_trace.cpp owns the public symbols it observes and forwards to these.
extern "C" void eod_rq_0034BDA8(void) { diff_run(0x0034BDA8, submit_model, f_0034BDA8_orig); }
extern "C" void eod_rq_0034B140(void) { diff_run(0x0034B140, append_batches, f_0034B140_orig); }
extern "C" void eod_rq_0034BF90(void) { diff_run(0x0034BF90, submit_model_key, f_0034BF90_orig); }
extern "C" void eod_rq_0034C9C0(void) { diff_run(0x0034C9C0, begin_frame, f_0034C9C0_orig); }
extern "C" void eod_rq_0034C930(void) { diff_run(0x0034C930, flush_layers_0_to_8, f_0034C930_orig); }
extern "C" void eod_rq_0034C860(void) { diff_run(0x0034C860, flush_layer_9, f_0034C860_orig); }
extern "C" void eod_rq_0034AF98(void) { diff_run(0x0034AF98, flush, f_0034AF98_orig); }
extern "C" void f_0034CC98(void) { diff_run(0x0034CC98, submit_static, f_0034CC98_orig); }

// Retain the generated guest allocation and teardown for Original and D2.
extern "C" void f_0034D1F8(void) {
    release_adaptive_queues();
    f_0034D1F8_orig();
}
extern "C" void f_0034D088(void) {
    release_adaptive_queues();
    f_0034D088_orig();
}
extern "C" void f_0034C828(void) {
    if (adaptive_enabled()) {
        reset_adaptive_queues();
        sw(kPoolCount, 0);
    }
    f_0034C828_orig();
}
extern "C" void f_0034C8C0(void) {
    const bool last_layer = reg64(4) == 9;
    f_0034C8C0_orig();
    if (last_layer) reset_adaptive_queues();
}
