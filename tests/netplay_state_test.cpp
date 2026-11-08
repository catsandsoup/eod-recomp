// Netplay state sync: wire format, quaternion/pose maths, remote-car smoothing, puppet writes.
#include "core/memory.h"
#include "netplay_car.h"
#include "netplay_wire.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <limits>
using namespace netplay_car;
uint8_t *g_ram;
extern "C" uint8_t ee_read8_slow(uint32_t a) { return g_ram[a]; }
extern "C" void ee_write8_slow(uint32_t a, uint8_t v) { g_ram[a] = v; }
extern "C" uint16_t ee_read16_slow(uint32_t a) { uint16_t v; std::memcpy(&v, g_ram + a, 2); return v; }
extern "C" void ee_write16_slow(uint32_t a, uint16_t v) { std::memcpy(g_ram + a, &v, 2); }
extern "C" uint32_t ee_read32_slow(uint32_t a) { uint32_t v; std::memcpy(&v, g_ram + a, 4); return v; }
extern "C" void ee_write32_slow(uint32_t a, uint32_t v) { std::memcpy(g_ram + a, &v, 4); }
extern "C" uint64_t ee_read64_slow(uint32_t a) { uint64_t v; std::memcpy(&v, g_ram + a, 8); return v; }
extern "C" void ee_write64_slow(uint32_t a, uint64_t v) { std::memcpy(g_ram + a, &v, 8); }
static std::vector<uint8_t> ram(EE_RAM_SIZE);
static void require(bool ok, const char *msg) { if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); std::exit(1); } }
static float getf(uint32_t a) { float f; std::memcpy(&f, g_ram + a, 4); return f; }
static State make(float x, float z, float yaw, float vx, float vz) {
    State s; float c = std::cos(yaw), n = std::sin(yaw);
    float m[9] = {c, 0, n, 0, 1, 0, -n, 0, c};
    std::memcpy(s.pose, m, sizeof m); s.pose[9] = x; s.pose[10] = 1; s.pose[11] = z; s.vel[0] = vx; s.vel[2] = vz; s.steer = .25f; s.gas = 1; s.gear = 3; s.damage = .4f;
    return s;
}
int main() {
    // quaternion round trip over many orientations
    for (int i = 0; i < 200; ++i) {
        float yaw = i * 0.05f, c = std::cos(yaw), n = std::sin(yaw), p = std::sin(i * .13f) * .4f, cp = std::cos(p), sp = std::sin(p);
        float m[9] = {c, n * sp, n * cp, 0, cp, -sp, -n, c * sp, c * cp}, q[4], b[9];
        matrix_to_quat(m, q); quat_to_matrix(q, b);
        for (int k = 0; k < 9; ++k) require(std::fabs(m[k] - b[k]) < 1e-4f, "quaternion round trip");
    }
    // wire format
    {
        State a = make(12.5f, -40.25f, 1.1f, 3.f, -4.f); a.ang[1] = .7f;
        netplay::Writer w; netplay::put_car(w, 5, 1234, a);
        netplay::Reader r(w.b.data(), w.b.size()); uint8_t slot; uint32_t tick; State b;
        require(netplay::get_car(r, slot, tick, b) && slot == 5 && tick == 1234, "wire header");
        require(std::fabs(b.pose[9] - 12.5f) < 1e-4f && std::fabs(b.pose[11] + 40.25f) < 1e-4f && std::fabs(b.vel[2] + 4.f) < 1e-4f, "wire position/velocity");
        for (int k = 0; k < 9; ++k) require(std::fabs(a.pose[k] - b.pose[k]) < 2e-3f, "wire orientation");
        require(std::fabs(b.ang[1] - .7f) < 1e-3f && std::fabs(b.steer - .25f) < 1e-3f && b.gear == 3 && std::fabs(b.damage - .4f) < 1e-3f, "wire controls/damage");
        require(w.b.size() < 70, "snapshot stays compact");
        netplay::Reader bad(w.b.data(), 10); require(!netplay::get_car(bad, slot, tick, b), "truncated packet rejected");
    }
    // a remote car driving straight at 15 m/s, snapshots at 30 Hz: the puppet follows ~6 ticks behind, smoothly
    {
        Remote rc; float maxstep = 0, prev = 0;
        for (uint32_t t = 0; t <= 300; t += 2) rc.push(t, make(15.f * t / 60.f, 0, 0, 15, 0));
        // replay as if snapshots arrive live: feed two per two local steps
        Remote live; State shown;
        for (uint32_t t = 0; t < 240; ++t) {
            if (t % 2 == 0) live.push(t, make(15.f * t / 60.f, 0, 0, 15, 0));
            if (!live.ready()) continue;
            shown = live.advance();
            if (t > 30) { maxstep = std::fmax(maxstep, std::fabs(shown.pose[9] - prev)); }
            prev = shown.pose[9];
        }
        float expect = 15.f * (239 - 6) / 60.f;
        require(std::fabs(shown.pose[9] - expect) < 1.0f, "remote car trails by the interpolation delay");
        require(maxstep < 0.45f, "remote car moves smoothly (no jumps)");
    }
    // a big correction is blended over several steps, never teleported
    {
        Remote live; State shown;
        for (uint32_t t = 0; t < 60; ++t) { if (t % 2 == 0) live.push(t, make(0, 0, 0, 0, 0)); if (live.ready()) shown = live.advance(); }
        for (uint32_t t = 60; t < 120; ++t) { if (t % 2 == 0) live.push(t, make(20, 0, 0, 0, 0)); shown = live.advance(); if (t == 70) require(shown.pose[9] < 19.5f, "correction is not an instant teleport"); }
        require(std::fabs(shown.pose[9] - 20.f) < 0.2f, "correction converges");
    }
    // dead reckoning when snapshots stop, bounded
    {
        Remote live; State shown;
        for (uint32_t t = 0; t < 40; ++t) { if (t % 2 == 0) live.push(t, make(10.f * t / 60.f, 0, 0, 10, 0)); if (live.ready()) shown = live.advance(); }
        float at_stop = shown.pose[9];
        for (uint32_t t = 40; t < 400; ++t) shown = live.advance();
        require(shown.pose[9] > at_stop && shown.pose[9] < at_stop + 10.f * 0.5f, "late car is dead-reckoned briefly, then held");
    }
    // A long-delayed datagram cannot rewind a current-event remote timeline.
    {
        Remote remote; remote.push(5000, make(20, 0, 0, 0, 0));
        remote.push(2, make(1000, 0, 0, 0, 0));
        require(remote.newest_tick() == 5000, "very old reordered snapshot rejected");
        require(remote.advance().pose[9] == 20, "late snapshot cannot replace current pose");
    }
    // writing a puppet updates the physics body record, found through the body table
    {
        g_ram = ram.data();
        const uint32_t car = 0x100000, body = 0x200000;
        std::memset(g_ram + car, 0, 0x1C40);
        auto putf = [](uint32_t a, float f) { std::memcpy(g_ram + a, &f, 4); };
        putf(car + 0x64, 1); putf(car + 0x74, 1); putf(car + 0x84, 1); putf(car + 0x88, 5); putf(car + 0x90, 7);
        putf(body, 5); putf(body + 8, 7); putf(body + 0x10, 1);
        MEM_SW(0x494D30, body); MEM_SW(0x494D28, 1); MEM_SW(0x3CE548, 1); MEM_SW(0x3CE4F8, car);
        require(body_of(car) == body, "body record located");
        State s = make(50, -20, 0.5f, 1, 2); write(car, s);
        require(std::fabs(getf(body) - 50) < 1e-4f && std::fabs(getf(body + 8) + 20) < 1e-4f && std::fabs(getf(body + 0x50) - 1) < 1e-4f, "body updated");
        require(std::fabs(getf(car + 0x88) - 50) < 1e-4f && std::fabs(getf(car + 0x104) - 1) < 1e-4f, "car copies updated");
        float q[4] = {getf(body + 0x40), getf(body + 0x44), getf(body + 0x48), getf(body + 0x4C)};
        require(std::fabs(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] - 1.f) < 1e-3f, "unit quaternion written");
        set_puppet(car, true); require(is_puppet(car), "puppet registered"); set_puppet(car, false); require(!is_puppet(car), "puppet removed");
    }
    // The allocator can reuse a car address on rematch while its body changes.
    // A cached mapping would corrupt the old allocation and leave the new body behind.
    {
        const uint32_t car = 0x100000, old_body = 0x200000, new_body = 0x210000;
        auto putf = [](uint32_t a, float f) { std::memcpy(g_ram + a, &f, 4); };
        State s = make(91, 37, 0, 0, 0);
        for (int i = 0; i < 12; ++i) putf(car + kPose + 4 * i, s.pose[i]);
        putf(new_body, 91); putf(new_body + 8, 37); putf(new_body + 0x10, 1);
        MEM_SW(0x494D30, new_body);
        const float old_x = getf(old_body);
        s.pose[9] = 112; write(car, s);
        require(getf(new_body) == 112, "rematch resolves the new physics body");
        require(getf(old_body) == old_x, "rematch never writes the stale body");
    }
    // Every body after zero is an array element, not another global pointer.
    // Repeated/overlapping positions must still resolve by native ownership.
    {
        const uint32_t car0 = 0x100000, car1 = 0x110000, base = 0x210000;
        MEM_SW(0x494D28, 2); MEM_SW(0x3CE548, 2); MEM_SW(0x3CE4F8 + 4, car1);
        require(body_of(car0) == base && body_of(car1) == base + 0xB0,
                "all contiguous native bodies resolve by car ownership");
        State s = make(57, 19, 0, 2, 3); write(car1, s);
        require(getf(base + 0xB0) == 57 && getf(base) == 112, "second car write targets its own body");
        // Native Human driver wrapper, controller, and controller's input pad.
        const uint32_t driver = 0x220000, human = 0x221000, input = 0x222000;
        MEM_SW(car1 + kType, 0x50); MEM_SW(car1 + 0x1C1C, driver); MEM_SW(driver, 0x3FA4B0);
        MEM_SW(driver + 0x10, human); MEM_SW(human, 0x3FA450); MEM_SW(human + 0x38, car1);
        MEM_SW(human + 0x3C, input); MEM_SW(input + 0x34, 0);
        require(human_controller_of(car1) == human, "native local human controller verified");
        MEM_SW(input + 0x34, 1); require(!human_controller_of(car1), "wrong input pad rejected");
        MEM_SW(input + 0x34, 0); MEM_SW(human + 0x38, car0);
        require(!human_controller_of(car1), "human controller bound to another car rejected");
    }
    // Reject invalid native/network state before it reaches terrain or physics.
    {
        State bad = make(1, 2, 0, 0, 0);
        bad.pose[9] = std::numeric_limits<float>::infinity();
        require(!valid_state(bad), "nonfinite native position rejected");
        Remote remote; remote.push(1, bad); require(!remote.ready(), "invalid snapshot never enters timeline");
        netplay::Writer w; netplay::put_car(w, 0, 1, bad);
        netplay::Reader r(w.b.data(), w.b.size()); uint8_t slot; uint32_t tick; State decoded;
        require(!netplay::get_car(r, slot, tick, decoded), "invalid wire position rejected");
        bad.pose[9] = 1e30f; require(!valid_state(bad), "corrupt terrain-scale position rejected");
    }
    std::puts("netplay_state_test ok");
    return 0;
}
