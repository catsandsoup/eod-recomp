// Synthetic fixtures exercise locally generated native routines, extracted only
// into the build directory. No retail source or data belongs in this file.
#include "ee.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
EEState g_ee;
uint8_t *g_ram;
volatile int g_ee_irq_pending;
#ifdef EE_TRACE_CALLS
uint32_t g_ee_trace[256], g_ee_trace_pos;
#endif
void reference_0017E000();
void reference_0017E4D8();
void reference_0017E5C8();
void reference_0017E718();
}

namespace {
constexpr uint32_t body = 0x10000, car = 0x20000, input = 0x30000;
constexpr uint32_t effect1 = 0x31000, effect2 = 0x32000;
constexpr uint32_t flags = 0x3c4190, clockAddress = 0x3cdacc, stack = 0x100000;
using Vec = std::array<float, 3>;
std::vector<uint8_t> ram(EE_RAM_SIZE);
const char *scenario;
int checks;

void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "%s: %s\n", scenario, message); std::exit(1); }
}
void putFloat(uint32_t address, float value) { std::memcpy(g_ram + address, &value, 4); }
float getFloat(uint32_t address) { float value; std::memcpy(&value, g_ram + address, 4); return value; }
void putVec(uint32_t address, Vec value) { std::memcpy(g_ram + address, value.data(), 12); }
void expectVec(uint32_t address, Vec expected, const char *message) {
    for (unsigned i = 0; i < 3; ++i) {
        float actual = getFloat(address + 4 * i);
        if (!std::isfinite(actual) || std::fabs(actual - expected[i]) > 0.0001f) {
            std::fprintf(stderr, "%s: %s[%u]: actual %.9g expected %.9g\n", scenario, message, i, actual, expected[i]);
            std::exit(1);
        }
        ++checks;
    }
}
void reset(const char *name, uint32_t enabled = 0x9) {
    scenario = name;
    std::fill(ram.begin(), ram.end(), 0);
    g_ram = ram.data(); g_ee = {};
    R[29].ud[0] = stack;
    MEM_SW(flags, enabled);
    putFloat(clockAddress, 10.0f);
    MEM_SW(body + 0x1e0, 2); // native armed state
    MEM_SW(body + 0x1e8, 7); // registered owner remains unchanged
    MEM_SH(car + 0x94, 0x1234);
    putVec(car + 0x34, {1, 0, 0}); // right
    putVec(car + 0x40, {0, 1, 0}); // up
    putVec(car + 0x4c, {0, 0, 1}); // forward
    putVec(car + 0x58, {10, 20, 30}); // world position
    putVec(body + 0x21c, {2, 3, 4}); // local muzzle offset
}
void trigger(bool pressed) {
    MEM_SW(input, pressed);
    R[4].ud[0] = body + 0x1dc; // weapon interface, not body/update address
    R[5].ud[0] = car;
    R[6].ud[0] = input;
    reference_0017E000();
    require(R[29].uw[0] == stack, "trigger must restore stack");
}
void update(float time) {
    putFloat(clockAddress, time);
    R[4].ud[0] = body;
    reference_0017E5C8();
    require(R[29].uw[0] == stack, "update must restore stack");
}

void battleAnglesAndSpeed() {
    // A mortar branch, fixed world axis, omitted car speed, or vector addition
    // instead of native speed magnitude each fails one of these literal cases.
    struct Case { const char *name; Vec right, up, forward, velocity, shot, muzzle; };
    const Case cases[] = {
        {"stationary forward", {1,0,0}, {0,1,0}, {0,0,1}, {0,0,0}, {0,0,28}, {12,23,34}},
        {"sideways velocity magnitude", {1,0,0}, {0,1,0}, {0,0,1}, {3,4,12}, {0,0,41}, {12,23,34}},
        {"reverse speed magnitude", {1,0,0}, {0,1,0}, {0,0,1}, {0,0,-12}, {0,0,40}, {12,23,34}},
        {"quarter turn", {0,0,-1}, {0,1,0}, {1,0,0}, {3,4,0}, {33,0,0}, {14,23,28}},
        {"oblique yaw", {.8f,0,-.6f}, {0,1,0}, {.6f,0,.8f}, {0,0,0}, {16.8f,0,22.4f}, {14,23,32}},
        {"pitched car", {1,0,0}, {0,.8f,-.6f}, {0,.6f,.8f}, {0,0,12}, {0,24,32}, {12,24.8f,31.4f}},
    };
    for (const auto &c : cases) {
        reset(c.name);
        putVec(car + 0x34, c.right); putVec(car + 0x40, c.up); putVec(car + 0x4c, c.forward);
        putVec(car + 0x104, c.velocity);
        trigger(true);
        expectVec(body + 0x104, c.shot, "Battle launch velocity");
        expectVec(body + 0x88, c.muzzle, "transformed muzzle position");
        require(MEM_LW(body + 0x1f4) == 1, "shot must be in flight");
        require(MEM_LW(body + 0x1e0) == 0, "shot must enter cooldown");
        require(getFloat(body + 0x1e4) == 13, "shot deadline must be now plus three seconds");
        require(MEM_LH(body + 0x94) == 0x1234, "shot must copy car identifier");
        require(MEM_LW(body + 0x1e8) == 7, "shot must retain registered owner");
    }
}

