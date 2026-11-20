// Drive the real queue entry points using retail-shaped EE tables and packet sink callbacks.
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/memory.h"
#include "render/native_renderer.h"
#include "render/scene_tap.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

EEState g_ee{};
uint8_t *g_ram;
volatile int g_ee_irq_pending = 0;
extern "C" void ee_service_interrupts() { std::abort(); }
extern "C" uint16_t ee_read16_slow(uint32_t) { std::abort(); }
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
uint64_t kernel_vblank_count() { return 1; }

extern "C" {
void eod_rq_0034BDA8();
void eod_rq_0034B140();
void eod_rq_0034BF90();
void eod_rq_0034C9C0();
void eod_rq_0034C930();
void eod_rq_0034C860();
void eod_rq_0034AF98();
void f_0034CC98();
void f_0034D088();
void f_0034D1F8();
}

namespace {

constexpr uint32_t kModelTable = 0x10000, kRecordPool = 0x500000, kPoolCount = 0x4C96C8;
constexpr uint32_t kPoolPointer = 0x4C96C4, kGuestBuckets = 0x4C96D0;
constexpr uint32_t kGuestLimit = 3071, kSpecial = 3, kNone = 0xffffffff;
constexpr uint32_t kLayerKeys = 0x11000;
constexpr uint32_t kTransform = 0x12000, kModelMatrix = 0x12100, kKeyMatrix = 0x12200;
constexpr uint32_t kMaterial = 0x12400, kAlternateMaterial = 0x12500;

void require(bool ok, const char *message) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}

struct Record {
    uint32_t batch, matrix, material, clip;
    int32_t key, layer;
    bool operator==(const Record &o) const {
        return batch == o.batch && matrix == o.matrix && material == o.material &&
               clip == o.clip && key == o.key && layer == o.layer;
    }
};

bool g_native = true;
uint32_t g_next_matrix = 0, g_current_matrix = kNone, g_current_material = kNone, g_current_clip = kNone;
int32_t g_current_key = -2;
bool g_special_open = false;
std::vector<Record> g_tapped, g_emitted;
std::vector<uint32_t> g_material_changes;
using Buckets = std::array<std::vector<Record>, 20>;

uint32_t model_header(unsigned model) {
    return MEM_LW(kModelTable + 20 * model + 8);
}

void build_model(unsigned model, uint32_t first_batch, unsigned count, uint32_t clip) {
    const uint32_t header = 0x13000 + 0x40 * model;
    const uint32_t batches = 0x14000 + 0x4000 * model, keys = 0x24000 + 0x4000 * model;
    MEM_SW(kModelTable + 20 * model + 8, header);
    MEM_SW(kModelTable + 20 * model + 16, keys);
    MEM_SW(header, count);
    MEM_SW(header + 4, batches);
    MEM_SW(header + 0x18, clip);
    constexpr uint32_t order[] = {1, 9, 17};
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t batch = first_batch + 0x40 * i, key = order[i % 3];
        MEM_SW(batches + 4 * i, batch);
        MEM_SW(keys + 4 * i, key);
        MEM_SW(batch + 0x18, key == 9 ? 9 : 0);
        MEM_SW(batch + 0x1c, i % 5 == 0 ? 3 : 0);
    }
}

void submit_model(unsigned model, uint32_t material = kMaterial) {
    g_next_matrix = kModelMatrix;
    g_ee.r[4].sd[0] = model;
    g_ee.r[5].sd[0] = kTransform;
    g_ee.r[6].sd[0] = material;
    g_ee.r[7].sd[0] = (1 << 9) | 1;
    eod_rq_0034BDA8();
}

void submit_key(unsigned model) {
    g_next_matrix = kKeyMatrix;
    g_ee.r[4].sd[0] = model;
    g_ee.r[5].sd[0] = kTransform;
    g_ee.r[6].sd[0] = 9;
    eod_rq_0034BF90();
}

