// debris_pool.cpp - the detached wheel and panel phob pools (SLUS_209.10), readable C++ replacing
// the recompiled owners. Stage R3 Phase A2 of the native renderer (docs/NATIVE_RENDERER.md "Debris
// phob pools"; docs/PS2_LIMITS.md).
//
//   wheels  array 0x484E68 (16 words), count 0x484EA8, ring index 0x484EAC
//     0x1B3068 DetachedWheelSpawn(car index, wheel, matrix, velocity, position): takes the ring
//              slot whether or not its wheel is still flying (caller 0x18CA98 at 0x18D680)
//     0x1B3180 / 0x1B3190 scene teardown: count = 0 (and ring index = 0)
//   panels  array 0x494D60 (16 words), count 0x494DA0
//     0x1E1E88 'Panl' factory / ctor: allocates 0x264 bytes, appends to the array
//     0x1E29C0 dtor (vtable 0x400EE0 +0xC): count - 1, frees +0x200, base dtor 0x17DA48
//     0x1E0DC8 DetachedPanelSpawn (caller 0x18CA98 at 0x18D1C8): first phob with +0x1DC = 0, else
//              the oldest spawn (+0x204); still recompiled, wrapped here to count live use
//
// Both kinds are scene objects made by 0x2100C0 through the create stream (0x1758B8 -> 0x1783E0);
// the arrays hold 16 words and the scene makes fewer (12 panels, 0x210444..0x2105A4; 5 wheels
// measured). On the PS2 a spawn past them takes a live phob (the wheel ring its next slot, the
// panel spawn the oldest). ExpandDebris (offline) keeps the array as a 16-word window and moves a
// live phob about to be taken to a host reserve, putting a free one (reused, else made as the scene
// makes them) in its slot: nothing in flight or on the ground is taken. [debris] lines and
// limit_note report live use. As src/car_damage.cpp: every replacement makes the same guest calls
// in the same order with the same return addresses and arguments, the same memory writes and
// leaves the same $v0, so EOD_DIFF proves it against the recompiled original.
#include "core/cpu.h"
#include "core/diff_harness.h"
#include "core/limit_tracker.h"
#include "core/config.h"
#include "core/memory.h"
#include "netplay_hooks.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>
#include <vector>

extern "C" {
void f_001B3068_orig(void);
void f_001B3180_orig(void);
void f_001B3190_orig(void);
void f_001E1E88_orig(void);
void f_001E29C0_orig(void);
void f_001E0DC8_orig(void);
void f_001E1E88(void);  // the 'Panl' factory, through its diff_run wrapper below
void f_00179A08(void);  // CarGetByParticipantIndex(index)
void f_001B2D58(void);  // WheelPhobLaunch(phob, matrix, velocity, position, car+0x94)
void f_0012E870(void);  // MemAlloc(size, name)
void f_0017D898(void);  // PhobConstruct(phob)
void f_001E6C10(void);  // panel trail emitter (4, 6, 2)
void f_001E3150(void);  // its release
void f_0017DA48(void);  // PhobDestruct(phob, flags)
void f_00179158(void);  // 'WHLP' factory: MemAlloc(0x1EC) + ctor 0x1B3028
void f_00178378(void);  // PhobListAdd(phob): phob_list 0x447404, then vtable+0x3C
}

namespace {

inline uint32_t reg(int r) { return g_ee.r[r].uw[0]; }
inline void set(int r, int32_t v) { g_ee.r[r].sd[0] = v; }
inline int32_t lw(uint32_t a) { return (int32_t)MEM_LW(a); }
inline uint32_t lwu(uint32_t a) { return MEM_LW(a); }
inline void sw(uint32_t a, uint32_t v) { MEM_SW(a, v); }
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
    explicit Frame(uint32_t size) : caller_sp(reg(29)), ra(g_ee.r[31].ud[0]) { set(29, (int32_t)(caller_sp - size)); }
    ~Frame() {
        set(29, (int32_t)caller_sp);
        g_ee.r[31].ud[0] = ra;
    }
};

// guest pools (PS2 EE addresses, lui/addiu in the owners above)
constexpr uint32_t kWheels = 0x484E68, kWheelCount = 0x484EA8, kWheelNext = 0x484EAC;
constexpr uint32_t kPanels = 0x494D60, kPanelCount = 0x494DA0;
constexpr uint32_t kPoolWords = 16;        // 0x484E68..0x484EA4, 0x494D60..0x494D9C
constexpr uint32_t kRaceClock = 0x3CDACC;  // g_RaceClockSeconds (0x1B30FC)
constexpr uint32_t kPanelSize = 0x264, kPanelName = 0x400ED8, kPanelVtable = 0x400EE0;  // 0x1E1E94..0x1E1EC0

