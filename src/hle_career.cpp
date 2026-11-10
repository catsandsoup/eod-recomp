// hle_career.cpp - Career unlocks (docs/CAREER_UNLOCKS.md): native Career functions wrapped so the
// whole Career is open from the start. Every wrapper does nothing unless its [Cheats] setting is on
// (all on by default), and none changes damage or physics values. The per-frame money / rank / gate
// patches are in game_patches.cpp (game_patches_vblank).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "funcs.h"
#include "hle_career.h"
#include "hle_showroom.h"
#include "platform/platform.h"

extern "C" {
void f_002E5200_orig(void);
void f_002E6C78_orig(void);
void f_002E0F10_orig(void);
void f_002E5758_orig(void);
void f_002E3BC8_orig(void);
void f_002E3E70_orig(void);
void f_002E35F0_orig(void);
void f_0018AAD0_orig(void);
void f_001741C0_orig(void);
void f_003702F0_orig(void);
void f_0036E9C0_orig(void);
void f_00370DD8_orig(void);
void f_0016F4E8_orig(void);
void f_0016F748_orig(void);
}

namespace {

// ---- Career state (see docs/CAREER_UNLOCKS.md)
const uint32_t kCDBRes = 0x3D9394;        // pointer: +0 rank (zero-based, 0 = champion)
const uint32_t kEventWord = 0x3CF19C;   // the running event: 0x8000226 in the free-roam world
const uint32_t kASChassisList = 0x3D93B8; // 8-byte records: name pointer, price
const uint32_t kASChassisCount = 0x3D93BC;
const uint32_t kASCarList = 0x3D93C0;     // 0x14-byte records: +0 index, +4 vehicle, +8 price, +0xC chassis name
const uint32_t kASCarCount = 0x3D93C4;
const uint32_t kShopSlots = 0x4AB130;     // the five dealer slots: indices into ASCarList
const uint32_t kShopCount = 0x3D93D0;     // 5

bool sane(uint32_t p, uint32_t span) { return p && !(p & 3) && p <= EE_RAM_SIZE - span; }

// ---- dealer ---------------------------------------------------------------------------------
// The native dealer offers five cars picked by wealth tier and chance. With AllCars the five
// slots are filled from one list of every chassis in the shop (cheapest first, Phantom and the
// Cup car included). Every visit (every build of the Homebase hub menu, which asks for the stock)
// shows the next page. L2/R2, L1/R1, Q/E, 1/3, or Left/Right cycle pages live on the Buy screen;
// entering Homebase also advances to the next page. Yard cars stay from lot load;
// off-page selections focus the yard bay smoothly without triggering the native get_slot_for_car panic.
int s_page = 0;            // current page of the dealer list
int s_variant = 0;         // paint job rotation, so every ASCarList entry shows up over time
std::atomic<int> s_dealer_frames{0};   // vblanks since the dealer's selection callback (f_003702F0) last ran
std::atomic<bool> s_banner_due{true};   // say once per visit which page this is
int s_pages = 0;                       // pages of the dealer list, for the banner
bool s_visited = false;                // a first visit shows the first page

struct Offer {
    uint32_t index;   // ASCarList index
    uint32_t vehicle; // vehicle number
};

std::vector<Offer> dealer_catalog() {
    std::vector<Offer> out;
    const uint32_t chassis = ee_r32(kASChassisList), nch = ee_r32(kASChassisCount);
    const uint32_t cars = ee_r32(kASCarList), ncar = ee_r32(kASCarCount);
    if (!sane(chassis, 8 * 64) || !sane(cars, 0x14) || nch == 0 || nch > 64 || ncar == 0 || ncar > 512) return out;
    struct Ch { uint32_t name, price; std::vector<uint32_t> idx; };
    std::vector<Ch> ch;
    for (uint32_t i = 0; i < nch; i++) ch.push_back({ee_r32(chassis + 8 * i), ee_r32(chassis + 8 * i + 4), {}});
    for (uint32_t i = 0; i < ncar; i++) {
        const uint32_t name = ee_r32(cars + 0x14 * i + 0xC);
        for (Ch &c : ch)
            if (c.name == name) { c.idx.push_back(i); break; }
    }
    std::stable_sort(ch.begin(), ch.end(), [](const Ch &a, const Ch &b) { return a.price < b.price; });
    for (const Ch &c : ch) {
        if (c.idx.empty()) continue;
        const uint32_t i = c.idx[(size_t)s_variant % c.idx.size()];
        out.push_back({i, ee_r32(cars + 0x14 * i + 4)});
    }
    return out;
}

void fill_dealer(uint32_t out) {
    const std::vector<Offer> cat = dealer_catalog();
    if (cat.empty()) return;
    const int n = (int)cat.size();
    const int pages = (n + 4) / 5;
    s_page = ((s_page % pages) + pages) % pages;
    for (int i = 0; i < 5; i++) {
        const Offer &o = cat[(size_t)((s_page * 5 + i) % n)];
        ee_w32(kShopSlots + 4 * i, o.index);
        if (out) ee_w32(out + 4 * i, o.vehicle);
    }
    ee_w32(kShopCount, 5);
    s_pages = pages;
    s_banner_due.store(true);
    static int logged;
    if (logged++ < 12)
        LOG(LOG_CORE, "career dealer: page %d/%d (%d cars): %u %u %u %u %u", s_page + 1, pages, n,
            ee_r32(kShopSlots), ee_r32(kShopSlots + 4), ee_r32(kShopSlots + 8), ee_r32(kShopSlots + 12),
            ee_r32(kShopSlots + 16));
}

static bool s_buyscreen_active = false;
static int s_queued_page_delta = 0;

void career_show_page_banner() {
    char text[96];
    if (s_pages > 1)
        std::snprintf(text, sizeof text, "DEALER: PAGE %d OF %d", s_page + 1, s_pages);
    else
        std::snprintf(text, sizeof text, "DEALER LOT: STOCK UNLOCKED");
    platform_show_hud_banner(text, 3000);
}

}  // namespace

