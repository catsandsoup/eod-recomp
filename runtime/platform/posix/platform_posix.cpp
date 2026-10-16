#include "../display_rect.h"
#include "../presentation.h"
// platform_posix.cpp - Linux/macOS backend used for development and
// automated testing.  Runs headless (frames can be dumped to disk).
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#if !defined(__aarch64__)
#include <ucontext.h>
#endif
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../platform.h"
#include <sched.h>
#include <thread>
#include "../pad_script.h"

#if !defined(EOD_SDL3)
// Headless backend (no SDL3): frames are kept for screenshots, audio can go
// to a WAV file, input comes only from the pad scripts.
static PlatformConfig g_cfg;

void platform_configure_display(const PlatformConfig &cfg) { g_cfg=cfg; }

bool platform_init(const PlatformConfig &cfg) {
    g_cfg = cfg;
    return true;
}

void platform_shutdown() {}

void platform_fatal_message(const char *) {}

bool platform_pump() { return true; }

static std::vector<uint32_t> g_last_frame;
static int g_last_w, g_last_h;

void platform_present(const uint32_t *pixels, int width, int height, int stride) {
    g_last_frame.resize((size_t)width * height);
    for (int y = 0; y < height; y++) memcpy(&g_last_frame[(size_t)y * width], pixels + (size_t)y * stride, width * 4);
    g_last_w = width;
    g_last_h = height;
}

// Input: only scripted (EOD_PAD_SCRIPT / EOD_PAD_SCRIPT1, see pad_script.h).
void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    out->connected = port == 0;
    if (pad_script_state(port, out)) out->connected = true;
}

void platform_set_vibration(int, uint8_t, uint8_t) {}

int platform_list_pads(int) {
    printf("built without SDL3: no controller support (scripted pads only)\n");
    return 1;
}

void platform_set_status(const char *) {}

void platform_display_rect(int cw, int ch, int *x, int *y, int *w, int *h) {
    const DisplayRect rect=display_rect(presentation::current(g_cfg.aspect),cw,ch);
    *x=rect.x;*y=rect.y;*w=rect.width;*h=rect.height;
}

void *platform_native_window() { return nullptr; }

