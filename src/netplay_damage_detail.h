#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace netplay_damage {
// The retail car has 157 dent slots. One full checkpoint fits in a single UDP datagram.
constexpr unsigned kMaxBones = 157;
std::vector<uint8_t> capture(uint32_t car);
std::vector<uint8_t> pack(const std::vector<uint8_t> &detail);
std::vector<uint8_t> unpack(const uint8_t *data, size_t size);
bool valid(const uint8_t *data, size_t size);
bool apply(uint32_t car, const std::vector<uint8_t> &detail);
void apply_parts(uint32_t car, uint16_t panels, uint8_t wheels, int side);
}
