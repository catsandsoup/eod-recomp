#include "mutator_engine.h"
#include "mutator_hooks.h"
#include "mutator_presets.h"
#include "mutator_warp.h"
#include "battle_layout.h"
#include "netplay_car.h"
#include "core/memory.h"
#include "vehicle_dynamics.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mutator {
namespace {

constexpr uint32_t kVehicleTable = 0x003CBEE4;
constexpr uint32_t kVehicleCount = 0x00439704;
constexpr uint32_t kVehicleStride = 0x54;

constexpr uint32_t kBodyCountAddr = 0x00494D28;
constexpr uint32_t kBodyArrayAddr = 0x00494D30;
constexpr uint32_t kTuningArrayAddr = 0x00494D38;
constexpr uint32_t kEntityListAddr = 0x003CE4F8;
constexpr uint32_t kEntityCountAddr = 0x003CE548;

constexpr uint32_t kCourseCacheAddr = 0x003CD190;
constexpr uint32_t kCourseCountAddr = 0x003CD1B4;
constexpr uint32_t kCourseMagic = 0x104F128Bu;

constexpr uint32_t kEventMasterAddr = 0x003CF194;
constexpr float kBaseGravity = 9.81f;

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

std::string read_cstr(uint32_t addr, size_t max_len) {
    std::string out;
    if (!addr || addr >= EE_RAM_SIZE) return out;
    for (size_t i = 0; i < max_len && addr + i < EE_RAM_SIZE; ++i) {
        const char c = static_cast<char>(ee_r8(addr + static_cast<uint32_t>(i)));
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

int body_index_of(uint32_t car) {
    if (!valid_ram(car, 0x1C34)) return -1;
    const uint32_t count = ee_r32(kEntityCountAddr);
    const uint32_t body_count = ee_r32(kBodyCountAddr);
    if (!count || count > kMaxVehiclePhysicsBodies || body_count != count) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        if (ee_r32(kEntityListAddr + i * 4u) == car) return static_cast<int>(i);
    }
    return -1;
}

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float rand_range(uint32_t &state, float lo, float hi) {
    state = mix32(state + 0x9e3779b9u);
    const float u = static_cast<float>(state & 0xFFFFu) / 65535.0f;
    return lo + u * (hi - lo);
}

uint32_t active_slot_count() {
    return std::min<uint32_t>(std::max<uint32_t>(netplay_car::slot_count(), 1u), kMaxSlots);
}

} // namespace

Engine &engine() {
    static Engine instance;
    return instance;
}

bool Engine::load_json(const std::string &json_text, std::string &err) {
    GametypeConfig parsed;
    if (!parse_gametype_json(json_text, parsed, err)) return false;
    set_config(parsed);
    return true;
}

bool Engine::load_file(const std::string &path, std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "Could not open gametype file: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return load_json(ss.str(), err);
}

void Engine::set_config(const GametypeConfig &cfg) {
    config_ = cfg;
    config_.sanitize();
    has_config_ = true;
    mutator_hooks::suppress_weapons = !config_uses_weapons(config_);
}

void Engine::clear_config() {
    has_config_ = false;
    config_ = GametypeConfig{};
    mutator_hooks::suppress_weapons = false;
    reset_match();
}

void Engine::reset_match() {
    match_active_ = false;
    is_authority_ = true;
    local_slot_ = 0;
    event_id_ = 0;
    seed_ = 0;
    current_round_ = 0;
    round_serial_ = 0;
    round_start_tick_ = 0;
    round_duration_ticks_ = 0;
    fiesta_epoch_ = 0;
    last_tick_ = 0;
    round_order_.clear();
    active_round_title_.clear();
    latest_round_ = RoundPacket{};
    for (auto &b : baselines_) b = BodyBaseline{};
    triggers_.reset();
    chains_.reset();
    mutator_hooks::reset();
    mutator_hooks::on_world_tick = nullptr;
    vehicle_dynamics_reset_gravity();
}

bool Engine::has_live_sync_state() const {
    return match_active_ && has_config_ &&
           (triggers_.has_triggers() || chains_.has_chains() ||
            (mutator_hooks::active_rule_flags & (RF_TRAILER | RF_INFECTION_TAG | RF_TIPS_TAG)) != 0);
}

TriggerSyncPacket Engine::build_trigger_packet(uint32_t sim_tick) const {
    const uint16_t hitch = (mutator_hooks::active_rule_flags & RF_TRAILER)
        ? ChainSystem::snapshot_hitch_mask(active_slot_count())
        : 0u;
    return triggers_.build_packet(sim_tick, chains_.pairs(), hitch);
}

void Engine::apply_remote_triggers(const TriggerSyncPacket &pkt) {
    if (!match_active_ || !has_config_) return;
    const uint32_t count = active_slot_count();
    triggers_.apply_remote(pkt, local_slot_, count);
    chains_.set_pairs(pkt.chains);
    if (mutator_hooks::active_rule_flags & RF_TRAILER) {
        ChainSystem::apply_remote_hitch_mask(pkt.hitch_mask, count);
    }
}

const WarpZone *Engine::find_warp_zone(const std::string &name) const {
    if (name.empty()) return nullptr;
    for (const auto &z : config_.warp_zones) {
        if (z.name == name) return &z;
    }
    return nullptr;
}

void Engine::apply_round_defaults(uint32_t active_flags, uint32_t active_feats, const PhysicsTraits &traits) {
    mutator_hooks::active = true;
    mutator_hooks::active_rule_flags = active_flags;

    const uint32_t count = active_slot_count();
    for (uint32_t slot = 0; slot < kMaxSlots; ++slot) {
        auto &st = mutator_hooks::slots[slot];
        st.active = (slot < count);
        st.role = RoleId::Default;
        st.feat_bits = active_feats;
        st.traits = traits;
        st.traits.sanitize();
        st.direction_sign = ((active_flags & RF_SUICIDE) && (slot & 1u)) ? -1 : 1;
        if ((active_flags & RF_WHIP_AROUND) && ((round_serial_ > 0 ? round_serial_ - 1u : current_round_) & 1u)) {
            st.direction_sign = static_cast<int8_t>(-st.direction_sign);
        }
    }

    if (!config_.roles.empty() && !(active_flags & RF_TIPS_TAG)) {
        apply_slot_role(0, config_.roles[0]);
        if (config_.roles.size() > 1) {
            for (uint32_t slot = 1; slot < count; ++slot) {
                const auto &r = config_.roles[1 + ((slot - 1) % (config_.roles.size() - 1))];
                apply_slot_role(slot, r);
            }
        }
    }
}

void Engine::build_round_packet(uint16_t def_index, uint32_t sim_tick, uint32_t duration_ticks,
                                uint32_t active_flags, uint32_t active_feats, const PhysicsTraits &traits) {
    RoundPacket pkt;
    pkt.round_serial = round_serial_;
    pkt.round_def_index = def_index;
    pkt.warp_gen = mutator_hooks::global_warp_gen;
    pkt.start_tick = sim_tick;
    pkt.duration_ticks = duration_ticks;
    pkt.rule_flags = active_flags;
    pkt.global_feats = active_feats;
    pkt.traits = traits;
    pkt.traits.sanitize();
    const uint32_t count = active_slot_count();
    pkt.slot_count = static_cast<uint8_t>(count);
    for (uint32_t i = 0; i < count; ++i) {
        pkt.slot_roles[i] = mutator_hooks::slots[i].role;
        pkt.slot_direction[i] = mutator_hooks::slots[i].direction_sign;
        pkt.slot_scores[i] = mutator_hooks::slots[i].score;
    }
    latest_round_ = pkt;
}

void Engine::begin_micro_round(uint32_t order_pos, uint32_t sim_tick) {
    if (config_.micro_rounds.empty()) return;
    current_round_ = order_pos;
    ++round_serial_;
    const uint16_t def_idx = round_order_.empty()
        ? static_cast<uint16_t>(order_pos % config_.micro_rounds.size())
        : round_order_[order_pos % round_order_.size()];
    const auto &def = config_.micro_rounds[def_idx];

    round_start_tick_ = sim_tick;
    round_duration_ticks_ = std::max<uint32_t>(60u, static_cast<uint32_t>(std::lround(def.duration_sec * 60.0f)));
    active_round_title_ = def.title.empty() ? config_.name : def.title;

    const uint32_t active_flags = config_.base_rule_flags | def.rule_flags;
    const uint32_t active_feats = config_.base_feats | def.global_feats;
    PhysicsTraits combined = def.traits;
    combined.sanitize();

    apply_round_defaults(active_flags, active_feats, combined);

    const uint32_t count = active_slot_count();
    if (!def.triggers.empty()) triggers_.spawn_from_defs(def.triggers);
    else if (!config_.base_triggers.empty()) triggers_.spawn_from_defs(config_.base_triggers);
    else triggers_.reset();

    if (active_flags & RF_TIPS_TAG) {
        triggers_.init_tips(seed_ ^ (round_serial_ * 0x9e3779b9u), count, config_.tips_fuse_sec, config_.roles);
    }

    if (active_flags & RF_CHAIN_TETHER) chains_.setup_pairs(count);
    else chains_.reset();

    if (const WarpZone *wz = find_warp_zone(def.warp_zone)) {
        warp_formation_to_zone(*wz, count, def.repair_all, seed_ ^ (round_serial_ * 0x9e3779b9u));
    } else if (def.repair_all) {
        for (uint32_t slot = 0; slot < count; ++slot) {
            if (const uint32_t car = netplay_car::car_of_slot(slot)) {
                if (mutator_hooks::native_repair_car) mutator_hooks::native_repair_car(car);
                else {
                    wf(car + 0x1BECu, 0.0f);
                    ee_w32(car + 0x1BE8u, 0u);
                }
            }
        }
    }

    sync_live_physics();
    build_round_packet(def_idx, sim_tick, round_duration_ticks_, active_flags, active_feats, combined);
    refresh_hud(sim_tick);

    // Fire transient HUD banner & native resident audio cues
    if (mutator_hooks::show_hud_banner && !active_round_title_.empty()) {
        mutator_hooks::show_hud_banner(active_round_title_.c_str(), 3500);
    }
    if (def.pa_speech_sid >= 0 && mutator_hooks::play_pa_speech) {
        mutator_hooks::play_pa_speech(def.pa_speech_sid);
    }
    if (!def.sfx_cue.empty() && mutator_hooks::play_sfx_cue) {
        mutator_hooks::play_sfx_cue(def.sfx_cue.c_str());
    }
}

bool Engine::apply_remote_round(const RoundPacket &pkt, uint32_t current_sim_tick) {
    if (!has_config_ || pkt.round_serial <= round_serial_) return false;
    round_serial_ = pkt.round_serial;
    current_round_ = pkt.round_serial - 1u;
    round_start_tick_ = pkt.start_tick;
    round_duration_ticks_ = pkt.duration_ticks;
    latest_round_ = pkt;

    const MicroRoundDef *def = (pkt.round_def_index < config_.micro_rounds.size())
        ? &config_.micro_rounds[pkt.round_def_index] : nullptr;
    active_round_title_ = (def && !def->title.empty()) ? def->title : config_.name;

    apply_round_defaults(pkt.rule_flags, pkt.global_feats, pkt.traits);
    const uint32_t count = std::min<uint32_t>(pkt.slot_count, kMaxSlots);
    for (uint32_t i = 0; i < count; ++i) {
        mutator_hooks::slots[i].role = pkt.slot_roles[i];
        mutator_hooks::slots[i].direction_sign = pkt.slot_direction[i] < 0 ? -1 : 1;
        mutator_hooks::slots[i].score = pkt.slot_scores[i];
        // Re-apply matching RoleOverride traits/feats if configured
        for (const auto &ro : config_.roles) {
            if (ro.role == pkt.slot_roles[i] && pkt.slot_roles[i] != RoleId::Default) {
                mutator_hooks::slots[i].feat_bits = pkt.global_feats | ro.feat_bits;
                mutator_hooks::slots[i].traits = ro.traits;
                break;
            }
        }
    }

    // If the host bumped warp_gen on a round transition, warp our local vehicle too
    if (pkt.warp_gen != mutator_hooks::global_warp_gen && def) {
        if (const WarpZone *wz = find_warp_zone(def->warp_zone)) {
            warp_formation_to_zone(*wz, std::max<uint32_t>(count, active_slot_count()),
                                   def->repair_all, seed_ ^ (pkt.round_serial * 0x9e3779b9u));
        }
        mutator_hooks::global_warp_gen = pkt.warp_gen;
        for (uint32_t i = 0; i < kMaxSlots; ++i) mutator_hooks::slots[i].warp_gen = pkt.warp_gen;
    }

    if (def && !def->triggers.empty() && !triggers_.has_triggers()) {
        triggers_.spawn_from_defs(def->triggers);
    } else if (!config_.base_triggers.empty() && !triggers_.has_triggers()) {
        triggers_.spawn_from_defs(config_.base_triggers);
    }
    if (pkt.rule_flags & RF_CHAIN_TETHER) {
        if (!chains_.has_chains()) chains_.setup_pairs(std::max<uint32_t>(count, active_slot_count()));
    } else {
        chains_.reset();
    }

    sync_live_physics();
    refresh_hud(current_sim_tick);

    if (mutator_hooks::show_hud_banner && !active_round_title_.empty()) {
        mutator_hooks::show_hud_banner(active_round_title_.c_str(), 3500);
    }
    if (def && def->pa_speech_sid >= 0 && mutator_hooks::play_pa_speech) {
        mutator_hooks::play_pa_speech(def->pa_speech_sid);
    }
    if (def && !def->sfx_cue.empty() && mutator_hooks::play_sfx_cue) {
        mutator_hooks::play_sfx_cue(def->sfx_cue.c_str());
    }
    return true;
}

void Engine::advance_round(uint32_t sim_tick) {
    if (!match_active_ || !has_config_ || !is_authority_) return;
    if (!config_.micro_rounds.empty()) {
        begin_micro_round(current_round_ + 1u, sim_tick);
    }
}

void Engine::on_world_bound(uint32_t event_id, uint32_t seed, bool authority, int local_slot) {
    if (!has_config_) {
        if (const char *env_path = std::getenv("EOD_GAMETYPE_JSON"); env_path && *env_path) {
            std::string err;
            if (!load_file(env_path, err)) {
                std::fprintf(stderr, "[mutator] failed to load EOD_GAMETYPE_JSON '%s': %s\n", env_path, err.c_str());
            }
        }
    }
    if (!has_config_) {
        reset_match();
        return;
    }

    for (auto &b : baselines_) b = BodyBaseline{};
    vehicle_dynamics_reset_gravity();
    event_id_ = event_id;
    seed_ = seed ? seed : 0x5EED1234u;
    is_authority_ = authority;
    local_slot_ = (local_slot >= 0 && local_slot < static_cast<int>(kMaxSlots)) ? local_slot : 0;
    current_round_ = 0;
    round_serial_ = 0;
    round_start_tick_ = 0;
    round_duration_ticks_ = 0;
    fiesta_epoch_ = 0;
    last_tick_ = 0;
    latest_round_ = RoundPacket{};
    match_active_ = true;
    mutator_hooks::suppress_weapons = !config_uses_weapons(config_);

    // Build deterministic micro-round order (optionally Fisher-Yates shuffled from seed)
    round_order_.clear();
    if (!config_.micro_rounds.empty()) {
        round_order_.resize(config_.micro_rounds.size());
        for (size_t i = 0; i < round_order_.size(); ++i) round_order_[i] = static_cast<uint16_t>(i);
        if (config_.shuffle_rounds && round_order_.size() > 1) {
            uint32_t shuf_rng = mix32(seed_ ^ 0x517CC1B7u);
            for (size_t i = round_order_.size() - 1; i > 0; --i) {
                shuf_rng = mix32(shuf_rng + 0x9e3779b9u);
                std::swap(round_order_[i], round_order_[shuf_rng % (i + 1)]);
            }
        }
    }

    if (!config_.micro_rounds.empty()) {
        if (is_authority_) {
            begin_micro_round(0, 0);
        } else {
            // Guest waits for host's authoritative K_MUTATOR_ROUND, using base defaults meanwhile
            active_round_title_ = config_.name;
            apply_round_defaults(config_.base_rule_flags, config_.base_feats, config_.base_traits);
            sync_live_physics();
            refresh_hud(0);
        }
    } else {
        active_round_title_ = config_.name;
        apply_round_defaults(config_.base_rule_flags, config_.base_feats, config_.base_traits);
        if (!config_.base_triggers.empty()) triggers_.spawn_from_defs(config_.base_triggers);
        else triggers_.reset();
        if (is_authority_ && (config_.base_rule_flags & RF_TIPS_TAG)) {
            triggers_.init_tips(seed_, active_slot_count(), config_.tips_fuse_sec, config_.roles);
        }
        if (config_.base_rule_flags & RF_CHAIN_TETHER) chains_.setup_pairs(active_slot_count());
        else chains_.reset();
        if (config_.base_rule_flags & RF_FIESTA_ROULETTE) {
            apply_fiesta_reroll(0);
        }
        sync_live_physics();
        if (is_authority_) {
            ++round_serial_;
            build_round_packet(0, 0, 0, config_.base_rule_flags, config_.base_feats, config_.base_traits);
        }
        refresh_hud(0);
    }

    mutator_hooks::on_world_tick = [] {
        engine().tick(ee_r32(0x003CDA8C));
    };
}

void Engine::apply_fiesta_reroll(uint32_t epoch) {
    fiesta_epoch_ = epoch;
    const uint32_t count = active_slot_count();
    uint32_t rng = mix32(seed_ ^ (epoch * 0x85ebca6bu) ^ 0xA5A5F00Du);

    constexpr uint32_t kFiestaFeats[] = {
        FB_CHICKEN_STRAIGHT,
        FB_CHICKEN_MORTAR,
        FB_TURBO,
        FB_CHICKEN_STRAIGHT | FB_TURBO,
        FB_DEADLY_AMBITION | FB_TURBO,
        FB_ONE_HIT_KILLS | FB_CHICKEN_STRAIGHT,
    };
    const uint32_t global_roll = kFiestaFeats[mix32(rng) % (sizeof(kFiestaFeats) / sizeof(kFiestaFeats[0]))];
    const float round_gravity = rand_range(rng, 0.35f, 1.45f);

    for (uint32_t slot = 0; slot < count; ++slot) {
        auto &st = mutator_hooks::slots[slot];
        st.active = true;
        st.feat_bits = config_.base_feats | global_roll;
        PhysicsTraits t = config_.base_traits;
        t.gravity_scale = round_gravity;
        t.mass_scale = rand_range(rng, 0.55f, 2.40f);
        t.grip_scale = rand_range(rng, 0.50f, 1.75f);
        t.torque_scale = rand_range(rng, 0.85f, 1.80f);
        t.top_speed_scale = rand_range(rng, 0.90f, 1.65f);
        t.damage_taken_scale = rand_range(rng, 0.50f, 2.00f);
        t.chicken_force_scale = rand_range(rng, 0.80f, 2.50f);
        t.chicken_radius_scale = rand_range(rng, 0.80f, 1.80f);
        t.chicken_reload_sec = rand_range(rng, 1.00f, 3.00f);
        t.sanitize();
        st.traits = t;
    }
    sync_live_physics();
}

void Engine::refresh_hud(uint32_t sim_tick) {
    auto &h = mutator_hooks::hud;
    h.visible = match_active_ && has_config_;
    h.gametype_name = config_.name;
    h.round_title = active_round_title_.empty() ? config_.name : active_round_title_;
    h.round_number = round_serial_ > 0 ? round_serial_ : (current_round_ + 1u);
    h.total_rounds = static_cast<uint32_t>(config_.micro_rounds.size());
    if (round_duration_ticks_ > 0) {
        const uint32_t elapsed = sim_tick >= round_start_tick_ ? (sim_tick - round_start_tick_) : 0u;
        const uint32_t rem = elapsed < round_duration_ticks_ ? (round_duration_ticks_ - elapsed) : 0u;
        h.remaining_sec = static_cast<float>(rem) / 60.0f;
    } else {
        h.remaining_sec = 0.0f;
    }
    h.active_rule_flags = mutator_hooks::active_rule_flags;
    const uint32_t slot = static_cast<uint32_t>(std::clamp(local_slot_, 0, static_cast<int>(kMaxSlots - 1)));
    const auto &st = mutator_hooks::slots[slot];
    h.local_feat_bits = st.feat_bits;
    h.local_role = st.role;
    h.local_direction_sign = st.direction_sign;
    h.local_gravity_scale = st.traits.gravity_scale;
    h.local_mass_scale = st.traits.mass_scale;
}

void Engine::tick(uint32_t sim_tick) {
    if (!match_active_ || !has_config_) return;
    const float dt = (last_tick_ > 0 && sim_tick > last_tick_)
        ? std::clamp(static_cast<float>(sim_tick - last_tick_) / 60.0f, 1.0f / 240.0f, 0.25f)
        : (1.0f / 60.0f);
    last_tick_ = sim_tick;

    if (is_authority_ && !config_.micro_rounds.empty() && round_duration_ticks_ > 0) {
        if (sim_tick >= round_start_tick_ + round_duration_ticks_) {
            begin_micro_round(current_round_ + 1u, sim_tick);
            return;
        }
    }

    if (config_.micro_rounds.empty() && (config_.base_rule_flags & RF_FIESTA_ROULETTE)) {
        const uint32_t interval_ticks = std::max<uint32_t>(
            60u, static_cast<uint32_t>(std::lround(config_.fiesta_interval_sec * 60.0f)));
        const uint32_t epoch = sim_tick / interval_ticks;
        if (epoch != fiesta_epoch_) {
            apply_fiesta_reroll(epoch);
            refresh_hud(sim_tick);
            return;
        }
    }

    if (is_authority_) {
        const uint32_t count = active_slot_count();
        const auto tr = triggers_.tick_host(dt, sim_tick, count, local_slot_,
                                            mutator_hooks::active_rule_flags, config_.roles);
        if (tr.round_dirty) {
            ++round_serial_;
            build_round_packet(latest_round_.round_def_index, round_start_tick_, round_duration_ticks_,
                               mutator_hooks::active_rule_flags, latest_round_.global_feats, latest_round_.traits);
        }
        if (mutator_hooks::active_rule_flags & RF_CHAIN_TETHER) {
            chains_.step_host(count);
        }
        if (mutator_hooks::active_rule_flags & RF_TRAILER) {
            ChainSystem::snapshot_hitch_mask(count);
        }
    }

    sync_live_physics();
    refresh_hud(sim_tick);
}

void Engine::apply_slot_role(uint32_t slot, const RoleOverride &role) {
    if (slot >= kMaxSlots) return;
    mutator_hooks::active = true;
    auto &st = mutator_hooks::slots[slot];
    st.active = true;
    st.role = role.role;
    st.direction_sign = role.direction_sign < 0 ? -1 : 1;
    st.feat_bits = role.feat_bits;
    st.traits = role.traits;
    st.traits.sanitize();
}

void Engine::set_slot_direction(uint32_t slot, int8_t direction_sign) {
    if (slot >= kMaxSlots) return;
    mutator_hooks::active = true;
    mutator_hooks::slots[slot].active = true;
    mutator_hooks::slots[slot].direction_sign = direction_sign < 0 ? -1 : 1;
}

void Engine::set_slot_traits(uint32_t slot, const PhysicsTraits &traits) {
    if (slot >= kMaxSlots) return;
    mutator_hooks::active = true;
    mutator_hooks::slots[slot].active = true;
    mutator_hooks::slots[slot].traits = traits;
    mutator_hooks::slots[slot].traits.sanitize();
}

void Engine::set_slot_feats(uint32_t slot, uint32_t feat_bits) {
    if (slot >= kMaxSlots) return;
    mutator_hooks::active = true;
    mutator_hooks::slots[slot].active = true;
    mutator_hooks::slots[slot].feat_bits = feat_bits;
}

void Engine::set_slot_score(uint32_t slot, int32_t score) {
    if (slot >= kMaxSlots) return;
    mutator_hooks::slots[slot].score = score;
}

void Engine::capture_baseline(uint32_t slot, uint32_t car, uint32_t body_idx) {
    const uint32_t body_base = ee_r32(kBodyArrayAddr);
    const uint32_t tuning_base = ee_r32(kTuningArrayAddr);
    const uint32_t body_ptr = body_base + body_idx * 0xB0u;
    const uint32_t tuning_ptr = tuning_base + body_idx * 0x180u;
    if (!valid_ram(body_ptr, 0xB0) || !valid_ram(tuning_ptr, 0x180)) return;

    auto &b = baselines_[slot];
    if (b.valid && b.car_ptr == car && b.body_ptr == body_ptr && b.tuning_ptr == tuning_ptr) return;
    b.car_ptr = car;
    b.body_ptr = body_ptr;
    b.tuning_ptr = tuning_ptr;
    for (int i = 0; i < 4; ++i) {
        b.inv_inertia[i] = rf(body_ptr + 0x80u + static_cast<uint32_t>(i) * 4u);
        b.grip[i] = rf(tuning_ptr + 0xA0u + static_cast<uint32_t>(i) * 4u);
    }
    b.damage_mult = rf(car + 0x4B0u);
    b.valid = true;
}

void Engine::sync_live_physics() {
    if (!mutator_hooks::active) return;
    const uint32_t count = std::min<uint32_t>(netplay_car::slot_count(), kMaxSlots);
    for (uint32_t slot = 0; slot < count; ++slot) {
        const auto &st = mutator_hooks::slots[slot];
        if (!st.active) continue;
        const uint32_t car = netplay_car::car_of_slot(slot);
        if (!car) continue;
        const int body_idx = body_index_of(car);
        if (body_idx < 0) continue;

        capture_baseline(slot, car, static_cast<uint32_t>(body_idx));
        const auto &b = baselines_[slot];
        if (!b.valid) continue;

        vehicle_dynamics_set_body_gravity(static_cast<unsigned>(body_idx), kBaseGravity * st.traits.gravity_scale);

        const float inv_mass_mult = 1.0f / std::max(st.traits.mass_scale, 0.25f);
        for (int i = 0; i < 4; ++i) {
            wf(b.body_ptr + 0x80u + static_cast<uint32_t>(i) * 4u, b.inv_inertia[i] * inv_mass_mult);
        }

        for (int i = 0; i < 4; ++i) {
            wf(b.tuning_ptr + 0xA0u + static_cast<uint32_t>(i) * 4u, b.grip[i] * st.traits.grip_scale);
        }

        wf(car + 0x4B0u, b.damage_mult * st.traits.damage_taken_scale);
    }
}

bool Engine::resolve_vehicle_key(const std::string &key, ResolvedVehicle &out) {
    if (key.empty()) return false;
    const uint32_t table = ee_r32(kVehicleTable);
    const uint32_t count = ee_r32(kVehicleCount);
    if (!count || count > 4096 || !valid_ram(table, count * kVehicleStride)) return false;

    const std::string needle = to_lower_ascii(key);
    bool have_partial = false;
    ResolvedVehicle partial{};

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t rec = table + i * kVehicleStride;
        const std::string name = read_cstr(rec + 8u, 32u);
        if (name.empty()) continue;
        const std::string low = to_lower_ascii(name);
        if (low == needle) {
            out.table_index = i;
            out.vehicle_number = ee_r32(rec + 4u);
            out.vehicle_class = ee_r32(rec + 0x40u);
            out.chassis_name = name;
            return true;
        }
        if (!have_partial && low.find(needle) != std::string::npos) {
            partial.table_index = i;
            partial.vehicle_number = ee_r32(rec + 4u);
            partial.vehicle_class = ee_r32(rec + 0x40u);
            partial.chassis_name = name;
            have_partial = true;
        }
    }
    if (have_partial) {
        out = partial;
        return true;
    }
    return false;
}

