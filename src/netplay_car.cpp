#include "netplay_car.h"
#include "../runtime/core/memory.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace netplay_car {
namespace {
bool valid(uint32_t a, uint32_t n) { return a && !(a & 3) && a <= EE_RAM_SIZE - n; }
float rf(uint32_t a) { uint32_t u = ee_r32(a); float f; std::memcpy(&f, &u, 4); return f; }
void wf(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); ee_w32(a, u); }
}  // namespace

uint32_t slot_count() { uint32_t n = ee_r32(0x48522C); return n > 16 ? 0 : n; }
uint32_t car_of_slot(uint32_t slot) {
    if (slot >= slot_count()) return 0;
    uint32_t c = ee_r32(kParticipantTable + 4 * slot);
    return valid(c, 0x1C34) ? c : 0;
}
bool is_human(uint32_t car) { return valid(car, 0x1C34) && ee_r32(car + kType) == 0x50; }

uint32_t body_of(uint32_t car) {
    if (!valid(car, 0x1C34)) return 0;
    const float px = rf(car + 0x88), pz = rf(car + 0x90);
    for (uint32_t a = 0x494000; a < 0x496000; a += 4) {
        uint32_t p = ee_r32(a);
        if (p > 0x100000 && p < EE_RAM_SIZE - 0x80 && !(p & 3) && std::fabs(rf(p) - px) < 1e-4f && std::fabs(rf(p + 8) - pz) < 1e-4f &&
            std::fabs(rf(p + 0x10) - rf(car + 0x64)) < 1e-4f)
            return p;
    }
    return 0;
}
bool read(uint32_t car, State &s) {
    if (!valid(car, 0x1C34)) return false;
    for (int i = 0; i < 12; ++i) s.pose[i] = rf(car + kPose + 4 * i);
    for (int i = 0; i < 3; ++i) { s.vel[i] = rf(car + kVel + 4 * i); s.ang[i] = rf(car + kAngVel + 4 * i); }
    s.steer = rf(car + kSteer); s.gas = rf(car + kGas); s.brake = rf(car + kBrake); s.handbrake = rf(car + kHandbrake);
    s.gear = (int32_t)ee_r32(car + kGear); s.damage = rf(car + kDamage);
    return true;
}
void matrix_to_quat(const float m[9], float q[4]) {
    float w = std::sqrt(std::fmax(0.f, 1.f + m[0] + m[4] + m[8])) * 0.5f, x, y, z;
    if (w > 0.2f) { float k = 0.25f / w; x = (m[7] - m[5]) * k; y = (m[2] - m[6]) * k; z = (m[3] - m[1]) * k; }
    else if (m[0] >= m[4] && m[0] >= m[8]) { float t = 2.f * std::sqrt(std::fmax(1e-6f, 1.f + m[0] - m[4] - m[8])); x = 0.25f * t; y = (m[1] + m[3]) / t; z = (m[2] + m[6]) / t; w = (m[7] - m[5]) / t; }
    else if (m[4] >= m[8]) { float t = 2.f * std::sqrt(std::fmax(1e-6f, 1.f + m[4] - m[0] - m[8])); y = 0.25f * t; x = (m[1] + m[3]) / t; z = (m[5] + m[7]) / t; w = (m[2] - m[6]) / t; }
    else { float t = 2.f * std::sqrt(std::fmax(1e-6f, 1.f + m[8] - m[0] - m[4])); z = 0.25f * t; x = (m[2] + m[6]) / t; y = (m[5] + m[7]) / t; w = (m[3] - m[1]) / t; }
    q[0] = x; q[1] = y; q[2] = z; q[3] = w;
}
void quat_to_matrix(const float q[4], float m[9]) {
    float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    float x = q[0], y = q[1], z = q[2], w = q[3];
    if (n > 1e-6f) { x /= n; y /= n; z /= n; w /= n; } else { x = y = z = 0; w = 1; }
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - z * w); m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w); m[7] = 2 * (y * z + x * w); m[8] = 1 - 2 * (x * x + y * y);
}
static void write_body(uint32_t B, const State &s) {
    for (int i = 0; i < 3; ++i) wf(B + 4 * i, s.pose[9 + i]);
    for (int r = 0; r < 3; ++r) for (int i = 0; i < 3; ++i) wf(B + 0x10 + 0x10 * r + 4 * i, s.pose[3 * r + i]);
    float q[4]; matrix_to_quat(s.pose, q);
    for (int i = 0; i < 4; ++i) wf(B + 0x40 + 4 * i, q[i]);
    for (int i = 0; i < 3; ++i) { wf(B + 0x50 + 4 * i, s.vel[i]); wf(B + 0x60 + 4 * i, s.ang[i]); }
}
void write(uint32_t car, const State &s) {
    if (!valid(car, 0x1C34)) return;
    static uint32_t cached_car[16], cached_body[16];
    uint32_t B = 0;
    for (int i = 0; i < 16; ++i) if (cached_car[i] == car) B = cached_body[i];
    if (!B) { B = body_of(car); if (B) for (int i = 0; i < 16; ++i) if (!cached_car[i]) { cached_car[i] = car; cached_body[i] = B; break; } }
    if (B) write_body(B, s);
    for (int i = 0; i < 12; ++i) wf(car + kPose + 4 * i, s.pose[i]);
    for (int i = 0; i < 3; ++i) { wf(car + kVel + 4 * i, s.vel[i]); wf(car + kAngVel + 4 * i, s.ang[i]); }
    wf(car + kSteer, s.steer); wf(car + kGas, s.gas); wf(car + kBrake, s.brake); wf(car + kHandbrake, s.handbrake);
    ee_w32(car + kGear, (uint32_t)s.gear);
}