void featureFlags() {
    reset("mortar enabled", 0x19);
    putVec(car + 0x104, {3,4,12});
    trigger(true);
    // Hand-checked native feat constants: (forward*.707 + up*.4949)*24.888889,
    // then add the car velocity vector. Deliberately distinct from Battle.
    expectVec(body + 0x104, {3,16.317511f,29.596445f}, "mortar launch velocity");
    require(MEM_LW(body + 0x1e0) == 0, "mortar with weapon flag must cool down");
    reset("rain precedes mortar", 0x99);
    trigger(true);
    expectVec(body + 0x104, {0,-19.62f,0}, "rain velocity");
    reset("master disabled ignores mortar", 0x18);
    trigger(true);
    expectVec(body + 0x104, {0,0,28}, "disabled cheat leaves native straight launch");
    require(MEM_LW(body + 0x1e0) == 2, "disabled master must not arm cheat cooldown");

    for (uint32_t enabled : {0u, 1u, 8u, 9u, 0x19u}) {
        reset("cooldown requires master and weapon", enabled);
        putFloat(body + 0x1e4, 91);
        R[4].ud[0] = body;
        reference_0017E4D8();
        const bool active = enabled == 9 || enabled == 0x19;
        require(MEM_LW(body + 0x1e0) == (active ? 0u : 2u), "cooldown flag gate");
        require(getFloat(body + 0x1e4) == (active ? 13 : 91), "cooldown deadline flag gate");
    }
}

void releaseRepressAfterCooldown() {
    reset("native cooldown release/repress");
    trigger(false);
    require(MEM_LW(body + 0x1f4) == 0, "released armed trigger must not fire");
    trigger(true);
    require(MEM_LW(body + 0x1f4) == 1, "first press must fire");
    putVec(body + 0x104, {91,92,93});
    trigger(false); trigger(true);
    expectVec(body + 0x104, {91,92,93}, "presses during cooldown must not refire");
    update(12.999f);
    require(MEM_LW(body + 0x1e0) == 0, "before deadline remains cooldown");
    update(13);
    require(MEM_LW(body + 0x1e0) == 0, "deadline equality remains cooldown");
    update(13.001f);
    require(MEM_LW(body + 0x1e0) == 1, "expired deadline waits for release");
    trigger(true);
    expectVec(body + 0x104, {91,92,93}, "holding after expiry must not refire");
    require(MEM_LW(body + 0x1e0) == 1, "held input stays release-waiting");
    trigger(false);
    require(MEM_LW(body + 0x1e0) == 2, "release rearms");
    expectVec(body + 0x104, {91,92,93}, "release must not fire");
    trigger(true);
    expectVec(body + 0x104, {0,0,28}, "new press fires again");
    require(MEM_LW(body + 0x1e0) == 0, "second shot restarts cooldown");
    require(std::fabs(getFloat(body + 0x1e4) - 16.001f) < .0001f, "second deadline uses current native clock");
}

void effects() {
    reset("native effect activation");
    MEM_SW(body + 0x210, effect1); MEM_SW(body + 0x214, effect2);
    trigger(true);
    expectVec(body + 0x1f8, {12,23,34}, "effect origin follows transformed muzzle");
    for (uint32_t effect : {effect1, effect2}) {
        require(getFloat(effect + 4) == 1, "effect activates");
        require(MEM_LW(effect + 84) == 1 && MEM_LW(effect + 100) == 1, "effect dirty flags set");
        require(MEM_LW(effect + 88) == 0, "effect timer reset");
    }
    require(MEM_LW(effect1 + 92) == 0 && MEM_LW(effect2 + 92) == 1, "effect modes retained");
}

// Feed the native collision callback a fixed world query. The real guest code
// decides which bodies qualify, writes each contact, and accumulates the impulse.
constexpr uint32_t candidates = 0x40000, nodes = 0x41000, classTable = 0x42000;
constexpr uint32_t contactPool = 0x50000, eligibilityMethod = 0x12340;
constexpr std::array<uint32_t, 7> targets = {car, 0x60000, 0x64000, 0x68000, 0x6c000, 0x70000, 0x74000};
int blastEvents;

void contact(uint32_t surface) {
    R[4].ud[0] = body;
    R[5].ud[0] = R[6].ud[0] = R[8].ud[0] = 0;
    R[7].ud[0] = surface;
    reference_0017E718();
    require(R[29].uw[0] == stack, "collision callback must restore stack");
}

