// hle_nascar_audio.cpp - Native NASCAR ESFX multi-gear engine audio & dynamic .gar
// track-class transmission hooks for SLUS_209.10 (0x23C400, 0x23C8D0, 0x186DC8).
#include "hle_nascar_audio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "core/eod_tracy.h"

#ifndef EOD_NASCAR_AUDIO_HOST_ONLY
#include "core/cpu.h"
#include "core/memory.h"
#endif

namespace eod::nascar_audio_hle {

namespace {

std::string to_lower_copy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool contains_ci(const std::string& haystack_lower, const char* needle_lower) {
    return haystack_lower.find(needle_lower) != std::string::npos;
}

bool ends_with_ci(const std::string& s_lower, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s_lower.size() >= n && s_lower.compare(s_lower.size() - n, n, suffix) == 0;
}

std::unordered_map<uint32_t, NascarEngineVoiceState>& voice_registry() {
    static std::unordered_map<uint32_t, NascarEngineVoiceState> reg;
    return reg;
}

} // namespace

bool detect_nascar_esfx_profile(const std::string& engine_sound,
                                const std::string& sound,
                                std::string& out_profile_name,
                                int& out_engine_index) {
    auto parse_token = [&](const std::string& raw) -> bool {
        const std::string low = to_lower_copy(raw);
        if (low.empty()) return false;
        // Never intercept native EOD *.val presets unless sound explicitly names eng1..4.
        if (ends_with_ci(low, ".val")) return false;

        if (low == "nseng1.ens" || low == "eng1e.ens" || low == "eng1" || low == "nseng1") {
            out_profile_name = "nseng1.ens";
            out_engine_index = 1;
            return true;
        }
        if (low == "nseng2.ens" || low == "eng2e.ens" || low == "eng2" || low == "nseng2") {
            out_profile_name = "nseng2.ens";
            out_engine_index = 2;
            return true;
        }
        if (low == "nseng3.ens" || low == "eng3e.ens" || low == "eng3" || low == "nseng3") {
            out_profile_name = "nseng3.ens";
            out_engine_index = 3;
            return true;
        }
        if (low == "nseng4.ens" || low == "eng4e.ens" || low == "eng4" || low == "nseng4") {
            out_profile_name = "nseng4.ens";
            out_engine_index = 4;
            return true;
        }
        if (ends_with_ci(low, ".ens")) {
            out_profile_name = raw;
            out_engine_index = 1;
            for (char c : low) {
                if (c >= '1' && c <= '4') {
                    out_engine_index = c - '0';
                    break;
                }
            }
            return true;
        }
        return false;
    };

    if (parse_token(engine_sound)) return true;
    if (parse_token(sound)) return true;
    return false;
}

TrackClass classify_track(const std::string& track_name,
                          const std::string& track_type,
                          const std::string& surface) {
    const std::string name = to_lower_copy(track_name);
    const std::string ttype = to_lower_copy(track_type);
    const std::string surf = to_lower_copy(surface);

    // Superspeedways (2.5+ mile high-banked restrictor/drafting ovals: 195-210 mph)
    if (contains_ci(name, "daytona") || contains_ci(name, "talladeg") || contains_ci(name, "super")) {
        return TrackClass::Superspeedway;
    }

    // Road courses (Watkins Glen, Sears Point, or track.val track_type == "Road Course")
    if (contains_ci(name, "watkin") || contains_ci(name, "sears") || contains_ci(name, "sonoma") ||
        contains_ci(name, "road") || contains_ci(ttype, "road")) {
        return TrackClass::RoadCourse;
    }

    // Intermediate / 1.0-2.0 mile Speedways
    if (contains_ci(name, "atlanta") || contains_ci(name, "charlott") || contains_ci(name, "californ") ||
        contains_ci(name, "darlingt") || contains_ci(name, "dover") || contains_ci(name, "kansas") ||
        contains_ci(name, "lasvegas") || contains_ci(name, "vegas") || contains_ci(name, "miami") ||
        contains_ci(name, "homestea") || contains_ci(name, "michigan") || contains_ci(name, "ncarolin") ||
        contains_ci(name, "rockingh") || contains_ci(name, "texas") || contains_ci(name, "indy") ||
        contains_ci(name, "speedway")) {
        return TrackClass::Speedway;
    }

    // Short asphalt ovals (Bristol, Martinsville, Richmond, Phoenix, New Hampshire)
    if (contains_ci(name, "bristol") || contains_ci(name, "martins") || contains_ci(name, "richmond") ||
        contains_ci(name, "phoenix") || contains_ci(name, "newhamp") || contains_ci(name, "loudon") ||
        contains_ci(surf, "asphalt") || contains_ci(surf, "pave")) {
        return TrackClass::ShortAsphalt;
    }

    // Default for all EOD dirt/fairground ovals (Afton, Welch, Kenyon, Mankato, Anoka, etc.)
    return TrackClass::ShortDirt;
}

GarTransmissionPreset transmission_preset_for_track(TrackClass track_class) {
    GarTransmissionPreset p{};
    p.track_class = track_class;
    p.num_gears = 4;
    switch (track_class) {
        case TrackClass::ShortDirt:
            // KENYON.TRK default/arcade short-dirt ratios: crisp 1->2->3->4 across 0..125 mph.
            p.ratios = {3.50f, 2.70f, 1.90f, 1.25f, 0.90f, 0.85f};
            p.rear_end_ratio = 5.43f;
            break;
        case TrackClass::ShortAsphalt:
            // BRISTOL.TRK/default.gar
            p.ratios = {3.00f, 2.40f, 1.70f, 1.00f, 0.90f, 0.85f};
            p.rear_end_ratio = 4.56f;
            break;
        case TrackClass::RoadCourse:
            // WATKIN_1.TRK/default.gar
            p.ratios = {2.80f, 1.80f, 1.30f, 1.00f, 0.90f, 0.85f};
            p.rear_end_ratio = 3.70f;
            break;
        case TrackClass::Speedway:
            // Intermediate 1.5-mile speedway (.gar)
            p.ratios = {2.75f, 1.95f, 1.35f, 0.95f, 0.85f, 0.80f};
            p.rear_end_ratio = 3.25f;
            break;
        case TrackClass::Superspeedway:
            // DAYTONA.TRK/default.gar: 195-210+ mph top speed
            p.ratios = {2.65f, 1.90f, 1.30f, 0.85f, 0.80f, 0.75f};
            p.rear_end_ratio = 2.80f;
            break;
    }
    return p;
}

float evaluate_engine_torque(const EngineTorqueCurve& c, float rpm) {
    float raw = 0.0f;
    if (c.split_rpm < rpm) {
        const float over = std::max(rpm - c.rev_start, 0.0f) * c.rev_slope;
        raw = std::max(c.a_hi * rpm * rpm + c.b_hi * rpm + c.c_hi - over, c.min_torque);
    } else {
        raw = c.a_lo * rpm * rpm + c.b_lo * rpm + c.c_lo;
    }
    return raw - c.drag_k * rpm;
}

std::vector<GearShiftEntry> build_monotonic_shift_table(const GarTransmissionPreset& preset,
                                                        const EngineTorqueCurve& curve,
                                                        float redline_rpm) {
    const int n = std::clamp(preset.num_gears, 1, 6);
    const float safe_redline = (std::isfinite(redline_rpm) && redline_rpm > 4000.0f) ? redline_rpm : 7500.0f;
    const float max_shift_rpm = safe_redline * 0.955f;
    const bool has_curve = (curve.split_rpm > 500.0f) &&
                           (std::fabs(curve.a_lo) + std::fabs(curve.b_lo) + std::fabs(curve.c_lo) > 1e-4f);

    std::vector<GearShiftEntry> table(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        table[i].ratio = std::max(preset.ratios[i], 0.25f);
    }

    float prev_up_ds = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float g_cur = table[i].ratio;
        if (i < n - 1) {
            const float g_next = table[i + 1].ratio;
            float shift_ds = max_shift_rpm / g_cur;
            if (has_curve && g_cur > g_next) {
                for (float rpm = 3500.0f; rpm < max_shift_rpm; rpm += 50.0f) {
                    const float ds = rpm / g_cur;
                    const float next_rpm = ds * g_next;
                    const float t_cur = evaluate_engine_torque(curve, rpm) * g_cur;
                    const float t_next = evaluate_engine_torque(curve, next_rpm) * g_next;
                    if (t_cur < t_next) {
                        shift_ds = ds;
                        break;
                    }
                }
            }
            // Enforce headroom below redline and strict monotonicity across gears.
            const float ceil_ds = max_shift_rpm / g_cur;
            shift_ds = std::min(shift_ds, ceil_ds);
            if (i > 0 && shift_ds <= prev_up_ds * 1.08f) {
                shift_ds = std::min(ceil_ds, prev_up_ds * 1.15f);
            }
            table[i].upshift_ds_rpm = shift_ds;
            prev_up_ds = shift_ds;
        } else {
            table[i].upshift_ds_rpm = 100000.0f;
        }

        if (i == 0) {
            table[i].downshift_lo_ds_rpm = 0.0f;
            table[i].downshift_hi_ds_rpm = 0.0f;
        } else {
            const float lim = std::min(safe_redline / g_cur, table[i - 1].upshift_ds_rpm);
            table[i].downshift_lo_ds_rpm = lim * 0.85f;
            table[i].downshift_hi_ds_rpm = lim * 0.90f;
        }
    }
    return table;
}

