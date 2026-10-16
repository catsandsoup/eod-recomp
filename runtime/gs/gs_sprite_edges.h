// gs_sprite_edges.h - whole native pixels at the scissor edges of upscaled sprites.
//
// The GS covers pixel x of a sprite when ceil(x0) <= x < ceil(x1) (12.4
// coordinates). Upscaled, native pixel x is the area [x - 0.5, x + 0.5) and
// only the sub-pixels whose centres lie inside the sprite are drawn, so an
// edge between two pixel centres covers part of a pixel the GS fills. Inside
// the scissor the neighbouring sprite draws the rest, but the game's
// full-target passes (32-pixel strips ending at 319.1 or 639.1) end inside the
// last scissor pixel: above RenderScale 1 the last column and row of the
// target kept a third of their old contents. The 2P car shadow pass clears
// its PSMCT16 stencil by writing Z=0 through a PSMZ16 Z buffer at the same
// address, and Z column 319 is colour column 287 (the block arrangements
// differ), so stale stencil sub-pixels showed as a dark vertical line.
//
// sprite_axis_segments() splits one axis of a sprite into its own span and
// the slivers that complete the first and last scissor pixels it covers. A
// sliver takes the texture coordinate of its native pixel centre, which is
// what the GS samples there. At RenderScale 1 nothing is added.
#pragma once
#include <cstdint>
#include <utility>

namespace gs {

struct SpriteSegment {
    float p0, p1;  // GS coordinates of the segment
    float c0, c1;  // texture coordinate (s, t or u, v) at p0 and p1
};

// a, b: 12.4 window coordinates of the two vertices on this axis; ca, cb their
// texture coordinates. clip0..clip1: the scissor pixels inside the target
// (inclusive). Returns the number of segments (1 to 3), in increasing
// position; with 1, out[0] is the sprite's own span.
inline int sprite_axis_segments(int32_t a, int32_t b, float ca, float cb, int clip0, int clip1, int scale,
                                SpriteSegment out[3]) {
    if (b < a) {
        std::swap(a, b);
        std::swap(ca, cb);
    }
    const float p0 = (float)a / 16.0f, p1 = (float)b / 16.0f;
    const SpriteSegment span = {p0, p1, ca, cb};
    const int32_t first = (a + 15) >> 4, end = (b + 15) >> 4;  // GS: pixels first .. end-1
    if (scale <= 1 || first >= end || clip0 > clip1 || first > clip1 || end - 1 < clip0) {
        out[0] = span;
        return 1;
    }
    auto at = [&](int x) { return ca + ((float)x - p0) * (cb - ca) / (p1 - p0); };
    int n = 0;
    if (first <= clip0 && p0 > (float)clip0 - 0.5f) {
        const float c = at(clip0);
        out[n++] = {(float)clip0 - 0.5f, p0, c, c};
    }
    out[n++] = span;
    if (end - 1 >= clip1 && p1 < (float)clip1 + 0.5f) {
        const float c = at(clip1);
        out[n++] = {p1, (float)clip1 + 0.5f, c, c};
    }
    return n;
}

}  // namespace gs
