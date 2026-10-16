// Tests src/hle_camera.cpp without a game process or graphics device: the
// projection trace leaves the native call untouched, and the per-frame
// presentation aspect follows the launch setting until the game's display
// mode (upstream 1.0 widescreen, src/game_patches.cpp) requests one.
#include "core/config.h"
#include "core/cpu.h"
#include "platform/presentation.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;

extern "C" void f_0034E530();
extern "C" void f_0034E530_orig() { g_ee.r[2].ud[1]++; }
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
static void check(bool ok, const char *why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main(int argc, char **argv) {
    check(argc == 2, "aspect argument");
    std::vector<uint8_t> ram(EE_RAM_SIZE);
    g_ram = ram.data();
    const std::string aspect = argv[1];
    g_config.aspect = aspect;
    const int launch = aspect == "16:9" ? 1 : aspect == "16:10" ? 3 : aspect == "stretch" ? 2 : 0;
    for (int i = 0; i < 24; i++) {
        EEState expected = g_ee;
        expected.r[2].ud[1]++;
        f_0034E530();
        check(std::memcmp(&expected, &g_ee, sizeof g_ee) == 0, "projection trace leaves native EE state untouched");
    }
    check(eod_presentation_aspect() == launch, "launch aspect before the game sets a display mode");
    for (int want : {0, 1, 2}) {
        presentation::requested.store(want);
        check(eod_presentation_aspect() == want, "the game's requested display aspect");
    }
    presentation::requested.store(-1);
    check(eod_presentation_aspect() == launch, "launch aspect again");
    std::printf("native camera %s: ok\n", aspect.c_str());
    return 0;
}