MixFrameOutput evaluate_nascar_audio_frame(NascarEngineVoiceState& state,
                                           float rpm,
                                           float throttle,
                                           int32_t gear,
                                           bool viewed,
                                           bool local_mix,
                                           bool silenced) {
    EOD_ZONE("hle_nascar_audio_tick");

    MixFrameOutput out{};
    const float safe_rpm = std::isfinite(rpm) ? std::max(0.0f, rpm) : 0.0f;
    const float safe_throttle = std::isfinite(throttle) ? std::clamp(throttle, 0.0f, 1.0f) : 0.0f;

    // Isolated audio-only RNG (never calls EE gameplay RNG).
    const float audio_random = state.audio_rng.next_caller_random();

    if (gear != state.last_gear && state.last_gear >= 1 && gear >= 1) {
        out.gear_changed = true;
        if (gear > state.last_gear) {
            out.upshifted = true;
            state.shift_transient_frames = 6;
            state.shift_rpm_dip = -std::clamp(safe_rpm * 0.045f, 120.0f, 360.0f);
            if (!silenced && (safe_rpm > 3500.0f || safe_throttle > 0.25f)) {
                out.trigger_backfire = true;
                out.backfire_pitch = 0.95f + 0.20f * state.audio_rng.next_unit();
            }
        } else {
            out.downshifted = true;
            state.shift_transient_frames = 5;
            state.shift_rpm_dip = std::clamp(safe_rpm * 0.040f, 100.0f, 300.0f);
            if (!silenced && safe_rpm > 3000.0f) {
                out.trigger_backfire = true;
                out.backfire_pitch = 0.88f + 0.18f * state.audio_rng.next_unit();
            }
        }
    }
    state.last_gear = gear;

    float effective_rpm = safe_rpm;
    if (state.shift_transient_frames > 0) {
        const float weight = float(state.shift_transient_frames) / 6.0f;
        effective_rpm = std::max(100.0f, safe_rpm + state.shift_rpm_dip * weight);
        --state.shift_transient_frames;
    }
    state.last_rpm = safe_rpm;
    out.effective_rpm = effective_rpm;

    out.layers = nascar_audio::mix(state.profile, effective_rpm, safe_throttle,
                                   audio_random, viewed, local_mix, silenced);
    out.idle = nascar_audio::evaluate_idle(effective_rpm, safe_throttle, viewed, local_mix, silenced);

    if (state.smoothed_volumes.size() != out.layers.size()) {
        state.smoothed_volumes.assign(out.layers.size(), 0.0f);
    }
    for (size_t i = 0; i < out.layers.size(); ++i) {
        const float target = out.layers[i].active ? out.layers[i].volume : 0.0f;
        const float prev = state.smoothed_volumes[i];
        const float delta = std::clamp(target - prev, -0.22f, 0.25f);
        state.smoothed_volumes[i] = std::max(0.0f, prev + delta);
        out.layers[i].volume = state.smoothed_volumes[i];
    }
    {
        const float target = out.idle.active ? out.idle.volume : 0.0f;
        const float delta = std::clamp(target - state.smoothed_idle_volume, -0.20f, 0.20f);
        state.smoothed_idle_volume = std::max(0.0f, state.smoothed_idle_volume + delta);
        out.idle.volume = state.smoothed_idle_volume;
    }

    return out;
}

