// Online launchers are ordinary native scene objects. The host simulates contacts.
// Replicas play the native flight, particles, sound and retirement once per host event.
#include "battle_driver.h"
#include "chicken_prop.h"
#include "chicken_weapon.h"
#include "decomp_chicken_weapon.h"
#include "netplay_hooks.h"
#include "core/cpu.h"
#include "funcs.h"

namespace {
using namespace chicken_weapon;
constexpr uint32_t kScratchSize = 128;

// Network callbacks run between guest physics steps, with the current EE
// stack still live. Reserve input arguments above the native callee's frame.
bool usable_stack() {
    const uint32_t sp = g_ee.r[29].uw[0];
    return sp >= 0x1000 && !(sp & 15) && valid(sp - kScratchSize, kScratchSize);
}
struct NativeCall {
    EEState saved = g_ee;
    uint8_t original[kScratchSize];
    uint32_t scratch = g_ee.r[29].uw[0] - kScratchSize;
    NativeCall() {
        ee_read_block(scratch, original, sizeof(original));
        g_ee.r[29].ud[0] = scratch;
    }
    ~NativeCall() {
        ee_write_block(scratch, original, sizeof(original));
        g_ee = saved;
    }
    void set_position(uint32_t body, const float pos[3]) {
        ee_write_block(scratch + 16, pos, 12);
        g_ee.r[4].ud[0] = body;
        g_ee.r[5].ud[0] = scratch + 16;
        f_0017AF58(); // update the object's pose and spatial bounds
    }
    void trigger(uint32_t iface, uint32_t car, bool held) {
        ee_w32(scratch + 16, held);
        g_ee.r[4].ud[0] = iface;
        g_ee.r[5].ud[0] = car;
        g_ee.r[6].ud[0] = scratch + 16;
        chicken_launch_native(); // already routed by the session
    }
};
}

bool battle_chicken_replay_launch(uint32_t slot, float pos[3], float vel[3]) {
    using namespace chicken_weapon;
    if (!usable_stack() || !finite3(pos) || !finite3(vel) || slot >= 16) return false;
    const uint32_t iface = interface_for(slot), car = ee_r32(0x447458 + slot * 4);
    if (!iface || !valid(car, 0x1C34) || (ee_r32(0x3C4190) & 9) != 9) return false;
    const uint32_t body = iface - kInterface;
    const int authority = netplay_hooks::chicken_authority ? netplay_hooks::chicken_authority() : -1;
    if (authority < 0) return false;

    NativeCall call;
    if (authority == 1) {
        // This is the owner's request. The host owns the car used to aim and
        // launch and will distribute the resulting native muzzle and speed.
        if (!ee_r32(iface + 4)) return false; // native update owns cooldown
        call.trigger(iface, car, false);
        if (ee_r32(iface + 4) != 2) return false;
    } else {
        // A confirmed launch must remain visible even if local clocks disagree.
        ee_w32(iface + 4, 2);
    }
    call.trigger(iface, car, true);
    if (!ee_r32(body + 0x1F4)) return false;
    if (authority == 1) {
        read3(body + 0x88, pos);
        read3(body + 0x104, vel);
    } else {
        call.set_position(body, pos);
        ee_write_block(body + 0x104, vel, 12);
        if (ee_r32(body + 0x210)) ee_write_block(body + 0x1F8, pos, 12); // launch effect origin
    }
    trace("replay_launch", slot, pos);
    return true;
}

extern "C" void f_0017E718(void) {
    if (netplay_hooks::chicken_authority && netplay_hooks::chicken_authority() == 0) {
        // Wait for the host contact position before retiring this projectile
        // or emitting its effects. Its ordinary base collision still runs.
        f_0017D338();
        return;
    }
    chicken_impact_native();
}

bool battle_chicken_replay_blast(uint32_t slot, const float pos[3]) {
    using namespace chicken_weapon;
    if (!usable_stack() || !finite3(pos) || !netplay_hooks::chicken_authority ||
        netplay_hooks::chicken_authority() != 0) return false;
    const uint32_t iface = interface_for(slot);
    if (!iface) return false;
    const uint32_t body = iface - kInterface;
    if (!ee_r32(body + 0x1F0)) return false;
    NativeCall call;
    call.set_position(body, pos);
    // Collision class 0x70 takes the regular straight-chicken impact path.
    // Its final base-contact call gets a zero impulse: only the host integrates recoil.
    ee_w32(body + 0x1F4, 1);
    ee_w32(call.scratch + 32, 0);
    ee_w32(call.scratch + 36, 0);
    ee_w32(call.scratch + 40, 0);
    g_ee.r[4].ud[0] = body;
    g_ee.r[5].ud[0] = call.scratch + 32;
    g_ee.r[6].ud[0] = call.scratch + 16;
    g_ee.r[7].ud[0] = 0x70;
    g_ee.r[8].ud[0] = 0;
    chicken_impact_native();
    trace("replay_blast", slot, pos);
    return !ee_r32(body + 0x1F4);
}

