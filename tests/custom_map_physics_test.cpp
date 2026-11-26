// Exercise the same HLE entry points as the generated car producer. The two
// original guest routines supply a retail terrain plane and one integrated pose.
#include "../src/battle_layout.h"
#include "../src/netplay_hooks.h"
#include "core/memory.h"
#include "vfs/custom_map.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using P = eod::physics::MapPoint;
using eod::physics::MapContact;
extern "C" void f_00212500();
extern "C" void f_001DE7B0();
extern "C" void f_00176798();
extern "C" void f_00176798_orig() {
    g_ee.r[2].sd[0] = 2;
}
EEState g_ee{};
uint8_t* g_ram = nullptr;
extern "C" uint32_t ee_read32_slow(uint32_t) {
    std::abort();
}
extern "C" uint8_t ee_read8_slow(uint32_t) {
    std::abort();
}
extern "C" void ee_write32_slow(uint32_t, uint32_t) {
    std::abort();
}
extern "C" void ee_write8_slow(uint32_t, uint8_t) {
    std::abort();
}
extern "C" void* ee_ptr(uint32_t ptr) {
    return g_ram + ptr;
}

namespace {
int failures;
#define CHECK(expr)                                                                                          \
    do {                                                                                                     \
        if (!(expr)) {                                                                                       \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #expr);                                          \
            ++failures;                                                                                      \
        }                                                                                                    \
    } while (0)
const uint32_t body = 0x500000, wheel = 0x510000, frozen = 0x520000, car = 0x530000, model = 0x540000;
const uint32_t sample = 0x550000, normal = 0x550010, material = 0x550020;
float base_height = 0;
bool base_hit = true;
P probe{}, destination{}, speed{};
void store(uint32_t addr, P p) {
    ee_write_block(addr, p.data(), sizeof p);
}
P load(uint32_t addr) {
    P p;
    ee_read_block(addr, p.data(), sizeof p);
    return p;
}
bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}
} // namespace
extern "C" void f_00212500_orig() {
    P p = load(sample);
    if (base_hit) {
        p[1] = base_height;
        store(sample, p);
        store(normal, {0, 1, 0});
        ee_w32(material, 2);
    }
    g_ee.r[2].sd[0] = base_hit;
}
extern "C" void f_001DE7B0_orig() {
    store(sample, probe);
    g_ee.r[5].uw[0] = sample;
    g_ee.r[6].uw[0] = normal;
    g_ee.r[7].uw[0] = material;
    f_00212500();
    store(body, destination);
    store(body + 0x50, speed);
    store(body + 0x60, {});
    store(wheel + 0x40, destination);
}