void reset_nascar_audio_registry() {
    voice_registry().clear();
}

size_t active_nascar_voice_count() {
    return voice_registry().size();
}

} // namespace eod::nascar_audio_hle

#ifndef EOD_NASCAR_AUDIO_HOST_ONLY

extern "C" void f_0023C400_orig(void);
extern "C" void f_0023C8D0_orig(void);
extern "C" void f_00186DC8_orig(void);
extern "C" void f_0012E870(void);
extern "C" void f_00132638(void);
extern "C" void f_00238440(void);
extern "C" void f_0023CB10(void);

namespace {

using namespace eod::nascar_audio_hle;

constexpr uint32_t kValtSectionVtable = 0x003F21B0u;
constexpr uint32_t kEngSndVtable      = 0x0040A768u;
constexpr uint32_t kEngSndTagStr      = 0x0040A760u;
constexpr uint32_t kArchiveListA      = 0x0043A0E0u;
constexpr uint32_t kArchiveListB      = 0x0043A0E4u;
constexpr uint32_t kViewportCountAddr = 0x00447430u;
constexpr uint32_t kCameraArrayBase   = 0x0048CB80u;
constexpr uint32_t kCameraStride      = 0x000001B8u;

bool valid_addr(uint32_t addr, uint32_t size = 4) {
    return addr != 0 && !(addr & 3u) && size <= EE_RAM_SIZE && addr <= EE_RAM_SIZE - size;
}

float ee_r32_f(uint32_t addr) {
    const uint32_t bits = ee_r32(addr);
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void ee_w32_f(uint32_t addr, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    ee_w32(addr, bits);
}

std::string read_ee_cstr(uint32_t addr, size_t max_len = 64) {
    std::string out;
    if (!addr || addr >= EE_RAM_SIZE) return out;
    const size_t lim = std::min(max_len, static_cast<size_t>(EE_RAM_SIZE - addr));
    for (size_t i = 0; i < lim; ++i) {
        const char c = static_cast<char>(ee_r8(addr + static_cast<uint32_t>(i)));
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

bool ee_cstr_ieq(uint32_t addr, const char* expected) {
    if (!addr || addr >= EE_RAM_SIZE || !expected) return false;
    size_t i = 0;
    for (; expected[i] != '\0'; ++i) {
        if (addr + i >= EE_RAM_SIZE) return false;
        const unsigned char a = ee_r8(addr + static_cast<uint32_t>(i));
        const unsigned char b = static_cast<unsigned char>(expected[i]);
        if (std::tolower(a) != std::tolower(b)) return false;
    }
    return (addr + i < EE_RAM_SIZE) && ee_r8(addr + static_cast<uint32_t>(i)) == 0;
}

std::string valt_lookup_str(uint32_t section, const char* key) {
    for (unsigned depth = 0; section && depth < 32; ++depth) {
        if (!valid_addr(section, 0x1C) || ee_r32(section) != kValtSectionVtable) return {};
        const uint32_t count = ee_r32(section + 0x0C);
        const uint32_t pairs = ee_r32(section + 0x10);
        if (count > 1024 || (count && !valid_addr(pairs, count * 8))) return {};
        for (uint32_t i = 0; i < count; ++i) {
            if (ee_cstr_ieq(ee_r32(pairs + i * 8), key)) {
                return read_ee_cstr(ee_r32(pairs + i * 8 + 4), 64);
            }
        }
        section = ee_r32(section + 0x14);
    }
    return {};
}

bool valt_has_key(uint32_t section, const char* key) {
    for (unsigned depth = 0; section && depth < 32; ++depth) {
        if (!valid_addr(section, 0x1C) || ee_r32(section) != kValtSectionVtable) return false;
        const uint32_t count = ee_r32(section + 0x0C);
        const uint32_t pairs = ee_r32(section + 0x10);
        if (count > 1024 || (count && !valid_addr(pairs, count * 8))) return false;
        for (uint32_t i = 0; i < count; ++i) {
            if (ee_cstr_ieq(ee_r32(pairs + i * 8), key)) return true;
        }
        section = ee_r32(section + 0x14);
    }
    return false;
}

bool find_resident_esfx(const std::string& name, nascar_audio::Profile& out_profile) {
    // First scan resident archive tables directly (zero EE call overhead).
    for (uint32_t list_addr : {kArchiveListB, kArchiveListA}) {
        uint32_t arch = ee_r32(list_addr);
        for (unsigned guard = 0; valid_addr(arch, 328) && guard < 128; ++guard) {
            const uint32_t entries = ee_r32(arch + 292);
            const uint32_t count = ee_r32(arch + 320);
            if (count <= 2048 && valid_addr(entries, count * 0x44)) {
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t ent = entries + i * 0x44;
                    if (!ee_cstr_ieq(ent, name.c_str())) continue;
                    const uint32_t flags = ee_r32(ent + 52);
                    const uint32_t hdr = ee_r32(ent + 60);
                    const uint32_t sz = ee_r32(ent + 40);
                    if ((flags & 0x8u) && valid_addr(hdr, 8) && sz >= 4 && sz <= 4096 &&
                        hdr + 8 + sz <= EE_RAM_SIZE) {
                        if (nascar_audio::parse(g_ram + hdr + 8, sz, out_profile)) {
                            return true;
                        }
                    }
                }
            }
            arch = ee_r32(arch + 288);
        }
    }

    // Fallback: ask EOD's resource loader f_00132638(name, 'ESFX', &ver, &size, 0, 0).
    const EEState saved = g_ee;
    const uint32_t sp = g_ee.r[29].uw[0] - 0x60u;
    if (sp < EE_RAM_SIZE - 0x60u) {
        g_ee.r[29].sd[0] = static_cast<int64_t>(static_cast<int32_t>(sp));
        const uint32_t name_buf = sp + 0x20u;
        const size_t n = std::min<size_t>(name.size(), 31);
        std::memcpy(g_ram + name_buf, name.c_str(), n);
        g_ram[name_buf + n] = '\0';
        ee_w32(sp + 0x10u, 0);
        ee_w32(sp + 0x14u, 0);
        g_ee.r[4].ud[0] = name_buf;
        g_ee.r[5].ud[0] = 0x45534658ull; // 'ESFX'
        g_ee.r[6].ud[0] = sp + 0x10u;    // out_version
        g_ee.r[7].ud[0] = sp + 0x14u;    // out_size
        g_ee.r[8].ud[0] = 0;
        g_ee.r[9].ud[0] = 0;
        f_00132638();
        const uint32_t payload = g_ee.r[2].uw[0];
        const uint32_t sz = ee_r32(sp + 0x14u);
        g_ee = saved;
        if (payload && sz >= 4 && sz <= 4096 && payload <= EE_RAM_SIZE - sz) {
            if (nascar_audio::parse(g_ram + payload, sz, out_profile)) {
                return true;
            }
        }
    } else {
        g_ee = saved;
    }
    return false;
}

uint32_t create_sfx_voice(const std::string& sample_name, uint32_t category, uint32_t car_ptr) {
    if (sample_name.empty() || sample_name.size() >= 31) return 0;
    const EEState saved = g_ee;
    const uint32_t sp = g_ee.r[29].uw[0] - 0x40u;
    if (sp >= EE_RAM_SIZE - 0x40u) return 0;
    g_ee.r[29].sd[0] = static_cast<int64_t>(static_cast<int32_t>(sp));
    const uint32_t str_addr = sp + 0x10u;
    std::memcpy(g_ram + str_addr, sample_name.c_str(), sample_name.size() + 1);
    g_ee.r[4].ud[0] = str_addr;
    g_ee.r[5].ud[0] = category;
    g_ee.r[6].ud[0] = car_ptr + 0x104u;
    g_ee.r[7].ud[0] = car_ptr + 0x88u;
    f_00238440();
    const uint32_t voice = g_ee.r[2].uw[0];
    g_ee = saved;
    if (valid_addr(voice, 132)) {
        ee_w32(voice + 12, ee_r32(car_ptr + 476));
        return voice;
    }
    return 0;
}

uint32_t create_sfx_voice_with_fallback(const std::string& primary_name, uint32_t category, uint32_t car_ptr) {
    if (uint32_t h = create_sfx_voice(primary_name, category, car_ptr)) return h;
    if (primary_name.size() > 2 &&
        (primary_name[0] == 'n' || primary_name[0] == 'N') &&
        (primary_name[1] == 's' || primary_name[1] == 'S')) {
        if (uint32_t h = create_sfx_voice(primary_name.substr(2), category, car_ptr)) return h;
    }
    return 0;
}

void write_voice_params(uint32_t voice, bool active, float pitch, float volume) {
    if (!valid_addr(voice, 112)) return;
    ee_w32(voice + 88, 0u); // playing (not paused)
    ee_w32(voice + 92, 1u); // looping
    ee_w32(voice + 84, 1u); // active
    if (active && std::isfinite(pitch) && pitch > 0.01f) {
        const float clamped_pitch = std::clamp(pitch, 0.08f, 4.0f);
        if (clamped_pitch != ee_r32_f(voice + 0)) {
            ee_w32_f(voice + 0, clamped_pitch);
            ee_w32(voice + 104, 1u);
        }
    }
    const float clamped_vol = (active && std::isfinite(volume)) ? std::clamp(volume, 0.0f, 1.5f) : 0.0f;
    if (clamped_vol != ee_r32_f(voice + 4)) {
        ee_w32_f(voice + 4, clamped_vol);
        ee_w32(voice + 100, 1u);
    }
}

void trigger_oneshot_voice(uint32_t voice, float pitch, float volume) {
    if (!valid_addr(voice, 112)) return;
    const float p = std::clamp(pitch, 0.5f, 2.0f);
    const float v = std::clamp(volume, 0.0f, 1.0f);
    if (p != ee_r32_f(voice + 0)) {
        ee_w32_f(voice + 0, p);
        ee_w32(voice + 104, 1u);
    }
    if (v != ee_r32_f(voice + 4)) {
        ee_w32_f(voice + 4, v);
        ee_w32(voice + 100, 1u);
    }
    ee_w32(voice + 88, 0u);
    ee_w32(voice + 84, 1u);
    ee_w32(voice + 92, 0u); // one-shot
}

std::string detect_loaded_trk_name() {
    std::string fallback;
    for (uint32_t list_addr : {kArchiveListB, kArchiveListA}) {
        uint32_t arch = ee_r32(list_addr);
        for (unsigned guard = 0; valid_addr(arch, 328) && guard < 128; ++guard) {
            const std::string name = read_ee_cstr(arch, 48);
            std::string low;
            low.reserve(name.size());
            for (unsigned char c : name) low.push_back(static_cast<char>(std::tolower(c)));
            if (low.size() > 4 && low.compare(low.size() - 4, 4, ".trk") == 0) {
                if (low.find("homebase") == std::string::npos && low.find("showroom") == std::string::npos) {
                    return name;
                }
                if (fallback.empty()) fallback = name;
            }
            arch = ee_r32(arch + 288);
        }
    }
    return fallback;
}

bool is_nascar_car(uint32_t car_ptr) {
    if (!valid_addr(car_ptr, 0x480)) return false;
    const uint32_t car_data = ee_r32(car_ptr + 604);
    if (!valid_addr(car_data, 32)) return false;
    const uint32_t eng_sec = ee_r32(car_data + 4);
    const uint32_t trans_sec = ee_r32(car_data + 8);

    std::string prof;
    int idx = 0;
    if (detect_nascar_esfx_profile(valt_lookup_str(eng_sec, "engine_sound"),
                                   valt_lookup_str(eng_sec, "sound"),
                                   prof, idx)) {
        return true;
    }
    if (valt_has_key(trans_sec, "ratio")) {
        return true;
    }
    return false;
}

} // namespace

// Hook 0x23C400: Engine Sound Loader
extern "C" void f_0023C400(void) {
    const uint32_t car_ptr = arg_u32(0);
    std::string profile_name;
    int engine_idx = 0;
    bool is_esfx = false;
    if (valid_addr(car_ptr, 0x480)) {
        const uint32_t car_data = ee_r32(car_ptr + 604);
        if (valid_addr(car_data, 16)) {
            const uint32_t eng_sec = ee_r32(car_data + 4);
            is_esfx = detect_nascar_esfx_profile(valt_lookup_str(eng_sec, "engine_sound"),
                                                 valt_lookup_str(eng_sec, "sound"),
                                                 profile_name, engine_idx);
        }
    }

    if (!is_esfx) {
        f_0023C400_orig();
        const uint32_t eng_snd = g_ee.r[2].uw[0];
        if (eng_snd) voice_registry().erase(eng_snd);
        return;
    }

    nascar_audio::Profile profile;
    if (!find_resident_esfx(profile_name, profile)) {
        profile = nascar_audio::builtin_profile(engine_idx);
    }

    // Allocate native 0x150-byte eng_snd container so f_0023CB10 cleanly owns voice lifetimes.
    const EEState saved = g_ee;
    g_ee.r[4].ud[0] = 0x150ull;
    g_ee.r[5].ud[0] = kEngSndTagStr;
    f_0012E870();
    const uint32_t eng_snd = g_ee.r[2].uw[0];
    g_ee = saved;

    if (!valid_addr(eng_snd, 0x150)) {
        f_0023C400_orig();
        return;
    }

    std::memset(g_ram + eng_snd, 0, 0x150);
    ee_w32(eng_snd + 0, kEngSndVtable);
    ee_w32(eng_snd + 316, 0u);
    ee_w32(eng_snd + 320, car_ptr);
    ee_w32_f(eng_snd + 324, 1.0f);
    ee_w32_f(eng_snd + 328, 0.9f);
    ee_w32_f(eng_snd + 332, 1.0f);

    NascarEngineVoiceState st{};
    st.eng_snd_addr = eng_snd;
    st.car_ptr = car_ptr;
    st.engine_index = engine_idx;
    st.profile_name = profile_name;
    st.profile = std::move(profile);
    st.audio_rng = nascar_audio::AudioRng(0x9E3779B9u ^ (car_ptr * 2654435761u) ^ (ee_r32(car_ptr + 476) + 1u));
    st.voice_handles.reserve(st.profile.size());
    st.smoothed_volumes.assign(st.profile.size(), 0.0f);

    uint32_t guest_slot_count = 0;
    uint32_t bound_main = 0;
    auto bind_guest_slot = [&](uint32_t voice) {
        if (voice && guest_slot_count < 6) {
            ee_w32(eng_snd + 4u + guest_slot_count * 0x34u, voice);
            ++guest_slot_count;
        }
    };

    for (const auto& rec : st.profile) {
        const std::string sample_name(rec.sample.data());
        const uint32_t h = create_sfx_voice_with_fallback(sample_name, 3u, car_ptr);
        st.voice_handles.push_back(h);
        if (h) {
            ++bound_main;
            bind_guest_slot(h);
        }
    }

    if (bound_main == 0) {
        // Sample bank not mounted yet: cleanly destroy temp object & fall back to retail mixer.
        const EEState s2 = g_ee;
        g_ee.r[4].ud[0] = eng_snd;
        g_ee.r[5].ud[0] = 1ull;
        f_0023CB10();
        g_ee = s2;
        f_0023C400_orig();
        return;
    }

    st.idle_voice = create_sfx_voice_with_fallback("nseng1i.sfx", 3u, car_ptr);
    bind_guest_slot(st.idle_voice);

    st.backfire_voice = create_sfx_voice("backfire.sfx", 5u, car_ptr);
    bind_guest_slot(st.backfire_voice);

    ee_w32(eng_snd + 316, guest_slot_count);
    voice_registry()[eng_snd] = std::move(st);
    g_ee.r[2].ud[0] = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(eng_snd)));
}