// AutoShopSetAvailable(out[5], mincap): the native pick, then the paged list over it.
extern "C" void f_002E35F0(void) {
    const uint32_t out = g_ee.r[4].uw[0];
    f_002E35F0_orig();
    if (!g_config.career_all_cars) return;
    static const char *start = std::getenv("EOD_CAREER_DEALER_PAGE");  // developer: the first page shown (0-based)
    if (!s_visited && start) s_page = std::atoi(start);
    else if (s_visited) { s_page++; s_variant++; }
    s_visited = true;
    fill_dealer(sane(out, 20) ? out : 0);
}

// The dealer's per-frame selection callback runs only while its car list is on screen.
extern "C" void f_003702F0(void) {
    s_dealer_frames.store(0);
    if (s_queued_page_delta != 0 && s_pages > 1) {
        s_page = ((s_page + s_queued_page_delta) % s_pages + s_pages) % s_pages;
        s_queued_page_delta = 0;
        s_variant++;
        fill_dealer(0);
        s_banner_due.store(true);
        career_show_page_banner();
    }
    f_003702F0_orig();
}

// dealer Buy screen: pass-through to native MenuRun loop without stack tampering
extern "C" void f_0036E9C0(void) {
    s_buyscreen_active = true;
    f_0036E9C0_orig();
    s_buyscreen_active = false;
}

// home: get_buy_slot_for_car(home, car) - maps car index (5..9 on page 1, 10..14 on page 2)
// to buy slot 0..4 without asserting on off-page cars (Panic 0x00331DF0).
extern "C" void f_0016F748(void) {
    const uint32_t home = g_ee.r[4].uw[0];
    const uint32_t car_num = g_ee.r[5].uw[0];
    if (sane(home, 0x4D0)) {
        for (uint32_t i = 0; i < 5; i++) {
            if (ee_r32(home + 0x4BC + 4 * i) == car_num) {
                g_ee.r[2].sd[0] = (int64_t)(int32_t)i;
                return;
            }
        }
    }
    uint32_t slot = (car_num >= 5) ? ((car_num - 5) % 5) : (car_num % 5);
    g_ee.r[2].sd[0] = (int64_t)(int32_t)slot;
}