void submit_static(unsigned model) {
    g_ee.r[4].sd[0] = model;
    f_0034CC98();
}

void flush(int layer, unsigned set) {
    g_ee.r[4].sd[0] = layer;
    g_ee.r[5].sd[0] = set;
    eod_rq_0034AF98();
}

void expect_model(Buckets &out, unsigned model, uint32_t matrix, uint32_t material, bool keyed,
                  uint32_t &submitted, uint32_t limit) {
    const uint32_t header = model_header(model), batches = MEM_LW(header + 4);
    for (unsigned i = 0; i < MEM_LW(header); ++i) {
        const uint32_t batch = MEM_LW(batches + 4 * i);
        const bool takes_material = MEM_LW(batch + 0x1c) == 3;
        const uint32_t key = keyed ? 9 : MEM_LW(MEM_LW(kModelTable + 20 * model + 16) + 4 * i);
        Record record{batch, matrix, takes_material ? material : kNone, MEM_LW(header + 0x18),
                      static_cast<int32_t>(key), 0};
        if (submitted < limit) {
            out[key].push_back(record);
            ++submitted;
        }
        if (!keyed && takes_material && submitted < limit) {
            record.key = kSpecial;
            out[kSpecial].push_back(record);
            ++submitted;
        }
    }
}

void expect_bucket(std::vector<Record> &out, Buckets &buckets, int key, int layer) {
    auto &records = buckets[key];
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        Record record = *it;
        record.layer = layer;
        out.push_back(record);
    }
    records.clear();
}

void expect_immediate(std::vector<Record> &out, Buckets &buckets) {
    for (int key : {17, 1, 3, 9}) expect_bucket(out, buckets, key, -1);
}

void verify(const std::vector<Record> &expected) {
    if (g_emitted.size() != expected.size())
        std::fprintf(stderr, "emitted %zu of %zu expected records\n", g_emitted.size(), expected.size());
    require(g_emitted == expected, "batch emission matches reverse insertion within each ordered layer key");
    require(g_tapped == expected, "native feed receives each scene tap record in emission order");
    require(!g_special_open && g_current_key == -2, "key and special-layer brackets are closed");
}

}  // namespace

// The generated callees below implement the fixture's visible sphere, keys, and EE packet sink.
void diff_run(uint32_t, void (*replacement)(), void (*)(), unsigned) { replacement(); }

namespace native {
bool enabled() { return g_native; }
void ee_frame_begin() {}
void ee_object(uint32_t batch) {
    const auto *obj = scene_tap::last_object();
    require(obj && obj->batch == batch, "native hook receives the tapped batch's guest address");
    g_tapped.push_back({obj->batch, obj->matrix_packet, obj->material_packet, obj->clip,
                        obj->texture_key, obj->layer});
}
void ee_object_end() {}
}  // namespace native

