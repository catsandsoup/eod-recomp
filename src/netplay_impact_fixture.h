#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "netplay_car.h"

namespace netplay_impact_fixture {

constexpr uint32_t kStartTick = 300;

inline bool test_session() {
    const char *headless = std::getenv("EOD_HEADLESS");
    const char *rendered = std::getenv("EOD_NETPLAY_TEST_RENDERED");
    return (headless && std::strcmp(headless, "1") == 0) || (rendered && std::strcmp(rendered, "1") == 0);
}

inline bool enabled() {
    static const bool on = [] {
        const char *impact = std::getenv("EOD_NETPLAY_TEST_IMPACT");
        return test_session() && impact && std::strcmp(impact, "1") == 0;
    }();
    return on;
}

// Anoka Battle shooting range. Only seed the starting grid: the actual pad,
// native AI, weapon, contact solver and damage path run throughout the test.
inline void position_chicken_test_car(uint32_t car, uint8_t owner, unsigned ai_index, netplay_car::State &state) {
    static const bool enabled = test_session() && std::getenv("EOD_NETPLAY_TEST_CHICKEN_GRID");
    if (!enabled) return;
    const bool human = owner != 0xff;
    const float facing = human ? (owner == 0 ? 1.f : -1.f) : (ai_index % 2 == 0 ? 1.f : -1.f);
    state.pose[0] = state.pose[1] = state.pose[3] = state.pose[5] = state.pose[7] = state.pose[8] = 0.f;
    state.pose[2] = -facing;
    state.pose[4] = 1.f;
    state.pose[6] = facing;
    // Humans face each other inside the stock splash radius at the midpoint.
    // Each AI faces an opponent outside its minimum safe firing distance.
    state.pose[9] = -facing * (human ? 5.f : 15.f);
    state.pose[10] = .35f;
    state.pose[11] = human ? -45.f : (ai_index < 2 ? 0.f : 40.f);
    for (int axis = 0; axis < 3; ++axis)
        state.vel[axis] = state.ang[axis] = 0.f;
    netplay_car::write(car, state);
}

inline unsigned pair_slot(uint8_t owner) {
    static const bool guest_pair = std::getenv("EOD_NETPLAY_TEST_GUEST_PAIR") != nullptr;
    if (owner == 1) return 0;
    return owner == (guest_pair ? 2 : 0) ? 1 : 2;
}

inline bool pack_collision() {
    static const bool on = std::getenv("EOD_NETPLAY_TEST_PACK_COLLISION") != nullptr;
    return on;
}

inline unsigned collider_count() { return pack_collision() ? 3 : 2; }

struct Observation {
    uint32_t event = 0;
    uint32_t tick = 0;
    uint32_t local_slot = 0;
    std::array<uint32_t, 3> cars{};  // guest, opposing driver, optional third collider
    uint32_t observer = 0;
    std::array<int32_t, 3> native_indices{};
    unsigned contacts = 0;
    unsigned probes = 0;
    bool started = false;
};

inline Observation observation;

inline int collider_index(int32_t native_index) {
    for (unsigned i = 0; i < collider_count(); ++i)
        if (observation.native_indices[i] == native_index) return static_cast<int>(i);
    return -1;
}

inline void inspect_guard(uint32_t car, int32_t other, uint32_t surface, bool allowed) {
    if (!observation.started || observation.tick > 180 || observation.probes >= 64) return;
    for (unsigned i = 0; i < collider_count(); ++i) {
        const int peer = collider_index(other);
        if (car != observation.cars[i] || ((peer < 0 || peer == static_cast<int>(i)) && surface != 0x70)) continue;
        ++observation.probes;
        std::fprintf(stderr, "[netplay-impact] guard event=%u tick=%u local=%u victim=%u other_native=%d surface=%u allowed=%u\n",
                     observation.event, observation.tick, observation.local_slot, i, other, surface, allowed ? 1u : 0u);
        return;
    }
}

inline void contact(uint32_t car, int32_t other, float impulse, float before, float after, uint32_t wreck) {
    if (!observation.started || observation.tick > 180 || observation.contacts >= 64) return;
    for (unsigned i = 0; i < collider_count(); ++i) {
        const int peer = collider_index(other);
        if (car != observation.cars[i] || peer < 0 || peer == static_cast<int>(i)) continue;
        ++observation.contacts;
        std::fprintf(stderr, "[netplay-impact] contact event=%u tick=%u local=%u victim=%u other=%d impulse=%.3f damage_before=%.6f damage_after=%.6f wreck=%u\n",
                     observation.event, observation.tick, observation.local_slot, i, peer, impulse, before, after, wreck);
        return;
    }
}

}  // namespace netplay_impact_fixture
