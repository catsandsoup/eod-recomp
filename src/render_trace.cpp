// render_trace.cpp - EOD_RENDER_TRACE (diagnostic): runtime evidence for the EE 3D submission path in
// docs/EE_RENDER_SUBMISSION.md. The wrappers only observe; the queue functions themselves run as the
// readable C++ in src/render_queue.cpp (eod_rq_*), the others as recompiled.
//
//   EOD_RENDER_TRACE=<vblank>[,<vblank>...]  log every submission/flush event in those vblanks, in order
//   EOD_RENDER_TRACE_EVERY=<n>               per-vblank counters every n vblanks (default 0: off)
//
// Checks it reports: whether batch emission (0x35CB88) ever happens outside a flush (0x34AF98), whether
// queued records equal flushed records, and which callers queue through 0x34BDA8 / 0x34BF90.
#include "core/cpu.h"
#include "core/memory.h"
#include "kernel/kernel.h"
#include "render/native_renderer.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

extern uint32_t g_vu1_program_starts[0x800];  // runtime/vu/vif.cpp
extern void (*g_vu1_start_hook)(uint32_t addr);  // runtime/vu/vif.cpp
extern uint32_t g_vif1_dma_tag;                  // runtime/hw/hw.cpp: DMA tag VIF1 is reading

extern "C" {
void f_0034C9C0_orig(void);  // FrameBegin
void f_0034C930_orig(void);  // flush layers 0..8
void f_0034C860_orig(void);  // flush layer 9 and reset the pool
void f_0034AF98_orig(void);  // Flush(layer, set)
void f_0034BDA8_orig(void);  // queue (model, m, material, flags)
void f_0034BF90_orig(void);  // queue without material (model, m, layer)
void eod_r4_0034C208(void);  // projected shadow (src/native_shadows_flares.cpp)
void eod_r4_0034C4B0(void);  // projected shadow (layer mask, src/native_shadows_flares.cpp)
void f_0035CB88_orig(void);  // emit one batch
void f_0035B3D0_orig(void);  // camera packet (VU1 q1..16)
void f_0035CD28_orig(void);  // matrix packet (TOPS+0..6)
void eod_r4_0035D298(void);  // shadow batch emit (src/native_shadows_flares.cpp)
void f_0034B140_orig(void);  // append a model's batches to the queue (from 0x34BDA8 and 0x34CC98)
void f_00366C88_orig(void);  // PacketReserveTop(qwc), packet manager *(0x3E5978)
void f_00366D30_orig(void);  // PacketReserve(qwc), packet manager *(0x3E5978); 0x366CD0 commits
void f_00366D78_orig(void);  // PacketReserve(qwc), packet manager *(0x3E597C)
void f_00366DD8_orig(void);  // PacketReserve(qwc), packet manager *(0x3E597C)
// Readable replacements (src/render_queue.cpp), checked against the originals with EOD_DIFF.
void eod_rq_0034C9C0(void);
void eod_rq_0034C930(void);
void eod_rq_0034C860(void);
void eod_rq_0034AF98(void);
void eod_rq_0034BDA8(void);
void eod_rq_0034BF90(void);
void eod_rq_0034B140(void);
}

