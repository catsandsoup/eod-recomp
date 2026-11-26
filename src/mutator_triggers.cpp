#include "mutator_triggers.h"
#include "mutator_engine.h"
#include "mutator_hooks.h"
#include "netplay_car.h"
#include "core/memory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mutator {
namespace {

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

bool car_alive(uint32_t car) {
    return valid_ram(car, 0x1C34u) && ee_r32(car + 0x1BE8u) < 10u;
}

void read_car_pos(uint32_t car, float out[3]) {
    out[0] = rf(car + 0x88u);
    out[1] = rf(car + 0x8Cu);
    out[2] = rf(car + 0x90u);
}

float read_car_speed(uint32_t car) {
    const float vx = rf(car + 0x104u);
    const float vy = rf(car + 0x108u);
    const float vz = rf(car + 0x10Cu);
    return std::sqrt(vx * vx + vy * vy + vz * vz);
}

void repair_car_now(uint32_t car) {
    if (!valid_ram(car, 0x1C34u)) return;
    if (mutator_hooks::native_repair_car) {
        mutator_hooks::native_repair_car(car);
    } else {
        wf(car + 0x1BECu, 0.0f);
        ee_w32(car + 0x1BE8u, 0u);
    }
}

void wreck_car_now(uint32_t car) {
    if (!valid_ram(car, 0x1C34u)) return;
    wf(car + 0x1BECu, 1.0f);
    ee_w32(car + 0x1BE8u, 10u);
}

bool point_in_trigger(const ActiveTriggerState &t, const float p[3]) {
    const float dx = p[0] - t.center[0];
    const float dy = p[1] - t.center[1];
    const float dz = p[2] - t.center[2];
    switch (t.kind) {
    case TriggerKind::Sphere:
        return (dx * dx + dy * dy + dz * dz) <= (t.radius * t.radius);
    case TriggerKind::Cylinder:
    case TriggerKind::RingOut:
        return (dx * dx + dz * dz) <= (t.radius * t.radius) &&
               (t.kind == TriggerKind::RingOut || std::fabs(dy) <= std::max(t.extents[1], 2.0f));
    case TriggerKind::OBB: {
        const float c = std::cos(-t.yaw_rad);
        const float s = std::sin(-t.yaw_rad);
        const float lx = dx * c - dz * s;
        const float lz = dx * s + dz * c;
        return std::fabs(lx) <= t.extents[0] &&
               std::fabs(dy) <= std::max(t.extents[1], 2.0f) &&
               std::fabs(lz) <= t.extents[2];
    }
    case TriggerKind::Flag:
        return (dx * dx + dz * dz) <= (4.5f * 4.5f) && std::fabs(dy) <= 4.0f;
    }
    return false;
}

void update_infection_counts(uint32_t slot_count) {
    uint8_t survivors = 0, infected = 0;
    const uint32_t lim = std::min<uint32_t>(slot_count, kMaxSlots);
    for (uint32_t i = 0; i < lim; ++i) {
        const auto &st = mutator_hooks::slots[i];
        if (!st.active) continue;
        if (st.role == RoleId::Survivor) ++survivors;
        else if (st.role == RoleId::Infected) ++infected;
    }
    mutator_hooks::hud.survivors_alive = survivors;
    mutator_hooks::hud.infected_count = infected;
}

uint32_t mix32_tips(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void apply_tips_role(uint32_t slot, RoleId role, const std::vector<RoleOverride> &roles) {
    if (slot >= kMaxSlots) return;
    auto &st = mutator_hooks::slots[slot];
    st.active = true;
    st.role = role;
    const RoleOverride *found = nullptr;
    for (const auto &ro : roles) {
        if (ro.role == role) {
            found = &ro;
            break;
        }
    }
    if (found) {
        st.feat_bits = found->feat_bits;
        st.traits = found->traits;
        st.traits.sanitize();
    } else if (role == RoleId::Tagger) {
        st.feat_bits = FB_TURBO | FB_INVINCIBLE;
        st.traits = PhysicsTraits{};
        st.traits.top_speed_scale = 1.12f;
        st.traits.torque_scale = 1.18f;
        st.traits.grip_scale = 1.10f;
    } else {
        st.feat_bits = FB_INVINCIBLE;
        st.traits = PhysicsTraits{};
    }
}

} // namespace

void TriggerSystem::reset() {
    defs_.clear();
    states_.clear();
    for (auto &row : dwell_) row.fill(0.0f);
    tag_cooldown_.fill(0.0f);
    tips_fuse_.fill(0.0f);
    eliminated_mask_ = 0;
    tips_in_slot_ = 0xFF;
    tips_fuse_remaining_ = 0.0f;
    tips_rng_ = 0;
    tips_winner_announced_ = false;
    mutator_hooks::tips_immune.fill(false);
    mutator_hooks::active_triggers.clear();
    mutator_hooks::hud.zone_warning.clear();
    mutator_hooks::hud.storm_radius_m = 0.0f;
    mutator_hooks::hud.survivors_alive = 0;
    mutator_hooks::hud.infected_count = 0;
    mutator_hooks::hud.eliminated_mask = 0;
    mutator_hooks::hud.tips_in_slot = 0xFF;
    mutator_hooks::hud.tips_fuse_sec = 0.0f;
    mutator_hooks::hud.tips_alive_count = 0;
}

void TriggerSystem::init_tips(uint32_t seed, uint32_t slot_count, float fuse_sec,
                              const std::vector<RoleOverride> &roles) {
    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);
    const float clamped_fuse = std::isfinite(fuse_sec) ? std::clamp(fuse_sec, 5.0f, 120.0f) : 25.0f;
    tips_fuse_.fill(clamped_fuse);
    tag_cooldown_.fill(0.0f);
    mutator_hooks::tips_immune.fill(false);
    eliminated_mask_ = 0;
    tips_winner_announced_ = false;
    tips_rng_ = mix32_tips(seed ^ 0x71B57A6u);
    tips_in_slot_ = 0xFF;
    tips_fuse_remaining_ = 0.0f;

