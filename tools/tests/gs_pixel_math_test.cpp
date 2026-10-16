#include "../../runtime/gs/gs_pixel_math.h"
#include <cstdio>
#include <cstdlib>
static void expect(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}
int main() {
    // Primary reference: parallel-gs ubershader's documented hardware case
    // F=255, Cs=128, FogColor=128 must give 128, not the old 127.
    expect(gs::fog_channel(128, 128, 255) == 128, "fog must preserve the documented equal-colour case");
    for (int color = 0; color < 256; color++)
        for (int factor = 0; factor < 256; factor++)
            expect(gs::fog_channel(color, color, factor) == color,
                   "equal source/fog colour stays constant at every coefficient");
    expect(gs::fog_channel(222, 12, 0) == 12, "F=0 selects fog colour exactly");
    expect(gs::fog_channel(0, 255, 255) == 1, "F=255 is 255/256, not opaque source");
    expect(gs::fog_channel(255, 0, 255) == 254, "high-end F=255 rounding");
    expect(gs::fog_channel(0, 255, 128) == 128, "negative halfway difference rounds up");
    expect(gs::fog_channel(255, 0, 128) == 128, "positive halfway difference rounds up");
    std::puts("GS pixel arithmetic tests passed");
}
