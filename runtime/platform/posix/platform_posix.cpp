// platform_posix.cpp - Linux/macOS backend used for development and
// automated testing.  Runs headless (frames can be dumped to disk).
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../platform.h"
#include "../pad_script.h"

static PlatformConfig g_cfg;

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

uint64_t platform_time_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

void platform_sleep_us(uint32_t us) { usleep(us); }

// Input: only scripted (EOD_PAD_SCRIPT, see pad_script.h).
void platform_get_pad(int port, HostPadState *out) {
    *out = HostPadState();
    out->connected = port == 0;
    uint16_t b;
    if (port == 0 && pad_script_buttons(b)) out->buttons = b;
}

void platform_set_vibration(int, uint8_t, uint8_t) {}

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

// ------------------------------------------------------------------ fibers
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

HostFiber *fiber_current() { return g_current_fiber; }

void fiber_delete(HostFiber *f) { delete f; }

// ------------------------------------------------------------------ ticker
void platform_start_ticker(void (*cb)(), unsigned period_us) {
    std::thread([cb, period_us] {
        for (;;) {
            usleep(period_us);
            cb();
        }
    }).detach();
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

// Exposes the last presented frame for screenshots in headless mode.
bool platform_last_frame(std::vector<uint32_t> &pix, int &w, int &h) {
    if (g_last_frame.empty()) return false;
    pix = g_last_frame;
    w = g_last_w;
    h = g_last_h;
    return true;
}

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
