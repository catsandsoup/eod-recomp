// platform.h - host OS abstraction (Win32 for the real target, POSIX for
// development/testing).
#pragma once
#include <cstddef>
#include <cstdint>

struct PlatformConfig {
    int window_scale = 2;      // initial window size multiplier (640x448 base)
    bool headless = false;     // no window (tests)
    bool vsync_throttle = true;
    const char *title = "Eve of Destruction";
};

bool platform_init(const PlatformConfig &cfg);
void platform_shutdown();
void platform_fatal_message(const char *msg);

// Pumps OS events. Returns false when the user closed the window.
bool platform_pump();

// Displays a 32-bit (0xAABBGGRR little endian, i.e. bytes R,G,B,A) image.
void platform_present(const uint32_t *pixels, int width, int height, int stride);

uint64_t platform_time_us();
void platform_sleep_us(uint32_t us);

// ------------------------------------------------------------------ input
// DualShock 2 button bits as reported by padman (active high here; the pad
// HLE inverts them).
enum PadButton : uint16_t {
    PAD_SELECT = 0x0001,
    PAD_L3 = 0x0002,
    PAD_R3 = 0x0004,
    PAD_START = 0x0008,
    PAD_UP = 0x0010,
    PAD_RIGHT = 0x0020,
    PAD_DOWN = 0x0040,
    PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100,
    PAD_R2 = 0x0200,
    PAD_L1 = 0x0400,
    PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000,
    PAD_CIRCLE = 0x2000,
    PAD_CROSS = 0x4000,
    PAD_SQUARE = 0x8000,
};

struct HostPadState {
    bool connected = false;
    uint16_t buttons = 0;           // PadButton bits, 1 = pressed
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;
    uint8_t pressure[12] = {};      // right,left,up,down,tri,circle,cross,square,L1,R1,L2,R2
};

void platform_get_pad(int port, HostPadState *out);
void platform_set_vibration(int port, uint8_t small_motor, uint8_t large_motor);

// ------------------------------------------------------------------ audio
// 48 kHz signed 16-bit stereo interleaved.
bool platform_audio_open(int sample_rate);
void platform_audio_submit(const int16_t *samples, int frames);
int platform_audio_queued_frames();

// ----------------------------------------------------------------- fibers
struct HostFiber;
HostFiber *fiber_init_main();  // converts the calling thread
HostFiber *fiber_create(size_t stack_size, void (*fn)(void *), void *arg);
void fiber_switch(HostFiber *to);
void fiber_delete(HostFiber *f);

// ---------------------------------------------------------- timer thread
// Calls cb() from a background thread roughly every period_us.
void platform_start_ticker(void (*cb)(), unsigned period_us);

// --------------------------------------------------------------- files
// Case-insensitive lookup of a relative path under a root directory.
// Returns true and fills out (absolute host path) when found.
bool platform_find_file(const char *root, const char *rel, char *out, size_t out_size);
bool platform_mkdir(const char *path);