State lerp(const State &a, const State &b, float t) {
    State o = a;
    auto L = [&](float x, float y) { return x + (y - x) * t; };
    for (int i = 9; i < 12; ++i) o.pose[i] = L(a.pose[i], b.pose[i]);
    float qa[4], qb[4], q[4];
    matrix_to_quat(a.pose, qa); matrix_to_quat(b.pose, qb);
    float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    if (d < 0) { for (float &v : qb) v = -v; d = -d; }
    for (int i = 0; i < 4; ++i) q[i] = qa[i] + (qb[i] - qa[i]) * t;   // nlerp: ample at 1/30 s spacing
    quat_to_matrix(q, o.pose);
    for (int i = 0; i < 3; ++i) { o.vel[i] = L(a.vel[i], b.vel[i]); o.ang[i] = L(a.ang[i], b.ang[i]); }
    o.steer = L(a.steer, b.steer); o.gas = L(a.gas, b.gas); o.brake = L(a.brake, b.brake);
    o.handbrake = t < .5f ? a.handbrake : b.handbrake; o.gear = t < .5f ? a.gear : b.gear; o.damage = L(a.damage, b.damage);
    return o;
}
State extrapolate(const State &s, float seconds) {
    State o = s;
    for (int i = 0; i < 3; ++i) o.pose[9 + i] += s.vel[i] * seconds;
    float q[4]; matrix_to_quat(s.pose, q);
    // small-angle rotation by angular velocity (world space): q' = q + 0.5 * dt * (0,w) * q
    float wx = s.ang[0] * seconds * .5f, wy = s.ang[1] * seconds * .5f, wz = s.ang[2] * seconds * .5f;
    float nq[4] = {q[0] + wx * q[3] + wy * q[2] - wz * q[1], q[1] + wy * q[3] + wz * q[0] - wx * q[2],
                   q[2] + wz * q[3] + wx * q[1] - wy * q[0], q[3] - wx * q[0] - wy * q[1] - wz * q[2]};
    quat_to_matrix(nq, o.pose);
    return o;
}