// HomebaseGetSlotForCar(car) - looks up Homebase instance at 0x003CDA04 and maps car to slot
extern "C" void f_0016F4E8(void) {
    const uint32_t car_num = g_ee.r[4].uw[0];
    const uint32_t home = ee_r32(0x003CDA04);
    if (sane(home, 0x4D0)) {
        for (uint32_t i = 0; i < 5; i++) {
            if (ee_r32(home + 0x4BC + 4 * i) == car_num) {
                g_ee.r[2].sd[0] = (int64_t)(int32_t)i;
                return;
            }
        }
    }
    uint32_t slot = (car_num >= 5) ? ((car_num - 5) % 5) : (car_num % 5);
    g_ee.r[2].sd[0] = (int64_t)(int32_t)slot;
}

// dealer get_slot_for_car: maps vehicle to yard slot or UI slot without panicking
extern "C" void f_00370DD8(void) {
    const uint32_t vehicle_id = g_ee.r[4].uw[0];
    const uint32_t yard_table = ee_r32(0x00495D80);
    const uint32_t yard_count = sane(yard_table, 0x1778) ? ee_r32(yard_table + 0x1774) : 0;
    for (uint32_t i = 0; i < yard_count && i < 16; i++) {
        const uint32_t entry = yard_table + i * 0x12C;
        if (sane(entry, 0x124) && ee_r32(entry + 0x120) == vehicle_id) {
            f_00370DD8_orig();
            return;
        }
    }
    // Car is on another dealer page and not spawned as a physical yard model:
    // Focus the bay corresponding to the UI selection without asserting.
    uint32_t sel = ee_r32(0x004DD388 + 0x280);
    if (sel >= 5) sel = 0;
    uint32_t slot = (yard_count > 0) ? (sel % yard_count) : sel;
    g_ee.r[2].sd[0] = (int64_t)(int32_t)(slot % 5);
}

// ---- Eves, gates, Troy's Place -----------------------------------------------------------
// The Eve rank window [max_rank, min_rank]: f_002E5200(rank, i) is true when Eve record i is open at
// this rank. The Eve screen lists the open Eves in record order (easiest first) but shows four of them,
// so with OpenCareer an Eve is open when its native window says so OR it has not been played yet (result
// byte 0xFF at CDBRes + 0x118 + Eve): the screen offers the first four Eves not played yet, whatever the
// rank, and finishing one brings the next up. With every Eve played they are all open again (replay).
// Every other caller (the refill of the four open Eves, the "new Eves" count) sees the same. career_eves.val
// is untouched.
bool eve_unplayed(uint32_t record) {
    const uint32_t res = ee_r32(kCDBRes), recs = 0x4AB158;
    if (!sane(res, 0x1C0) || record >= ee_r32(0x4AB150) || record > 64) return true;
    const uint32_t eve = ee_r32(recs + 12 * record);
    return eve >= 40 || ee_r8(res + 0x118 + eve) == 0xFF;
}

extern "C" void f_002E5200(void) {
    const uint32_t record = g_ee.r[5].uw[0];
    f_002E5200_orig();
    if (!g_config.career_open || g_ee.r[2].sd[0]) return;
    bool any_unplayed = false;
    for (uint32_t i = 0, n = ee_r32(0x4AB150); i < n && i < 64 && !any_unplayed; i++) any_unplayed = eve_unplayed(i);
    g_ee.r[2].sd[0] = !any_unplayed || eve_unplayed(record) ? 1 : 0;
}

// Home challenges (Troy's Place, T.J.'s): rep_required (record + 0x2C) is the reputation a
// challenge asks for; none is asked for with OpenCareer. Records are 0x44 bytes at a2.
extern "C" void f_002E5758(void) {
    const uint32_t count = g_ee.r[4].uw[0], base = g_ee.r[6].uw[0], max = g_ee.r[7].uw[0];
    f_002E5758_orig();
    if (!g_config.career_open || !sane(count, 4) || !sane(base, 0x44)) return;
    const uint32_t n = ee_r32(count);
    if (n > max || n > 256) return;
    for (uint32_t i = 0; i < n; i++) ee_w32(base + 0x44 * i + 0x2C, 0);
}

