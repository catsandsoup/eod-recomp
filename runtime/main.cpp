// main.cpp - entry point of the native port.
//
// Usage: eod [--data DIR] [--log cats] [--headless] [--frames N]
//            [--screenshot-every N] [--scale N]
//
// DIR is the root of the game disc (the directory that contains
// SLUS_209.10 and DATA/).  Defaults to the executable's directory, then the
// current directory.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define HAVE_SSE 1
#endif

#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "gs/gs.h"
#include "hw/hw.h"
#include "hw/ipu.h"
#include "iop/iop.h"
#include "iop/sif.h"
#include "kernel/kernel.h"
#include "platform/platform.h"
#include "vu/vif.h"
#include "vu.h"

void vu_init();

namespace {

uint32_t crc32_buf(const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

// Loads the PT_LOAD segments of the original executable into EE RAM.
uint32_t load_elf(const std::vector<uint8_t> &elf) {
    auto rd32 = [&](size_t o) {
        uint32_t v;
        memcpy(&v, &elf[o], 4);
        return v;
    };
    auto rd16 = [&](size_t o) {
        uint16_t v;
        memcpy(&v, &elf[o], 2);
        return v;
    };
    uint32_t entry = rd32(0x18), phoff = rd32(0x1C);
    uint16_t phentsize = rd16(0x2A), phnum = rd16(0x2C);
    for (int i = 0; i < phnum; i++) {
        size_t ph = phoff + (size_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint32_t off = rd32(ph + 4), va = rd32(ph + 8), fsz = rd32(ph + 16), msz = rd32(ph + 20);
        memcpy(g_ram + va, &elf[off], fsz);
        memset(g_ram + va + fsz, 0, msz - fsz);
        LOG(LOG_CORE, "loaded segment %08x-%08x", va, va + msz);
    }
    return entry;
}

std::string dir_of(const char *path) {
    std::string p = path;
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
}

}  // namespace

int g_opt_frames = 0;           // exit after N frames (testing)
int g_opt_screenshot_every = 0; // dump a screenshot every N frames
std::string g_opt_screenshot_dir = ".";

static void on_signal(int sig) {
    log_write(LOG_CORE, "signal %d - state dump:", sig);
    cpu_dump_state();
    _Exit(1);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGQUIT
    signal(SIGQUIT, on_signal);
#endif
    PlatformConfig cfg;
    std::string data_dir;
    const char *log_spec = getenv("EOD_LOG");
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char * {
            if (i + 1 >= argc) {
                fprintf(stderr, "missing value for %s\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--data") data_dir = next();
        else if (a == "--log") log_spec = next();
        else if (a == "--headless") cfg.headless = true;
        else if (a == "--frames") g_opt_frames = atoi(next());
        else if (a == "--scale") cfg.window_scale = atoi(next());
        else if (a == "--screenshot-every") g_opt_screenshot_every = atoi(next());
        else if (a == "--screenshot-dir") g_opt_screenshot_dir = next();
        else if (a == "--help" || a == "-h") {
            printf("usage: %s [--data DIR] [--log cats] [--headless] [--frames N] [--scale N]\n", argv[0]);
            return 0;
        }
    }
    log_init(log_spec);

#ifdef HAVE_SSE
    // The EE FPU flushes denormals to zero.
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif

    // locate the game data
    std::vector<std::string> roots;
    if (!data_dir.empty()) roots.push_back(data_dir);
    roots.push_back(dir_of(argv[0]));
    roots.push_back(".");
    std::vector<uint8_t> elf;
    std::string root;
    for (auto &r : roots) {
        char path[1024];
        if (platform_find_file(r.c_str(), "SLUS_209.10", path, sizeof path) && read_file(path, elf)) {
            root = r;
            break;
        }
    }
    if (root.empty()) {
        fatal("Could not find SLUS_209.10. Copy the contents of the game disc next to the executable "
              "or pass --data <disc directory>.");
    }
    uint32_t crc = crc32_buf(elf.data(), elf.size());
    if (elf.size() != ee_elf_size || crc != ee_elf_crc32)
        fatal("SLUS_209.10 does not match the executable this port was built from "
              "(crc %08x, expected %08x). Only the NTSC-U v1.00 release is supported.",
              crc, ee_elf_crc32);
    g_game_root = root;
    LOG(LOG_CORE, "game root: %s", root.c_str());

    if (!platform_init(cfg)) fatal("platform initialisation failed");
    mem_init();
    cpu_init();
    hw_init();
    vu_init();
    vif_init();
    ipu_init();
    gs_init();
    sif_init();
    kernel_init(argc, argv);
    uint32_t entry = load_elf(elf);
    LOG(LOG_CORE, "starting at %08x", entry);
    kernel_run(entry);
    platform_shutdown();
    return 0;
}
