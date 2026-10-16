// lod_test off|one|two|invalid - src/hle_lod.cpp with a stand-in for the
// generated loader: the multiplier parsing and scaling, that only the positive
// distances of a valid table change (and nothing with 1 or a bad value), that
// unsafe tables are left alone, and that the loader's own register and memory
// results are kept.
#include "core/cpu.h"
#include "core/log.h"
#include "lod_thresholds.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
EEState g_ee{};
bool g_log_enabled[LOG_CAT_COUNT];
void log_write(LogCat, const char *, ...) {}
uint8_t *g_ram;
constexpr uint32_t object = 0x10000;
static EEState native_after;
static unsigned native_calls;
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
static void check(bool ok, const char *s) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", s);
        std::exit(1);
    }
}
static void putf(uint32_t a, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    MEM_SW(a, u);
}
// the loader stand-in overwrites every register (also the upper GPR halves)
extern "C" void f_0021ABF8_orig() {
    ++native_calls;
    std::memset(&g_ee, 0xa5, sizeof g_ee);
    native_after = g_ee;
    MEM_SW(0x200, 0x12345678);
}
extern "C" void f_0021ABF8();
static void invoke() {
    std::memset(&g_ee, 0, sizeof g_ee);
    g_ee.r[4].uw[0] = object;
    f_0021ABF8();
    check(std::memcmp(&g_ee, &native_after, sizeof g_ee) == 0, "the loader's register results are kept");
    check(MEM_LW(0x200) == 0x12345678, "the loader's memory writes are kept");
}
static void fixture() {
    MEM_SW(object + 0x220, object + 0xf8);
    MEM_SW(object + 0x224, object + 0x1f8);
    MEM_SW(object + 0x21c, object + 0xb4);
    MEM_SW(object + 0x1f8, 5);
    MEM_SW(object + 0x550, 1);
    float values[] = {-0.0f, 10, 15, 30, 500};
    for (unsigned i = 0; i < 5; ++i) {
        putf(object + 0xf8 + i * 16, values[i]);
        for (unsigned j = 1; j < 4; ++j) MEM_SW(object + 0xf8 + i * 16 + j * 4, 0x12340000 + i * 4 + j);
        MEM_SW(object + 0x74 + i * 4, i);
        MEM_SW(object + 0xb4 + i * 4, i);
    }
}
int main(int argc, char **argv) {
    check(argc == 2, "mode: off/one/two/invalid");
    const std::string mode = argv[1];
    unsetenv("EOD_CAR_LOD_MULTIPLIER");
    if (mode == "one") setenv("EOD_CAR_LOD_MULTIPLIER", "1", 1);
    if (mode == "two") setenv("EOD_CAR_LOD_MULTIPLIER", "2", 1);
    if (mode == "invalid") setenv("EOD_CAR_LOD_MULTIPLIER", "nan", 1);
    float parsed = 0;
    for (const char *s : {"", "0", "-2", "nan", "inf", "2oops", "1e90"})
        check(!eod_lod::parse_multiplier(s, parsed) && parsed == 1, "a bad multiplier gives 1");
    float in[] = {-0.0f, 10, 15, 30, 500}, out[5];
    check(eod_lod::scale(in, out, 5, 1) && std::memcmp(in, out, sizeof in) == 0, "x1 keeps every bit, -0 too");
    check(eod_lod::scale(in, out, 5, 2) && out[1] == 20 && out[4] == 1000, "x2 scales the positive distances");
    float bad[] = {10, 5}, guard[] = {77, 88};
    check(!eod_lod::scale(bad, guard, 2, 2) && guard[0] == 77 && guard[1] == 88, "unordered: nothing written");
    float huge[] = {std::numeric_limits<float>::max()};
    check(!eod_lod::scale(huge, guard, 1, 2) && guard[0] == 77, "overflow: nothing written");
    check(!eod_lod::scale(in, out, 0, 2) && !eod_lod::scale(in, out, 17, 2), "count 1..16");
    std::vector<uint8_t> memory(EE_RAM_SIZE);
    g_ram = memory.data();
    fixture();
    MEM_SW(0x200, 0x12345678);
    const auto before = memory;
    invoke();
    auto expected = before;
    if (mode == "two")
        for (unsigned i = 1; i < 5; ++i) {
            const float v = in[i] * 2;
            std::memcpy(expected.data() + object + 0xf8 + i * 16, &v, 4);
        }
    check(memory == expected, "only the positive distances change (and nothing when off, x1 or invalid)");
    // a copy's table, a level without a model, an overflow: left alone
    for (unsigned which = 0; which < 3; ++which) {
        fixture();
        if (which == 0) MEM_SW(object + 0x220, object + 0x100);
        if (which == 1) MEM_SW(object + 0x74 + 4, 0xffffffffu);
        if (which == 2) putf(object + 0xf8 + 64, std::numeric_limits<float>::max());
        const auto rejected = memory;
        invoke();
        check(memory == rejected, "an unsafe table is not changed");
    }
    check(native_calls == 4, "the loader runs exactly once per call");
    std::printf("car LOD %s: passed\n", mode.c_str());
}