// ---- rank ------------------------------------------------------------------------------------
// New rank after an Eve (native: rank - 6 + place, and 0 = champion when it is won at rank 1).
// With CareerRank the player stays on the list: never below 1.
extern "C" void f_002E6C78(void) {
    f_002E6C78_orig();
    const uint32_t res = ee_r32(kCDBRes);
    if (!g_config.career_rank || !sane(res, 4) || (int32_t)ee_r32(res) < 1) return;
    if ((int32_t)g_ee.r[2].sd[0] < 1) g_ee.r[2].sd[0] = 1;
}

// The Career screens show rank + 1 (stored 1 -> "2nd"). With CareerRank the screens that only
// display the rank (return addresses below: the ones that follow the call with the "st/nd/th"
// suffix function f_002E2A70) see 0, so the player reads 1st. Game logic gets the real value.
// Developer probe (EOD_CAREER_TRACE): from the free-roam HUD's rank call (guest context, once a second) asks the
// native "can this car be damaged" test (f_0018AAD0) about every world car and logs the answers.
static void can_damage_probe() {
    static int calls;
    if (++calls % 60 != 0 || calls > 60 * 12) return;
    const uint32_t n = ee_r32(0x48522C);
    if (n < 1 || n > 20) return;
    const EEState saved = g_ee;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t car = ee_r32(0x447458 + 4 * i);
        if (!sane(car, 0x270)) continue;
        g_ee.r[4].ud[0] = car;
        f_0018AAD0_orig();
        const int result = (int)g_ee.r[2].sd[0];
        const uint32_t drv = ee_r32(car + 0x260);
        LOG(LOG_CORE, "[CAN_DAMAGE_PROBE] call=%d car#%u %08x id=%x driver=%08x -> %d event_word=%08x", calls, i, car,
            ee_r32(car + 0x1E0), drv, result, ee_r32(kEventWord));
    }
    g_ee = saved;
}

extern "C" void f_002E0F10(void) {
    static const bool probe = std::getenv("EOD_CAREER_TRACE") != nullptr;
    if (probe && g_ee.r[31].uw[0] == 0x170DDC) can_damage_probe();
    f_002E0F10_orig();
    if (!g_config.career_rank || (int32_t)g_ee.r[2].sd[0] != 1) return;
    static const uint32_t kDisplay[] = {0x36A7AC, 0x36A7B4, 0x36AD54, 0x36AD5C, 0x36B11C, 0x36B124, 0x36B780, 0x36B788,
                                        0x36BC7C, 0x36BC84, 0x36C858, 0x36C860, 0x382DA8, 0x382DB0, 0x383164, 0x38316C,
                                        0x170DDC /* the free-roam HUD */, 0x372CB4 /* "You are ranked" */, 0x3716B0 /* Eve list */};
    const uint32_t ra = g_ee.r[31].uw[0];
    for (uint32_t a : kDisplay)
        if (ra == a) { g_ee.r[2].sd[0] = 0; return; }
}

// ---- cars -------------------------------------------------------------------------------------
// Depreciation of a car being sold or traded in. With FreeSwap there is none (full refund).
extern "C" void f_002E3BC8(void) {
    if (g_config.career_free_swap) {
        g_ee.r[2].sd[0] = 0;
        return;
    }
    f_002E3BC8_orig();
}

// "Used Value" of an owned car (slot in a0): the native sale price is 90 % of the price paid
// (record + 8 of the owned-car list at 0x4AB0E0, 0x14 bytes each). FreeSwap pays the whole price.
extern "C" void f_002E3E70(void) {
    const uint32_t slot = g_ee.r[4].uw[0];
    if (g_config.career_free_swap && slot < 8) {
        g_ee.r[2].sd[0] = (int32_t)ee_r32(0x4AB0E0 + 0x14 * slot + 8);
        return;
    }
    f_002E3E70_orig();
}

// ---- challengers ------------------------------------------------------------------------------
extern "C" void f_0018AAD0(void) {
    const uint32_t car = g_ee.r[4].uw[0];
    f_0018AAD0_orig();
    static const bool trace = std::getenv("EOD_CAREER_TRACE") != nullptr;
    if (trace && sane(car, 0x270) && ee_r32(car + 0x1E0) != 0) {
        static int logged;
        const uint32_t drv = ee_r32(car + 0x260);
        if (logged++ < 400)
            LOG(LOG_CORE, "[CAN_DAMAGE] car=%08x id=%x driver=%08x -> %d", car, ee_r32(car + 0x1E0), drv,
                (int)g_ee.r[2].sd[0]);
    }
}