// ---- live use and the expanded pools (host only) ----
// A phob is live from its spawn until +0x1DC is cleared (wheels: 0x1B2DD8; panels keep +0x1DC =
// their car until they are taken again). The guest arrays stay a 16-word window the recompiled
// readers (0x1E0DC8, 0x1B2868) see; with ExpandDebris every other phob sits in `reserve`.
constexpr uint32_t kPhobList = 0x447408;        // phob_list count; base *(0x447404)
constexpr uint32_t kPhobListSize = 0x800 / 4;   // MemAlloc(0x800) at 0x1769A8: 512 entries
constexpr uint32_t kHeapPermit = 0x3CC458;      // MemAlloc 0x12E870 panics if 0 while 0x3CC460 locks
constexpr uint32_t kPanelRecord = 0x50616E6C, kPanelRecordSize = 8;  // 'Panl', 8 (0x2100C0 at 0x210444)

struct Pool {
    const char *name;
    uint32_t array, count;
    uint64_t spawns = 0, steals = 0, peak = 0, made = 0;
    std::vector<uint32_t> reserve;      // phobs outside the window, live or free
    std::unordered_set<uint32_t> ours;  // phobs this file created (not counted by the guest)
};
Pool g_wheel_use{"detached wheel debris (0x484E68)", kWheels, kWheelCount};
Pool g_panel_use{"detached panel debris (0x494D60)", kPanels, kPanelCount};
std::vector<uint8_t> g_wheel_record;  // the 'WHLP' create record the scene's wheels were made from

bool expanded() {
    const bool online = netplay_hooks::race_rules && netplay_hooks::race_rules() >= 0;
    return g_config.expand_debris && !online;
}

uint32_t live_in(const Pool &p) {
    uint32_t live = 0;
    const int32_t count = lw(p.count);
    for (int32_t i = 0; i < count && i < (int32_t)kPoolWords; i++) {
        const uint32_t phob = lwu(p.array + 4 * (uint32_t)i);
        if (phob && lwu(phob + 0x1DC) != 0) live++;
    }
    for (uint32_t phob : p.reserve)
        if (lwu(phob + 0x1DC) != 0) live++;
    return live;
}

// `demand` = the phobs that would be live with no cap; `stole` = a live phob was taken.
void note_use(Pool &u, uint32_t demand, bool stole) {
    u.spawns++;
    if (stole) u.steals++;
    const bool peak = demand > u.peak;
    if (peak) u.peak = demand;
    const uint32_t window = (uint32_t)lw(u.count);
    limit_note(u.name, demand, expanded() ? 0 : window, kPoolWords);
    if (peak || stole || (u.spawns & (u.spawns - 1)) == 0)
        std::fprintf(stderr,
                     "[debris] %s: %llu spawns, live now %u, peak %llu (scene phobs %u, PS2 array %u), %llu made, %llu steals of a live phob%s\n",
                     u.name, (unsigned long long)u.spawns, demand, (unsigned long long)u.peak, window, kPoolWords,
                     (unsigned long long)u.made, (unsigned long long)u.steals, expanded() ? "" : " (PS2 pool)");
}

// A new phob made as the scene makes them (0x1783E0: factory, vtable+8 Init(phob, record),
// 0x178378 into phob_list), mid-race. The factory / Init append at the guest count, so the count
// is parked on slot 0 and restored. Returns 0 when phob_list is full.
uint32_t create_phob(Pool &p, bool wheel, uint32_t ra) {
    if ((uint32_t)lw(kPhobList) >= kPhobListSize) return 0;
    if (wheel && g_wheel_record.size() < 8) return 0;
    const EEState saved = g_ee;
    const uint32_t permit = lwu(kHeapPermit), slot0 = lwu(p.array), count = lwu(p.count);
    sw(kHeapPermit, 1);
    sw(p.count, 0);
    call(wheel ? f_00179158 : f_001E1E88, ra);
    const uint32_t phob = reg(2);
    if (phob) {
        const uint32_t size = wheel ? (uint32_t)g_wheel_record.size() : kPanelRecordSize;
        const uint32_t rec = (reg(29) - size - 0x10) & ~0xFu;
        if (wheel) {
            for (uint32_t i = 0; i < size; i++) MEM_SB(rec + i, g_wheel_record[i]);
        } else {
            sw(rec, kPanelRecord), sw(rec + 4, kPanelRecordSize);
        }
        set(29, (int32_t)(rec - 0x10));
        set(4, (int32_t)phob), set(5, (int32_t)rec);
        call_at(lwu(lwu(phob + 0x98) + 8), ra);  // 0x1B2868 (0x3FCE50) / 0x1E28B8 (0x400EE8)
        set(4, (int32_t)phob);
        call(f_00178378, ra);
    }
    sw(p.array, slot0), sw(p.count, count), sw(kHeapPermit, permit);
    g_ee = saved;
    if (phob) p.ours.insert(phob), p.made++;
    return phob;
}

