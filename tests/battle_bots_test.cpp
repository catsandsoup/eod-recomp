// Tests the C++/native construction boundary with synthetic EE memory. Native
// vehicle selection, driving and weapon physics need separate gameplay checks.
#include "battle_bots.h"
#include "battle_layout.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

EEState g_ee{};
uint8_t *g_ram = nullptr;
Config::Config() = default;
Config g_config;

namespace {
constexpr uint32_t battle = 0x18000224;
constexpr uint32_t contest = 0x10000, description = 0x20000;
constexpr uint32_t sceneBase = 0x30000;
std::vector<uint8_t> ram(EE_RAM_SIZE);
std::vector<uint32_t> created;
std::array<uint32_t, 4> registry{};
int originalCountCalls, originalWorldCalls, lookupCalls, suppressionCalls, resetCalls;
uint32_t nativeCountResult, suppressionResult;
bool createSceneInOriginal, registerImmediately;
EEState nativeWorldReturn{};
uint32_t variantContest, variantAi;
uint32_t drawSlot, drawResult;
int placementOriginal, grounding, rosterOriginal, resolveOriginal;
uint32_t resolvedDescription;
uint32_t groundOut;
float groundX, groundZ, groundFx, groundFz;

void require(bool okay, const char *message) {
    if (!okay) { std::fprintf(stderr, "Battle bots: %s\n", message); std::exit(1); }
}
uint32_t record(uint32_t slot) { return description + 0x2C + slot * 0x78; }
void reset() {
    std::fill(ram.begin(), ram.end(), 0);
    g_ram = ram.data(); g_ee = {}; registry = {}; created.clear();
    originalCountCalls = originalWorldCalls = lookupCalls = suppressionCalls = resetCalls = 0;
    nativeCountResult = 2; suppressionResult = 0; createSceneInOriginal = true;
    registerImmediately = false; // Native factory queues physical registration.
    g_config.battle_bots = true; variantContest = variantAi = 0;
    ee_w32(contest + 8, battle); ee_w32(0x439858, 2);
    ee_w32(description + 0x9A4, battle); ee_w32(description + 0x98C, 4);
    ee_w32(0x3CF19C, battle);
    ee_w32(0x495CB4, 0x40000); ee_w32(0x495CB8, 0x50000);
    for (uint32_t i = 0; i < 4; ++i) {
        ee_w32(record(i), i < 2 ? 0x50 : 0x51);
        // Intentionally deceptive pad bits: they must not determine AI status.
        ee_w8(record(i) + 0x70, i < 2 ? 3 : 0);
    }
}
void makeWorld() { g_ee.r[4].ud[0] = description; f_0020FD48(); }
void checkReturnContext() {
    require(std::memcmp(&g_ee, &nativeWorldReturn, sizeof(g_ee)) == 0,
            "post-construction calls must preserve the native return context");
}
void countCases() {
    reset();
    for (uint32_t humans = 1; humans <= 4; ++humans) {
        ee_w32(0x439858, humans); g_ee.r[4].ud[0] = contest;
        f_0012C158(); require(g_ee.r[2].uw[0] == 4, "Battle fills four slots");
        require(ee_r32(contest + 8) == battle && ee_r32(contest + 0x10) == 0,
                "count hook must not mutate contest records");
    }
    require(originalCountCalls == 4, "native count function always runs");
    for (uint32_t humans : {0u, 5u}) {
        ee_w32(0x439858, humans); g_ee.r[4].ud[0] = contest;
        f_0012C158(); require(g_ee.r[2].uw[0] == nativeCountResult, "invalid human count is unchanged");
    }
    ee_w32(0x439858, 2); g_config.battle_bots = false;
    g_ee.r[4].ud[0] = contest; f_0012C158();
    require(g_ee.r[2].uw[0] == nativeCountResult, "disabled mode is native");
    g_config.battle_bots = true; ee_w32(contest + 8, 0x18000223);
    g_ee.r[4].ud[0] = contest; f_0012C158();
    require(g_ee.r[2].uw[0] == nativeCountResult, "other event is native");
}
void weaponsAndIdentity() {
    reset(); makeWorld();
    require(originalWorldCalls == 1 && created == std::vector<uint32_t>({2, 3}),
            "two humans retain native weapons and two AI receive native constructors");
    require(resetCalls == 1, "world construction resets host driver state");
    require(!battle_bots_is_ai_slot(0) && !battle_bots_is_ai_slot(1) &&
            battle_bots_is_ai_slot(2) && battle_bots_is_ai_slot(3) && !battle_bots_is_ai_slot(4),
            "AI identity follows native types and Battle capacity");
    checkReturnContext();
    makeWorld(); require(created.size() == 2, "existing weapon scenes are not duplicated");
    checkReturnContext();

    g_config.battle_bots = false;
    require(battle_bots_is_ai_slot(2) && battle_bots_is_ai_slot(3),
            "next-event toggle retains current event AI ownership");
    makeWorld();
    require(created.size() == 2 && !battle_bots_is_ai_slot(2) && !battle_bots_is_ai_slot(3),
            "next construction applies disabled setting and clears AI ownership");

    reset(); ee_w32(record(0), 0x51); ee_w32(record(2), 0x50); makeWorld();
    require(created == std::vector<uint32_t>({0, 3}), "interleaved humans are preserved");
    require(battle_bots_is_ai_slot(0) && !battle_bots_is_ai_slot(2), "slot order is not human identity");
    reset(); registry[2] = 0x60000; makeWorld();
    require(created == std::vector<uint32_t>({3}), "registered weapon without scene is not duplicated");

    reset(); suppressionResult = 1; makeWorld();
    require(created.empty() && suppressionCalls == 1, "native suppression gate is honored");
    checkReturnContext();
    reset(); createSceneInOriginal = false; makeWorld();
    require(created.empty(), "missing car scenes cannot construct weapons");
    reset(); g_config.battle_bots = false; makeWorld();
    require(created.empty() && originalWorldCalls == 1 && !battle_bots_is_ai_slot(2), "disabled world remains native");
    reset(); ee_w32(description + 0x9A4, 0x18000223); makeWorld();
    require(created.empty() && !battle_bots_is_ai_slot(2), "other world clears previous AI identity");
    reset(); ee_w32(description + 0x98C, battle_layout::kCapacity + 1); makeWorld();
    require(created.empty(), "participant count beyond the enlarged BattleMaster is rejected");
    // Battle (8 AI) with two humans: ten records, eight AI armed natively even
    // when the optional fill-to-four setting is off.
    reset(); g_config.battle_bots = false; ee_w32(description + 0x98C, 10);
    for (uint32_t i = 0; i < 10; ++i) ee_w32(record(i), i == 3 || i == 7 ? 0x50 : 0x51);
    makeWorld();
    require(created == std::vector<uint32_t>({0, 1, 2, 4, 5, 6, 8, 9}), "eight AI of a ten-car Battle get native weapons");
    require(battle_bots_is_ai_slot(9) && !battle_bots_is_ai_slot(7) && !battle_bots_is_ai_slot(10),
            "AI identity covers every enlarged slot");
}
void variantCounts() {
    reset();
    variantContest = contest; variantAi = 8;
    for (uint32_t humans = 1; humans <= 4; ++humans) {
        ee_w32(0x439858, humans); g_ee.r[4].ud[0] = contest; f_0012C158();
        require(g_ee.r[2].uw[0] == std::min(humans + 8, battle_layout::kMaxCars), "Battle (8 AI) adds eight AI within ten cars");
    }
    variantAi = 4; ee_w32(0x439858, 1); g_ee.r[4].ud[0] = contest; f_0012C158();
    require(g_ee.r[2].uw[0] == 5, "Battle (4 AI) with one human holds five cars");
    g_config.battle_bots = false; ee_w32(0x439858, 2); g_ee.r[4].ud[0] = contest; f_0012C158();
    require(g_ee.r[2].uw[0] == 6, "variant count does not depend on the fill-to-four setting");
    variantAi = 1; g_config.battle_bots = true; ee_w32(0x439858, 2); g_ee.r[4].ud[0] = contest; f_0012C158();
    require(g_ee.r[2].uw[0] == 2, "Battle (1v1) holds only the two humans, even with fill-to-four on");
    g_config.battle_bots = false;
    variantContest = 0; g_ee.r[4].ud[0] = contest; f_0012C158();
    require(g_ee.r[2].uw[0] == nativeCountResult, "native Battle with the setting off stays native");
}
float getf(uint32_t a) { const uint32_t u = ee_r32(a); float f; std::memcpy(&f, &u, 4); return f; }
void putf(uint32_t a, float v) { uint32_t u; std::memcpy(&u, &v, 4); ee_w32(a, u); }
void mixedClasses() {
    // Native tables: vehicle and driver per number; classes 0..3, two of each.
    constexpr uint32_t vehicles = 0x70000, drivers = 0x74000;
    reset();
    ee_w32(0x3CBEE4, vehicles); ee_w32(0x439704, 8); ee_w32(0x3CBEE0, drivers); ee_w32(0x439700, 8);
    const char *names[8] = {"Hurricane", "Bulldog", "Empire", "Arrow", "Cannon", "Rocket", "Hearse", "Bus"};
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t v = vehicles + i * 0x54, d = drivers + i * 0x54;
        ee_w32(v, i); ee_w32(v + 4, 100 + i); std::memcpy(g_ram + v + 8, names[i], std::strlen(names[i]) + 1);
        ee_w32(v + 0x40, i / 2);
        ee_w32(d, i); ee_w32(d + 4, 100 + i); std::snprintf(reinterpret_cast<char *>(g_ram + d + 8), 32, "Driver %u", i);
    }
    // One human (number 100, class 0) between four native same-class AI picks.
    ee_w32(0x439858, 1); ee_w32(0x439708 + 4, 100);
    auto setup = [&] {
        ee_w32(description + 0x98C, 5);
        for (uint32_t i = 0; i < 5; ++i) {
            ee_w32(record(i) + 0x68, i == 2 ? 100 : 101); ee_w32(record(i) + 0x6C, i == 2 ? 100 : 101);
        }
    };
    setup();
    g_ee.r[4].ud[0] = 0x78000; g_ee.r[5].ud[0] = 7; g_ee.r[16].ud[0] = 55; f_0012AEF0();
    g_ee.r[4].ud[0] = description; f_001116E0();
    require(rosterOriginal == 1 && resolveOriginal == 1, "native roster and resolver always run");
    for (uint32_t i = 0; i < 5; ++i) require(ee_r32(record(i) + 0x68) == (i == 2 ? 100u : 101u), "native contests keep the native roster");
    variantAi = 4; variantContest = battle_layout::kContestTable + 7 * battle_layout::kContestStride;
    g_ee.r[4].ud[0] = 0x78000; g_ee.r[5].ud[0] = 7; f_0012AEF0();
    require(g_ee.r[16].ud[0] == 55, "native roster return context survives");
    g_ee.r[4].ud[0] = description; f_001116E0();
    require(resolveOriginal == 2 && resolvedDescription == description, "native resolver runs on the mixed description");
    require(ee_r32(description + 0x98C) == 5 && ee_r32(record(2) + 0x68) == 100 && ee_r32(record(2) + 0x6C) == 100,
            "car count and the human's record are kept");
    std::vector<uint32_t> numbers, classes;
    for (uint32_t i : {0u, 1u, 3u, 4u}) {
        const uint32_t n = ee_r32(record(i) + 0x68);
        require(n >= 101 && n <= 107 && ee_r32(record(i) + 0x6C) == n, "AI record names a native driver with its own vehicle");
        numbers.push_back(n); classes.push_back(ee_r32(vehicles + (n - 100) * 0x54 + 0x40));
    }
    std::sort(numbers.begin(), numbers.end());
    require(std::adjacent_find(numbers.begin(), numbers.end()) == numbers.end(), "no driver/vehicle is used twice");
    std::sort(classes.begin(), classes.end());
    require(classes == std::vector<uint32_t>({0, 1, 2, 3}), "four AI cover every class, including the human's");
    setup(); g_ee.r[4].ud[0] = description; f_001116E0();
    require(ee_r32(record(0) + 0x68) == 101, "a mix applies once, to the description after its roster");
}
void placement() {
    constexpr uint32_t master = 0x60000, out = 0x61000;
    for (uint32_t index : {0u, 5u, 6u, 9u}) {
        reset(); placementOriginal = grounding = 0;
        ee_w32(master, battle_layout::kMasterVtable); ee_w32(master + battle_layout::kRespawnCount, 6);
        g_ee.r[4].ud[0] = master; g_ee.r[5].ud[0] = out; g_ee.r[6].ud[0] = index; g_ee.r[16].ud[0] = 77;
        f_00167C70();
        require(placementOriginal == 1 && g_ee.r[2].uw[0] == 1 && g_ee.r[16].ud[0] == 77,
                "native placement always runs and its return context survives");
        if (index < 6) { require(!grounding, "cars within the native respawn count keep the native pose"); continue; }
        require(grounding == 1 && groundOut == out, "extra cars are re-grounded by the native respawn builder");
        require(std::fabs(groundX - 16.0f) < 1e-4f && std::fabs(groundZ - 20.0f) < 1e-4f &&
                groundFx == 0.0f && groundFz == 1.0f, "extra car moves one car-width lane along the record's right axis");
    }
    reset(); placementOriginal = grounding = 0;
    ee_w32(master, battle_layout::kMasterVtable); ee_w32(master + battle_layout::kRespawnCount, 0);
    g_ee.r[4].ud[0] = master; g_ee.r[5].ud[0] = out; g_ee.r[6].ud[0] = 3; f_00167C70();
    require(placementOriginal == 1 && !grounding, "no respawn records: native behaviour only");
}
void killNames() {
    // The native lookup (0x119428) is replaced only inside the message draw of a Battle that has AI.
    constexpr uint32_t native = 0x1119428;
    auto lookup = [](uint32_t slot) { g_ee.r[4].ud[0] = slot; f_00119428(); return g_ee.r[2].uw[0]; };
    auto inDraw = [](uint32_t slot) { drawSlot = slot; f_00166B50(); return drawResult; };
    auto setName = [](uint32_t slot, const char *text) {
        std::strcpy(reinterpret_cast<char *>(ram.data() + record(slot) + 4), text);
    };
    reset(); makeWorld();
    setName(0, "Ben Adams"); setName(2, "Ben Adams"); setName(3, "Jeff Gaines");
    require(lookup(3) == native, "outside the message draw the native lookup runs");
    require(inDraw(3) == record(3) + 4 && std::strcmp(reinterpret_cast<char *>(ram.data() + record(3) + 4), "Jeff Gaines") == 0,
            "an AI slot in a kill message is named from its description record");
    require(inDraw(2) == record(2) + 4, "a slot below the profile count is named from the record too");
    require(inDraw(4) == native && inDraw(11) == native, "slots past the roster keep the native lookup");
    require(lookup(3) == native, "the draw window closes after the native draw");
    require(inDraw(1) == native, "an empty record name falls back to the native lookup");
    // No AI (native two-car Battle): native names, even inside the draw.
    reset(); for (uint32_t i = 2; i < 4; ++i) ee_w32(record(i), 0x50);
    makeWorld(); setName(0, "player1");
    require(inDraw(0) == native, "a Battle without AI keeps the native kill-message names");
}
void capacity() {
    for (uint32_t address : {0x3CE4B8u, 0x495CACu, 0x495CB0u}) {
        reset(); ee_w32(address, address == 0x3CE4B8 ? 16 : 1200); makeWorld();
        require(created.empty(), "full native registry or world array cannot be appended");
    }
    for (bool immediate : {false, true}) {
        reset(); registerImmediately = immediate; ee_w32(0x3CE4B8, 15); makeWorld();
        require(created == std::vector<uint32_t>({2}),
                "queued constructor reserves the last registry entry before the next slot");
        if (!immediate)
            require(ee_r32(0x3CE4B8) == 15, "deferred registry remains unchanged during bridge");
        reset(); registerImmediately = immediate; ee_w32(0x3CE4B8, 14); makeWorld();
        require(created == std::vector<uint32_t>({2, 3}),
                "two free registry entries allow both constructors without double reservation");
    }
    reset(); ee_w32(0x3CE4B8, 13);
    for (uint32_t slot = 0; slot < 4; ++slot) ee_w32(record(slot), 0x51);
    makeWorld();
    require(created == std::vector<uint32_t>({0, 1, 2}),
            "deferred construction stops at capacity across multiple queued slots");
    reset(); ee_w32(0x495CB4, 0); makeWorld();
    require(created.empty(), "missing native world array cannot be appended");
}
} // namespace