// Hook 0x23C8D0: Per-Frame Engine Sound Mixer
extern "C" void f_0023C8D0(void) {
    const uint32_t eng_snd = arg_u32(0);
    auto& reg = voice_registry();
    auto it = reg.find(eng_snd);
    if (it == reg.end() || !valid_addr(eng_snd, 0x150) ||
        ee_r32(eng_snd + 320) != it->second.car_ptr) {
        f_0023C8D0_orig();
        return;
    }

    NascarEngineVoiceState& st = it->second;
    const uint32_t car_ptr = st.car_ptr;
    if (!valid_addr(car_ptr, 7104)) {
        f_0023C8D0_orig();
        return;
    }

    const uint32_t car_id = ee_r32(car_ptr + 476);
    const float rpm = ee_r32_f(car_ptr + 7008);
    const float throttle = ee_r32_f(car_ptr + 7044);
    const int32_t gear = static_cast<int32_t>(ee_r32(car_ptr + 7068));
    const bool silenced = (arg_u32(1) != 0u);

    // Resolve viewed camera & local_mix matching f_001D3F98 / f_001D3F78.
    bool viewed = false;
    bool local_mix = false;
    const uint32_t view_count = std::min<uint32_t>(ee_r32(kViewportCountAddr), 4u);
    for (uint32_t v = 0; v < view_count; ++v) {
        const uint32_t cam = kCameraArrayBase + v * kCameraStride;
        if (ee_r32(cam + 28) == car_id) {
            viewed = true;
            local_mix = !nascar_audio::is_external_camera_mode(ee_r32(cam + 12));
            break;
        }
    }

    const MixFrameOutput frame =
        evaluate_nascar_audio_frame(st, rpm, throttle, gear, viewed, local_mix, silenced);

    const size_t n = std::min(frame.layers.size(), st.voice_handles.size());
    for (size_t i = 0; i < n; ++i) {
        write_voice_params(st.voice_handles[i], frame.layers[i].active,
                           frame.layers[i].pitch, frame.layers[i].volume);
    }
    if (st.idle_voice) {
        write_voice_params(st.idle_voice, frame.idle.active, frame.idle.pitch, frame.idle.volume);
    }
    if (frame.trigger_backfire && st.backfire_voice && viewed && !silenced) {
        trigger_oneshot_voice(st.backfire_voice, frame.backfire_pitch, 0.85f);
    }
}