extern "C" void f_0034B140() { eod_rq_0034B140(); }
extern "C" void f_0034AF98() { eod_rq_0034AF98(); }
extern "C" void f_0034BDA8_orig() { std::abort(); }
extern "C" void f_0034B140_orig() { std::abort(); }
extern "C" void f_0034BF90_orig() { std::abort(); }
extern "C" void f_0034CC98_orig() { std::abort(); }
extern "C" void f_0034C9C0_orig() { std::abort(); }
extern "C" void f_0034C930_orig() { std::abort(); }
extern "C" void f_0034C860_orig() { std::abort(); }
extern "C" void f_0034AF98_orig() { std::abort(); }
extern "C" void f_0034C828_orig() { std::abort(); }
extern "C" void f_0034C8C0_orig() { std::abort(); }
extern "C" void f_0034D088_orig() { MEM_SW(kPoolPointer, 0); }
extern "C" void f_0034D1F8_orig() {
    MEM_SW(kPoolPointer, kRecordPool + 0x10000);
    std::memset(g_ram + kGuestBuckets, 0, 0x1000);
}
extern "C" void f_0034D060() { g_ee.r[2].sd[0] = model_header(g_ee.r[4].uw[0]); }
extern "C" void f_00353470() {}
extern "C" void f_0034F7B0() { g_ee.r[2].sd[0] = 1; }
extern "C" void f_0034E308() {
    MEM_SW(g_ee.r[5].uw[0], g_ee.fpr[12].u);
    MEM_SW(g_ee.r[6].uw[0], 0);
    g_ee.r[2].sd[0] = 1;
}
extern "C" void f_0035CD28() { g_ee.r[2].sd[0] = g_next_matrix; }
extern "C" void f_0035DF40() { g_ee.r[2].sd[0] = MEM_LW(g_ee.r[4].uw[0] + 0x1c) == 3; }
extern "C" void f_00354BB8() {
    MEM_SW(g_ee.r[4].uw[0], kLayerKeys);
    MEM_SW(g_ee.r[5].uw[0], 4);
}
extern "C" void f_00354C00() {
    const auto layer = g_ee.r[4].sd[0];
    MEM_SW(g_ee.r[5].uw[0], kLayerKeys + (layer == 1 ? 8 : layer == 9 ? 12 : 0));
    MEM_SW(g_ee.r[6].uw[0], layer == 0 ? 2 : layer == 1 || layer == 9 ? 1 : 0);
}
extern "C" void f_0035A4D0() {
    const bool open = g_ee.r[4].sd[0] == 1;
    require(open != g_special_open && g_current_key == -2, "special bracket surrounds its key");
    g_special_open = open;
}
extern "C" void f_0034F988() {
    require(g_current_key == -2, "each texture key starts after the previous one ends");
    g_current_key = g_ee.r[4].sw[0];
    require(g_special_open == (g_current_key == static_cast<int32_t>(kSpecial)), "special key bracket applied");
}
extern "C" void f_0034F888() { g_current_key = -2; }
extern "C" void f_0035E018() { g_current_matrix = g_ee.r[4].uw[0]; }
extern "C" void f_0035DDC8() {
    g_current_material = g_ee.r[4].uw[0];
    g_material_changes.push_back(g_current_material);
}
extern "C" void f_00365288() { g_current_clip = g_ee.r[4].uw[0]; }
extern "C" void f_0035CB88() {
    require(g_tapped.size() == g_emitted.size() + 1, "one native feed hook per batch");
    const auto &obj = g_tapped.back();
    require(obj.batch == g_ee.r[4].uw[0] && obj.key == g_current_key &&
                obj.matrix == g_current_matrix && obj.clip == g_current_clip &&
                (obj.material == kNone || obj.material == g_current_material),
            "the guest emitter sees the same batch, key, matrix, clip and material");
    g_emitted.push_back(obj);
}