    std::vector<uint8_t> alive_list;
    alive_list.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        apply_tips_role(i, RoleId::Survivor, roles);
        if (car_alive(netplay_car::car_of_slot(i))) {
            alive_list.push_back(static_cast<uint8_t>(i));
        }
    }
    if (alive_list.empty()) {
        for (uint32_t i = 0; i < count; ++i) {
            alive_list.push_back(static_cast<uint8_t>(i));
        }
    }
    if (!alive_list.empty()) {
        const uint8_t chosen = alive_list[tips_rng_ % alive_list.size()];
        tips_in_slot_ = chosen;
        tips_fuse_remaining_ = tips_fuse_[chosen];
        apply_tips_role(chosen, RoleId::Tagger, roles);
    }

    mutator_hooks::hud.eliminated_mask = eliminated_mask_;
    mutator_hooks::hud.tips_in_slot = tips_in_slot_;
    mutator_hooks::hud.tips_fuse_sec = tips_fuse_remaining_;
    mutator_hooks::hud.tips_alive_count = static_cast<uint8_t>(alive_list.size());
}

void TriggerSystem::spawn_from_defs(const std::vector<TriggerDef> &defs) {
    reset();
    const size_t n = std::min<size_t>(defs.size(), kMaxTriggersPerRound);
    defs_.reserve(n);
    states_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        TriggerDef d = defs[i];
        GametypeConfig::sanitize_trigger(d);
        float center[3] = {d.origin[0], d.origin[1], d.origin[2]};
        Engine::resolve_anchor(d.anchor, d.origin, d.spline_pct, center);

        ActiveTriggerState st;
        st.id = d.id ? d.id : static_cast<uint16_t>(i + 1);
        st.kind = d.kind;
        st.center[0] = center[0];
        st.center[1] = center[1];
        st.center[2] = center[2];
        st.radius = std::max(d.extents[0], 1.0f);
        st.extents[0] = d.extents[0];
        st.extents[1] = d.extents[1];
        st.extents[2] = d.extents[2];
        st.yaw_rad = d.yaw_rad;
        st.carrier_slot = 0xFF;
        st.color_rgba = d.color_rgba;
        st.active = true;

        defs_.push_back(d);
        states_.push_back(st);
    }
    mutator_hooks::active_triggers = states_;
}