int main() {
    auto map = std::make_shared<eod::vfs::CustomMap>();
    auto tri = [&](P a, P b, P c) {
        for (P p : {a, b, c}) {
            map->indices.push_back(static_cast<uint32_t>(map->vertices.size()));
            map->vertices.push_back(p);
        }
    };
    // Elevated double-sided deck, vertical wall and a slope at another X.
    tri({-8, 3, -8}, {8, 3, -8}, {8, 3, 8});
    tri({-8, 3, -8}, {8, 3, 8}, {-8, 3, 8});
    tri({3, -8, -8}, {3, 8, -8}, {3, 8, 8});
    tri({3, -8, -8}, {3, 8, 8}, {3, -8, 8});
    tri({10, 1, 0}, {10, 1, 4}, {14, 3, 0});
    // 33k triangles exercise the 32-bit BVH above SCOL's signed-index limit.
    for (int i = 0; i < 33000; ++i) {
        const float x = 20.f + i;
        tri({x, 1, 0}, {x, 1, 1}, {x + 1, 1, 0});
    }
    auto& mesh = map->collision;
    CHECK(mesh.build(map->vertices, map->indices));
    MapContact hit;
    CHECK(mesh.ground({33019.25f, 2, .25f}, 0, hit));
    CHECK(mesh.ground({11, 3, 1}, 0, hit));
    CHECK(near(hit.point[1], 1.5f) && near(hit.normal[1], 1 / std::sqrt(1.25f)));
    CHECK(!mesh.ground({13.5f, 3, 2}, 0, hit)); // inside AABB but outside triangle
    CHECK(!mesh.ground({0, 2, 0}, -1, hit));    // deck is overhead
    CHECK(!mesh.build({{0, 0, 0}}, {0, 0, 3}));
    CHECK(!mesh.build({{std::numeric_limits<float>::infinity(), 0, 0}}, {0, 0, 0}));
    CHECK(mesh.ground({0, 4, 0}, 0, hit)); // rejected rebuild preserved valid tree
    const std::array<P, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    CHECK(mesh.sweep_box({0, 0, 0}, {6, 0, 0}, axes, {1, .5, 1}, hit));
    CHECK(near(hit.fraction, 1.f / 3) && near(hit.normal[0], -1));
    CHECK(mesh.sweep_box({6, 0, 0}, {0, 0, 0}, axes, {1, .5, 1}, hit));
    CHECK(near(hit.normal[0], 1));
    CHECK(mesh.sweep_box({0, 0, 0}, {0, 5, 0}, axes, {1, .5, 1}, hit));
    CHECK(near(hit.normal[1], -1) && near(hit.fraction, .5f));
    CHECK(!mesh.sweep_box({0, 0, 12}, {6, 0, 12}, axes, {1, .5, 1}, hit));

    g_ram = static_cast<uint8_t*>(std::calloc(1, EE_RAM_SIZE));
    if (!g_ram) return 1;
    ee_w32(0x494D28, 1);
    ee_w32(0x494D30, body);
    ee_w32(0x494D34, wheel);
    ee_w32(0x494D3C, frozen);
    ee_w32(0x3CE548, 1);
    ee_w32(0x3CE4F8, car);
    ee_w32(car + 0x25C, model);
    ee_w32(model + 0x1C, 2);
    store(model + 0x3C, {-1, -.5, -1});
    store(model + 0x3C + 0x34, {1, .5, 1});
    for (int d = 0; d < 3; ++d)
        store(body + 0x10 * (1 + d), axes[d]);
    const float inverse[4] = {1, 1, 1, 1};
    ee_write_block(body + 0x80, inverse, sizeof inverse);
    auto step = [](P start, P end, P velocity, P hub) {
        store(body, start);
        destination = end;
        speed = velocity;
        probe = hub;
        f_001DE7B0();
    };
    auto& vfs = eod::vfs::global_vfs();
    CHECK(vfs.mount(eod::vfs::Layer::EphemeralPie, ".", "", map));
    step({0, 0, 0}, {6, 0, 1}, {30, 0, 5}, {0, 3.3f, 0});
    CHECK(near(load(sample)[1], 3) && ee_r32(material) == 6);
    CHECK(near(load(body)[0], 1.999f) && near(load(body)[2], 1));
    CHECK(near(load(body + 0x50)[0], 0) && near(load(body + 0x50)[2], 5));
    CHECK(near(load(wheel + 0x40)[0], load(body)[0]));
    step({0, 0, 0}, {0, 0, 0}, {}, {0, 1, 0});
    CHECK(near(load(sample)[1], 0) && ee_r32(material) == 2);
    base_height = 4;
    step({0, 0, 0}, {0, 0, 0}, {}, {0, 4.1f, 0});
    CHECK(near(load(sample)[1], 4) && ee_r32(material) == 2);
    base_hit = false;
    step({0, 0, 0}, {0, 5, 0}, {0, 30, 0}, {11, 2, 1});
    CHECK(near(load(body)[1], 2.499f) && near(load(body + 0x50)[1], 0));
    CHECK(near(load(sample)[1], 1.5f) && near(load(normal)[0], -.5f / std::sqrt(1.25f)));
    ee_w32(frozen, 1);
    step({0, 0, 0}, {6, 0, 0}, {30, 0, 0}, {30, 10, 10});
    CHECK(near(load(body)[0], 6) && g_ee.r[2].ud[0] == 0);
    ee_w32(frozen, 0);
    base_hit = true;
    base_height = 0;
    // Only the native controller release call for one offline editor driver
    // is eligible. The underlying mode return value must still be 2.
#ifdef _WIN32
    _putenv_s("EOD_EDITOR_MAP_TEST", "1");
#else
    setenv("EOD_EDITOR_MAP_TEST", "1", 1);
#endif
    constexpr uint32_t master = 0x580000;
    ee_w32(0x3CF194, master);
    ee_w32(master, battle_layout::kMasterVtable);
    ee_w32(master + battle_layout::kCount, 1);
    ee_w32(0x3C41AC, 1);
    ee_w32(0x3CDA8C, 3);
    ee_w32(0x4850A8, 179);
    g_ee.r[31].uw[0] = 0;
    f_00176798();
    CHECK(ee_r32(0x4850A8) == 179);
    g_ee.r[31].uw[0] = 0x001B8DA8;
    netplay_hooks::active = [] { return true; };
    f_00176798();
    CHECK(ee_r32(0x4850A8) == 179);
    netplay_hooks::active = nullptr;
    f_00176798();
    CHECK(ee_r32(0x4850A8) == 3 && g_ee.r[2].sw[0] == 2);
    vfs.unmount(eod::vfs::Layer::EphemeralPie);
    step({0, 0, 0}, {6, 0, 0}, {30, 0, 0}, {0, 3.3f, 0});
    CHECK(near(load(body)[0], 6) && near(load(sample)[1], 0) && ee_r32(material) == 2);
    std::free(g_ram);
    return failures ? 1 : 0;
}
