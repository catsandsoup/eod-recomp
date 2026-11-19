// diff_harness.cpp - see diff_harness.h.
#include "diff_harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "ee.h"
#include "memory.h"

namespace {

struct Config {
    bool all = false;
    std::vector<uint32_t> addrs;
    uint64_t every = 1;
    uint32_t ram_low = 64u << 20;
    bool on = false;
    std::map<uint32_t, uint32_t> replay;  // EOD_DIFF_REPLAY=<hex addr>:<n>,...: extra checks per checked call
};

const Config &config() {
    static const Config c = [] {
        Config c;
        const char *e = std::getenv("EOD_DIFF");
        if (!e || !*e) return c;
        c.on = true;
        if (!std::strcmp(e, "all")) {
            c.all = true;
        } else {
            for (const char *p = e; *p;) {
                char *end;
                c.addrs.push_back((uint32_t)std::strtoul(p, &end, 16));
                p = *end ? end + 1 : end;
            }
        }
        if (const char *n = std::getenv("EOD_DIFF_EVERY")) c.every = std::strtoull(n, nullptr, 10) ? std::strtoull(n, nullptr, 10) : 1;
        if (const char *r = std::getenv("EOD_DIFF_REPLAY")) {
            for (const char *p = r; *p;) {
                char *end;
                const uint32_t a = (uint32_t)std::strtoul(p, &end, 16);
                uint32_t n = 0;
                if (*end == ':') n = (uint32_t)std::strtoul(end + 1, &end, 10);
                if (n) c.replay[a] = n;
                while (*end && *end != ',') end++;
                p = *end ? end + 1 : end;
            }
        }
        if (const char *m = std::getenv("EOD_DIFF_RAM_MB")) {
            const uint64_t mb = std::strtoull(m, nullptr, 10);
            if (mb && (mb << 20) <= EE_RAM_SIZE) c.ram_low = (uint32_t)(mb << 20);
        }
        return c;
    }();
    return c;
}

bool watched(uint32_t addr) {
    const Config &c = config();
    if (!c.on) return false;
    if (c.all) return true;
    for (uint32_t a : c.addrs)
        if (a == addr) return true;
    return false;
}

// Everything a call can change that the harness restores between the two runs.
struct Snapshot {
    EEState ee;
    VUState vu0;
    std::vector<uint8_t> vu0_mem;
    uint8_t spr[0x4000];
    std::vector<uint8_t> low;    // [0, ram_low)
    uint32_t stack_from = 0;     // [stack_from, EE_RAM_SIZE): caller frames and above
    std::vector<uint8_t> stack;