void battle_driver_reset() { ++resetCalls; }
uint32_t battle_events_ai_for_contest(uint32_t c) { return c && c == variantContest ? variantAi : 0; }
extern "C" void f_00166B50_orig() {
    g_ee.r[4].ud[0] = drawSlot; f_00119428(); drawResult = g_ee.r[2].uw[0];
}
extern "C" void f_00119428_orig() { g_ee.r[2].ud[0] = 0x1119428; }
extern "C" void f_001116E0_orig() { ++resolveOriginal; resolvedDescription = arg_u32(0); }
extern "C" void f_0012AEF0_orig() { ++rosterOriginal; std::memset(&g_ee, 0x11, sizeof(g_ee)); g_ee.r[16].ud[0] = 55; }
extern "C" void f_00167C70_orig() {
    ++placementOriginal;
    const uint32_t out = arg_u32(1);
    // Native pose copy: right (1,0,0), up, forward (0,0,1), position (10,0,20).
    putf(out, 1); putf(out + 0x10, 1); putf(out + 0x20, 1); putf(out + 0x24, 10); putf(out + 0x2C, 20);
    ee_w32(out + 0x44, 2);
    std::memset(&g_ee, 0x3C, sizeof(g_ee)); g_ee.r[2].ud[0] = 1; g_ee.r[16].ud[0] = 77;
}
extern "C" void f_001D8F58() {
    ++grounding; groundOut = arg_u32(0);
    groundX = getf(arg_u32(1)); groundZ = getf(arg_u32(1) + 4);
    groundFx = getf(arg_u32(2)); groundFz = getf(arg_u32(2) + 4);
    std::memset(&g_ee, 0x77, sizeof(g_ee));
}
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t) { std::abort(); }
extern "C" void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
extern "C" void f_0012C158_orig() {
    ++originalCountCalls; g_ee.r[2].ud[0] = nativeCountResult;
}
extern "C" void f_0020FD48_orig() {
    require(arg_u32(0) == description, "native world receives original description");
    ++originalWorldCalls;
    if (createSceneInOriginal)
        for (uint32_t i = 0; i < 12; ++i) ee_w32(0x3D1C90 + 4 * i, sceneBase + 0x1000 * i);
    std::memset(&g_ee, 0x5A, sizeof(g_ee));
    g_ee.r[29].ud[0] = 0x100000;
    nativeWorldReturn = g_ee;
}
extern "C" void f_002CE180() {
    ++suppressionCalls; g_ee.r[2].ud[0] = suppressionResult;
    g_ee.fpr[12].f = 1234;
}
extern "C" void f_0017DE78() {
    require(originalWorldCalls > 0, "lookup must follow native car construction");
    require(arg_u32(0) < battle_layout::kCapacity && arg_u32(1) == 0, "weapon lookup uses car owner and weapon slot zero");
    ++lookupCalls; g_ee.r[2].ud[0] = arg_u32(0) < registry.size() ? registry[arg_u32(0)] : 0;
}
extern "C" void f_00227EE8() {
    const uint32_t owner = arg_u32(0);
    require(owner < battle_layout::kCapacity && ee_r32(record(owner)) == 0x51, "only native AI records get weapons");
    require(arg_u32(1) == 0x51 && arg_u32(2) == record(owner) + 0x44,
            "weapon bridge retains native call argument layout");
    require(g_ee.r[29].uw[0] == 0x100000, "native factory gets restored EE stack");
    created.push_back(owner);
    if (registerImmediately) {
        registry[owner] = 0x60000 + owner * 0x1000;
        ee_w32(0x3CE4B8, ee_r32(0x3CE4B8) + 1);
    }
    ee_w32(0x3D20A8 + 4 * owner, 0x70000 + owner * 0x1000);
    for (uint32_t a : {0x495CACu, 0x495CB0u}) ee_w32(a, ee_r32(a) + 1);
    std::memset(&g_ee, 0xA5, sizeof(g_ee));
}

int main() {
    countCases(); killNames(); weaponsAndIdentity(); capacity(); variantCounts(); placement(); mixedClasses();
    std::puts("Battle construction bridge checks passed");
}