// The window slot `slot` holds a live phob that a spawn is about to take: move it to the reserve
// and put a free phob (a free one from the reserve, else a new one) in its place.
bool make_room(Pool &p, uint32_t slot, bool wheel, uint32_t ra) {
    const uint32_t live = lwu(p.array + 4 * slot);
    for (uint32_t &r : p.reserve)
        if (lwu(r + 0x1DC) == 0) {
            sw(p.array + 4 * slot, r);
            r = live;
            return true;
        }
    const uint32_t fresh = create_phob(p, wheel, ra);
    if (!fresh) return false;
    sw(p.array + 4 * slot, fresh);
    p.reserve.push_back(live);
    return true;
}

void release(Pool &p) {
    if (!p.reserve.empty() || !p.ours.empty())
        std::fprintf(stderr, "[debris] %s: scene ends, %zu reserve phobs and %zu made phobs released\n", p.name,
                     p.reserve.size(), p.ours.size());
    std::vector<uint32_t>().swap(p.reserve);
    std::unordered_set<uint32_t>().swap(p.ours);
}

// ---- 0x1B3068 ----
void wheel_spawn() {
    Frame fr(0xA0);
    const int32_t car_index = (int32_t)reg(4), wheel = (int32_t)reg(5);
    const uint32_t matrix = reg(6), velocity = reg(7), position = reg(8);
    const int32_t count = lw(kWheelCount);
    if (count <= 0) {
        g_ee.r[2].sd[0] = count;
        return;
    }
    const uint32_t phob = lwu(kWheels + 4 * (uint32_t)lw(kWheelNext));

    call(f_00179A08, 0x1B30D8);  // a0 is still the car index
    const uint32_t car = reg(2);
    set(4, (int32_t)phob), set(5, (int32_t)matrix), set(6, (int32_t)velocity), set(7, (int32_t)position);
    set(8, (int32_t)(car + 0x94));
    call(f_001B2D58, 0x1B30F4);
    const int32_t parts = lw(phob + 0x24);
    sw(phob + 0x1E0, (uint32_t)car_index);
    sw(phob + 0x1DC, lwu(kRaceClock));
    sw(phob + 0x1E4, (uint32_t)wheel);
    sw(phob + 0x20, (uint32_t)parts);
    const uint32_t model = parts > 0 ? lwu(phob + 0x10) : 0;
    const int32_t next = lw(kWheelNext) + 1;
    const bool wraps = !(next < lw(kWheelCount));
    sw(kWheelNext, (uint32_t)next);
    sw(model + 0x20, lwu(car + 0x1A00 + 0x54u * (uint32_t)wheel + 0x38));  // the hub radius
    if (wraps) sw(kWheelNext, 0);
    g_ee.r[2].sd[0] = next;
}

// ---- 0x1B3180 / 0x1B3190 (scene teardown) ----
// both leave $v0 (and 0x1B3190 $v1) = 0x480000, the lui of the pool page
void wheel_count_reset() {
    g_ee.r[2].sd[0] = 0x480000;
    sw(kWheelCount, 0);
}
void wheel_pool_reset() {
    g_ee.r[2].sd[0] = 0x480000, g_ee.r[3].sd[0] = 0x480000;
    sw(kWheelCount, 0);
    sw(kWheelNext, 0);
}

// ---- 0x1E1E88 ----
void panel_create() {
    Frame fr(0x20);
    set(4, (int32_t)kPanelSize), set(5, (int32_t)kPanelName);
    call(f_0012E870, 0x1E1EA4);
    const uint64_t phob64 = g_ee.r[2].ud[0];
    const uint32_t phob = (uint32_t)phob64;
    set(4, (int32_t)phob);
    g_ee.r[4].ud[0] = phob64;
    call(f_0017D898, 0x1E1EB0);
    const int32_t n = lw(kPanelCount);
    sw(kPanelCount, (uint32_t)(n + 1));
    sw(phob + 0x98, kPanelVtable);
    sw(phob + 0x1E0, 0xFFFFFFFFu);
    sw(kPanels + 4 * (uint32_t)n, phob);  // past 16 this runs into the count (0x494DA0), as on the PS2
    sw(phob + 0x1DC, 0);
    limit_note("panel debris phobs made by the scene (0x494D60)", (uint64_t)(n + 1), kPoolWords, kPoolWords);
    set(4, 4), set(5, 6), set(6, 2);
    call(f_001E6C10, 0x1E1EFC);
    sw(phob + 0x200, reg(2));
    g_ee.r[2].ud[0] = phob64;
}

