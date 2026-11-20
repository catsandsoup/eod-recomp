// Uses the recompiled 0x20F4A0, 0x218E68 and 0x21C370 consumers. Stubs record
// their calls at the model, wheel, transform and queue boundaries.
#include "ee.h"
#include "visual_car_list.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>


EEState g_ee{};
uint8_t *g_ram;
volatile int g_ee_irq_pending;
static unsigned current_player;
static bool native_renderer = true;
static unsigned wheel_calls;
static std::vector<uint32_t> simple, select, body, matrix, parts;
static std::vector<uint32_t> queue_ops, transforms;

extern "C" {
void f_0020F4A0();
void f_0021C370();
void f_00218E68_orig();
void ee_service_interrupts() { std::abort(); }
uint32_t ee_read32_slow(uint32_t) { std::abort(); }
uint64_t ee_read64_slow(uint32_t) { std::abort(); }
void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
void ee_write64_slow(uint32_t, uint64_t) { std::abort(); }
void f_0034C9C0() { queue_ops.push_back(0); }
void f_0034C930() { queue_ops.push_back(1); }
void f_0034C860() { queue_ops.push_back(2); }
void f_0021A9B0() { simple.push_back(R[4].uw[0]); }
void f_002112E8() { R[2].uw[0] = current_player; }
void f_00211318() { FPR[0].f = 1; }
void f_00218958() { matrix.push_back(R[4].uw[0]); }
void f_002189C8() { body.push_back(R[4].uw[0]); }
void f_00218C38() { parts.push_back(R[4].uw[0]); }
void f_00220698() { ++wheel_calls; }
void f_0034F570() { FPR[0].f = 1; }
void f_0034F5A0() { R[2].uw[0] = ~0u; }
void f_0034F620() { FPR[0].f = 1; }
void f_0034FA98() {
    select.push_back(R[4].uw[0] - 0x28);
    FPR[0].f = 10 + MEM_LW(R[4].uw[0] - 0x28 + 0x40);
}
void f_001795E8() { R[2].ud[0] = 1; }
void f_0021BDF0() { transforms.push_back(R[4].uw[0]); }
}
namespace native { bool enabled() { return native_renderer; } }