int main(int argc, char **argv) {
    require(argc == 2, "mode is native, original or diff");
    g_native = std::strcmp(argv[1], "original") != 0;
    const bool adaptive = std::strcmp(argv[1], "native") == 0;
    const uint32_t limit = adaptive ? UINT32_MAX : kGuestLimit;
    std::vector<uint8_t> memory(0x800000);
    g_ram = memory.data();
    g_ee.r[29].sd[0] = 0x710000;
    MEM_SW(0x4C96C0, kModelTable);
    MEM_SW(kPoolPointer, kRecordPool);
    MEM_SW(0x3DD634, (1 << 9) | 1);
    MEM_SW(0x3DD680, kSpecial);
    constexpr uint32_t keys[] = {17, 1, 3, 9};
    for (unsigned i = 0; i < 4; ++i) {
        MEM_SW(kLayerKeys + 4 * i, keys[i]);
    }
    build_model(0, 0x40000, 3700, 3);
    build_model(1, 0x80000, 2700, 5);
    build_model(2, 0xb0000, 100, 7);
    const uint32_t canary = kRecordPool + 0xF000;
    MEM_SW(canary, 0x12345678);
    scene_tap::set_enabled(true);

    Buckets queued, immediate;
    std::vector<Record> expected;
    uint32_t count = 0;
    eod_rq_0034C9C0();
    submit_model(0);
    expect_model(queued, 0, kModelMatrix, kMaterial, false, count, limit);
    submit_model(2, kAlternateMaterial);
    expect_model(queued, 2, kModelMatrix, kAlternateMaterial, false, count, limit);
    submit_key(1);
    expect_model(queued, 1, kKeyMatrix, kNone, true, count, limit);
    submit_static(2);
    expect_model(queued, 2, 0, kNone, false, count, limit);

    eod_rq_0034C930();
    expect_bucket(expected, queued, 17, 0);
    expect_bucket(expected, queued, 1, 0);
    expect_bucket(expected, queued, 3, 1);
    flush(0, 1);

    submit_key(1);  // set 0 uses key 9 while set 1's key 9 still has pending records
    expect_model(immediate, 1, kKeyMatrix, kNone, true, count, limit);
    expect_immediate(expected, immediate);
    submit_static(2);
    expect_model(immediate, 2, 0, kNone, false, count, limit);
    expect_immediate(expected, immediate);
    if (MEM_LW(kPoolCount) != count)
        std::fprintf(stderr, "queue accepted %u of %u expected records\n", MEM_LW(kPoolCount), count);
    require(MEM_LW(kPoolCount) == count, "native queue accepts all submissions beyond the guest pool");
    eod_rq_0034C860();
    expect_bucket(expected, queued, 9, 9);
    flush(9, 1);
    verify(expected);
    require(MEM_LW(kPoolCount) == 0, "layer 9 resets the guest record counter");
    require(MEM_LW(canary) == 0x12345678, "overflow stays within the guest allocation");
    std::vector<uint32_t> material_changes;
    uint32_t material = kNone;
    for (const auto &record : expected) {
        if (record.material != kNone && record.material != material) {
            material = record.material;
            material_changes.push_back(material);
        }
    }
    require(g_material_changes == material_changes, "material updates occur only when the next record changes it");

    // A quiet view and a following small view must not see a previously flushed host or guest chain.
    eod_rq_0034C9C0();
    eod_rq_0034C930();
    eod_rq_0034C860();
    verify(expected);
    count = 0;
    eod_rq_0034C9C0();
    submit_static(2);
    expect_model(queued, 2, 0, kNone, false, count, limit);
    eod_rq_0034C930();
    expect_bucket(expected, queued, 17, 0);
    expect_bucket(expected, queued, 1, 0);
    expect_bucket(expected, queued, 3, 1);
    eod_rq_0034C860();
    expect_bucket(expected, queued, 9, 9);
    verify(expected);
    if (adaptive) {
        // Leave a busy view unfinished across the same free/init entry points used by asset swaps.
        eod_rq_0034C9C0();
        submit_model(0);
        require(MEM_LW(kPoolCount) > kGuestLimit, "unfinished view has pending overflow");
        f_0034D088();
        require(MEM_LW(kPoolCount) == 0 && MEM_LW(kPoolPointer) == 0, "teardown resets the count and frees the pool");
        flush(0, 1);
        flush(9, 1);
        verify(expected);
        f_0034D1F8();
        flush(0, 1);
        flush(9, 1);
        verify(expected);

        // Exercise idle resets and growth again in a standalone submission on the new guest pool.
        for (int i = 0; i < 65; ++i) {
            eod_rq_0034C9C0();
            eod_rq_0034C930();
            eod_rq_0034C860();
        }
        count = 0;
        submit_model(0);
        expect_model(immediate, 0, kModelMatrix, kMaterial, false, count, limit);
        expect_immediate(expected, immediate);
        verify(expected);
        require(MEM_LW(kPoolCount) == count, "new lifetime can grow from a standalone draw");
    }
    std::printf("%s fixture emitted all %zu expected records\n", argv[1], expected.size());
}
