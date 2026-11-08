#pragma once
// Netplay car state: read the native car, impose state on a puppet, smooth remote cars.
// Layout verified in docs/NETPLAY_STATE_SYNC.md (all offsets SLUS_209.10):
//   car object: 4x3 pose at +0x64 (right,up,forward,position), linear velocity +0x104,
//   angular velocity +0x110, controls +0x1B6C.., damage +0x1BEC.
//   physics master: body record in the global body table (0x494000..0x496000):
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
};
constexpr uint32_t kPose = 0x64, kVel = 0x104, kAngVel = 0x110, kSteer = 0x1B6C, kGas = 0x1B70,
                   kBrake = 0x1B74, kHandbrake = 0x1B7C, kGear = 0x1B90, kDamage = 0x1BEC, kType = 0x1E0;
constexpr uint32_t kParticipantTable = 0x447458;   // car by description slot (0x179A08)
uint32_t slot_count();
uint32_t car_of_slot(uint32_t slot);        // 0 when absent
bool is_human(uint32_t car);                // type 0x50
uint32_t body_of(uint32_t car);             // physics master record, 0 if not found
bool read(uint32_t car, State &s);
void write(uint32_t car, const State &s);   // body record + car copies
void before_step();                         // world-step wrapper hooks
void after_step();

void matrix_to_quat(const float rows[9], float q[4]);
void quat_to_matrix(const float q[4], float rows[9]);
State lerp(const State &a, const State &b, float t);   // pose slerp, linear elsewhere
State extrapolate(const State &s, float seconds);       // dead reckoning

// Remote car timeline: snapshots keyed by the sender's tick, a play-head that follows
// the newest tick at a fixed delay, a short dead-reckoning window, and a blend that
// turns big corrections into a few frames of movement. The car is never made intangible.
class Remote {
public:
    static constexpr float kDelay = 6.f;          // ticks (1/60 s) behind the newest snapshot
    static constexpr float kExtrapolateMax = 15.f;
    void push(uint32_t tick, const State &s);
    bool ready() const { return !ring_.empty(); }
    State advance();                              // one local world step
    void reset() { ring_.clear(); head_init_ = false; shown_init_ = false; }
    float newest_tick() const { return ring_.empty() ? 0.f : (float)ring_.back().tick; }
private:
    struct Entry { uint32_t tick; State s; };
    std::vector<Entry> ring_;
    float head_ = 0;
    bool head_init_ = false, shown_init_ = false;
    State shown_;
    State sample(float tick) const;
};
}  // namespace netplay_car