static void check(bool result, const char *message) {
    if (!result) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
constexpr uint32_t object(uint32_t slot) { return 0xB00000 + slot * 0x800; }
static void put_car(uint32_t slot) {
    const uint32_t o = object(slot);
    MEM_SW(o + 0x3c, 0x10000);
    MEM_SW(o + 0x40, slot);
    MEM_SW(o + 0x220, o + 0xf8);
    MEM_SW(o + 0x224, o + 0x1f8);
    MEM_SW(o + 0x1f8, 2);
    MEM_SW(o + 0xf8, 0x41a00000);
    MEM_SW(o + 0x108, 0x463b8000);
    MEM_SW(o + 0x554, 0x3f800000);
    eod_visual_car_register(kVisualCarSlots + 4 * slot, o);
}
static void run_view(uint32_t callback_object) {
    simple.clear();
    select.clear();
    matrix.clear();
    body.clear();
    parts.clear();
    queue_ops.clear();
    transforms.clear();
    wheel_calls = 0;
    R[29].ud[0] = 0x700000;
    for (unsigned i = 16; i <= 23; ++i) R[i].ud[0] = 0x404040 + i;
    R[31].ud[0] = 0xCAFE00;
    FPR[20].u = 0xA5A5A5A5;
    FPR[21].u = 0xA6A6A6A6;
    f_0020F4A0();
    f_0021C370();
    R[4].ud[0] = callback_object;
    VisualCarTraversal cars(true);
    if (cars.adaptive())
        f_00218E68_cars(&cars);
    else
        f_00218E68_orig();
    check(queue_ops == std::vector<uint32_t>{0, 1, 2}, "one open and two flushes");
    check(R[29].ud[0] == 0x700000 && R[31].ud[0] == 0xCAFE00, "stack and RA restored");
    for (unsigned i = 16; i <= 23; ++i)
        check(R[i].ud[0] == 0x404040 + i, "callee-saved registers restored");
    check(FPR[20].u == 0xA5A5A5A5 && FPR[21].u == 0xA6A6A6A6, "callee-saved FPR restored");
    std::printf("frame loop %zu, selected %zu, matrix %zu, body %zu, cockpit %zu\n",
                simple.size(), select.size(), matrix.size(), body.size(), parts.size());
}
static void check_view(const std::vector<uint32_t> &expected, uint32_t callback_object) {
    run_view(callback_object);
    check(simple == expected, "frame loop submitted every active car in slot order");
    check(select == expected, "batch calculated every active LOD in slot order");
    std::vector<uint32_t> body_expected;
    for (const auto o : expected) {
        check(MEM_LW(o + 0x54c) == (MEM_LW(o + 0x40) >= 10), "retail distance threshold selected");
        if (o != object(current_player)) body_expected.push_back(o);
    }
    check(body == body_expected && matrix == body_expected, "batch submitted others in slot order");
    check(transforms == expected, "transform update processed every active car in slot order");
    check(wheel_calls == 4 * body_expected.size(), "every other car passed through all wheel submissions");
    const uint32_t cockpit = object(current_player);
    const bool in_view = std::find(expected.begin(), expected.end(), cockpit) != expected.end();
    check(parts == (in_view ? std::vector<uint32_t>{cockpit} : std::vector<uint32_t>{}),
          "current view cockpit selection");
}
int main(int argc, char **argv) {
    const bool original = argc == 2 && std::strcmp(argv[1], "original") == 0;
    const bool diff = argc == 2 && std::strcmp(argv[1], "diff") == 0;
    native_renderer = !original;
    check(visual_car_adaptive_enabled() == (!original && !diff), "renderer mode selection");
    constexpr size_t ram_size = 16 * 1024 * 1024;
    g_ram = static_cast<uint8_t *>(std::calloc(ram_size, 1));
    check(g_ram, "EE test RAM");
    constexpr uint32_t system_active = kVisualCarSlots + 0x50;
    MEM_SW(system_active, 1);
    std::vector<uint32_t> expected;
    const uint32_t count = original || diff ? 12 : 28;
    for (uint32_t i = count; i-- > 0;) put_car(i); // registration order differs from slot order
    for (uint32_t i = 0; i < count; ++i) expected.push_back(object(i));
    check_view(expected, object(0));

    if (original || diff) {
        eod_visual_car_reset();
        check(MEM_LW(kVisualCarSlots) == object(0), "legacy reset keeps original guest behavior");
        eod_visual_car_unregister(kVisualCarSlots + 4 * 6, object(6));
        expected.resize(6);
        check_view(expected, object(0));
        std::free(g_ram);
        return 0;
    }

    current_player = 23;
    check_view(expected, object(0));
    MEM_SW(system_active, 0x11111111);
    for (uint32_t slot : {0u, 1u, 24u}) {
        eod_visual_car_unregister(kVisualCarSlots + 4 * slot, object(slot));
        expected.erase(std::find(expected.begin(), expected.end(), object(slot)));
    }
    eod_visual_car_unregister(kVisualCarSlots + 4 * 25, object(24)); // stale destructor
    check_view(expected, object(2)); // the lowest surviving slot now owns the batch
    check(eod_visual_car_slot_read(kVisualCarSlots + 4 * 23) == object(23),
          "overflow lookup uses the original slot");
    check(MEM_LW(system_active) == 0x11111111, "overflow preserved the adjacent lifecycle flag");

    eod_visual_car_reset();
    check_view({}, object(2)); // reject a stale queued callback
    check(eod_visual_car_slot_read(kVisualCarSlots + 4 * 23) == 0, "reset invalidates old slots");
    put_car(21); // overflow-only view and no current cockpit
    check_view({object(21)}, object(21));
    eod_visual_car_unregister(kVisualCarSlots + 4 * 21, object(21));
    put_car(0);
    current_player = 0;
    check_view({object(0)}, object(0));
    std::free(g_ram);
}