bool platform_audio_open(int) { return true; }
// EOD_WAV=file: the headless build writes the audio (48 kHz stereo) to a WAV file.
void platform_audio_submit(const int16_t *samples, int frames) {
    static FILE *f = [] {
        const char *p = getenv("EOD_WAV");
        FILE *h = p ? fopen(p, "wb") : nullptr;
        if (h) {
            uint8_t hdr[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                               0x80, 0xBB, 0, 0, 0, 0xEE, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
            fwrite(hdr, 1, 44, h);
        }
        return h;
    }();
    static uint32_t bytes;
    if (!f || frames <= 0) return;
    fwrite(samples, 4, (size_t)frames, f);
    bytes += (uint32_t)frames * 4;
    // keep the header valid so the file can be read at any time
    long pos = ftell(f);
    uint32_t riff = bytes + 36;
    fseek(f, 4, SEEK_SET);
    fwrite(&riff, 4, 1, f);
    fseek(f, 40, SEEK_SET);
    fwrite(&bytes, 4, 1, f);
    fseek(f, pos, SEEK_SET);
}
int platform_audio_queued_frames() { return 0; }

// Exposes the last presented frame for screenshots in headless mode.
bool platform_last_frame(std::vector<uint32_t> &pix, int &w, int &h) {
    if (g_last_frame.empty()) return false;
    pix = g_last_frame;
    w = g_last_w;
    h = g_last_h;
    return true;
}

#endif  // !EOD_SDL3

uint64_t platform_time_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

void platform_sleep_us(uint32_t us) { usleep(us); }

void platform_thread_fast() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

int platform_core_count() { return platform_cpu_count(); }

int platform_cpu_count() {
#if defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
#endif
    unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 1;
}

// ------------------------------------------------------------------ fibers
#if defined(__aarch64__)
// arm64: a direct switch of the callee-saved registers (x19-x30, sp, d8-d15).
// macOS only offers ucontext as a deprecated API, and swapcontext makes a
// sigprocmask system call on every switch; the game switches threads often.
//
// eod_fiber_switch(&from->sp, to->sp) pushes the callee-saved registers on
// the current stack, stores sp, loads the other fiber's sp and pops its set.
// A new fiber's stack holds a frame whose x30 is eod_fiber_start and whose
// x19 is the fiber, so its first switch "returns" into the entry function.
#if defined(__APPLE__)
#define EOD_SYM(x) "_" #x
#else
#define EOD_SYM(x) #x
#endif
extern "C" void eod_fiber_switch(void **save_sp, void *new_sp);
extern "C" void eod_fiber_start();
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl " EOD_SYM(eod_fiber_switch) "\n"
    EOD_SYM(eod_fiber_switch) ":\n"
    "  sub sp, sp, #160\n"
    "  stp x19, x20, [sp, #0]\n"
    "  stp x21, x22, [sp, #16]\n"
    "  stp x23, x24, [sp, #32]\n"
    "  stp x25, x26, [sp, #48]\n"
    "  stp x27, x28, [sp, #64]\n"
    "  stp x29, x30, [sp, #80]\n"
    "  stp d8, d9, [sp, #96]\n"
    "  stp d10, d11, [sp, #112]\n"
    "  stp d12, d13, [sp, #128]\n"
    "  stp d14, d15, [sp, #144]\n"
    "  mov x9, sp\n"
    "  str x9, [x0]\n"
    "  mov sp, x1\n"
    "  ldp x19, x20, [sp, #0]\n"
    "  ldp x21, x22, [sp, #16]\n"
    "  ldp x23, x24, [sp, #32]\n"
    "  ldp x25, x26, [sp, #48]\n"
    "  ldp x27, x28, [sp, #64]\n"
    "  ldp x29, x30, [sp, #80]\n"
    "  ldp d8, d9, [sp, #96]\n"
    "  ldp d10, d11, [sp, #112]\n"
    "  ldp d12, d13, [sp, #128]\n"
    "  ldp d14, d15, [sp, #144]\n"
    "  add sp, sp, #160\n"
    "  ret\n"
    ".globl " EOD_SYM(eod_fiber_start) "\n"
    EOD_SYM(eod_fiber_start) ":\n"
    "  mov x0, x19\n"
    "  mov x29, #0\n"
    "  bl " EOD_SYM(eod_fiber_entry) "\n"
    "  brk #0\n");

struct HostFiber {
    void *sp = nullptr;
    std::vector<uint8_t> stack;
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
};

static HostFiber *g_current_fiber;

HostFiber *fiber_init_main() {
    HostFiber *f = new HostFiber();
    g_current_fiber = f;
    return f;
}

extern "C" void eod_fiber_entry(HostFiber *f) {
    f->fn(f->arg);
    fprintf(stderr, "fiber returned\n");
    abort();
}

HostFiber *fiber_create(size_t stack_size, void (*fn)(void *), void *arg) {
    HostFiber *f = new HostFiber();
    f->stack.resize(stack_size);
    f->fn = fn;
    f->arg = arg;
    uintptr_t top = (reinterpret_cast<uintptr_t>(f->stack.data()) + stack_size) & ~(uintptr_t)15;
    uint64_t *frame = reinterpret_cast<uint64_t *>(top - 160);
    memset(frame, 0, 160);
    frame[0] = reinterpret_cast<uint64_t>(f);                    // x19
    frame[11] = reinterpret_cast<uint64_t>(&eod_fiber_start);    // x30
    f->sp = frame;
    return f;
}

