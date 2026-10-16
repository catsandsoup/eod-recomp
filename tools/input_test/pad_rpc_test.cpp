// Exercise the real padman RPC handler and EE packet with fake host pads.
#include "iop/iop.h"
#include "core/log.h"
#include "platform/platform.h"
#include <cstdio>
#include <cstdlib>
#include <utility>

static uint8_t ram[4096], iop[4096];
uint8_t *g_ram = ram;
uint8_t *g_iop_ram = iop;
bool g_log_enabled[LOG_CAT_COUNT] = {};
void log_write(LogCat, const char *, ...) {}
void *ee_ptr(uint32_t a) { if (a >= sizeof ram) std::abort(); return ram + a; }
void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
uint32_t iop_alloc(uint32_t) { return 128; }
static RpcHandler handler;
void iop_register_server(uint32_t, const char *, RpcHandler h) { handler = std::move(h); }
static HostPadState pads[2];
void platform_get_pad(int p, HostPadState *s) { *s = pads[p]; }
static int rumble_port = -1;
static uint8_t small, large;
void platform_set_vibration(int p, uint8_t s, uint8_t l) { rumble_port = p; small = s; large = l; }
extern void iop_pad_vblank();
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void call(uint32_t cmd, int port, int slot, const uint8_t *payload = nullptr, size_t n = 0) {
    uint8_t buf[128] = {};
    uint32_t words[] = {cmd, (uint32_t)port, (uint32_t)slot};
    memcpy(buf, words, sizeof words);
    if (payload) memcpy(buf + 12, payload, n);
    CHECK(handler(RpcCall{}, buf) == 128);
}
int main() {
    iop_pad_init();
    uint32_t open[] = {0, 512};  // OPEN's EE padArea is at +0x10
    pads[1].connected = true;
    pads[1].buttons = PAD_R2 | PAD_CROSS;
    pads[1].pressure[11] = 137;
    pads[1].pressure[10] = 19;
    pads[1].lx = 25;
    call(1, 1, 0, (uint8_t *)open, sizeof open);
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
    call(7, 0, 1, motors, 6);  // absent multitap cannot vibrate a host pad
    CHECK(rumble_port == 1 && small == 1 && large == 173);
    call(0x0E, 1, 0);
    CHECK(rumble_port == 1 && small == 0 && large == 0);
    printf("pad RPC/EE pressure and actuator checks: %d failures\n", failures);
    return failures ? 1 : 0;
}