namespace {

constexpr uint32_t kPoolCount = 0x4C96C8;

std::set<uint64_t> g_frames;
uint64_t g_every = 0;
bool g_on = false;

struct Counts {
    uint64_t vb = ~0ull;
    unsigned begin = 0, flush = 0, q1 = 0, q1_kept = 0, q2 = 0, q2_kept = 0, shadow = 0, batch = 0,
             batch_outside = 0, cam = 0, mat = 0, shadow_batch = 0, app = 0, app_records = 0, app_world = 0,
             app_world_records = 0;
} g_c;
int g_in_flush = 0;

// ---- VU1 program-start attribution (milestone M1, docs/NATIVE_RENDERER_SUCCESS.md) ----------------
// Every packet the EE reserves (0x366C88, 0x366D30, 0x366D78, 0x366DD8) is tagged with the reserving
// call's return address and the emitter it ran under (batch, shadow, camera, matrix). When VIF1
// starts a VU1 program, the DMA tag it was reading (runtime/hw/hw.cpp) is looked up: the start is
// attributed when that tag lies in a tagged packet. Model data a tag REFs is attributed through the
// tag, so the same data drawn as a model and as a shadow is told apart.
const char *g_ctx = "-";
struct Ctx {
    const char *prev;
    explicit Ctx(const char *c) : prev(g_ctx) { g_ctx = c; }
    ~Ctx() { g_ctx = prev; }
};
struct Sender {
    uint32_t ra;
    const char *ctx;
    bool operator<(const Sender &o) const { return ra != o.ra ? ra < o.ra : std::strcmp(ctx, o.ctx) < 0; }
};
std::map<Sender, uint32_t> g_sender_ids;
std::vector<Sender> g_senders;
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_ranges;  // start -> (end, sender id); latest writer wins

void tag_range(uint32_t start, uint32_t bytes, uint32_t reserve_ra) {
    if (!bytes) bytes = 16;
    const uint32_t end = start + bytes;
    const Sender key{reserve_ra, g_ctx};
    auto [sit, added] = g_sender_ids.try_emplace(key, (uint32_t)g_senders.size());
    if (added) g_senders.push_back(key);
    const uint32_t id = sit->second;
    auto it = g_ranges.lower_bound(start);
    if (it != g_ranges.begin()) {
        auto prev = std::prev(it);
        if (prev->second.first > start) {
            const auto old = prev->second;
            prev->second.first = start;
            if (old.first > end) g_ranges[end] = old;
        }
    }
    it = g_ranges.lower_bound(start);
    while (it != g_ranges.end() && it->first < end) {
        if (it->second.first > end) {
            const auto old = it->second;
            g_ranges.erase(it);
            g_ranges[end] = old;
            break;
        }
        it = g_ranges.erase(it);
    }
    g_ranges[start] = {end, id};
}

std::map<std::pair<uint32_t, uint32_t>, unsigned> g_vu1_by_sender;  // (sender id, program) -> starts
std::map<std::pair<uint32_t, uint32_t>, unsigned> g_vu1_unknown;    // (tag address, program) -> starts
unsigned g_vu1_total = 0, g_vu1_attributed = 0;

void on_vu1_start(uint32_t program) {
    g_vu1_total++;
    const uint32_t tag = g_vif1_dma_tag;
    auto it = g_ranges.upper_bound(tag);
    if (it != g_ranges.begin()) {
        --it;
        if (tag < it->second.first) {
            g_vu1_attributed++;
            g_vu1_by_sender[{it->second.second, program}]++;
            return;
        }
    }
    g_vu1_unknown[{tag, program}]++;
}
std::map<uint32_t, unsigned> g_q1_callers, g_q2_callers;

const bool g_init = [] {
    if (const char *e = std::getenv("EOD_RENDER_TRACE")) {
        for (const char *p = e; *p;) {
            char *end;
            const unsigned long long v = std::strtoull(p, &end, 10);
            if (end == p) break;
            g_frames.insert(v);
            p = *end == ',' ? end + 1 : end;
        }
    }
    if (const char *e = std::getenv("EOD_RENDER_TRACE_EVERY")) g_every = std::strtoull(e, nullptr, 10);
    g_on = !g_frames.empty() || g_every;
    if (g_on) g_vu1_start_hook = on_vu1_start;
    return true;
}();

uint32_t arg(int i) { return g_ee.r[4 + i].uw[0]; }
uint32_t ra() { return g_ee.r[31].uw[0]; }
uint32_t pool() { return MEM_LW(kPoolCount); }
float MEM_LF(uint32_t a) {
    const uint32_t w = MEM_LW(a);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

void report(const Counts &c) {
    std::fprintf(stderr,
                 "[rtrace] vb %llu begin %u flush %u queue34BDA8 %u (kept %u) queue34BF90 %u (kept %u) shadow %u "
                 "shadow_batches %u batches %u (outside flush %u) camera_pkts %u matrix_pkts %u append %u (records %u) "
                 "append_from_34CC98 %u (records %u)\n",
                 (unsigned long long)c.vb, c.begin, c.flush, c.q1, c.q1_kept, c.q2, c.q2_kept, c.shadow,
                 c.shadow_batch, c.batch, c.batch_outside, c.cam, c.mat, c.app, c.app_records, c.app_world,
                 c.app_world_records);
    std::string s;
    for (auto &kv : g_q1_callers) { char t[32]; std::snprintf(t, sizeof t, " %08x:%u", kv.first, kv.second); s += t; }
    std::fprintf(stderr, "[rtrace] vb %llu 34BDA8 callers(ra):%s\n", (unsigned long long)c.vb, s.c_str());
    s.clear();
    for (auto &kv : g_q2_callers) { char t[32]; std::snprintf(t, sizeof t, " %08x:%u", kv.first, kv.second); s += t; }
    std::fprintf(stderr, "[rtrace] vb %llu 34BF90 callers(ra):%s\n", (unsigned long long)c.vb, s.c_str());
    // VU1 program starts since the previous trace frame, by program address (runtime/vu/vif.cpp)
    s.clear();
    for (uint32_t i = 0; i < 0x800; i++)
        if (g_vu1_program_starts[i]) {
            char t[32];
            std::snprintf(t, sizeof t, " %04x:%u", i * 8, g_vu1_program_starts[i]);
            s += t;
        }
    std::fprintf(stderr, "[rtrace] vb %llu vu1 programs:%s\n", (unsigned long long)c.vb, s.c_str());
    // split-screen state (1P/2P comparison): track-pass gate, view count, 0x212DD0 source, 0x495D68
    std::fprintf(stderr, "[rtrace] vb %llu views: gate3D1B0C %u viewcount3D1A74 %u v495D64 %08x v495D68 %u v495D58 %08x\n",
                 (unsigned long long)c.vb, MEM_LW(0x3D1B0C), MEM_LW(0x3D1A74), MEM_LW(0x495D64), MEM_LW(0x495D68),
                 MEM_LW(0x495D58));
    std::fprintf(stderr, "[rtrace] vb %llu vu1 starts %u attributed %u unattributed %u\n", (unsigned long long)c.vb,
                 g_vu1_total, g_vu1_attributed, g_vu1_total - g_vu1_attributed);
    std::map<uint32_t, std::string> per;
    for (const auto &[k, n] : g_vu1_by_sender) {
        char t[32];
        std::snprintf(t, sizeof t, " %04x:%u", k.second, n);
        per[k.first] += t;
    }
    for (const auto &[id, list] : per)
        std::fprintf(stderr, "[rtrace] vb %llu vu1 sender %s reserve_ra %08x:%s\n", (unsigned long long)c.vb,
                     g_senders[id].ctx, g_senders[id].ra, list.c_str());
    unsigned shown = 0;
    for (const auto &[k, n] : g_vu1_unknown)
        if (shown++ < 24)
            std::fprintf(stderr, "[rtrace] vb %llu vu1 unattributed tag %08x program %04x: %u\n",
                         (unsigned long long)c.vb, k.first, k.second, n);
}

// A trace frame runs from the first FrameBegin of a vblank to the next one in a later vblank (the game
// renders every other vblank in races; split screen opens one frame per viewport in the same vblank).
bool g_log = false;
uint64_t g_last_bucket = ~0ull;

void roll() {
    const uint64_t vb = kernel_vblank_count();
    if (vb == g_c.vb) return;
    if (g_c.vb != ~0ull) {
        if (g_log || (g_every && g_c.vb / g_every != g_last_bucket)) {
            report(g_c);
            if (g_every) g_last_bucket = g_c.vb / g_every;
        }
    }
    g_c = Counts{};
    g_c.vb = vb;
    std::memset(g_vu1_program_starts, 0, sizeof g_vu1_program_starts);
    g_vu1_by_sender.clear();
    g_vu1_unknown.clear();
    g_vu1_total = g_vu1_attributed = 0;
    g_q1_callers.clear();
    g_q2_callers.clear();
    g_log = false;
    if (!g_frames.empty() && *g_frames.begin() <= vb) {
        g_log = true;
        while (!g_frames.empty() && *g_frames.begin() <= vb) g_frames.erase(g_frames.begin());
    }
}

bool tick() { return g_log; }

#define EV(...) do { if (log) std::fprintf(stderr, "[rtrace] vb %llu " __VA_ARGS__); } while (0)
#define VB (unsigned long long)g_c.vb

}  // namespace

extern "C" void f_0034C9C0(void) {
    if (!g_on) return eod_rq_0034C9C0();
    roll();
    const bool log = tick();
    g_c.begin++;
    EV("FrameBegin ra %08x pool %u\n", VB, ra(), pool());
    eod_rq_0034C9C0();
}

extern "C" void f_0034C930(void) {
    if (!g_on) return eod_rq_0034C930();
    const bool log = tick();
    EV("FlushLayers0-8 ra %08x pool %u\n", VB, ra(), pool());
    eod_rq_0034C930();
}

extern "C" void f_0034C860(void) {
    if (!g_on) return eod_rq_0034C860();
    const bool log = tick();
    EV("FlushLayer9+Reset ra %08x pool %u\n", VB, ra(), pool());
    eod_rq_0034C860();
}

extern "C" void f_0034AF98(void) {
    if (!g_on) return eod_rq_0034AF98();
    const bool log = tick();
    const uint32_t layer = arg(0), set = arg(1), r = ra();
    const unsigned before = g_c.batch;
    g_c.flush++;
    g_in_flush++;
    eod_rq_0034AF98();
    g_in_flush--;
    if (g_c.batch != before) EV("Flush layer %d set %u ra %08x batches %u\n", VB, (int)layer, set, r, g_c.batch - before);
}

extern "C" void f_0034BDA8(void) {
    if (!g_on) return eod_rq_0034BDA8();
    const bool log = tick();
    const uint32_t r = ra(), model = arg(0), mat = arg(2), flags = arg(3), p0 = pool();
    eod_rq_0034BDA8();
    g_c.q1++;
    g_q1_callers[r]++;
    if (pool() != p0) g_c.q1_kept++;
    EV("Queue34BDA8 ra %08x model %u material %d flags %08x records +%u\n", VB, r, model, (int)mat, flags, pool() - p0);
}

extern "C" void f_0034BF90(void) {
    if (!g_on) return eod_rq_0034BF90();
    const bool log = tick();
    const uint32_t r = ra(), model = arg(0), layer = arg(2), p0 = pool();
    eod_rq_0034BF90();
    g_c.q2++;
    g_q2_callers[r]++;
    if (pool() != p0) g_c.q2_kept++;
    EV("Queue34BF90 ra %08x model %u layer %d records +%d\n", VB, r, model, (int)layer, (int)(pool() - p0));
}

extern "C" void f_0034C208(void) {
    if (!g_on) return eod_r4_0034C208();
    const bool log = tick();
    g_c.shadow++;
    EV("Shadow34C208 ra %08x model %u\n", VB, ra(), arg(1));
    eod_r4_0034C208();
}

extern "C" void f_0034C4B0(void) {
    if (!g_on) return eod_r4_0034C4B0();
    const bool log = tick();
    g_c.shadow++;
    EV("Shadow34C4B0 ra %08x model %u mask %08x\n", VB, ra(), arg(1), arg(3));
    eod_r4_0034C4B0();
}

extern "C" void f_0035CB88(void) {
    if (!g_on) return f_0035CB88_orig();
    tick();
    g_c.batch++;
    const Ctx ctx("batch");
    if (!g_in_flush) {
        g_c.batch_outside++;
        std::fprintf(stderr, "[rtrace] vb %llu batch emitted outside a flush: ra %08x batch %08x\n", VB, ra(), arg(0));
    }
    f_0035CB88_orig();
}

extern "C" void f_0035B3D0(void) {
    if (!g_on) return f_0035B3D0_orig();
    const bool log = tick();
    g_c.cam++;
    const Ctx ctx("camera");
    EV("CameraPacket ra %08x near %g far %g viewport %g,%g %gx%g\n", VB, ra(), (double)MEM_LF(0x4D7C10),
       (double)MEM_LF(0x4D7C14), (double)MEM_LF(0x4D7C20), (double)MEM_LF(0x4D7C24), (double)MEM_LF(0x4D7C28),
       (double)MEM_LF(0x4D7C2C));
    f_0035B3D0_orig();
}

extern "C" void f_0035CD28(void) {
    if (!g_on) return f_0035CD28_orig();
    tick();
    g_c.mat++;
    const Ctx ctx("matrix");
    f_0035CD28_orig();
}

extern "C" void f_0035D298(void) {
    if (!g_on) return eod_r4_0035D298();
    tick();
    g_c.shadow_batch++;
    const Ctx ctx("shadow");
    eod_r4_0035D298();
}

extern "C" void f_0034B140(void) {
    if (!g_on) return eod_rq_0034B140();
    const bool log = tick();
    const uint32_t r = ra(), model = arg(1), p0 = pool();
    eod_rq_0034B140();
    const uint32_t n = pool() - p0;  // 0 also when it flushed at once (no frame open)
    g_c.app++;
    g_c.app_records += n;
    if (r == 0x0034CCFC) {  // return address of the call in 0x34CC98
        g_c.app_world++;
        g_c.app_world_records += n;
        EV("Append34CC98 model %u records +%u\n", VB, model, n);
    }
}

// Packet reserves: tag the returned space with the reserving call (VU1 start attribution), and tell
// the native renderer (render/native_renderer.h), which tags the batch packets of queue records.
#define RESERVE_WRAPPER(addr)                                                     \
    extern "C" void f_##addr(void) {                                             \
        const bool nat = native::enabled();                                       \
        if (!g_on && !nat) return f_##addr##_orig();                              \
        const uint32_t qwc = arg(0), r = ra();                                    \
        f_##addr##_orig();                                                        \
        if (g_on && g_ee.r[2].uw[0]) tag_range(g_ee.r[2].uw[0], qwc * 16, r);     \
        if (nat) native::ee_packet_reserved(g_ee.r[2].uw[0], qwc, r);             \
    }
RESERVE_WRAPPER(00366C88)
RESERVE_WRAPPER(00366D30)
RESERVE_WRAPPER(00366D78)
RESERVE_WRAPPER(00366DD8)