// Hook 0x186DC8: Transmission & Auto-Shift Table Builder
extern "C" void f_00186DC8(void) {
    const uint32_t car_ptr = arg_u32(0);
    f_00186DC8_orig();

    if (!is_nascar_car(car_ptr)) return;

    const uint32_t car_data = ee_r32(car_ptr + 604);
    const uint32_t trans_sec = ee_r32(car_data + 8);
    const uint32_t track_sec = ee_r32(car_data + 24);

    // Respect explicit custom gearing locks (Career/World Studio overrides).
    if (valt_lookup_str(trans_sec, "lock_gearing") == "1" ||
        valt_lookup_str(trans_sec, "custom_gearing") == "1") {
        return;
    }

    const std::string trk_name = detect_loaded_trk_name();
    const std::string trk_type = valt_lookup_str(track_sec, "track_type");
    const std::string trk_surf = valt_lookup_str(track_sec, "surface");
    const TrackClass tc = classify_track(trk_name, trk_type, trk_surf);
    const GarTransmissionPreset preset = transmission_preset_for_track(tc);

    const float old_final = ee_r32_f(car_ptr + 1028);
    const float old_wsf = ee_r32_f(car_ptr + 1036);
    const float redline = ee_r32_f(car_ptr + 1032); // Already includes Career CarTuningBlock redline_mult!

    ee_w32_f(car_ptr + 1028, preset.rear_end_ratio);
    if (std::isfinite(old_final) && old_final > 0.1f && std::isfinite(old_wsf) && old_wsf > 0.0f) {
        ee_w32_f(car_ptr + 1036, old_wsf * (old_final / preset.rear_end_ratio));
    }
    ee_w32(car_ptr + 1040, static_cast<uint32_t>(preset.num_gears));

    EngineTorqueCurve curve{};
    curve.a_hi       = ee_r32_f(car_ptr + 0x3D8 + 0);
    curve.b_hi       = ee_r32_f(car_ptr + 0x3D8 + 4);
    curve.c_hi       = ee_r32_f(car_ptr + 0x3D8 + 8);
    curve.a_lo       = ee_r32_f(car_ptr + 0x3D8 + 12);
    curve.b_lo       = ee_r32_f(car_ptr + 0x3D8 + 16);
    curve.c_lo       = ee_r32_f(car_ptr + 0x3D8 + 20);
    curve.drag_k     = ee_r32_f(car_ptr + 0x3D8 + 24);
    curve.rev_start  = ee_r32_f(car_ptr + 0x3D8 + 28);
    curve.rev_slope  = ee_r32_f(car_ptr + 0x3D8 + 32);
    curve.min_torque = ee_r32_f(car_ptr + 0x3D8 + 36);
    curve.split_rpm  = ee_r32_f(car_ptr + 0x3D8 + 40);

    const auto table = build_monotonic_shift_table(preset, curve, redline);
    std::memset(g_ram + car_ptr + 0x414, 0, 0x70);
    for (size_t i = 0; i < table.size() && i < 7; ++i) {
        const uint32_t slot = car_ptr + 0x414u + static_cast<uint32_t>(i * 16);
        ee_w32_f(slot + 0, table[i].ratio);
        ee_w32_f(slot + 4, table[i].upshift_ds_rpm);
        ee_w32_f(slot + 8, table[i].downshift_lo_ds_rpm);
        ee_w32_f(slot + 12, table[i].downshift_hi_ds_rpm);
    }
}

#endif // EOD_NASCAR_AUDIO_HOST_ONLY