    void take(uint32_t ram_low, uint32_t sp) {
        ee = g_ee;
        vu0 = g_vu0;
        vu0_mem.assign(g_vu0.mem, g_vu0.mem + g_vu0.mem_mask + 1);
        std::memcpy(spr, g_scratchpad, sizeof spr);
        low.assign(g_ram, g_ram + ram_low);
        // dead stack below $sp may be scribbled; keep 256 KB under it so it is restored
        stack_from = sp >= 0x40000 ? (sp - 0x40000) & ~0xFu : 0;
        if (stack_from < ram_low) stack_from = ram_low;
        stack.assign(g_ram + stack_from, g_ram + EE_RAM_SIZE);
    }
    void restore() const {
        g_ee = ee;
        uint8_t *mem = g_vu0.mem;
        g_vu0 = vu0;
        g_vu0.mem = mem;
        std::memcpy(g_vu0.mem, vu0_mem.data(), vu0_mem.size());
        std::memcpy(g_scratchpad, spr, sizeof spr);
        std::memcpy(g_ram, low.data(), low.size());
        std::memcpy(g_ram + stack_from, stack.data(), stack.size());
    }
};

struct Stats {
    uint64_t calls = 0, checked = 0, mismatched = 0, replayed = 0;
};
std::map<uint32_t, Stats> &stats() {
    static std::map<uint32_t, Stats> s;
    return s;
}

void report_at_exit() {
    for (const auto &[a, s] : stats())
        std::fprintf(stderr, "[diff] %08x: %llu calls, %llu checked (%llu replayed), %llu mismatched\n", a,
                     (unsigned long long)s.calls, (unsigned long long)s.checked, (unsigned long long)s.replayed,
                     (unsigned long long)s.mismatched);
}

int compare(uint32_t addr, const Snapshot &orig, uint32_t caller_sp, unsigned flags) {
    std::vector<int> kGpr = {2, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
    std::vector<int> kFpr = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
    if (flags & kDiffRet64) kGpr.push_back(3);
    if (flags & kDiffFloatRet) kFpr.push_back(0);
    int bad = 0;
    auto note = [&](const std::string &s) {
        if (bad++ < 12) std::fprintf(stderr, "[diff] %08x: %s\n", addr, s.c_str());
    };
    char t[160];
    for (int r : kGpr)
        if (std::memcmp(&g_ee.r[r], &orig.ee.r[r], 16)) {
            std::snprintf(t, sizeof t, "r%d new %016llx%016llx orig %016llx%016llx", r, (unsigned long long)g_ee.r[r].ud[1],
                          (unsigned long long)g_ee.r[r].ud[0], (unsigned long long)orig.ee.r[r].ud[1],
                          (unsigned long long)orig.ee.r[r].ud[0]);
            note(t);
        }
    for (int f : kFpr)
        if (std::memcmp(&g_ee.fpr[f], &orig.ee.fpr[f], sizeof(FReg))) {
            uint32_t a, b;
            std::memcpy(&a, &g_ee.fpr[f], 4), std::memcpy(&b, &orig.ee.fpr[f], 4);
            std::snprintf(t, sizeof t, "f%d new %08x orig %08x", f, a, b);
            note(t);
        }
    if (std::memcmp(g_vu0.vf, orig.vu0.vf, sizeof g_vu0.vf) || std::memcmp(g_vu0.vi, orig.vu0.vi, 16 * sizeof(uint32_t)))
        note("VU0 registers differ");
    if (std::memcmp(g_vu0.mem, orig.vu0_mem.data(), orig.vu0_mem.size())) note("VU0 memory differs");
    for (uint32_t i = 0; i < sizeof orig.spr; i += 4)
        if (std::memcmp(g_scratchpad + i, orig.spr + i, 4)) {
            std::snprintf(t, sizeof t, "scratchpad +%04x new %08x orig %08x", i, *(const uint32_t *)(g_scratchpad + i),
                          *(const uint32_t *)(orig.spr + i));
            note(t);
        }
    auto ram = [&](uint32_t base, const std::vector<uint8_t> &ref) {
        const uint32_t n = (uint32_t)ref.size();
        for (uint32_t off = 0; off < n; off += 4096) {
            const uint32_t len = n - off < 4096 ? n - off : 4096;
            if (!std::memcmp(g_ram + base + off, ref.data() + off, len)) continue;
            for (uint32_t i = 0; i < len; i += 4) {
                const uint32_t a = base + off + i;
                if (a < caller_sp && a + 0x40000 >= caller_sp) continue;  // dead stack below the caller's $sp
                uint32_t x, y;
                std::memcpy(&x, g_ram + a, 4), std::memcpy(&y, ref.data() + off + i, 4);
                if (x == y) continue;
                std::snprintf(t, sizeof t, "ram %08x new %08x orig %08x", a, x, y);
                note(t);
            }
        }
    };
    ram(0, orig.low);
    ram(orig.stack_from, orig.stack);
    return bad;
}

}  // namespace

// Inside a comparison, nested wrapped calls follow the side being run.
static enum class Side { kNone, kOriginal, kReplacement } g_side = Side::kNone;

bool diff_in_original_side() { return g_side == Side::kOriginal; }
static std::vector<void (*)()> &exit_reports() {
    static std::vector<void (*)()> v;
    return v;
}
void diff_add_exit_report(void (*fn)()) {
    for (auto f : exit_reports())
        if (f == fn) return;
    exit_reports().push_back(fn);
}
void diff_report() {
    report_at_exit();
    for (auto f : exit_reports()) f();
}

void diff_run(uint32_t addr, void (*replacement)(void), void (*original)(void), unsigned flags) {
    if (g_side == Side::kOriginal) {
        original();
        return;
    }
    if (g_side == Side::kReplacement || !watched(addr)) {
        replacement();
        return;
    }
    Stats &s = stats()[addr];
    static bool hooked = false;
    if (!hooked) hooked = true, std::atexit(report_at_exit);
    if (s.calls++ % config().every != 0) {
        replacement();
        return;
    }
    const uint32_t sp = g_ee.r[29].uw[0];
    static Snapshot before, after_orig;  // reused: no per-call allocation after the first
    // Interrupts wait until both runs are done: the recompiled code delivers them at loop
    // back-edges, the replacement at other points, and a handler would make the runs differ.
    g_ee_irq_hold++;
    before.take(config().ram_low, sp);
    // EOD_DIFF_REPLAY: the recorded entry state (registers, RAM, scratchpad, VU0) is run again through
    // both sides `n` more times; every run is compared like the live one. Host-side state of the
    // replacement (counters, limit notes) advances once per run.
    const auto rp = config().replay.find(addr);
    const uint32_t runs = 1 + (rp == config().replay.end() ? 0 : rp->second);
    for (uint32_t run = 0; run < runs; run++) {
        if (run) before.restore();
        g_side = Side::kOriginal;
        original();
        g_side = Side::kNone;
        after_orig.take(config().ram_low, sp);
        after_orig.stack_from = before.stack_from;
        after_orig.stack.assign(g_ram + before.stack_from, g_ram + EE_RAM_SIZE);
        before.restore();
        g_side = Side::kReplacement;
        replacement();
        g_side = Side::kNone;
        s.checked++;
        if (run) s.replayed++;
        if (compare(addr, after_orig, sp, flags)) s.mismatched++;
        if ((s.checked & (s.checked - 1)) == 0 || s.checked % 1024 == 0)  // 1, 2, 4, ... then every 1024 (the game may exit without atexit)
            std::fprintf(stderr, "[diff] %08x: %llu calls, %llu checked (%llu replayed), %llu mismatched\n", addr,
                         (unsigned long long)s.calls, (unsigned long long)s.checked, (unsigned long long)s.replayed,
                         (unsigned long long)s.mismatched);
    }
    after_orig.restore();  // the game continues from the original's results
    g_ee_irq_hold--;
}