// Native f_001741C0 returns 1 when the Career free-roam / Highway challenge state machine
// (*(int32_t*)0x3CDA24) is active (!= 6). In f_0018AAD0 (can_damage, RA = 0x18AB2C) that 1
// enables collision damage in Career free roam; in f_0018CA98 (collision deformation and damage,
// RA = 0x18D3F8 and RA = 0x18D7EC) that same 1 clamps car->damage (car + 0x1BEC) back to 0.9f
// (0x3F666666) and skips the instant-wreck path so free-roam cars never die.
// With KillableChallengers on, returning 0 at those two call sites for any non-player car
// (car in $s2 = R[18], car + 0x1E0 != 0x50) lets Highway and Malt Shop challengers reach 1.0f
// damage and wreck while keeping the player's car from softlocking free roam.
extern "C" void f_001741C0(void) {
    const uint32_t ra = g_ee.r[31].uw[0];
    const uint32_t car = g_ee.r[18].uw[0];
    f_001741C0_orig();
    if (!g_config.career_killable || !g_ee.r[2].ud[0]) return;
    if ((ra == 0x0018D3F8 || ra == 0x0018D7EC) && sane(car, 0x1C00) && ee_r32(car + 0x1E0) != 0x50) {
        g_ee.r[2].ud[0] = 0;
    }
}

// ---- per-frame (game_patches_vblank) ----------------------------------------------------------
namespace {
const uint32_t kCareerSave = 0x3D9384;  // pointer: +0 non-zero while a Career is active
const uint32_t kCDBupgr = 0x3D9398;     // pointer: +4 money
const uint32_t kRankGates = 0x41EFF8;   // {80th, 50th, champion}: stored rank thresholds
const uint32_t kFreeRoamEvent = 0x8000226;
const int32_t kMoneyFloor = 10000;      // the dearest car (Rocket) costs $10,000
}  // namespace

// Developer probes (docs/CAREER_UNLOCKS.md, "Challengers"): EOD_CAREER_TRACE logs the native damage test and the
// event word; EOD_CAREER_CRASH_TEST=<world car index of an AI> (after a Jump To TJ's) drives the player at it.
// Developer probe (EOD_CAREER_CRASH_TEST=<world car index of an AI>): in Career free roam, throws the
// player's car at that AI car and logs what each car's damage state does. Off unless the variable is set.
static float rf(uint32_t a) { uint32_t v = ee_r32(a); float f; std::memcpy(&f, &v, 4); return f; }
static void wf(uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); ee_w32(a, v); }
static void crash_probe() {
    static const char *text = std::getenv("EOD_CAREER_CRASH_TEST");
    if (!text) return;
    static int frame;
    frame++;
    const uint32_t n = ee_r32(0x48522C);
    if (n < 2 || n > 20) return;
    const uint32_t target = ee_r32(0x447458 + 4 * (uint32_t)std::atoi(text));
    uint32_t player = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t c = ee_r32(0x447458 + 4 * i);
        if (sane(c, 0x1C34) && ee_r32(c + 0x1E0) == 0x50) player = c;
    }
    if (!sane(target, 0x1C34) || !player) return;
    // Close enough (after a jump to TJ's): drive at the target for a second and a half.
    const float dx = rf(target + 0x88) - rf(player + 0x88), dz = rf(target + 0x90) - rf(player + 0x90);
    const float dist = std::sqrt(dx * dx + dz * dz);
    static int started;
    if (!started && dist < 60.0f && frame > 120) started = frame;
    if (started && frame < started + 90) {
        wf(player + 0x104, 30.0f * dx / dist); wf(player + 0x108, 0.0f); wf(player + 0x10C, 30.0f * dz / dist);
    }
    if ((started && frame >= started - 5 && frame <= started + 300 && frame % 6 == 0) || frame % 120 == 0)
        LOG(LOG_CORE, "[CRASH_PROBE] f=%d dist=%.1f target pos=%.1f,%.1f,%.1f vel=%.2f health=%.3f id=%x | player pos=%.1f,%.1f,%.1f vel=%.1f health=%.3f",
            frame, dist, rf(target + 0x88), rf(target + 0x8C), rf(target + 0x90), rf(target + 0x104), rf(target + 0x1BEC),
            ee_r32(target + 0x1E0), rf(player + 0x88), rf(player + 0x8C), rf(player + 0x90), rf(player + 0x104), rf(player + 0x1BEC));
}