// ---- 0x1E29C0 ----
void panel_destroy() {
    const uint64_t ra = g_ee.r[31].ud[0];
    const uint64_t phob64 = g_ee.r[4].ud[0], flags64 = g_ee.r[5].ud[0];
    const uint32_t phob = (uint32_t)phob64;
    const uint32_t sp = reg(29);
    set(29, (int32_t)(sp - 0x30));
    const int32_t n = lw(kPanelCount) - 1;
    sw(phob + 0x98, kPanelVtable);
    set(4, lw(phob + 0x200));
    sw(kPanelCount, (uint32_t)n);
    call(f_001E3150, 0x1E29FC);
    g_ee.r[4].ud[0] = phob64, g_ee.r[5].ud[0] = flags64;
    set(29, (int32_t)sp);
    g_ee.r[31].ud[0] = ra;  // j 0x17DA48: the base dtor returns to our caller
    f_0017DA48();
}

// Host bookkeeping (reserve, made phobs, counters) done while the original side of an enclosing
// EOD_DIFF comparison runs (0x18CA98 calls 0x1B3068 and 0x1E0DC8) is undone: the replacement side
// repeats the same guest work from the same state, and the guest keeps the original's results.
struct HostUndo {
    Pool &p;
    const bool active;
    std::vector<uint32_t> reserve;
    std::unordered_set<uint32_t> ours;
    uint64_t spawns, steals, peak, made;
    explicit HostUndo(Pool &pool)
        : p(pool), active(diff_in_original_side()), reserve(pool.reserve), ours(pool.ours), spawns(pool.spawns),
          steals(pool.steals), peak(pool.peak), made(pool.made) {}
    ~HostUndo() {
        if (!active) return;
        p.reserve = reserve, p.ours = ours;
        p.spawns = spawns, p.steals = steals, p.peak = peak, p.made = made;
    }
};

// The expansion runs before diff_run (outside any EOD_DIFF comparison), so both sides of a checked
// call see the same window and the replacement is still proven against the recompiled original.
void wheel_spawn_expanded() {
    if (expanded() && lw(kWheelCount) > 0) {
        const uint32_t slot = (uint32_t)lw(kWheelNext);  // the ring slot 0x1B3068 takes
        const uint32_t phob = lwu(kWheels + 4 * slot);
        if (phob && lwu(phob + 0x1DC) != 0) make_room(g_wheel_use, slot, true, 0x1B3068);
    }
    // live use, counted once per spawn here (not in the replacement, which EOD_DIFF_REPLAY reruns)
    if (lw(kWheelCount) > 0) {
        const uint32_t phob = lwu(kWheels + 4 * (uint32_t)lw(kWheelNext));
        note_use(g_wheel_use, live_in(g_wheel_use) + 1, lwu(phob + 0x1DC) != 0);
    }
    diff_run(0x001B3068, wheel_spawn, f_001B3068_orig);
}
}  // namespace

