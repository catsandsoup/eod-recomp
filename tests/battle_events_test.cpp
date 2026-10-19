// Synthetic native-resource fixtures for safe single-player Battle exposure.
#include "battle_events.h"
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
constexpr uint32_t eves = 0x3C4D08, contests = 0x3C61A8;
constexpr uint32_t root = 0x10000, sections = 0x10010;
constexpr uint32_t battle_section = sections + 0x1C;
constexpr uint32_t pairs = 0x10100, key = 0x10200, battle_value = 0x10220;
std::vector<uint8_t> ram(EE_RAM_SIZE);
int original_calls, integer_calls;
bool recursive, inside_original;
uint32_t max_cars;
EEState native_return;

void require(bool okay, const char *message) {
    if (!okay) { std::fprintf(stderr, "Battle events: %s\n", message); std::exit(1); }
}
void text_at(uint32_t address, const char *s) { std::memcpy(g_ram + address, s, std::strlen(s) + 1); }
void reset() {
    std::fill(ram.begin(), ram.end(), 0); g_ram = ram.data(); g_ee = {};
    original_calls = integer_calls = 0; recursive = inside_original = false; max_cars = 2;
    g_config.sp_battle_mode = true;
    ee_w32(0x3C4C04, 0x12F); ee_w32(0x3C4CF4, 1); ee_w32(0x3C4CF8, 2);
    ee_w32(eves, 10); ee_w32(eves + 0x70, 2); ee_w32(eves + 0x80, sections);
    for (uint32_t i = 0; i < 2; ++i) {
        ee_w32(contests + i * 0x28, i); ee_w32(contests + i * 0x28 + 4, 10);
        ee_w32(contests + i * 0x28 + 8, 0x100); ee_w32(contests + i * 0x28 + 0x24, 1);
    }
    ee_w32(root, 0x3F2190); ee_w32(root + 4, 2); ee_w32(root + 8, 1); ee_w32(root + 0xC, sections);
    for (uint32_t section : {sections, battle_section}) {
        ee_w32(section, 0x3F21B0); ee_w32(section + 0x18, root);
    }
    ee_w32(battle_section + 0xC, 1); ee_w32(battle_section + 0x10, pairs);
    ee_w32(pairs, key); ee_w32(pairs + 4, battle_value);
    text_at(key, "event_type"); text_at(battle_value, "battle");
}

void appendPreservesNativeData() {
    reset();
    std::array<uint8_t, 0x50> original_contests;
    std::array<uint8_t, 0x300> original_resource;
    std::memcpy(original_contests.data(), g_ram + contests, original_contests.size());
    std::memcpy(original_resource.data(), g_ram + root, original_resource.size());
    f_001244A8();
    require(original_calls == 1 && ee_r32(0x3C4CF8) == 3 && ee_r32(eves + 0x70) == 3,
            "exactly one Battle is appended after native load");
    require(std::memcmp(original_contests.data(), g_ram + contests, original_contests.size()) == 0,
            "existing races and contest descriptors are unchanged");
    require(std::memcmp(original_resource.data(), g_ram + root, original_resource.size()) == 0,
            "native VAL objects, pairs and strings are unchanged");
    const uint32_t appended = contests + 0x50;
    const std::array<uint32_t, 10> expected = {2,10,0x18000224,0,2,0x10,1,1000,battle_section,1};
    for (uint32_t i = 0; i < expected.size(); ++i)
        require(ee_r32(appended + i * 4) == expected[i], "new descriptor follows native defaults and exact section identity");
    require(std::memcmp(&g_ee, &native_return, sizeof(g_ee)) == 0, "native integer calls preserve loader EE context");
    f_001244A8();
    require(ee_r32(0x3C4CF8) == 3, "repeated loader callbacks cannot duplicate Battle");
}

void recursiveLoad() {
    reset(); recursive = true; f_001244A8();
    require(original_calls == 2 && ee_r32(0x3C4CF8) == 3, "only outer include callback appends Battle");
}

void gatesAndMalformedResources() {
    reset(); g_config.sp_battle_mode = false; f_001244A8();
    require(ee_r32(0x3C4CF8) == 2 && integer_calls == 0, "disabled mode remains native");
    reset(); ee_w32(0x3C4C04, 0x131); f_001244A8();
    require(ee_r32(0x3C4CF8) == 2, "multiplayer tables remain native");
    reset(); ee_w32(0x3C4C04, 0x130); f_001244A8();
    require(ee_r32(0x3C4CF8) == 3, "Career uses the same safe native resource path");
    reset(); ee_w32(0x3C4CF8, 320); f_001244A8();
    require(ee_r32(0x3C4CF8) == 320 && integer_calls == 0, "native total capacity is respected");
    reset(); ee_w32(eves + 0x70, 32); f_001244A8();
    require(ee_r32(0x3C4CF8) == 2, "native per-Eve capacity is respected");
    reset(); ee_w32(eves + 0x80, 0); f_001244A8();
    require(ee_r32(0x3C4CF8) == 2, "missing native config is never fabricated");
    reset(); text_at(battle_value, "race"); f_001244A8();
    require(ee_r32(0x3C4CF8) == 2, "track without native Battle section is unchanged");
    reset(); ee_w32(root + 4, 0xFFFFFFFF); f_001244A8();
    require(ee_r32(0x3C4CF8) == 2, "invalid section bounds are rejected");
    reset(); ee_w32(battle_section + 0x14, battle_section); f_001244A8();
    require(ee_r32(0x3C4CF8) == 2 && integer_calls == 0, "cyclic inheritance never enters native lookup");
    reset(); max_cars = 20; f_001244A8();
    require(ee_r32(0x3C4CF8) == 2, "native BattleMaster's four-car capacity is respected");
    reset();
    ee_w32(battle_section + 0xC, 2); ee_w32(pairs + 8, 0x10240); ee_w32(pairs + 12, 0x10260);
    text_at(0x10240, "external_table"); text_at(0x10260, "x");
    f_001244A8();
    require(ee_r32(0x3C4CF8) == 2 && std::strcmp("x", reinterpret_cast<char *>(g_ram + 0x10260)) == 0,
            "unsupported short shared layout string is never overwritten");
}
} // namespace

extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t) { std::abort(); }
extern "C" void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
extern "C" void f_001244A8_orig() {
    ++original_calls;
    if (recursive && !inside_original) {
        inside_original = true; f_001244A8(); inside_original = false;
        require(ee_r32(0x3C4CF8) == 2 && integer_calls == 0, "include return must not append before remaining native Eves");
    }
    std::memset(&g_ee, 0x5A, sizeof(g_ee)); g_ee.r[29].ud[0] = 0x100000; native_return = g_ee;
}
extern "C" void f_00135BB0() {
    ++integer_calls;
    const uint32_t section = arg_u32(0), name = arg_u32(1), fallback = arg_u32(2);
    require(section == sections || section == battle_section, "integer query must use owned native section");
    uint32_t result = fallback;
    if (name == 0x3EF240) result = max_cars;
    if (name == 0x3EF2C0 && section == sections) result = 0x10;
    std::memset(&g_ee, 0xA5, sizeof(g_ee)); g_ee.r[2].ud[0] = result;
}

int main() {
    appendPreservesNativeData(); recursiveLoad(); gatesAndMalformedResources();
    std::puts("Battle event exposure checks passed");
}