void career_vblank(uint16_t pressed) {
    if (s_buyscreen_active || s_dealer_frames.load() < 10) {
        if (pressed & (PAD_R2 | PAD_R1 | PAD_RIGHT)) {
            s_queued_page_delta = 1;
        } else if (pressed & (PAD_L2 | PAD_L1 | PAD_LEFT)) {
            s_queued_page_delta = -1;
        }
    }
    // the idle state: the Career's free-roam world (event word 0x8000226; a race or Battle has another value)
    const uint32_t event = ee_r32(kEventWord);
    const bool idle = event == kFreeRoamEvent;
    static const bool trace = std::getenv("EOD_CAREER_TRACE") != nullptr;
    if (trace) {
        static uint32_t last_event = 0xFFFFFFFF, last_mode = 0xFFFFFFFF;
        const uint32_t mode = ee_r32(0x3C4C04);
        if (event != last_event || mode != last_mode) {
            LOG(LOG_CORE, "[CAREER_STATE] event=%08x mode=%x dealer_frames=%d", event, mode, s_dealer_frames.load());
            last_event = event; last_mode = mode;
        }
    }
    if (idle) crash_probe();
    showroom_update_camera(1.0f / 60.0f);
    showroom_update_turntable(1.0f / 60.0f);
    // the dealer's car list is on screen (its callback ran within the last few vblanks): say which page it is
    if (g_config.career_all_cars && s_dealer_frames.fetch_add(1) < 4 && s_banner_due.exchange(false))
        career_show_page_banner();
    // The rank gate table (data in the executable's image, read by f_002E27F8 / f_002E2838): the
    // Upgrades and the Time Challenges ask for rank 80th, the dealer's full stock and Highway for
    // 50th, the champion lock-out for 1st. All open with OpenCareer; the champion entry stays 0.
    static const uint32_t kNative[2] = {79, 49}, kOpen[2] = {99, 99};
    const uint32_t *want = g_config.career_open ? kOpen : kNative;
    if (ee_r32(kRankGates) != want[0]) ee_w32(kRankGates, want[0]);
    if (ee_r32(kRankGates + 4) != want[1]) ee_w32(kRankGates + 4, want[1]);

    if (!idle) return;
    if (g_config.career_killable) {
        // If the active Highway challenger (0x3CDA78 -> entry+8 = car_idx) is wrecked while the
        // challenge prompt is waiting or showing (state 0 or 1 at 0x3CDA24), retire the prompt
        // (state 6) so a wrecked car never offers a race.
        const int32_t hwy_state = (int32_t)ee_r32(0x3CDA24);
        const uint32_t entry = ee_r32(0x3CDA78);
        if ((hwy_state == 0 || hwy_state == 1) && sane(entry, 12)) {
            const uint32_t idx = ee_r32(entry + 8);
            if (idx < 20) {
                const uint32_t car = ee_r32(0x447458 + 4 * idx);
                if (sane(car, 0x1C00) && rf(car + 0x1BEC) >= 1.0f) ee_w32(0x3CDA24, 6);
            }
        }
    }
    const uint32_t save = ee_r32(kCareerSave);
    if (!sane(save, 4) || !ee_r32(save)) return;  // no Career active
    if (g_config.career_money) {
        const uint32_t up = ee_r32(kCDBupgr);
        if (sane(up, 0x10) && (int32_t)ee_r32(up + 4) < kMoneyFloor) ee_w32(up + 4, (uint32_t)kMoneyFloor);
    }
    if (g_config.career_rank) {
        const uint32_t res = ee_r32(kCDBRes);
        if (sane(res, 4) && (int32_t)ee_r32(res) > 1) ee_w32(res, 1);
    }
}
