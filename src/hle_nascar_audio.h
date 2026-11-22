// hle_nascar_audio.h - Native NASCAR ESFX multi-gear engine audio & dynamic .gar
// track-class transmission hooks for SLUS_209.10 (0x23C400, 0x23C8D0, 0x186DC8).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nascar_engine_audio.h"

namespace eod::nascar_audio_hle {

enum class TrackClass : uint8_t {
    ShortDirt = 0,
    ShortAsphalt = 1,
    RoadCourse = 2,
    Speedway = 3,
    Superspeedway = 4,
};

struct GarTransmissionPreset {
    TrackClass track_class = TrackClass::ShortDirt;
    int32_t num_gears = 4;
    std::array<float, 6> ratios{3.50f, 2.70f, 1.90f, 1.25f, 0.90f, 0.85f};
    float rear_end_ratio = 5.43f;
};

struct EngineTorqueCurve {
    float a_hi = 0.0f;
    float b_hi = 0.0f;
    float c_hi = 0.0f;
    float a_lo = 0.0f;
    float b_lo = 0.0f;
    float c_lo = 0.0f;
    float drag_k = 0.0f;
    float rev_start = 0.0f;
    float rev_slope = 0.0f;
    float min_torque = 0.0f;
    float split_rpm = 0.0f;
};

struct GearShiftEntry {
    float ratio = 1.0f;
    float upshift_ds_rpm = 100000.0f;
    float downshift_lo_ds_rpm = 0.0f;
    float downshift_hi_ds_rpm = 0.0f;
};

struct NascarEngineVoiceState {
    uint32_t eng_snd_addr = 0;
    uint32_t car_ptr = 0;
    int engine_index = 1;
    std::string profile_name;
    nascar_audio::Profile profile;
    std::vector<uint32_t> voice_handles;
    std::vector<float> smoothed_volumes;
    uint32_t idle_voice = 0;
    float smoothed_idle_volume = 0.0f;
    uint32_t backfire_voice = 0;
    nascar_audio::AudioRng audio_rng;
    int32_t last_gear = 1;
    int32_t shift_transient_frames = 0;
    float shift_rpm_dip = 0.0f;
    float last_rpm = 900.0f;
};

struct MixFrameOutput {
    std::vector<nascar_audio::Voice> layers;
    nascar_audio::Voice idle{false, 0.0f, 0.0f};
    bool trigger_backfire = false;
    float backfire_pitch = 1.0f;
    float effective_rpm = 0.0f;
    bool gear_changed = false;
    bool upshifted = false;
    bool downshifted = false;
};

// Detects whether [EngineNormal] engine_sound / sound refers to a NASCAR ESFX profile.
// Retail EOD *.val presets return false and stay on the exact native path.
bool detect_nascar_esfx_profile(const std::string& engine_sound,
                                const std::string& sound,
                                std::string& out_profile_name,
                                int& out_engine_index);

// Classifies an active track from its archive/display name, track_type, and surface.
TrackClass classify_track(const std::string& track_name,
                          const std::string& track_type,
                          const std::string& surface);

// Returns authentic D2D .gar ratios and final drive for a circuit class.
GarTransmissionPreset transmission_preset_for_track(TrackClass track_class);

// Evaluates EOD's native piecewise quadratic torque curve (car + 0x3D8).
float evaluate_engine_torque(const EngineTorqueCurve& curve, float rpm);

// Computes EOD's auto-shift table (car + 0x414) with strict monotonic upshift/downshift
// thresholds and rev-limit headroom so cars cleanly cycle 1 -> 2 -> 3 -> 4.
std::vector<GearShiftEntry> build_monotonic_shift_table(const GarTransmissionPreset& preset,
                                                        const EngineTorqueCurve& curve,
                                                        float redline_rpm);

// Evaluates one audio frame for a NASCAR ESFX engine state using isolated audio RNG.
MixFrameOutput evaluate_nascar_audio_frame(NascarEngineVoiceState& state,
                                           float rpm,
                                           float throttle,
                                           int32_t gear,
                                           bool viewed,
                                           bool local_mix,
                                           bool silenced);

// Clears dynamic host voice registry (useful on race reset or tests).
void reset_nascar_audio_registry();
size_t active_nascar_voice_count();

} // namespace eod::nascar_audio_hle
