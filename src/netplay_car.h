#pragma once
// Netplay car state: read the native car, impose state on a puppet, smooth remote cars.
// Layout verified in docs/NETPLAY_STATE_SYNC.md (all offsets SLUS_209.10):
//   car object: 4x3 pose at +0x64 (right,up,forward,position), linear velocity +0x104,
//   angular velocity +0x110, controls +0x1B6C.., damage +0x1BEC.
//   physics master: contiguous 0xB0-byte records at *(0x494D30), count *(0x494D28):
//   +0x00 position, +0x10/+0x20/+0x30 rotation rows, +0x40 quaternion xyzw, +0x50 linear
//   velocity, +0x60 angular velocity. The car's copies are refreshed from it every step.
#include <cstdint>
#include <vector>
#include "netplay_puppet.h"
namespace netplay_car {
struct State {
    float pose[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};   // rows right/up/forward, then position
    float vel[3] = {}, ang[3] = {};
    float steer = 0, gas = 0, brake = 0, handbrake = 0;
    int32_t gear = 0;
    float damage = 0;
    uint32_t damage_stage = 0;
    uint8_t warp_gen = 0;
};
constexpr uint32_t kPose = 0x64, kVel = 0x104, kAngVel = 0x110, kSteer = 0x1B6C, kGas = 0x1B70,
                   kBrake = 0x1B74, kHandbrake = 0x1B7C, kGear = 0x1B90, kDamage = 0x1BEC, kType = 0x1E0;
constexpr uint32_t kParticipantTable = 0x447458;   // car by description slot (0x179A08)
uint32_t slot_count();
uint32_t car_of_slot(uint32_t slot);        // 0 when absent
bool is_human(uint32_t car);                // type 0x50
uint32_t human_controller_of(uint32_t car); // verifies native HumanDriver + pad zero input
uint32_t body_of(uint32_t car);             // physics master record, 0 if not found
uint32_t attached_trailer(uint32_t tow_car); // single native trailer hitched to this tow car
bool valid_state(const State &s);           // finite pose/controls within native world limits
bool read(uint32_t car, State &s);
void write_controls(uint32_t car, const State &s);
void release_controls(uint32_t car);
void write_condition(uint32_t car, const State &s);
void write(uint32_t car, const State &s);   // body record + car copies
void before_step();                         // world-step wrapper hooks
void after_step();

void matrix_to_quat(const float rows[9], float q[4]);
void quat_to_matrix(const float q[4], float rows[9]);
State lerp(const State &a, const State &b, float t);   // pose slerp, linear elsewhere
State extrapolate(const State &s, float seconds);       // dead reckoning

// Remote car timeline. Nearby solid cars use bounded prediction for contact;
// at distance the play-head buffers snapshots to smooth network jitter.
class Remote {
public:
    static constexpr float kExtrapolateMax = 15.f;
    static constexpr unsigned kStallSteps = 30;   // half a second without a newer snapshot
    void push(uint32_t tick, const State &s);
    bool ready() const { return !ring_.empty(); }
    float proximity_to(const State &car) const;
    State advance(int buffer_ms = 100, float proximity = 0, unsigned rtt_ms = 0);
    bool stalled() const { return steps_since_sample_ == kStallSteps; }
    void follow_native(const State &s) { shown_ = s; }
    void reset() { ring_.clear(); head_init_ = false; shown_init_ = false; steps_since_sample_ = 0; warp_gen_init_ = false; warp_gen_ = 0; }
    float newest_tick() const { return ring_.empty() ? 0.f : (float)ring_.back().tick; }
private:
    struct Entry { uint32_t tick; State s; };
    std::vector<Entry> ring_;
    float head_ = 0;
    unsigned steps_since_sample_ = 0;
    bool head_init_ = false, shown_init_ = false;
    bool warp_gen_init_ = false;
    uint8_t warp_gen_ = 0;
    State shown_;
    State sample(float tick) const;
};
}  // namespace netplay_car