void Remote::push(uint32_t tick, const State &s) {
    if (!ring_.empty() && tick <= ring_.back().tick) {
        if (tick + 600 < ring_.back().tick) reset();   // sender restarted its clock
        else return;                                   // stale / duplicate
    }
    ring_.push_back({tick, s});
    if (ring_.size() > 90) ring_.erase(ring_.begin(), ring_.begin() + 30);
}
State Remote::sample(float tick) const {
    const Entry *prev = nullptr;
    for (const Entry &e : ring_) {
        if ((float)e.tick >= tick) {
            if (!prev) return e.s;
            float span = (float)(e.tick - prev->tick);
            return lerp(prev->s, e.s, span > 0 ? (tick - (float)prev->tick) / span : 1.f);
        }
        prev = &e;
    }
    const Entry &last = ring_.back();   // late: dead reckon from the newest snapshot, then hold
    float ahead = std::min(tick - (float)last.tick, kExtrapolateMax);
    return extrapolate(last.s, ahead / 60.f);
}
State Remote::advance() {
    const float newest = (float)ring_.back().tick;
    if (!head_init_) { head_ = newest - kDelay; head_init_ = true; }
    head_ += 1.f;
    // keep the play-head near newest - delay: speed up / slow down gently, jump only after a long gap
    const float err = (newest - kDelay) - head_;
    if (std::fabs(err) > 40.f) head_ = newest - kDelay; else head_ += err * 0.03f;
    State target = sample(head_);
    if (!shown_init_) { shown_ = target; shown_init_ = true; return shown_; }
    // follow the target: ride the car's own velocity, then close the position error over a few
    // steps (large corrections are blended, never teleported, and the car stays solid throughout)
    State moved = extrapolate(shown_, 1.f / 60.f);
    float dx = target.pose[9] - moved.pose[9], dy = target.pose[10] - moved.pose[10], dz = target.pose[11] - moved.pose[11];
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    float k = dist < 0.03f ? 1.f : dist > 8.f ? 0.5f : 0.3f;   // beyond 8 m the car still moves half way per step
    State out = lerp(moved, target, k);
    out.vel[0] = target.vel[0]; out.vel[1] = target.vel[1]; out.vel[2] = target.vel[2];
    out.ang[0] = target.ang[0]; out.ang[1] = target.ang[1]; out.ang[2] = target.ang[2];
    shown_ = out;
    return shown_;
}

// Developer proof harness (N1): EOD_NETPLAY_REC=<file> records the human car each race
// tick; EOD_NETPLAY_PLAY=<file> replays it onto the first AI car as a puppet.
namespace {
std::vector<State> g_rec;
bool g_init = false;
FILE *g_rec_file = nullptr;
uint32_t g_puppet = 0;
double g_sum = 0, g_max = 0; uint64_t g_n = 0;
uint32_t race_tick() { return ee_r32(0x3CDA8C); }
uint32_t first_car(bool human) {
    for (uint32_t s = 0; s < slot_count(); ++s) { uint32_t c = car_of_slot(s); if (c && is_human(c) == human) return c; }
    return 0;
}
void init_harness() {
    if (g_init) return;
    g_init = true;
    if (const char *p = std::getenv("EOD_NETPLAY_REC")) g_rec_file = std::fopen(p, "wb");
    if (const char *p = std::getenv("EOD_NETPLAY_PLAY"))
        if (FILE *f = std::fopen(p, "rb")) { State s; while (std::fread(&s, sizeof s, 1, f) == 1) g_rec.push_back(s); std::fclose(f); }
}
}  // namespace
void before_step() {
    init_harness();
    if (!g_rec_file && g_rec.empty()) return;
    uint32_t t = race_tick();
    if (g_rec_file) { State s; if (uint32_t c = first_car(true)) if (read(c, s)) { std::fwrite(&s, sizeof s, 1, g_rec_file); if ((t & 255) == 0) std::fflush(g_rec_file); } }
    if (!g_rec.empty()) {
        if (!g_puppet) { g_puppet = first_car(false); if (g_puppet) set_puppet(g_puppet, true); }
        if (g_puppet && t < g_rec.size()) write(g_puppet, g_rec[t]);
    }
}
void after_step() {
    if (g_rec.empty() || !g_puppet) return;
    uint32_t t = race_tick();
    if (t >= g_rec.size()) return;
    State s;
    if (read(g_puppet, s)) {
        double d = 0;
        for (int i = 9; i < 12; ++i) { double e = s.pose[i] - g_rec[t].pose[i]; d += e * e; }
        d = std::sqrt(d); g_sum += d; ++g_n; g_max = std::max(g_max, d);
        if (g_n % 600 == 0) std::fprintf(stderr, "[netplay] puppet native drift per step: mean %.4f max %.4f m over %llu steps\n", g_sum / g_n, g_max, (unsigned long long)g_n);
    }
    write(g_puppet, g_rec[t]);
}
}  // namespace netplay_car
