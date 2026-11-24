#include "render/static_immediate.h"
#include <algorithm>
#include <array>
#include <cstdio>

int main() {
    // 0x361C98 reserves four vertices. After three emissions, 0x364BC8
    // writes 0x8003 at word 5. Word 11 still holds unused color storage.
    std::array<uint32_t, 40> packet{0x01000202, 0x05000000, 0x6102c0c6, 0, 0x6001c000, 0x8003,
        0x01000103, 0x6e04c001, 0, 0, 0, 0xdeadbeef, 0x01000203, 0x05000001, 0x7808c002};
    packet[39] = 0x1400004e;
    native::StaticImmediateChunk chunk{};
    const auto decode = [&](size_t size) {
        return native::decode_static_immediate_chunk(reinterpret_cast<const uint8_t *>(packet.data()), size, chunk);
    };
    if (!decode(sizeof(packet)) || chunk.emitted != 3 || chunk.vertices != 60 || chunk.bytes != sizeof(packet)) return 1;
    if (decode(sizeof(packet) - 4)) return 1;
    packet[5] = 0x8000;
    if (!decode(sizeof(packet)) || chunk.emitted != 0) return 1;
    packet[5] = 0x8005;
    if (decode(sizeof(packet))) return 1;
    packet[5] = 0x8003;
    packet[39] = 0x140004a7; // A+D state dispatch cannot be retired as geometry.
    if (decode(sizeof(packet))) return 1;
    // The first chunk may establish the VIF column mask before position unpack.
    packet[39] = 0x1400004e;
    std::array<uint32_t, 48> masked{};
    std::copy_n(packet.begin(), 12, masked.begin());
    constexpr uint32_t mask[] = {0x20000000, 0x40404040, 0x30000000, 0, 0, 0, 0x3f800000};
    std::copy_n(mask, 7, masked.begin() + 12);
    std::copy(packet.begin() + 12, packet.end(), masked.begin() + 19);
    const auto decode_masked = [&] {
        return native::decode_static_immediate_chunk(reinterpret_cast<const uint8_t *>(masked.data()), sizeof(masked), chunk);
    };
    if (!decode_masked() || chunk.emitted != 3 || chunk.vertices != 88 || chunk.bytes != sizeof(masked)) return 1;
    masked[13] = 0;
    if (decode_masked()) return 1;
    std::puts("static immediate chunk extents and dispatch checks passed");
}
