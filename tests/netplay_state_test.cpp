// Netplay state sync: wire format, quaternion/pose maths, remote-car smoothing, puppet writes.
#include "core/memory.h"
#include "netplay_car.h"
#include "netplay_peer_activity.h"
#include "netplay_names.h"
#include "netplay_wire.h"
#include "netplay_battle.h"
#include "netplay_chicken.h"
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
static uint64_t simulated_time;
uint64_t platform_time_us() { return simulated_time; }

int main() {
    netplay::PeerActivity peer;
    simulated_time = 100;
    peer.heard();
    // The next poll takes six seconds to drain, and accepts a packet at its end.
    simulated_time += 6000000;
    peer.heard();
    simulated_time += 1;
    if (peer.expired()) return 1;
    simulated_time -= 2;
    if (peer.expired()) return 1;
    simulated_time += 5000001;
    if (peer.expired()) return 1;
    ++simulated_time;
    if (!peer.expired()) return 1;
    {
        using namespace netplay;
        ChickenEvent shot;
        shot.slot = 3; shot.shot = 48; shot.life = 901;
        shot.position = {3.5f, 1.2f, -8.f}; shot.velocity = {1.f, 2.f, 42.f};
        Writer w;
        put_chicken(w, shot);
        ChickenEvent decoded;
        Reader full(w.b.data(), w.b.size());
        require(get_chicken(full, decoded) && decoded.shot == 48 &&
                decoded.position == shot.position && decoded.velocity == shot.velocity, "chicken launch round trip");
        Reader truncated(w.b.data(), w.b.size()-1);
        require(!get_chicken(truncated, decoded), "chicken packet rejects missing velocity");
        auto bad_shot = shot;
        bad_shot.velocity[1] = std::numeric_limits<float>::quiet_NaN();
        Writer invalid; put_chicken(invalid, bad_shot);
        Reader bad(invalid.b.data(), invalid.b.size());
        require(!get_chicken(bad, decoded), "chicken packet rejects nonfinite velocity");

        auto prop = shot;
        prop.action = ChickenAction::Prop;
        prop.prop.type = chicken_prop::Type::Panel;
        prop.prop.object = 2; prop.prop.part = 1u << 8; prop.prop.donor_life = 356;
        prop.prop.pose = {1,0,0, 0,1,0, 0,0,1, -12,0.4f,43};
        prop.prop.impulse = {-12500,6250,0};
        prop.prop.point = {-11.7f,0.6f,42.9f};
        Writer contact; put_chicken(contact, prop);
        Reader contact_in(contact.b.data(), contact.b.size());
        require(get_chicken(contact_in, decoded) && decoded.prop.impulse == prop.prop.impulse &&
                decoded.prop.pose == prop.prop.pose && decoded.prop.point == prop.prop.point &&
                decoded.prop.part == prop.prop.part && decoded.prop.donor_life == prop.prop.donor_life,
                "host-selected loose part preserves identity, pose, impulse and contact point");
        ChickenChannel sent_props, received_props;
        auto contact_blast = shot; contact_blast.action = ChickenAction::Blast;
        require(sent_props.queue(contact_blast) && sent_props.queue(prop), "queue blast and its prop contact");
        sent_props.transmit(1, [&](uint32_t serial, const ChickenEvent &event) {
            if (serial == 2) received_props.receive(serial, event); // initial blast datagram lost
        });
        std::vector<ChickenAction> delivered;
        auto accept = [&](const ChickenEvent &event) { delivered.push_back(event.action); return true; };
        received_props.apply(accept);
        require(delivered.empty(), "prop waits behind its missing blast");
        sent_props.transmit(100002, [&](uint32_t serial, const ChickenEvent &event) {
            received_props.receive(serial, event); received_props.receive(serial, event);
        });
        received_props.apply(accept);
        require(delivered == std::vector<ChickenAction>{ChickenAction::Blast, ChickenAction::Prop},
                "retransmitted prop contact applies exactly once following the blast");

        prop.prop.part = 3;
        Writer bad_part; put_chicken(bad_part, prop);
        Reader part_in(bad_part.b.data(), bad_part.b.size());
        require(!get_chicken(part_in, decoded), "two combined panel bits cannot identify a detached part");

        auto blast = shot; blast.action = ChickenAction::Blast;
        auto next = shot; next.shot++;
        ChickenChannel source, sink;
        require(source.queue(shot) && source.queue(blast) && source.queue(next), "queue consecutive shots");
        uint32_t applied = 0;
        source.transmit(1, [&](uint32_t seq, const ChickenEvent &e) {
            if (seq != 1) { sink.receive(seq, e); sink.receive(seq, e); }
        });
        sink.apply([&](const ChickenEvent &) { ++applied; return true; });
        require(!applied, "a reordered blast waits for its dropped launch");
        source.acknowledge(500); // impossible acknowledgement cannot discard the real launch
        uint32_t retries = 0;
        source.transmit(100002, [&](uint32_t seq, const ChickenEvent &e) {
            ++retries; sink.receive(seq, e);
        });
        require(retries == 3, "unacknowledged events retransmitted");
        std::vector<ChickenAction> effects;
        sink.apply([&](const ChickenEvent &e) {
            if (e.action == ChickenAction::Blast) return false; // native seat/life not ready
            effects.push_back(e.action); return true;
        });
        require(sink.received() == 1, "wait for native replay before acknowledging a blast");
        sink.apply([&](const ChickenEvent &e) { effects.push_back(e.action); return true; });
        require(effects == std::vector<ChickenAction>{ChickenAction::Launch, ChickenAction::Blast, ChickenAction::Launch},
                "lost, duplicate, reordered launch and blast replay in native order once");
        source.transmit(200003, [&](uint32_t seq, const ChickenEvent &e) { sink.receive(seq, e); });
        sink.apply([&](const ChickenEvent &) { ++applied; return true; });
        require(!applied, "lost acknowledgement does not double-apply a native effect");
        source.acknowledge(sink.received());
        source.transmit(300004, [&](uint32_t, const ChickenEvent &) { ++applied; });
        require(!applied, "completed stream stops sending");
    }
    {
        using namespace netplay;
        BattleFrame host, guest;
        host.tick = 480;
        host.seats[1].deaths = 1;
        host.seats[1].spawn_tick = 450;
        host.seats[1].spawn.pose[9] = 23;
        host.seats[0].kills = 1;
        Writer w;
        put_battle_frame(w, host, 2);
        Reader full(w.b.data(), w.b.size());
        require(get_battle_frame(full, 2, guest) && guest.seats[0].kills == 1 &&
                guest.seats[1].spawn.pose[9] == 23 && guest.seats[1].deaths == 1, "Battle life round trip");
        Reader short_frame(w.b.data(), w.b.size() - 1);
        require(!get_battle_frame(short_frame, 2, guest) && guest.tick == 480, "truncated Battle spawn preserves last frame");
        host.tick = 449;
        Writer future;
        put_battle_frame(future, host, 2);
        Reader bad(future.b.data(), future.b.size());
        require(!get_battle_frame(bad, 2, guest), "Battle spawn cannot come from a future simulation step");
    }
    {
        using namespace netplay::names;
        const Camera camera = {{2,3,4}, {{1,0,0}, {0,1,0}, {0,0,1}}, 320, -300, 640, 448};
        Tag tag{};
        require(project(camera, {2,3,14}, tag) && tag.x == .5f && tag.y == .5f, "tag at scene optical center");
        require(project(camera, {3,4,14}, tag) && tag.x > .5f && tag.y < .5f, "tag moves right and up");
        require(!project(camera, {2,3,3}, tag) && !project(camera, {100,3,14}, tag), "behind and off-screen tags culled");
        const Tube rail = {{0,0,0}, {0,0,1}, 1, 2};
        require(intersects(rail, {-5,0,0}, {5,0,0}), "tube body hides tag");
        require(intersects(rail, {0,.5f,6}, {0,.5f,2.5f}), "rounded end hides tag along tube axis");
        require(!intersects(rail, {0,1.5f,6}, {0,1.5f,-6}), "sightline clears tube");
        const Box wall = {{0,0,0}, {{0,0,1},{0,1,0},{-1,0,0}}, {2,1,.25f}};
        require(intersects(wall, {-5,0,0}, {5,0,0}), "rotated wall hides tag");
        require(!intersects(wall, {-5,2,0}, {5,2,0}), "tag above wall remains visible");
        const TerrainFace ground = {{{1,0,0}, {0,1,0}, {-1,-1,5}}, {0,1,0}, 0, 0};
        require(intersects(ground, {1,2,1}, {1,-3,1}), "terrain hides tag");
        require(!intersects(ground, {5,2,5}, {5,-3,5}), "outside terrain triangle remains visible");
        require(enabled("No-Rules Race", 0) && !enabled("Demolition Derby", 0), "race and derby name defaults");
        require(!enabled("No-Rules Race", toggle_bit("No-Rules Race")) &&
                enabled("Demolition Derby", toggle_bit("Demolition Derby")), "host can override each mode group");
    }

    // quaternion round trip over many orientations
    for (int i = 0; i < 200; ++i) {
        float yaw = i * 0.05f, c = std::cos(yaw), n = std::sin(yaw), p = std::sin(i * .13f) * .4f, cp = std::cos(p), sp = std::sin(p);
        float m[9] = {c, n * sp, n * cp, 0, cp, -sp, -n, c * sp, c * cp}, q[4], b[9];
        matrix_to_quat(m, q); quat_to_matrix(q, b);
        for (int k = 0; k < 9; ++k) require(std::fabs(m[k] - b[k]) < 1e-4f, "quaternion round trip");
    }
    // wire format
    {
        State a = make(12.5f, -40.25f, 1.1f, 3.f, -4.f); a.ang[1] = .7f; a.damage = .534291f; a.damage_stage = 3;
        netplay::Writer w; netplay::put_car(w, 5, 1234, a);
        netplay::Reader r(w.b.data(), w.b.size()); uint8_t slot; uint32_t tick; State b;
        require(netplay::get_car(r, slot, tick, b) && slot == 5 && tick == 1234, "wire header");
        require(std::fabs(b.pose[9] - 12.5f) < 1e-4f && std::fabs(b.pose[11] + 40.25f) < 1e-4f && std::fabs(b.vel[2] + 4.f) < 1e-4f, "wire position/velocity");
        for (int k = 0; k < 9; ++k) require(std::fabs(a.pose[k] - b.pose[k]) < 2e-3f, "wire orientation");
        require(std::fabs(b.ang[1] - .7f) < 1e-3f && std::fabs(b.steer - .25f) < 1e-3f && b.gear == 3 && b.damage == a.damage && b.damage_stage == 3, "wire controls/damage");
        auto corrupted = w.b;
        corrupted.back() = 255;
        netplay::Reader invalid(corrupted.data(), corrupted.size());
        require(!netplay::get_car(invalid, slot, tick, b), "invalid damage stage rejected");
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
    {
        Remote nearby, distant;
        State now, buffered;
        for (uint32_t tick = 0; tick < 180; ++tick) {
            // At 20 m/s an extra 100 ms of visual buffering moves a
            // solid bumper two metres behind even the last received pose.
            const uint32_t sender_tick = 100 + tick;
            State leader = make(20.f * sender_tick / 60.f, 0, 0, 20, 0);
            State follower = make(leader.pose[9] - 5, 0, 0, 20, 0);
            State observer = make(leader.pose[9] - 100, 0, 0, 20, 0);
            if (!(tick % 2)) {
                State arrived = make(20.f * (sender_tick - 8) / 60.f, 0, 0, 20, 0);
                nearby.push(sender_tick - 8, arrived);
                distant.push(sender_tick - 8, arrived);
            }
            now = nearby.advance(100, nearby.proximity_to(follower), 266);
            buffered = distant.advance(100, distant.proximity_to(observer), 266);
            if (tick > 30) {
                require(std::fabs(now.pose[9] - leader.pose[9]) < .55f, "nearby solid car keeps its bumper near the owner");
                require(buffered.pose[9] + 3 < now.pose[9], "distant cars keep jitter protection");
            }
        }
    }
    {
        Remote short_buffer, long_buffer;
        State recent, delayed;
        for (uint32_t tick = 0; tick < 240; ++tick) {
            if (tick % 2 == 0) {
                const State car = make(15.f * tick / 60.f, 0, 0, 15, 0);
                short_buffer.push(tick, car);
                long_buffer.push(tick, car);
            }
            recent = short_buffer.advance(60);
            delayed = long_buffer.advance(140);
        }
        require(std::fabs((recent.pose[9] - delayed.pose[9]) - 1.2f) < .1f,
                "80 ms of extra buffering at 15 m/s shifts the remote by 1.2 metres");
    }
    {
        Remote remote;
        auto intact = make(0, 0, 0, 10, 0);
        auto hit = intact; hit.damage = .58f; hit.damage_stage = 3;
        remote.push(1, intact); remote.advance();
        remote.push(3, hit); remote.push(2, intact);
        const auto shown = remote.advance();
        require(shown.damage == hit.damage && shown.damage_stage == 3, "damage bypasses pose interpolation and stale snapshots");
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
    // Prolonged loss hands a solid car to native physics until its owner resumes.
    {
        Remote remote;
        State airborne = make(0, 0, 0, 0, 0);
        airborne.pose[10] = 8;
        remote.push(100, airborne);
        for (unsigned i = 0; i < Remote::kStallSteps; ++i) {
            require(!remote.stalled(), "short gaps keep the interpolated pose");
            remote.push(100, airborne);  // the host may forward the last owner snapshot repeatedly
            remote.advance();
        }
        remote.push(99, airborne);
        require(remote.stalled(), "duplicate and reordered packets do not postpone native coasting");
        State landed = airborne;
        landed.pose[10] = .35f;
        remote.follow_native(landed);
        State owner = landed;
        owner.pose[9] = 2.f;
        owner.damage = .7f;
        remote.push(174, owner);
        State resumed = remote.advance();
        require(!remote.stalled() && std::fabs(resumed.pose[10] - .35f) < .001f &&
                resumed.pose[9] > 0 && resumed.pose[9] < 2 && resumed.damage == owner.damage,
                "fresh owner state blends from the native landing and immediately restores confirmed damage");
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
        release_controls(car);
        require(getf(car + kGas) == 0 && getf(car + kSteer) == 0 &&
                ee_r32(car + kGear) == static_cast<uint32_t>(s.gear), "stalled owner coasts in its current gear");
        require(std::fabs(getf(body) - 50) < 1e-4f && std::fabs(getf(body + 8) + 20) < 1e-4f && std::fabs(getf(body + 0x50) - 1) < 1e-4f, "body updated");
        require(std::fabs(getf(car + 0x88) - 50) < 1e-4f && std::fabs(getf(car + 0x104) - 1) < 1e-4f, "car copies updated");
        s.damage_stage = 3; write_condition(car, s);
        require(getf(car + kDamage) == s.damage && ee_r32(car + 0x1BE8) == 3, "remote native health and impairment");
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
