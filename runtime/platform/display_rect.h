#pragma once
#include <cstdint>
#include <numeric>
#include <string>

// The four original frame codes are kept for recorded GS command streams.
// A custom ratio carries its reduced numerator and denominator in the frame
// itself so captures, resizes and GS replay use the projection for that frame.
inline constexpr int kCustomAspect = 0x20000000;
inline constexpr int kAspectHeightMask = 0x3fff;
inline constexpr int kAspectWidthMask = 0x7fff;
inline bool custom_aspect(int aspect) {
    return (aspect & 0xe0000000u) == kCustomAspect &&
           ((aspect >> 14) & kAspectWidthMask) && (aspect & kAspectHeightMask);
}
struct AspectRatio { int width, height; };
inline AspectRatio aspect_ratio(int aspect) {
    if (custom_aspect(aspect)) return {(aspect >> 14) & kAspectWidthMask, aspect & kAspectHeightMask};
    if (aspect == 1) return {16, 9};
    if (aspect == 3) return {16, 10};
    return {4, 3};
}
inline int drawable_aspect(int width, int height) {
    // Retain the original field of view on a portrait/narrow drawable.
    if (width <= 0 || height <= 0 || int64_t(width) * 3 <= int64_t(height) * 4) return 0;
    const int divisor = std::gcd(width, height);
    width /= divisor;
    height /= divisor;
    if (width == 16 && height == 9) return 1;
    if (width == 8 && height == 5) return 3;
    if (width > kAspectWidthMask || height > kAspectHeightMask) return 0;
    return kCustomAspect | (width << 14) | height;
}
inline float projection_x_scale(int aspect) {
    const auto ratio = aspect_ratio(aspect);
    return (4.0f * ratio.height) / (3.0f * ratio.width);
}
inline int display_aspect(const std::string &aspect, int drawable = 0) {
    if (aspect == "display") return drawable;
    return aspect == "16:9" ? 1 : aspect == "stretch" ? 2 : aspect == "16:10" ? 3 : 0;
}
struct DisplayRect { int x, y, width, height; };
inline DisplayRect display_rect(int aspect, int width, int height) {
    if (width <= 0 || height <= 0) return {0,0,0,0};
    int w=width,h=height;
    if (aspect != 2) {
        const auto ratio = aspect_ratio(aspect);
        h = int(int64_t(width) * ratio.height / ratio.width);
        if (h > height) { h=height; w=int(int64_t(height) * ratio.width / ratio.height); }
    }
    return {(width-w)/2,(height-h)/2,w,h};
}