uint32_t Engine::count_distinct_chassis(const std::vector<uint32_t> &vehicle_numbers) {
    const uint32_t table = ee_r32(kVehicleTable);
    const uint32_t count = ee_r32(kVehicleCount);
    if (!count || count > 4096 || !valid_ram(table, count * kVehicleStride)) return 0;

    std::vector<std::string> unique_chassis;
    for (uint32_t num : vehicle_numbers) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t rec = table + i * kVehicleStride;
            if (ee_r32(rec + 4u) != num) continue;
            const std::string name = to_lower_ascii(read_cstr(rec + 8u, 32u));
            if (!name.empty() && std::find(unique_chassis.begin(), unique_chassis.end(), name) == unique_chassis.end()) {
                unique_chassis.push_back(name);
            }
            break;
        }
    }
    return static_cast<uint32_t>(unique_chassis.size());
}

bool Engine::within_chassis_budget(const std::vector<uint32_t> &vehicle_numbers) {
    return count_distinct_chassis(vehicle_numbers) <= kMaxLoadedChassis;
}

bool Engine::resolve_anchor(AnchorKind anchor, const float origin[3], float spline_pct, float out_xyz[3]) {
    const float ox = origin ? origin[0] : 0.0f;
    const float oy = origin ? origin[1] : 0.0f;
    const float oz = origin ? origin[2] : 0.0f;

    if (anchor == AnchorKind::WorldXYZ) {
        out_xyz[0] = ox; out_xyz[1] = oy; out_xyz[2] = oz;
        return true;
    }

    auto find_course = [&]() -> uint32_t {
        const uint32_t entries = ee_r32(kCourseCountAddr);
        if (entries < 1 || entries > 8) return 0;
        uint32_t chosen = 0;
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t row = kCourseCacheAddr + i * 12u;
            const uint32_t cand = ee_r32(row);
            if (valid_ram(cand, 48) && ee_r32(cand) == kCourseMagic && ee_r32(cand + 8u) == 60u) {
                chosen = cand;
                if (ee_r32(row + 4u) == 0x101u) break;
            }
        }
        return chosen;
    };

    if (anchor == AnchorKind::SplinePct) {
        const uint32_t res = find_course();
        if (!res) return false;
        const uint32_t n = ee_r32(res + 28u) >> 16;
        const uint32_t root = ee_r32(res + 32u);
        if (n < 2 || n > 4096 || !valid_ram(root, n * 60u)) return false;
        float pct = std::isfinite(spline_pct) ? (spline_pct - std::floor(spline_pct)) : 0.0f;
        if (pct < 0.0f) pct += 1.0f;
        const float fidx = pct * static_cast<float>(n);
        const uint32_t i0 = static_cast<uint32_t>(fidx) % n;
        const uint32_t i1 = (i0 + 1u) % n;
        const float frac = fidx - std::floor(fidx);
        const uint32_t p0 = root + i0 * 60u;
        const uint32_t p1 = root + i1 * 60u;
        const float x = rf(p0) + (rf(p1) - rf(p0)) * frac + ox;
        const float z = rf(p0 + 4u) + (rf(p1 + 4u) - rf(p0 + 4u)) * frac + oz;
        float y = oy;
        if (mutator_hooks::ground_snap_y) {
            float snapped = y;
            if (mutator_hooks::ground_snap_y(x, z, snapped)) y = snapped + oy;
        }
        out_xyz[0] = x; out_xyz[1] = y; out_xyz[2] = z;
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }

    const uint32_t master = ee_r32(kEventMasterAddr);
    if (valid_ram(master, battle_layout::kMasterSize) && ee_r32(master) == battle_layout::kMasterVtable) {
        const uint32_t rcount = ee_r32(master + battle_layout::kRespawnCount);
        if (rcount > 0 && rcount <= 16) {
            double sx = 0.0, sy = 0.0, sz = 0.0;
            uint32_t valid_n = 0;
            for (uint32_t i = 0; i < rcount; ++i) {
                const uint32_t rec = master + 0x10u + i * 0x44u;
                const float rx = rf(rec + 0x24u), ry = rf(rec + 0x28u), rz = rf(rec + 0x2Cu);
                if (std::isfinite(rx) && std::isfinite(ry) && std::isfinite(rz)) {
                    sx += rx; sy += ry; sz += rz; ++valid_n;
                }
            }
            if (valid_n > 0) {
                out_xyz[0] = static_cast<float>(sx / valid_n) + ox;
                out_xyz[1] = static_cast<float>(sy / valid_n) + oy;
                out_xyz[2] = static_cast<float>(sz / valid_n) + oz;
                if (mutator_hooks::ground_snap_y) {
                    float snapped = out_xyz[1];
                    if (mutator_hooks::ground_snap_y(out_xyz[0], out_xyz[2], snapped)) out_xyz[1] = snapped + oy;
                }
                return true;
            }
        }
    }

    if (const uint32_t res = find_course()) {
        const uint32_t n = ee_r32(res + 28u) >> 16;
        const uint32_t root = ee_r32(res + 32u);
        if (n >= 3 && n <= 4096 && valid_ram(root, n * 60u)) {
            double sx = 0.0, sz = 0.0;
            for (uint32_t i = 0; i < n; ++i) {
                sx += rf(root + i * 60u);
                sz += rf(root + i * 60u + 4u);
            }
            out_xyz[0] = static_cast<float>(sx / n) + ox;
            out_xyz[2] = static_cast<float>(sz / n) + oz;
            out_xyz[1] = oy;
            if (mutator_hooks::ground_snap_y) {
                float snapped = out_xyz[1];
                if (mutator_hooks::ground_snap_y(out_xyz[0], out_xyz[2], snapped)) out_xyz[1] = snapped + oy;
            }
            return true;
        }
    }
    return false;
}

} // namespace mutator
