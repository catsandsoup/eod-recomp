// Snapshot of the local records and live return state of 0x1DEA4C.
// Goldens were calculated by running the retail microprogram in the reference runtime.
#include "vehicle_dynamics.h"
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>

static uint64_t hash(uint64_t h, const void* data, size_t length) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
static uint64_t digest(const VUState& v) {
    uint64_t h = hash(1469598103934665603ull, v.mem, 4096);
    h = hash(h, v.vf, sizeof v.vf);
    h = hash(h, v.vi, sizeof v.vi);
    h = hash(h, &v.acc, sizeof v.acc);
    h = hash(h, &v.q, sizeof v.q);
    h = hash(h, &v.i, sizeof v.i);
    h = hash(h, &v.status, sizeof v.status);
    h = hash(h, &v.mac, sizeof v.mac);
    h = hash(h, &v.cycle, sizeof v.cycle);
    h = hash(h, &v.q_ready, sizeof v.q_ready);
    h = hash(h, &v.tpc, sizeof v.tpc);
    h = hash(h, v.fq, sizeof v.fq);
    return h;
}
static uint32_t next(uint32_t& seed) {
    seed = seed * 1664525u + 1013904223u;
    return seed;
}
static void seed_fixture(VUState& v, unsigned profile) {
    uint32_t seed = 0x502bc231u + profile;
    std::array<VUReg, 256> data{};
    for (auto& row : data)
        for (unsigned lane = 0; lane < 4; ++lane) {
            const uint32_t u = next(seed);
            switch (profile) {
            case 0:
                row.u[lane] = 0;
                break;
            case 1:
                row.u[lane] = 0x80000000u;
                break;
            case 2:
                row.u[lane] = u;
                break;
            case 3:
                row.u[lane] = (u & 0x807fffffu);
                break;
            default:
                row.f[lane] = float(int32_t(u)) / 134217728.f;
                break;
            }
        }
    memcpy(v.mem, data.data(), sizeof data);
    for (unsigned i = 1; i < 32; ++i)
        for (unsigned j = 0; j < 4; ++j)
            v.vf[i].u[j] = next(seed);
    v.vf[0].f[3] = 1.f;
    for (unsigned i = 4; i < 32; ++i)
        v.vi[i] = next(seed) & 0xffff;
    v.vi[1] = 0;
    v.vi[2] = 11;
    v.vi[3] = 42;
    v.fq_head = profile & 7;
    v.status = next(seed);
    v.cycle = next(seed);
}
int main() {
    constexpr uint64_t expected[] = {
        0xcc66dc06217d48d2ull, 0x0d07819b711b63bdull, 0x776d9cfe873a0d95ull, 0xff0b435003e686b1ull,
        0x690f39901487536dull, 0x7c8debce5b0814f2ull, 0x6ffddf706a5ed0beull, 0x4f0841537e846f85ull,

    };
    for (unsigned profile = 0; profile < sizeof expected / sizeof expected[0]; ++profile) {
        alignas(16) std::array<uint8_t, 4096> local{};
        VUState state{};
        state.mem = local.data();
        seed_fixture(state, profile);
        vehicle_dynamics_step(state);
        const uint64_t actual = digest(state);
        if (actual != expected[profile]) {
            std::fprintf(stderr, "vehicle dynamics fixture %u: expected %016llx, got %016llx\n", profile,
                         (unsigned long long)expected[profile], (unsigned long long)actual);
            return 1;
        }
    }
    return 0;
}