void battle_chicken_cancel(uint32_t slot) {
    const uint32_t iface = chicken_weapon::interface_for(slot);
    if (!iface || !usable_stack()) return;
    const uint32_t body = iface - chicken_weapon::kInterface;
    if (!ee_r32(body + 0x1F4)) return;
    NativeCall call;
    g_ee.r[4].ud[0] = body;
    f_0017E8C0(); // native silent retirement, hide the scene and restart its reload
}

namespace chicken_prop {
namespace {
constexpr uint32_t kWobble = 0x3FC6C8, kPanel = 0x400EE0, kWheel = 0x3FCE48;
constexpr uint32_t kPanelPool = 0x494D60, kWheelPool = 0x484E68;
uint32_t participant(uint32_t slot) {
    const uint32_t count = ee_r32(0x48522C);
    if (count > 16 || slot >= count) return 0;
    const uint32_t car = ee_r32(0x447458 + slot * 4);
    return chicken_weapon::valid(car, 0x1C34) ? car : 0;
}

// Map track objects by their order among placements of the same native class.
// Debris pools recycle indices on each machine. Match the donor car and part.
uint32_t wobble(uint32_t body, uint16_t &ordinal) {
    const uint32_t count = ee_r32(0x447408), list = ee_r32(0x447404);
    if (count > 512 || !chicken_weapon::valid(list, count * 4)) return 0;
    uint16_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t obj = ee_r32(list + i * 4);
        if (!chicken_weapon::valid(obj, 0x218) || ee_r32(obj + 0x98) != kWobble) continue;
        if ((body && obj == body) || (!body && n == ordinal)) { ordinal = n; return obj; }
        ++n;
    }
    return 0;
}
bool is_part(uint32_t obj, uint32_t car, const Hit &hit) {
    const bool wheel = hit.type == Type::Wheel;
    return chicken_weapon::valid(obj, wheel ? 0x1EC : 0x264) &&
           ee_r32(obj + 0x98) == (wheel ? kWheel : kPanel) &&
           ee_r32(obj + 0x1DC) && ee_r32(obj + 0x1E4) == hit.part &&
           (wheel ? ee_r32(obj + 0x1E0) == ee_r32(car + 0x1DC) : ee_r32(obj + 0x1DC) == car);
}
uint32_t find_body(const Hit &hit) {
    if (hit.type == Type::Wobble) {
        uint16_t ordinal = hit.object;
        return wobble(0, ordinal);
    }
    const uint32_t car = participant(hit.object);
    if (!car) return 0;
    const uint32_t pool = hit.type == Type::Wheel ? kWheelPool : kPanelPool;
    const uint32_t count = ee_r32(pool + 0x40);
    if (count > 16) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t obj = ee_r32(pool + i * 4);
        if (is_part(obj, car, hit)) return obj;
    }
    return 0;
}
}

bool capture(uint32_t body, uint32_t point, uint32_t impulse, Hit &hit) {
    using namespace chicken_weapon;
    if (!valid(body, 0x1E8) || !valid(point, 12) || !valid(impulse, 12)) return false;
    const uint32_t vt = ee_r32(body + 0x98);
    if (vt == kWobble) {
        hit.type = Type::Wobble;
        if (!wobble(body, hit.object)) return false;
    } else if (vt == kPanel || vt == kWheel) {
        hit.type = vt == kWheel ? Type::Wheel : Type::Panel;
        hit.part = ee_r32(body + 0x1E4);
        for (; hit.object < 16; ++hit.object) {
            const uint32_t car = participant(hit.object);
            if (!car) return false;
            if (is_part(body, car, hit)) break;
        }
        if (hit.object == 16) return false;
    } else return false;
    ee_read_block(body + 0x64, hit.pose.data(), 48);
    read3(body + 0x104, hit.linear.data());
    read3(body + 0x110, hit.angular.data());
    read3(point, hit.point.data());
    read3(impulse, hit.impulse.data());
    return valid_hit(hit);
}

bool apply(uint32_t shooter, const Hit &hit) {
    using namespace chicken_weapon;
    const uint32_t car = participant(shooter), body = find_body(hit);
    if (!car || !body || !usable_stack()) return false;
    NativeCall call;
    ee_write_block(call.scratch + 16, hit.pose.data(), 48);
    g_ee.r[4].ud[0] = body;
    g_ee.r[5].ud[0] = call.scratch + 16;
    f_0017AF80();  // host position/orientation, including bounds registration
    ee_write_block(body + 0x104, hit.linear.data(), 12);
    ee_write_block(body + 0x110, hit.angular.data(), 12);
    ee_write_block(call.scratch + 64, hit.impulse.data(), 12);
    ee_write_block(call.scratch + 80, hit.point.data(), 12);
    g_ee.r[4].ud[0] = body;
    g_ee.r[5].ud[0] = 0;
    g_ee.r[6].ud[0] = call.scratch + 64;
    g_ee.r[7].ud[0] = call.scratch + 80;
    g_ee.r[8].ud[0] = 0x70;
    g_ee.r[9].ud[0] = 0;
    g_ee.r[10].ud[0] = car;
    g_ee.fpr[12].f = 1.f;
    f_0017B918();  // record and integrate the exact native contact once
    return true;
}
}
