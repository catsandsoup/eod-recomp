// Layout written by ImmStatic (0x361C98) and updated per emitted vertex (0x364BC8).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace native {
struct StaticImmediateChunk {
    uint32_t emitted;
    size_t colors, vertices, bytes;
};

inline bool decode_static_immediate_chunk(const uint8_t *data, size_t bytes, StaticImmediateChunk &out) {
    const auto word = [data](size_t offset) {
        uint32_t value;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    if (bytes < 32 || word(0) != 0x01000202 || word(4) != 0x05000000 || word(8) != 0x6102c0c6 ||
        word(16) != 0x6001c000 || word(24) != 0x01000103) return false;
    const uint32_t unpack_colors = word(28);
    const uint32_t capacity = (unpack_colors >> 16) & 0xff;
    const uint32_t emitted = word(20) & 0x7fff;
    if ((unpack_colors & 0xff00ffff) != 0x6e00c001 || capacity == 0 || capacity > 60 ||
        word(20) != (0x8000 | emitted) || emitted > capacity) return false;
    size_t pos = 32 + capacity * 4;
    if (pos + 12 > bytes) return false;
    if (word(pos) == 0x20000000) {
        constexpr uint32_t mask[] = {0x20000000, 0x40404040, 0x30000000, 0, 0, 0, 0x3f800000};
        if (pos + sizeof(mask) + 12 > bytes || std::memcmp(data + pos, mask, sizeof(mask)) != 0) return false;
        pos += sizeof(mask);
    }
    if (word(pos) != 0x01000203 || word(pos + 4) != 0x05000001 ||
        word(pos + 8) != (0x7800c002 | (capacity * 2 << 16))) return false;
    const size_t vertices = pos + 12;
    const size_t kick = vertices + capacity * 24;
    const size_t next = (kick + 4 + 15) & ~size_t(15);
    if (next > bytes || word(kick) != 0x1400004e) return false;
    for (pos = kick + 4; pos < next; pos += 4)
        if (word(pos) != 0) return false;
    out = {emitted, 32, vertices, next};
    return true;
}
}  // namespace native