TriggerSystem::TickResult TriggerSystem::tick_host(
    float dt, uint32_t sim_tick, uint32_t slot_count, int local_slot,
    uint32_t active_rule_flags, const std::vector<RoleOverride> &roles) {
    (void)sim_tick;
    TickResult res{};
    dt = std::clamp(dt, 0.0f, 0.25f);
    const uint32_t count = std::min<uint32_t>(slot_count, kMaxSlots);

    std::string warning;
    float storm_radius = 0.0f;

    for (size_t ti = 0; ti < states_.size() && ti < defs_.size(); ++ti) {
        auto &st = states_[ti];
        const auto &def = defs_[ti];
        if (!st.active) continue;

        if (st.kind == TriggerKind::RingOut) {
            if (def.shrink_rate_mps > 0.0f && st.radius > def.min_radius_m) {
                st.radius = std::max(def.min_radius_m, st.radius - def.shrink_rate_mps * dt);
                st.extents[0] = st.extents[2] = st.radius;
                res.triggers_dirty = true;
            }
            storm_radius = st.radius;

            for (uint32_t slot = 0; slot < count; ++slot) {
                const uint32_t car = netplay_car::car_of_slot(slot);
                if (!car_alive(car)) continue;
                float pos[3];
                read_car_pos(car, pos);
                const float dx = pos[0] - st.center[0];
                const float dz = pos[2] - st.center[2];
                const bool outside = (dx * dx + dz * dz) > (st.radius * st.radius);
                if (outside) {
                    if (static_cast<int>(slot) == local_slot) warning = "OUTSIDE SAFE ZONE!";
                    if (def.dps_outside > 0.0f) {
                        const float next_dmg = std::clamp(rf(car + 0x1BECu) + def.dps_outside * dt, 0.0f, 1.0f);
                        wf(car + 0x1BECu, next_dmg);
                        if (next_dmg >= 1.0f) {
                            wreck_car_now(car);
                            eliminated_mask_ |= static_cast<uint16_t>(1u << slot);
                            res.triggers_dirty = true;
                            if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                        }
                    } else if (def.dwell_sec > 0.0f) {
                        dwell_[ti][slot] += dt;
                        if (dwell_[ti][slot] >= def.dwell_sec) {
                            dwell_[ti][slot] = 0.0f;
                            wreck_car_now(car);
                            eliminated_mask_ |= static_cast<uint16_t>(1u << slot);
                            res.triggers_dirty = true;
                            if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                        }
                    }
                } else {
                    dwell_[ti][slot] = 0.0f;
                }
            }
        } else if (st.kind == TriggerKind::Flag) {
            if (st.carrier_slot < count) {
                const uint32_t carrier_car = netplay_car::car_of_slot(st.carrier_slot);
                if (!car_alive(carrier_car)) {
                    // Carrier wrecked -> drop flag where carrier died
                    st.carrier_slot = 0xFF;
                    res.triggers_dirty = true;
                    if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                } else {
                    float pos[3];
                    read_car_pos(carrier_car, pos);
                    st.center[0] = pos[0];
                    st.center[1] = pos[1] + 1.5f;
                    st.center[2] = pos[2];
                    res.triggers_dirty = true;
                    if (static_cast<int>(st.carrier_slot) == local_slot && warning.empty()) {
                        warning = "HOLDING THE FLAG!";
                    }
                    dwell_[ti][st.carrier_slot] += dt;
                    if (dwell_[ti][st.carrier_slot] >= 1.0f) {
                        dwell_[ti][st.carrier_slot] -= 1.0f;
                        mutator_hooks::slots[st.carrier_slot].score += 1;
                        res.round_dirty = true;
                    }
                }
            }
            if (st.carrier_slot == 0xFF) {
                for (uint32_t slot = 0; slot < count; ++slot) {
                    const uint32_t car = netplay_car::car_of_slot(slot);
                    if (!car_alive(car)) continue;
                    float pos[3];
                    read_car_pos(car, pos);
                    if (point_in_trigger(st, pos)) {
                        st.carrier_slot = static_cast<uint8_t>(slot);
                        dwell_[ti][slot] = 0.0f;
                        res.triggers_dirty = true;
                        if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                        if (mutator_hooks::show_hud_banner) mutator_hooks::show_hud_banner("FLAG CAPTURED!", 2000);
                        break;
                    }
                }
            }
        } else {
            // Cylinder / Sphere / OBB (Stop-and-Go box, King of the Hill, Safe Zone)
            const bool require_stop = (active_rule_flags & RF_STOP_AND_GO) != 0;
            for (uint32_t slot = 0; slot < count; ++slot) {
                const uint32_t car = netplay_car::car_of_slot(slot);
                if (!car_alive(car)) {
                    dwell_[ti][slot] = 0.0f;
                    continue;
                }
                float pos[3];
                read_car_pos(car, pos);
                const bool inside = point_in_trigger(st, pos);
                const bool speed_ok = !require_stop || (read_car_speed(car) <= 2.5f);
                if (inside && speed_ok) {
                    dwell_[ti][slot] += dt;
                    if (static_cast<int>(slot) == local_slot && warning.empty() && def.dwell_sec > 0.0f) {
                        warning = require_stop ? "HOLDING STOP BOX..." : "CAPTURING ZONE...";
                    }
                    if (def.dwell_sec <= 0.0f || dwell_[ti][slot] >= def.dwell_sec) {
                        dwell_[ti][slot] = 0.0f;
                        mutator_hooks::slots[slot].score += 1;
                        res.round_dirty = true;
                        if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                    }
                } else {
                    dwell_[ti][slot] = 0.0f;
                }
            }
        }
    }

    // Halo Infection (Tag) contact conversion
    if (active_rule_flags & RF_INFECTION_TAG) {
        for (uint32_t i = 0; i < count; ++i) {
            if (tag_cooldown_[i] > 0.0f) tag_cooldown_[i] = std::max(0.0f, tag_cooldown_[i] - dt);
        }

        const RoleOverride *infected_role = nullptr;
        for (const auto &ro : roles) {
            if (ro.role == RoleId::Infected) {
                infected_role = &ro;
                break;
            }
        }

        constexpr float kTagRadiusSq = 5.0f * 5.0f;
        for (uint32_t a = 0; a < count; ++a) {
            if (!mutator_hooks::slots[a].active || mutator_hooks::slots[a].role != RoleId::Infected) continue;
            if (tag_cooldown_[a] > 0.0f) continue;
            const uint32_t car_a = netplay_car::car_of_slot(a);
            if (!car_alive(car_a)) continue;
            float pos_a[3];
            read_car_pos(car_a, pos_a);

            for (uint32_t b = 0; b < count; ++b) {
                if (b == a || !mutator_hooks::slots[b].active || mutator_hooks::slots[b].role != RoleId::Survivor)
                    continue;
                if (tag_cooldown_[b] > 0.0f) continue;
                const uint32_t car_b = netplay_car::car_of_slot(b);
                if (!car_alive(car_b)) continue;
                float pos_b[3];
                read_car_pos(car_b, pos_b);

                const float dx = pos_a[0] - pos_b[0];
                const float dy = pos_a[1] - pos_b[1];
                const float dz = pos_a[2] - pos_b[2];
                if (dx * dx + dy * dy + dz * dz <= kTagRadiusSq) {
                    auto &victim = mutator_hooks::slots[b];
                    victim.role = RoleId::Infected;
                    if (infected_role) {
                        victim.feat_bits = infected_role->feat_bits;
                        victim.traits = infected_role->traits;
                        victim.traits.sanitize();
                        if (infected_role->repair_on_assign) repair_car_now(car_b);
                    } else {
                        victim.feat_bits |= FB_TURBO;
                        repair_car_now(car_b);
                    }
                    tag_cooldown_[b] = 1.5f;
                    mutator_hooks::slots[a].score += 1;
                    res.round_dirty = true;
                    if (mutator_hooks::show_hud_banner) mutator_hooks::show_hud_banner("INFECTED!", 2200);
                    if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                }
            }
        }
        update_infection_counts(count);
    }

    // Tips (Tag) — Hot Potato Elimination
    if (active_rule_flags & RF_TIPS_TAG) {
        for (uint32_t i = 0; i < count; ++i) {
            if (tag_cooldown_[i] > 0.0f) tag_cooldown_[i] = std::max(0.0f, tag_cooldown_[i] - dt);
            mutator_hooks::tips_immune[i] = (tag_cooldown_[i] > 0.0f);
        }

        std::vector<uint8_t> alive_list;
        alive_list.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            if (eliminated_mask_ & static_cast<uint16_t>(1u << i)) continue;
            if (car_alive(netplay_car::car_of_slot(i))) {
                alive_list.push_back(static_cast<uint8_t>(i));
            }
        }
        mutator_hooks::hud.tips_alive_count = static_cast<uint8_t>(alive_list.size());

        if (alive_list.size() <= 1) {
            if (!tips_winner_announced_ && count > 1) {
                tips_winner_announced_ = true;
                if (mutator_hooks::show_hud_banner) mutator_hooks::show_hud_banner("TIPS WINNER!", 3500);
                if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
            }
            tips_in_slot_ = 0xFF;
            tips_fuse_remaining_ = 0.0f;
        } else {
            const bool in_alive = std::find(alive_list.begin(), alive_list.end(), tips_in_slot_) != alive_list.end();
            if (!in_alive) {
                tips_rng_ = mix32_tips(tips_rng_ + 0x9e3779b9u);
                tips_in_slot_ = alive_list[tips_rng_ % alive_list.size()];
                apply_tips_role(tips_in_slot_, RoleId::Tagger, roles);
                res.round_dirty = true;
                res.triggers_dirty = true;
            }

            const uint8_t in = tips_in_slot_;
            tips_fuse_[in] = std::max(0.0f, tips_fuse_[in] - dt);
            tips_fuse_remaining_ = tips_fuse_[in];
            res.triggers_dirty = true;

            if (tips_fuse_[in] <= 0.0f) {
                // Eliminate the player whose personal fuse hit 0.0 s
                mutator_hooks::slots[in].feat_bits &= ~FB_INVINCIBLE;
                mutator_hooks::slots[in].role = RoleId::Default;
                wreck_car_now(netplay_car::car_of_slot(in));
                eliminated_mask_ |= static_cast<uint16_t>(1u << in);
                tag_cooldown_[in] = 0.0f;
                mutator_hooks::tips_immune[in] = false;
                if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                if (mutator_hooks::show_hud_banner) mutator_hooks::show_hud_banner("TIME'S UP! ELIMINATED!", 2500);

                alive_list.erase(std::remove(alive_list.begin(), alive_list.end(), in), alive_list.end());
                mutator_hooks::hud.tips_alive_count = static_cast<uint8_t>(alive_list.size());
                if (alive_list.size() <= 1) {
                    tips_in_slot_ = 0xFF;
                    tips_fuse_remaining_ = 0.0f;
                    if (!tips_winner_announced_) {
                        tips_winner_announced_ = true;
                        if (mutator_hooks::show_hud_banner) mutator_hooks::show_hud_banner("TIPS WINNER!", 3500);
                    }
                } else {
                    tips_rng_ = mix32_tips(tips_rng_ + 0x9e3779b9u);
                    tips_in_slot_ = alive_list[tips_rng_ % alive_list.size()];
                    apply_tips_role(tips_in_slot_, RoleId::Tagger, roles);
                    tips_fuse_remaining_ = tips_fuse_[tips_in_slot_];
                }
                res.round_dirty = true;
                res.triggers_dirty = true;
            } else if (tag_cooldown_[in] <= 0.0f) {
                const uint32_t car_in = netplay_car::car_of_slot(in);
                if (car_alive(car_in)) {
                    float pos_in[3];
                    read_car_pos(car_in, pos_in);
                    constexpr float kTipsTagRadiusSq = 5.2f * 5.2f;
                    for (uint32_t b = 0; b < count; ++b) {
                        if (b == in) continue;
                        if (eliminated_mask_ & static_cast<uint16_t>(1u << b)) continue;
                        if (!mutator_hooks::slots[b].active || mutator_hooks::slots[b].role != RoleId::Survivor)
                            continue;
                        if (tag_cooldown_[b] > 0.0f) continue;
                        const uint32_t car_b = netplay_car::car_of_slot(b);
                        if (!car_alive(car_b)) continue;
                        float pos_b[3];
                        read_car_pos(car_b, pos_b);
                        const float dx = pos_in[0] - pos_b[0];
                        const float dy = pos_in[1] - pos_b[1];
                        const float dz = pos_in[2] - pos_b[2];
                        if (dx * dx + dy * dy + dz * dz < kTipsTagRadiusSq) {
                            apply_tips_role(in, RoleId::Survivor, roles);
                            tag_cooldown_[in] = 3.5f;
                            mutator_hooks::tips_immune[in] = true;
                            mutator_hooks::slots[in].score += 1;

                            apply_tips_role(b, RoleId::Tagger, roles);
                            tag_cooldown_[b] = 0.75f;
                            mutator_hooks::tips_immune[b] = true;

                            tips_in_slot_ = static_cast<uint8_t>(b);
                            tips_fuse_remaining_ = tips_fuse_[b];
                            res.round_dirty = true;
                            res.triggers_dirty = true;
                            if (mutator_hooks::play_sfx_cue) mutator_hooks::play_sfx_cue("arena_horn.sfx");
                            if (mutator_hooks::show_hud_banner) mutator_hooks::show_hud_banner("TIPS!", 1800);
                            break;
                        }
                    }
                }
            }
        }

        mutator_hooks::hud.tips_in_slot = tips_in_slot_;
        mutator_hooks::hud.tips_fuse_sec = tips_fuse_remaining_;
    }

    mutator_hooks::active_triggers = states_;
    mutator_hooks::hud.zone_warning = warning;
    mutator_hooks::hud.storm_radius_m = storm_radius;
    mutator_hooks::hud.eliminated_mask = eliminated_mask_;
    return res;
}