void radialContact() {
    reset("native detonation and radial contact");
    blastEvents = 0;
    trigger(true);
    putVec(body + 0x88, {0,0,0}); // deterministic impact point after launch
    MEM_SW(body + 0x1f0, 1);      // collision processing enabled
    MEM_SW(0x3ce3e8, targets.size());
    MEM_SW(0x3ce3f0, contactPool);
    MEM_SW(candidates, nodes);
    MEM_SW(classTable + 60, eligibilityMethod);
    const Vec positions[] = {{1.875f,0,0}, {-3.75f,0,0}, {0,0,5.625f}, {7.5f,0,0},
                             {-8,0,0}, {0,0,0}, {1,0,0}};
    for (size_t i = 0; i < targets.size(); ++i) {
        const uint32_t node = nodes + uint32_t(i) * 8;
        MEM_SW(node, targets[i]);
        MEM_SW(node + 4, i + 1 == targets.size() ? 0 : node + 8);
        MEM_SW(targets[i] + 0x98, classTable);
        putVec(targets[i] + 0x88, positions[i]);
    }
    contact(10);
    require(blastEvents == 0 && MEM_LW(body + 0x1f4) == 1, "ineligible contact keeps the shot in flight");
    contact(112);
    require(blastEvents == 1 && MEM_LW(body + 0x1f4) == 0, "eligible contact detonates the shot");
    require(MEM_LW(0x3ce3ec) == 3, "exactly three damage contacts, including the shooter");
    const Vec expected[] = {{18750,9375,0}, {-12500,6250,0}, {0,3125,6250}};
    for (size_t i = 0; i < targets.size(); ++i) {
        if (i < 3) {
            require(MEM_LW(targets[i] + 0x1d0) == 1, "one contact per affected body");
            const uint32_t hit = MEM_LW(targets[i] + 0x190);
            require(MEM_LW(hit + 32) == car, "damage contact records the original shooter");
            require(MEM_LW(hit + 28) == 0x70, "damage contact records the native surface type");
            expectVec(hit + 4, expected[i], "falloff and upward component at contact");
            expectVec(targets[i] + 0x174, expected[i], "accumulated linear impulse");
        } else {
            require(MEM_LW(targets[i] + 0x1d0) == 0, "boundary, outside, coincident, or ineligible body excluded");
            expectVec(targets[i] + 0x174, {0,0,0}, "excluded body gets no impulse");
        }
    }
    contact(112);
    require(blastEvents == 1 && MEM_LW(0x3ce3ec) == 3, "second contact cannot detonate the same shot twice");
}
} // namespace

extern "C" void f_00179A08() {
    require(R[4].uw[0] == 7, "blast resolves the launcher's registered owner");
    R[2].ud[0] = car;
}
extern "C" void f_001D57A8() {
    MEM_SW(R[4].uw[0], candidates);
    MEM_SW(R[5].uw[0], 1);
}
extern "C" void ee_call(uint32_t address) {
    require(address == eligibilityMethod, "expected shockwave eligibility predicate");
    R[2].ud[0] = R[4].uw[0] != targets.back();
}
extern "C" void f_0012F3C8() { FPR[0].f = 0; } // centre the random contact-point offset
extern "C" void f_00177348() { ++blastEvents; }
extern "C" void f_0017D338() {} // enclosing contact callback
extern "C" void f_00212600() { require(false, "unexpected alternate-mode blast"); }

// Native pose copy is retained; stop at scene/bounds propagation and per-frame
// physics/render work. None of these stubs changes weapon state or velocity.
extern "C" void f_0017A498() {}
extern "C" void f_0017F4E0() {}
extern "C" void f_0017BBC0() {}
// These branches are outside the synthetic update fixtures and must not silently
// succeed if a future change accidentally takes them.
extern "C" void f_0017F530() { require(false, "unexpected owner callback"); }
extern "C" void f_00178DB0() { require(false, "unexpected owner lookup"); }
extern "C" void f_0017AF58() { require(false, "unexpected hidden pose update"); }
extern "C" void f_0017EC58() { require(false, "unexpected rain spawn"); }
extern "C" void ee_service_interrupts() { require(false, "unexpected IRQ"); }
extern "C" uint16_t ee_read16_slow(uint32_t) { require(false, "unexpected slow read16"); return 0; }
extern "C" uint32_t ee_read32_slow(uint32_t) { require(false, "unexpected slow read32"); return 0; }
extern "C" uint64_t ee_read64_slow(uint32_t) { require(false, "unexpected slow read64"); return 0; }
extern "C" void ee_write16_slow(uint32_t, uint16_t) { require(false, "unexpected slow write16"); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { require(false, "unexpected slow write32"); }
extern "C" void ee_write64_slow(uint32_t, uint64_t) { require(false, "unexpected slow write64"); }

int main() {
    battleAnglesAndSpeed(); featureFlags(); releaseRepressAfterCooldown(); effects(); radialContact();
    std::printf("Native chicken weapon: %d checks passed (launch, cooldown, contact, attribution, radial impulse)\n", checks);
}
