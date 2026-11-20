// Exercise the real padman RPC handler and EE packet with fake host pads.
#include "iop/iop.h"
#include "core/log.h"
#include "platform/platform.h"
#include "kernel/vblank_input.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

static uint8_t ram[4096], iop[4096];
uint8_t *g_ram = ram;
uint8_t *g_iop_ram = iop;
bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
void *ee_ptr(uint32_t a) { if (a >= sizeof ram) std::abort(); return ram + a; }
void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
uint32_t iop_alloc(uint32_t size) {
    static uint32_t next = 128;
    uint32_t address = next;
    next += size;
    if (next > sizeof iop) std::abort();
    return address;
}
static std::map<uint32_t, RpcHandler> handlers;
void iop_register_server(uint32_t sid, const char *, RpcHandler h) { handlers[sid] = std::move(h); }
static HostPadState pads[5];
static bool pending_input, quit_input;
static uint16_t pending_buttons;
bool platform_pump() {
    if (quit_input) return false;
    if (pending_input) { pads[1].buttons = pending_buttons; pending_input = false; }
    return true;
}
void platform_get_pad(int p, HostPadState *s) { *s = pads[p]; }
uint16_t game_patches_suppressed_buttons() { return 0; }
static int rumble_port = -1;
static uint8_t small, large;
static int rumble_calls;
void platform_set_vibration(int p, uint8_t s, uint8_t l) { rumble_port = p; small = s; large = l; rumble_calls++; }
extern void iop_pad_vblank();
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static uint32_t call(uint32_t cmd, int port, int slot, const uint8_t *payload = nullptr, size_t n = 0) {
    uint8_t buf[128] = {};
    uint32_t words[] = {cmd, (uint32_t)port, (uint32_t)slot};
    memcpy(buf, words, sizeof words);
    if (payload) memcpy(buf + 12, payload, n);
    CHECK(handlers.at(0x80000100)(RpcCall{}, buf) == 128);
    uint32_t offset = 0xC;
    if (cmd == 6 || cmd == 7 || cmd == 8) offset = 0x14;
    if (cmd == 0x0A) offset = 0x10;
    if (cmd == 0x0B) offset = 0x1C;
    uint32_t result;
    memcpy(&result, buf + offset, 4);
    if (cmd == 6 || cmd == 7 || cmd == 8 || cmd == 0x0A || cmd == 0x0B) CHECK(result == 1);
    return result;
}
static uint32_t tap_call(uint32_t sid, uint32_t port, uint32_t result_offset) {
    uint8_t buf[128] = {};
    memcpy(buf, &port, 4);
    CHECK(handlers.at(sid)(RpcCall{sid, 1, buf, sizeof buf, 0, sizeof buf, 0}, buf) == sizeof buf);
    uint32_t result;
    memcpy(&result, buf + result_offset, 4);
    return result;
}
int main() {
    setenv("EOD_PAD_SCRIPT_SLOT1", "0:4000", 1);
    setenv("EOD_PAD_SCRIPT_SLOT2", "0:8000", 1);
    setenv("EOD_PAD_SCRIPT_SLOT3", "0:1000", 1);
    iop_misc_init();
    iop_pad_init();
    CHECK(tap_call(0x80000905, 0, 0) == 0x302);
    CHECK(tap_call(0x80000903, 0, 4) == 0);
    CHECK(call(0x0D, 0, 0) == 1);
    CHECK(tap_call(0x80000901, 0, 4) == 1);
    CHECK(tap_call(0x80000903, 0, 4) == 1);
    CHECK(call(0x0C, 0, 0) == 2);
    CHECK(call(0x0D, 0, 0) == 4);
    CHECK(call(0x0D, 1, 0) == 1);
    CHECK(tap_call(0x80000901, 1, 4) == 1);
    CHECK(tap_call(0x80000903, 1, 4) == 0);
    CHECK(tap_call(0x80000901, 4, 4) == 0);
    uint32_t open[] = {0, 512};  // OPEN's EE padArea is at +0x10
    pads[1].connected = true;
    pads[1].buttons = PAD_R2 | PAD_CROSS;
    pads[1].pressure[11] = 137;
    pads[1].pressure[10] = 19;
    pads[1].lx = 25;
    call(1, 1, 0, (uint8_t *)open, sizeof open);
    uint32_t analog[] = {1, 0}; // unlocked: old handler returned lock=0 as failure
    call(6, 1, 0, (uint8_t *)analog, sizeof analog);
    uint8_t reference[12] = {};
    call(0x0B, 1, 0, reference, sizeof reference);
    uint32_t pressure = 1;
    call(0x0A, 1, 0, (uint8_t *)&pressure, 4);
    iop_pad_vblank();
    const uint8_t *d = ram + 512; // second write uses first half
    CHECK(d[1] == 0x79 && d[0x70] == 6);
    CHECK((uint16_t)(d[2] | d[3] << 8) == (uint16_t)~pads[1].buttons);
    CHECK(d[6] == 25 && d[18] == 19 && d[19] == 137);
    uint8_t align[] = {1, 0xff, 0, 0xff, 0xff, 0xff};
    uint8_t motors[] = {173, 99, 7, 0, 0, 0};
    call(8, 1, 0, align, 6);
    call(7, 1, 0, motors, 6);
    CHECK(rumble_port == 1 && small == 1 && large == 173);
    const int standalone_rumble_calls = rumble_calls;
    call(7, 0, 1, motors, 6);  // a scripted slot has no physical motor
    CHECK(rumble_port == 1 && small == 1 && large == 173);
    CHECK(rumble_calls == standalone_rumble_calls);
    // SLUS_209.10's 0x00390678 (SDK 2.8) produces DMA, unlike current
    // open-source PS2SDK's RPC implementation. At +0 sequence, +4 command=1,
    // +8 length=6, +12 actuator bytes; 0x0038F878 increments the sequence and
    // transfers 32 bytes to iop_buf + ((sequence & 1) * 32).
    // Reconstruct that producer's packet rather than invoking SET_ACTDIR.
    auto dma = [&](uint32_t sequence, uint32_t command, uint32_t length, uint8_t motor) {
        uint8_t packet[32] = {};
        uint32_t header[] = {sequence, command, length};
        memcpy(packet, header, sizeof header);
        packet[12] = motor; // alignment above maps slot 0 to large motor
        memcpy(iop + 128 + (sequence & 1) * 32, packet, sizeof packet);
    };
    dma(1, 1, 6, 91);
    iop_pad_vblank();
    CHECK(rumble_port == 1 && small == 0 && large == 91);
    // Both halves may change between vblanks. Consume only the newest.
    dma(2, 1, 6, 120);
    dma(3, 1, 6, 201);
    iop_pad_vblank();
    CHECK(large == 201);
    int calls = rumble_calls;
    iop_pad_vblank();
    CHECK(rumble_calls == calls);
    CHECK(large == 201); // stale other half must not overwrite newer state
    dma(4, 1, 6, 0);
    iop_pad_vblank();
    CHECK(small == 0 && large == 0);
    dma(5, 1, 7, 255); // invalid actuator length cannot command a motor
    iop_pad_vblank();
    CHECK(large == 0);
    dma(6, 2, 6, 255); // unknown DMA operation
    iop_pad_vblank();
    CHECK(large == 0);
    call(0x0E, 1, 0);
    CHECK(rumble_port == 1 && small == 0 && large == 0);
    // A new OPEN starts a fresh producer sequence and clears old DMA halves.
    call(1, 1, 0, (uint8_t *)open, sizeof open);
    calls = rumble_calls;
    iop_pad_vblank();
    CHECK(rumble_calls == calls && large == 0);
    dma(1, 1, 6, 63);
    iop_pad_vblank();
    CHECK(large == 63);
    call(0x0E, 1, 0);
    // Model SDL's event-updated keyboard cache: a queued physical key only
    // reaches platform_get_pad when platform_pump services that event. Test
    // the production vblank step and inspect its actual SDK 2.8 DMA output.
    uint32_t latency_open[] = {0, 1024};
    pads[1].buttons = 0;
    call(1, 1, 0, (uint8_t *)latency_open, sizeof latency_open);
    auto latest = [&]() {
        uint32_t a, b;
        memcpy(&a, ram + 1024 + 0x58, 4); memcpy(&b, ram + 1152 + 0x58, 4);
        return ram + 1024 + (b > a ? 128 : 0);
    };
    pending_input = true; pending_buttons = PAD_CROSS;
    CHECK(vblank_poll_input());
    d = latest();
    CHECK((uint16_t)(d[2] | d[3] << 8) == (uint16_t)~PAD_CROSS);
    pending_input = true; pending_buttons = 0;
    CHECK(vblank_poll_input());
    d = latest();
    CHECK((uint16_t)(d[2] | d[3] << 8) == 0xFFFF);
    // An input delivered while the renderer blocks belongs to this vblank,
    // not the next one. Check the real published DMA packet.
    CHECK(vblank_finish_and_poll_input([&]() { pending_input=true; pending_buttons=PAD_TRIANGLE; }));
    d = latest();
    CHECK((uint16_t)(d[2] | d[3] << 8) == (uint16_t)~PAD_TRIANGLE);   // (Start and Select alone are held back a few frames: chord_guard.h)
    uint8_t before_quit[256]; memcpy(before_quit, ram + 1024, sizeof before_quit);
    quit_input = true;
    CHECK(!vblank_poll_input());
    CHECK(memcmp(before_quit, ram + 1024, sizeof before_quit) == 0);
    for (int slot = 0; slot < 4; ++slot) {
        const int host = slot ? slot + 1 : 0;
        pads[host].connected = true;
        pads[host].lx = 40 * (slot + 1);
        uint32_t slot_open[] = {0, (uint32_t)(2048 + 256 * slot)};
        call(1, 0, slot, (uint8_t *)slot_open, sizeof slot_open);
        const uint8_t *packet = ram + slot_open[1] + 128;
        CHECK(packet[0x70] == 6 && packet[6] == pads[host].lx);
    }
    CHECK(tap_call(0x80000902, 0, 4) == 1);
    CHECK(tap_call(0x80000903, 0, 4) == 0);
    CHECK(call(0x0D, 0, 0) == 1);
    iop_pad_vblank();
    CHECK(ram[2048 + 0x70] == 6);
    CHECK(ram[2304 + 0x70] == 0);
    printf("pad RPC/EE pressure and actuator checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
