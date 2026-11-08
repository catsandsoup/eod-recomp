#pragma once
// Netplay v2 wire format (state sync). Plain UDP datagrams, big endian, no host structs.
// Every message except SNAP is idempotent state that is simply resent until the peer's
// heartbeat shows it arrived, so no reliable-transport layer is needed.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "netplay_car.h"
namespace netplay {
constexpr uint16_t kMagic = 0x7D3E, kProtocol = 3;
constexpr int kMaxPlayers = 16;
enum Kind : uint8_t {
    K_JOIN = 1, K_WELCOME, K_REFUSE, K_HEARTBEAT, K_LOBBY, K_START, K_TRACK, K_ROSTER, K_GO, K_END, K_LEAVE,
    K_SNAP, K_BEACON, K_KICK,
    K_PUNCH, K_PROBE, K_PROBE_REPLY,     // internet: NAT punch, friend-presence probe and its answer
    K_VOICE                              // Opus voice frame (netplay::voice::Packet)
};
// guest phase, reported in heartbeats
enum Phase : uint8_t { P_LOBBY = 0, P_STARTED, P_CAR_DONE, P_LOADED, P_RACING, P_DONE };

struct Writer {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u16(uint32_t v) { u8(v >> 8); u8(v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v); }
    void f32(float f) { uint32_t u; std::memcpy(&u, &f, 4); u32(u); }
    void i16(float v, float scale) { int x = (int)(v * scale); if (x > 32767) x = 32767; if (x < -32768) x = -32768; u16((uint16_t)x); }
    void str(const std::string &s, size_t max) { size_t n = s.size() < max ? s.size() : max; u8((uint32_t)n); b.insert(b.end(), s.begin(), s.begin() + n); }
};
struct Reader {
    const uint8_t *p, *e; bool ok = true;
    Reader(const uint8_t *d, size_t n) : p(d), e(d + n) {}
    uint32_t u8() { if (p >= e) { ok = false; return 0; } return *p++; }
    uint32_t u16() { uint32_t a = u8(); return (a << 8) | u8(); }
    uint32_t u32() { uint32_t a = u16(); return (a << 16) | u16(); }
    float f32() { uint32_t u = u32(); float f; std::memcpy(&f, &u, 4); return f; }
    float i16(float scale) { return (float)(int16_t)u16() / scale; }
    std::string str() { size_t n = u8(); if (p + n > e) { ok = false; return {}; } std::string s((const char *)p, n); p += n; return s; }
};
inline void put_car(Writer &w, uint8_t slot, uint32_t tick, const netplay_car::State &s) {
    w.u8(slot); w.u32(tick);
    for (int i = 9; i < 12; ++i) w.f32(s.pose[i]);
    for (int i = 0; i < 3; ++i) w.f32(s.vel[i]);
    for (int i = 0; i < 3; ++i) w.i16(s.ang[i], 2000.f);
    float q[4]; netplay_car::matrix_to_quat(s.pose, q);
    if (q[3] < 0) for (float &v : q) v = -v;
    for (float v : q) w.i16(v, 32000.f);
    w.i16(s.steer, 30000.f); w.i16(s.gas, 30000.f); w.i16(s.brake, 30000.f); w.u8(s.handbrake > .5f); w.u8((uint8_t)(int8_t)s.gear);
    w.i16(s.damage, 8000.f);
}
inline bool get_car(Reader &r, uint8_t &slot, uint32_t &tick, netplay_car::State &s) {
    slot = (uint8_t)r.u8(); tick = r.u32();
    for (int i = 9; i < 12; ++i) s.pose[i] = r.f32();
    for (int i = 0; i < 3; ++i) s.vel[i] = r.f32();
    for (int i = 0; i < 3; ++i) s.ang[i] = r.i16(2000.f);
    float q[4]; for (float &v : q) v = r.i16(32000.f);
    netplay_car::quat_to_matrix(q, s.pose);
    s.steer = r.i16(30000.f); s.gas = r.i16(30000.f); s.brake = r.i16(30000.f); s.handbrake = r.u8() ? 1.f : 0.f; s.gear = (int8_t)r.u8();
    s.damage = r.i16(8000.f);
    return r.ok;
}
}  // namespace netplay