void fiber_switch(HostFiber *to) {
    HostFiber *from = g_current_fiber;
    if (from == to) return;
    g_current_fiber = to;
    eod_fiber_switch(&from->sp, to->sp);
}
#else
struct HostFiber {
    ucontext_t ctx;
    std::vector<uint8_t> stack;
    void (*fn)(void *) = nullptr;
    void *arg = nullptr;
};

static HostFiber *g_current_fiber;

HostFiber *fiber_init_main() {
    HostFiber *f = new HostFiber();
    g_current_fiber = f;
    return f;
}

static void fiber_trampoline(unsigned lo, unsigned hi) {
    HostFiber *f = reinterpret_cast<HostFiber *>(((uintptr_t)hi << 32) | (uintptr_t)lo);
    f->fn(f->arg);
    fprintf(stderr, "fiber returned\n");
    abort();
}

HostFiber *fiber_create(size_t stack_size, void (*fn)(void *), void *arg) {
    HostFiber *f = new HostFiber();
    f->stack.resize(stack_size);
    f->fn = fn;
    f->arg = arg;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack.data();
    f->ctx.uc_stack.ss_size = stack_size;
    f->ctx.uc_link = nullptr;
    uintptr_t p = reinterpret_cast<uintptr_t>(f);
    makecontext(&f->ctx, (void (*)())fiber_trampoline, 2, (unsigned)(p & 0xFFFFFFFF), (unsigned)(p >> 32));
    return f;
}

void fiber_switch(HostFiber *to) {
    HostFiber *from = g_current_fiber;
    if (from == to) return;
    g_current_fiber = to;
    swapcontext(&from->ctx, &to->ctx);
}
#endif

HostFiber *fiber_current() { return g_current_fiber; }

void fiber_delete(HostFiber *f) { delete f; }

// ------------------------------------------------------------------ ticker
namespace {
std::thread g_ticker_thread;
std::atomic<bool> g_ticker_stop{false};
}
void platform_stop_ticker() {
    g_ticker_stop = true;
    if (g_ticker_thread.joinable()) g_ticker_thread.join();
}
void platform_start_ticker(void (*cb)(), unsigned period_us) {
    platform_stop_ticker();
    g_ticker_stop = false;
    g_ticker_thread = std::thread([cb, period_us] {
        while (!g_ticker_stop.load()) {
            usleep(period_us);
            if (!g_ticker_stop.load()) cb();
        }
    });
}

// ------------------------------------------------------------------- files
static bool ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}

bool platform_find_file(const char *root, const char *rel, char *out, size_t out_size) {
    std::string cur = root;
    std::string r = rel;
    for (char &c : r)
        if (c == '\\') c = '/';
    size_t pos = 0;
    while (pos <= r.size()) {
        size_t slash = r.find('/', pos);
        std::string part = r.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        pos = slash == std::string::npos ? r.size() + 1 : slash + 1;
        if (part.empty() || part == ".") continue;
        DIR *d = opendir(cur.c_str());
        if (!d) return false;
        bool found = false;
        while (dirent *e = readdir(d)) {
            if (ieq(e->d_name, part.c_str())) {
                cur += "/";
                cur += e->d_name;
                found = true;
                break;
            }
        }
        closedir(d);
        if (!found) return false;
    }
    snprintf(out, out_size, "%s", cur.c_str());
    return true;
}

bool platform_mkdir(const char *path) { return mkdir(path, 0755) == 0; }

bool platform_list_dir(const char *path, std::vector<DirEntryInfo> &out) {
    DIR *d = opendir(path);
    if (!d) return false;
    while (dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        DirEntryInfo info = {};
        snprintf(info.name, sizeof info.name, "%s", e->d_name);
        std::string full = std::string(path) + "/" + e->d_name;
        struct stat st;
        if (stat(full.c_str(), &st) != 0) continue;
        info.is_dir = S_ISDIR(st.st_mode);
        info.size = (uint64_t)st.st_size;
        out.push_back(info);
    }
    closedir(d);
    return true;
}