// EOD_WHEEL_REPLAY=<n> (test only): after each real wheel toss, the same toss (car index, matrix,
// velocity and hub position the caller built) is repeated n times on the car's wheels in turn, as
// a deterministic replay of 0x1B3068 that fills the pool past the PS2 array (16) and the scene's
// wheel phobs. Each replayed spawn goes through the wrapper (expansion, EOD_DIFF) like a real one.
extern "C" void f_001B3068(void) {
    HostUndo undo(g_wheel_use);
    static const uint32_t replays = [] {
        const char *e = std::getenv("EOD_WHEEL_REPLAY");
        return e ? (uint32_t)std::strtoul(e, nullptr, 10) : 0u;
    }();
    if (!replays) {
        wheel_spawn_expanded();
        return;
    }
    const EEState entry = g_ee;
    wheel_spawn_expanded();
    const EEState done = g_ee;
    for (uint32_t k = 1; k <= replays; k++) {
        g_ee = entry;
        g_ee.r[5].sd[0] = (int32_t)((entry.r[5].uw[0] + k) & 3u);  // wheel 0..3 (car+0x1A00 + 0x54 w)
        wheel_spawn_expanded();
    }
    g_ee = done;
    if (diff_in_original_side()) return;
    static uint64_t replayed = 0;
    replayed += replays;
    std::fprintf(stderr, "[debris] wheel toss replay: car %d wheel %d, %u more tosses (%llu replayed in all)\n",
                 (int32_t)entry.r[4].uw[0], (int32_t)entry.r[5].uw[0], replays, (unsigned long long)replayed);
}
extern "C" void f_001B3180(void) {
    HostUndo undo(g_wheel_use);
    diff_run(0x001B3180, wheel_count_reset, f_001B3180_orig);
    release(g_wheel_use);
}
extern "C" void f_001B3190(void) {
    HostUndo undo(g_wheel_use);
    diff_run(0x001B3190, wheel_pool_reset, f_001B3190_orig);
    release(g_wheel_use);
}
extern "C" void f_001E1E88(void) { diff_run(0x001E1E88, panel_create, f_001E1E88_orig); }
// A panel this file made was never counted by the guest: give its decrement back, so the count
// returns to 0 when the scene's 'Panl' phobs are gone, as on the PS2.
extern "C" void f_001E29C0(void) {
    HostUndo undo(g_panel_use);
    const uint32_t phob = reg(4);
    diff_run(0x001E29C0, panel_destroy, f_001E29C0_orig);
    Pool &p = g_panel_use;
    if (p.ours.erase(phob)) sw(kPanelCount, (uint32_t)(lw(kPanelCount) + 1));
    for (size_t i = 0; i < p.reserve.size(); i++)
        if (p.reserve[i] == phob) {
            p.reserve.erase(p.reserve.begin() + (std::ptrdiff_t)i);
            break;
        }
    if (lw(kPanelCount) <= 0 && p.ours.empty()) release(p);
}

// 0x1B2868 (wheel Init, vtable 0x3FCE50) stays recompiled; wrapped to keep the create record the
// scene's wheels were made from (0x1783E0 passes it; Init reads +0x18..+0x24), so a wheel made by
// create_phob is the same wheel.
extern "C" void f_001B2868_orig(void);
extern "C" void f_001B2868(void) {
    const uint32_t rec = reg(5);
    uint32_t size = rec ? lwu(rec + 4) : 0;
    if (size >= 0x28 && size <= 0x100) {
        g_wheel_record.resize(size);
        for (uint32_t i = 0; i < size; i++) g_wheel_record[i] = (uint8_t)MEM_LB(rec + i);
    }
    f_001B2868_orig();
}

// 0x1E0DC8 stays recompiled; counted here (HostUndo drops the count made on the original side of
// an enclosing EOD_DIFF comparison, so a nested call is not counted twice). It spawns unless the panel box is flat on every
// axis (|min - max| <= FLT_EPSILON 0x34000000, the first test of 0x1E0DC8), into the first free
// phob of the window, else the one with the oldest spawn clock (+0x204). With ExpandDebris a full
// window gets a free phob in slot 0 first, so nothing live is taken.
namespace {
bool apart(uint32_t a, uint32_t b) {
    FReg x, y, d;
    x.u = MEM_LW(a), y.u = MEM_LW(b);
    d.f = fsub_ps2(x.f, y.f);
    d.u &= 0x7FFFFFFFu;
    FReg eps;
    eps.u = 0x34000000u;
    return fcmp_lt(eps.f, d.f) != 0;
}
bool panel_spawns() {
    const uint32_t lo = reg(7), hi = reg(8);
    return apart(lo, hi) || apart(lo + 4, hi + 4) || apart(lo + 8, hi + 8);
}
bool window_full(const Pool &p) {
    const int32_t count = lw(p.count);
    for (int32_t i = 0; i < count && i < (int32_t)kPoolWords; i++) {
        const uint32_t phob = lwu(p.array + 4 * (uint32_t)i);
        if (!phob || lwu(phob + 0x1DC) == 0) return false;
    }
    return count > 0;
}
}  // namespace
extern "C" void f_001E0DC8(void) {
    HostUndo undo(g_panel_use);
    if (expanded() && panel_spawns() && window_full(g_panel_use)) make_room(g_panel_use, 0, false, 0x1E0DC8);
    if (panel_spawns() && lw(kPanelCount) > 0) note_use(g_panel_use, live_in(g_panel_use) + 1, window_full(g_panel_use));
    f_001E0DC8_orig();
}
