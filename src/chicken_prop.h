#pragma once
#include <array>
#include <cstdint>
#include <cmath>

namespace chicken_prop {
enum class Type : uint8_t { Wobble = 1, Panel, Wheel };
struct Hit {
    Type type{};
    uint16_t object = 0;  // track placement ordinal, or donor's participant slot
    uint32_t part = 0, donor_life = 0;
    std::array<float, 12> pose{};
    std::array<float, 3> linear{}, angular{}, point{}, impulse{};
};
// Use the same bounds before enqueueing and after decoding. An unsupported
// native value must not create an unacknowledgeable hole in the ordered stream.
inline bool valid_hit(const Hit &h) {
    if (h.type == Type::Wobble) {
        if (h.object >= 512 || h.part || h.donor_life) return false;
    } else if (h.type == Type::Panel || h.type == Type::Wheel) {
        if (h.object >= 16 || (h.type == Type::Wheel ? h.part > 3 :
            (!h.part || (h.part & (h.part - 1))))) return false;
    } else return false;
    auto bounded = [](float v, float limit) { return std::isfinite(v) && std::fabs(v) <= limit; };
    for (unsigned i = 0; i < h.pose.size(); ++i)
        if (!bounded(h.pose[i], i < 9 ? 1.01f : 100000.f)) return false;
    for (const auto *vec : {&h.linear, &h.angular, &h.point})
        for (float v : *vec) if (!bounded(v, 100000.f)) return false;
    // The stock upward component can grow large for a near-centre contact.
    for (float v : h.impulse) if (!bounded(v, 16000000.f)) return false;
    return true;
}
bool capture(uint32_t body, uint32_t point, uint32_t impulse, Hit &hit);
bool apply(uint32_t shooter, const Hit &hit);
}
