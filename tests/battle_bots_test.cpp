// Tests the C++/native construction boundary with synthetic EE memory. Native
// vehicle selection, driving and weapon physics need separate gameplay checks.
#include "battle_bots.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

#include <algorithm>
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
    g_config.battle_bots = true;
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
    reset(); ee_w32(description + 0x98C, 5); makeWorld();
    require(created.empty(), "unsafe Battle participant count is rejected");
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
        for (uint32_t i = 0; i < 4; ++i) ee_w32(0x3D1C90 + 4 * i, sceneBase + 0x1000 * i);
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
    require(arg_u32(0) < 4 && arg_u32(1) == 0, "weapon lookup uses car owner and weapon slot zero");
    ++lookupCalls; g_ee.r[2].ud[0] = registry[arg_u32(0)];
}
extern "C" void f_00227EE8() {
    const uint32_t owner = arg_u32(0);
    require(owner < 4 && ee_r32(record(owner)) == 0x51, "only native AI records get weapons");
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
    countCases(); weaponsAndIdentity(); capacity();
    std::puts("Battle construction bridge checks passed");
}
