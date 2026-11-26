// Native EE runtime bridge for MutatorEngine (Milestone 2).
// Linked only into the full `eod` executable (picked up by src/*.cpp glob);
// standalone unit tests inject mock callbacks into mutator_hooks directly.
#include "mutator_hooks.h"
#include "mutator_engine.h"
#include "mutator_presets.h"
#include "netplay_names.h"
#include "battle_layout.h"
#include "session_start.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
void f_001D8F58(void); // Terrain ground-snap pose builder(out_pose, pt_xz, dir_xz)
void f_00189F50(void); // CarRepair(car)
void f_0017AF80(void); // SceneObjectSetPose(obj, pose_4x3)
void f_00201848(void); // Reset participant HUD/camera wreck latch(slot)
void f_002149E8(void); // DetachTrailer(trailer)
}

namespace {

constexpr uint32_t kBridgeScratch = battle_layout::kScratch + 0x180u;

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

bool ee_ground_snap_y(float x, float z, float &out_y) {
    if (!g_ram || !std::isfinite(x) || !std::isfinite(z)) return false;
    const uint32_t out_pose = kBridgeScratch;
    const uint32_t pt_xz = kBridgeScratch + 0x60u;
    const uint32_t dir_xz = kBridgeScratch + 0x70u;
    const EEState saved = g_ee;
    wf(pt_xz, x); wf(pt_xz + 4u, z);
    wf(dir_xz, 0.0f); wf(dir_xz + 4u, 1.0f);
    g_ee.r[4].ud[0] = out_pose;
    g_ee.r[5].ud[0] = pt_xz;
    g_ee.r[6].ud[0] = dir_xz;
    f_001D8F58();
    const float y = rf(out_pose + 0x28u);
    g_ee = saved;
    if (!std::isfinite(y) || std::fabs(y) > 10000.0f) return false;
    out_y = y;
    return true;
}

void ee_repair_car(uint32_t car) {
    if (!g_ram || !car || (car & 3u) || car > EE_RAM_SIZE - 0x1C34u) return;
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = car;
    f_00189F50();
    g_ee = saved;
}

void ee_set_car_pose(uint32_t car, const float pose_4x3[12]) {
    if (!g_ram || !car || (car & 3u) || car > EE_RAM_SIZE - 0x1C34u || !pose_4x3) return;
    const uint32_t scratch_pose = kBridgeScratch + 0x80u;
    for (int i = 0; i < 12; ++i) wf(scratch_pose + static_cast<uint32_t>(i) * 4u, pose_4x3[i]);
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = car;
    g_ee.r[5].ud[0] = scratch_pose;
    f_0017AF80();
    g_ee = saved;
}

void ee_reset_slot_hud(uint32_t slot) {
    if (!g_ram || slot >= mutator::kMaxSlots) return;
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = slot;
    f_00201848();
    g_ee = saved;
}

void ee_detach_trailer(uint32_t trailer) {
    if (!g_ram || !trailer || (trailer & 3u) || trailer > EE_RAM_SIZE - 0x1C34u) return;
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = trailer;
    f_002149E8();
    g_ee = saved;
}

void host_play_sfx_cue(const char *sfx_name) {
    (void)sfx_name;
    platform_audio_feedback(true);
}

void host_show_hud_banner(const char *text, int duration_ms) {
    if (text && *text) platform_show_hud_banner(text, duration_ms);
}

void host_offline_world_construct(uint32_t description) {
    if (!g_ram || !description || (description & 3u) || description > EE_RAM_SIZE - 0x9A8u) return;
    // Arena custom modes (Tips, Battle Royale, Infection, DestructionWare, Fiesta, Chain Gang)
    // require the Battle event template (0x18000224) for open-field respawns, AI, and weapons.
    // Never apply them onto retail Races, Derbies, or Career events.
    if (ee_r32(description + 0x9A4u) != 0x18000224u) {
        mutator::engine().clear_config();
        return;
    }
    mutator::GametypeConfig preset_cfg;
    bool have_cfg = false;
    const PendingOfflineMatch &pending = session_pending_offline();
    if (pending.active) {
        if (pending.has_custom_mutator && !pending.mutator_json.empty()) {
            std::string err;
            have_cfg = mutator::parse_gametype_json(pending.mutator_json, preset_cfg, err);
        } else if (pending.gametype_preset > 0) {
            have_cfg = mutator::build_preset(pending.gametype_preset, preset_cfg);
        }
    } else if (g_config.offline_gametype > 0) {
        const uint8_t preset_id = static_cast<uint8_t>(
            std::clamp(g_config.offline_gametype, 0, static_cast<int>(mutator::preset_count()) - 1));
        have_cfg = mutator::build_preset(preset_id, preset_cfg);
    }
    if (have_cfg) {
        mutator::engine().set_config(preset_cfg);
        mutator_hooks::suppress_weapons = !mutator::config_uses_weapons(preset_cfg);
        const uint32_t count = std::min<uint32_t>(ee_r32(description + 0x98Cu), mutator::kMaxSlots);
        int local_slot = 0;
        for (uint32_t i = 0; i < count; ++i) {
            if (ee_r32(description + 0x2Cu + i * 0x78u) == 0x50u) {
                local_slot = static_cast<int>(i);
                break;
            }
        }
        static uint32_t offline_event_seq = 1000u;
        ++offline_event_seq;
        const uint32_t seed = static_cast<uint32_t>(platform_time_us()) ^ (offline_event_seq * 0x9E3779B9u);
        mutator::engine().on_world_bound(offline_event_seq, seed, true, local_slot);
    } else {
        mutator::engine().clear_config();
    }
}

void host_mutator_world_view() {
    if (!mutator_hooks::active) return;
    const auto cam = netplay::names::read_world_camera();
    auto &wc = mutator_hooks::world_camera;
    for (int i = 0; i < 3; ++i) {
        wc.position[i] = cam.position[i];
        for (int j = 0; j < 3; ++j) wc.axes[i][j] = cam.axes[i][j];
    }
    wc.horizontal_scale = cam.horizontal_scale;
    wc.vertical_scale = cam.vertical_scale;
    wc.width = cam.width;
    wc.height = cam.height;
    wc.valid = (cam.width > 0.0f && cam.height > 0.0f);
}

struct MutatorNativeBridgeInstaller {
    MutatorNativeBridgeInstaller() {
        mutator_hooks::on_world_view = host_mutator_world_view;
        mutator_hooks::on_offline_world_construct = host_offline_world_construct;
        mutator_hooks::ground_snap_y = ee_ground_snap_y;
        mutator_hooks::native_repair_car = ee_repair_car;
        mutator_hooks::native_set_car_pose = ee_set_car_pose;
        mutator_hooks::native_reset_slot_hud = ee_reset_slot_hud;
        mutator_hooks::native_detach_trailer = ee_detach_trailer;
        mutator_hooks::play_sfx_cue = host_play_sfx_cue;
        mutator_hooks::show_hud_banner = host_show_hud_banner;
    }
};

static MutatorNativeBridgeInstaller g_mutator_native_bridge{};

} // namespace