void TriggerSystem::apply_remote(const TriggerSyncPacket &pkt, int local_slot, uint32_t slot_count) {
    states_ = pkt.triggers;
    eliminated_mask_ = pkt.eliminated_mask;
    tips_in_slot_ = pkt.tips_in_slot;
    tips_fuse_remaining_ = pkt.tips_fuse_remaining;
    mutator_hooks::active_triggers = states_;
    mutator_hooks::hud.eliminated_mask = eliminated_mask_;
    mutator_hooks::hud.tips_in_slot = tips_in_slot_;
    mutator_hooks::hud.tips_fuse_sec = tips_fuse_remaining_;
    if (mutator_hooks::active_rule_flags & RF_TIPS_TAG) {
        uint8_t alive = 0;
        const uint32_t lim = std::min<uint32_t>(slot_count, kMaxSlots);
        for (uint32_t i = 0; i < lim; ++i) {
            if ((eliminated_mask_ & static_cast<uint16_t>(1u << i)) == 0 &&
                car_alive(netplay_car::car_of_slot(i))) {
                ++alive;
            }
        }
        mutator_hooks::hud.tips_alive_count = alive;
    }

    std::string warning;
    float storm_radius = 0.0f;
    const uint32_t local_car = (local_slot >= 0 && local_slot < static_cast<int>(kMaxSlots))
        ? netplay_car::car_of_slot(static_cast<uint32_t>(local_slot)) : 0u;
    float lpos[3] = {};
    const bool have_local = car_alive(local_car);
    if (have_local) read_car_pos(local_car, lpos);

    for (const auto &st : states_) {
        if (!st.active) continue;
        if (st.kind == TriggerKind::RingOut) {
            storm_radius = st.radius;
            if (have_local) {
                const float dx = lpos[0] - st.center[0];
                const float dz = lpos[2] - st.center[2];
                if (dx * dx + dz * dz > st.radius * st.radius) warning = "OUTSIDE SAFE ZONE!";
            }
        } else if (st.kind == TriggerKind::Flag && st.carrier_slot == static_cast<uint8_t>(local_slot)) {
            if (warning.empty()) warning = "HOLDING THE FLAG!";
        }
    }
    mutator_hooks::hud.zone_warning = warning;
    mutator_hooks::hud.storm_radius_m = storm_radius;
    update_infection_counts(slot_count);
}

TriggerSyncPacket TriggerSystem::build_packet(
    uint32_t sim_tick, const std::vector<ChainPair> &chains, uint16_t hitch_mask) const {
    TriggerSyncPacket pkt;
    pkt.sim_tick = sim_tick;
    pkt.hitch_mask = hitch_mask;
    pkt.eliminated_mask = eliminated_mask_;
    pkt.tips_in_slot = tips_in_slot_;
    pkt.tips_fuse_remaining = tips_fuse_remaining_;
    pkt.triggers = states_;
    pkt.chains = chains;
    return pkt;
}

} // namespace mutator
