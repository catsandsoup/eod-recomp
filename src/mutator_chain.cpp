#include "mutator_chain.h"
#include "mutator_hooks.h"
#include "netplay_car.h"
#include "core/memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mutator {
namespace {

constexpr float kMaxChainForceN = 65000.0f;

bool valid_ram(uint32_t a, uint32_t bytes) {
    return a && !(a & 3u) && bytes <= EE_RAM_SIZE && a <= EE_RAM_SIZE - bytes;
}

float rf(uint32_t a) {
    const uint32_t u = ee_r32(a);
    float f = 0.0f;
    std::memcpy(&f, &u, 4);
    return f;
}

void wf(uint32_t a, float f) {
    uint32_t u = 0;
    std::memcpy(&u, &f, 4);
    ee_w32(a, u);
}

} // namespace

void ChainSystem::reset() {
    pairs_.clear();
    mutator_hooks::active_chains.clear();
}

void ChainSystem::setup_pairs(uint32_t slot_count, float rest_len_m, float stiffness, float damping) {
    pairs_.clear();
    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);
    for (uint32_t i = 0; i + 1 < count && pairs_.size() < kMaxChainPairs; i += 2) {
        ChainPair p;
        p.slot_a = static_cast<uint8_t>(i);
        p.slot_b = static_cast<uint8_t>(i + 1);
        p.rest_len_m = std::clamp(rest_len_m, 2.0f, 100.0f);
        p.stiffness = std::clamp(stiffness, 0.0f, 250000.0f);
        p.damping = std::clamp(damping, 0.0f, 50000.0f);
        pairs_.push_back(p);
    }
    mutator_hooks::active_chains = pairs_;
}

void ChainSystem::set_pairs(const std::vector<ChainPair> &pairs) {
    pairs_.clear();
    const size_t n = std::min<size_t>(pairs.size(), kMaxChainPairs);
    pairs_.assign(pairs.begin(), pairs.begin() + static_cast<std::ptrdiff_t>(n));
    mutator_hooks::active_chains = pairs_;
}

void ChainSystem::step_host(uint32_t slot_count) {
    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);
    for (const auto &pair : pairs_) {
        if (pair.slot_a >= count || pair.slot_b >= count) continue;
        const uint32_t car_a = netplay_car::car_of_slot(pair.slot_a);
        const uint32_t car_b = netplay_car::car_of_slot(pair.slot_b);
        if (!car_a || !car_b) continue;
        const uint32_t body_a = netplay_car::body_of(car_a);
        const uint32_t body_b = netplay_car::body_of(car_b);
        if (!valid_ram(body_a, 0xB0u) || !valid_ram(body_b, 0xB0u)) continue;

        const float ax = rf(body_a + 0x00u), ay = rf(body_a + 0x04u), az = rf(body_a + 0x08u);
        const float bx = rf(body_b + 0x00u), by = rf(body_b + 0x04u), bz = rf(body_b + 0x08u);
        const float dx = bx - ax, dy = by - ay, dz = bz - az;
        const float dist_sq = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(dist_sq) || dist_sq <= pair.rest_len_m * pair.rest_len_m || dist_sq < 1e-6f) {
            continue; // Chain is slack
        }

        const float dist = std::sqrt(dist_sq);
        const float inv_d = 1.0f / dist;
        const float nx = dx * inv_d, ny = dy * inv_d, nz = dz * inv_d;
        const float stretch = dist - pair.rest_len_m;

        const float vax = rf(body_a + 0x50u), vay = rf(body_a + 0x54u), vaz = rf(body_a + 0x58u);
        const float vbx = rf(body_b + 0x50u), vby = rf(body_b + 0x54u), vbz = rf(body_b + 0x58u);
        const float rel_v = (vbx - vax) * nx + (vby - vay) * ny + (vbz - vaz) * nz;

        const float f_mag = std::clamp(pair.stiffness * stretch + pair.damping * rel_v, 0.0f, kMaxChainForceN);
        if (f_mag <= 0.0f) continue;

        const float fx = f_mag * nx, fy = f_mag * ny, fz = f_mag * nz;
        wf(body_a + 0x90u, rf(body_a + 0x90u) + fx);
        wf(body_a + 0x94u, rf(body_a + 0x94u) + fy);
        wf(body_a + 0x98u, rf(body_a + 0x98u) + fz);

        wf(body_b + 0x90u, rf(body_b + 0x90u) - fx);
        wf(body_b + 0x94u, rf(body_b + 0x94u) - fy);
        wf(body_b + 0x98u, rf(body_b + 0x98u) - fz);
    }
    mutator_hooks::active_chains = pairs_;
}

bool ChainSystem::detach_trailer_for_slot(uint32_t slot) {
    const uint32_t car = netplay_car::car_of_slot(slot);
    if (!car) return false;
    const uint32_t trailer = netplay_car::attached_trailer(car);
    if (!trailer) return false;
    if (mutator_hooks::native_detach_trailer) {
        mutator_hooks::native_detach_trailer(trailer);
    } else if (valid_ram(trailer, 0x1C34u)) {
        ee_w32(trailer + 0x1C18u, 0u);
    }
    return true;
}

uint16_t ChainSystem::snapshot_hitch_mask(uint32_t slot_count) {
    uint16_t mask = 0;
    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);
    for (uint32_t slot = 0; slot < count; ++slot) {
        const uint32_t car = netplay_car::car_of_slot(slot);
        if (car && netplay_car::attached_trailer(car) != 0) {
            mask |= static_cast<uint16_t>(1u << slot);
        }
    }
    mutator_hooks::active_hitch_mask = mask;
    return mask;
}

void ChainSystem::apply_remote_hitch_mask(uint16_t host_hitch_mask, uint32_t slot_count) {
    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);
    for (uint32_t slot = 0; slot < count; ++slot) {
        if (!(host_hitch_mask & (1u << slot))) {
            detach_trailer_for_slot(slot);
        }
    }
    mutator_hooks::active_hitch_mask = host_hitch_mask;
}

} // namespace mutator
