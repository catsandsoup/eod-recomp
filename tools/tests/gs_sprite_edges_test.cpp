// sprite_axis_segments: upscaled sprites fill whole native pixels at the
// scissor edges and nothing else changes (see runtime/gs/gs_sprite_edges.h).
#include "../../runtime/gs/gs_sprite_edges.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
static void expect(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        std::exit(1);
    }
}
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
int main() {
    using gs::SpriteSegment;
    SpriteSegment s[3];
    // The game's last strip: -0.9..319.1 in 12.4 is 287.1..319.1, u 576..640.
    const int a = 287 * 16 + 2, b = 319 * 16 + 2;
    expect(gs::sprite_axis_segments(a, b, 576.f, 640.f, 0, 319, 1, s) == 1, "RenderScale 1 is unchanged");
    expect(s[0].p0 == a / 16.f && s[0].p1 == b / 16.f && s[0].c0 == 576.f && s[0].c1 == 640.f, "own span");
    expect(gs::sprite_axis_segments(a, b, 576.f, 640.f, 0, 319, 6, s) == 2, "last scissor pixel completed");
    expect(s[0].p1 == s[1].p0 && near(s[1].p1, 319.5f), "sliver from the edge to the pixel's end");
    expect(near(s[1].c0, 576.f + (319.f - a / 16.f) * 2.f) && s[1].c0 == s[1].c1,
           "sliver takes the texture coordinate of pixel 319's centre");
    // Same edges with the sprite reversed: same segments.
    expect(gs::sprite_axis_segments(b, a, 640.f, 576.f, 0, 319, 6, s) == 2 && near(s[1].p1, 319.5f), "reversed");
    // An interior edge (scissor wider than the sprite) is not touched.
    expect(gs::sprite_axis_segments(a, b, 576.f, 640.f, 0, 639, 6, s) == 1, "interior edge unchanged");
    // A sprite starting on the first scissor pixel's centre gets the left half.
    expect(gs::sprite_axis_segments(0, 32 * 16, 0.f, 32.f, 0, 639, 6, s) == 2, "first scissor pixel completed");
    expect(near(s[0].p0, -0.5f) && s[0].p1 == 0.f && s[0].c0 == 0.f && s[0].c1 == 0.f, "left sliver");
    // -0.9 already covers pixel 0 completely; an edge at the pixel boundary needs nothing.
    expect(gs::sprite_axis_segments(-14, 31 * 16 + 2, 0.f, 32.f, 0, 639, 6, s) == 1, "-0.9 left edge");
    expect(gs::sprite_axis_segments(600 * 16, 639 * 16 + 8, 0.f, 1.f, 0, 639, 6, s) == 1, "edge at 639.5");
    // Covers no scissor pixel at all (GS draws nothing there): no slivers.
    expect(gs::sprite_axis_segments(640 * 16 + 2, 650 * 16, 0.f, 1.f, 0, 639, 6, s) == 1, "outside the scissor");
    expect(gs::sprite_axis_segments(100 * 16 + 2, 100 * 16 + 8, 0.f, 1.f, 0, 100, 6, s) == 1, "empty sprite");
    std::puts("GS sprite edge tests passed");
}
