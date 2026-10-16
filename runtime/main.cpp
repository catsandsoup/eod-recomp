// main.cpp - entry point of the native port.
//
// Usage: eod [--data DIR] [--log cats] [--headless] [--frames N]
//            [--screenshot-every N] [--scale N]
//
// DIR is the root of the game disc (the directory that contains
// SLUS_209.10 and DATA/).  Defaults to the executable's directory, then the
// current directory.
#include <csignal>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__aarch64__)
#include "sse_compat.h"  // MXCSR on x86, FPCR.FZ on arm64
#define HAVE_SSE 1
#endif

#include "core/config.h"
#include "platform/display_rect.h"
#ifdef __APPLE__
#include "app/macos_app.h"
#include "app/macos_vulkan.h"
#endif
#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "core/eodsave.h"
#include "core/ps2mc.h"
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
int vu_bench(const char *path, int repeat);
void gs_sync();
std::string mc_port_dir(int port);  // iop/mc.cpp

// Ends the program from the EE thread: lets the GS thread finish what it was
// given, then leaves without running static destructors under the threads
// that are still running (the GS thread, sound).
// --save-edit FILE [money=N] [rep=A,B,POINTS] [slotN=VEHICLE] [option=value]...:
// shows or changes values in a save file (memcard/port0/BASLUS-20910DRIVEN/
// BASLUS-20910DRIVEN); "vehicles" lists the vehicle numbers.
static int save_edit(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "--save-edit: save file expected\n");
        return 1;
    }
    EodSave sv;
    std::string err;
    if (!sv.load(argv[0], err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    bool changed = false, looks_changed = false;
    std::vector<std::pair<int, int>> looks = eodsave_parse_looks(g_config.car_looks);
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "vehicles") {
            for (const EodVehicle &v : eodsave_vehicles()) printf("%4d  %s\n", v.id, eodsave_vehicle_label(v.id).c_str());
            return 0;
        }
        size_t eq = a.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "expected name=value: %s\n", a.c_str());
            return 1;
        }
        std::string k = a.substr(0, eq), v = a.substr(eq + 1);
        if (k == "money") {
            sv.set_money((int32_t)strtol(v.c_str(), nullptr, 10));
        } else if (k == "rep") {
            int ra = 0, rb = 0, rp = 0;
            if (sscanf(v.c_str(), "%d,%d,%d", &ra, &rb, &rp) != 3) {
                fprintf(stderr, "rep=A,B,POINTS expected\n");
                return 1;
            }
            sv.set_reputation(ra, rb, rp);
        } else if (k.size() == 5 && k.compare(0, 4, "slot") == 0 && k[4] >= '1' && k[4] <= '4') {
            std::vector<int> shown;
            for (int s = 0; s < sv.car_count(); s++) shown.push_back(eodsave_slot_shown(sv, s, looks));
            if (k[4] - '1' >= (int)shown.size()) {
                fprintf(stderr, "%s: no car in that slot\n", k.c_str());
                return 1;
            }
            shown[(size_t)(k[4] - '1')] = atoi(v.c_str());
            if (!eodsave_set_slots(sv, shown, looks, err)) {
                fprintf(stderr, "%s: %s\n", k.c_str(), err.c_str());
                return 1;
            }
            looks_changed = true;
        } else {
            size_t idx = sv.options().size();
            for (size_t j = 0; j < sv.options().size(); j++)
                if (sv.options()[j].name == k) {
                    idx = j;  // (names repeat per controller layout: the first one)
                    break;
                }
            if (idx == sv.options().size() || !sv.set_option(idx, v, err)) {
                fprintf(stderr, "%s: %s\n", k.c_str(), idx == sv.options().size() ? "no such setting" : err.c_str());
                return 1;
            }
        }
        changed = true;
    }
    if (changed && !sv.save(argv[0], err)) {
        fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (looks_changed) {
        g_config.car_looks = eodsave_format_looks(looks);
        if (!config_save(g_config_path)) fprintf(stderr, "can't write %s\n", g_config_path.c_str());
    }
    int32_t ra, rb, rp;
    sv.reputation(ra, rb, rp);
    printf("player %s, checksum %s\n", sv.player_name().c_str(), sv.checksum_ok() ? "ok" : "BAD");
    if (sv.has_career()) {
        printf("money %d\n", sv.money());
        for (int k = 0; k < sv.car_count(); k++) {
            int shown = eodsave_slot_shown(sv, k, looks), car = sv.slot_vehicle(k);
            printf("slot%d %d (%s)", k + 1, shown, eodsave_vehicle_label(shown).c_str());
            if (shown != car) printf(" over career car %d (%s)", car, eodsave_vehicle_label(car).c_str());
            printf("\n");
        }
    }
    if (sv.has_reputation())
        printf("reputation %d x100 + %d x50 + %d = %d\n", ra, rb, rp, EodSave::reputation_total(ra, rb, rp));
    if (!changed)
        for (const EodSaveOption &o : sv.options())
            printf("  %-14s %-20s %s\n", eodsave_group_name(o.group), o.name.c_str(), o.value.c_str());
    return 0;
}

[[noreturn]] void app_exit(int code) {
    gs_shutdown();  // drains and joins the GS worker (docs/WORKER_SHUTDOWN.md)
    if (const char *path = getenv("EOD_DUMP_RAM")) {  // EE main memory at exit (debugging)
        if (FILE *f = fopen(path, "wb")) {
            std::vector<uint8_t> buf(32u << 20);
            ee_read_block(0, buf.data(), (uint32_t)buf.size());
            fwrite(buf.data(), 1, buf.size(), f);
            fclose(f);
        }
    }
    platform_shutdown();
    fflush(nullptr);
    _exit(code);
}

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
#ifdef __APPLE__
    mac_app_init_vulkan(); // before replay, SDL window creation or vkCreateInstance
#endif
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef SIGQUIT
    signal(SIGQUIT, on_signal);
#endif
    PlatformConfig cfg;
    std::string data_dir;
    // settings: eod.ini next to the executable (--config overrides the path)
    std::string config_path = dir_of(argv[0]) + "/eod.ini";
#ifdef __APPLE__
    config_path = mac_app_support() + "/eod.ini";
#endif
    for (int i = 1; i + 1 < argc; i++)
        if (std::string(argv[i]) == "--config") config_path = argv[i + 1];
    config_load(config_path);
    cfg.width = g_config.width;
    cfg.height = g_config.height;
    cfg.fullscreen = g_config.fullscreen;
    cfg.aspect = display_aspect(g_config.aspect);
    cfg.smooth = g_config.smooth_display;
    if (!g_config.data_dir.empty()) data_dir = g_config.data_dir;
    const char *log_spec = getenv("EOD_LOG");
    const char *replay = nullptr;
    const char *vu_bench_file = nullptr;
    const char *import_card = nullptr;
    int repeat = 3;
#ifndef _WIN32
    bool list_pads = false;
    int watch_pads = 0;
#endif
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
        else if (a == "--config") next();
        else if (a == "--renderer") g_config.renderer = next();
        else if (a == "--fullscreen") cfg.fullscreen = true;
        else if (a == "--windowed") cfg.fullscreen = false;
        else if (a == "--log") log_spec = next();
        else if (a == "--log-file") log_set_file(next());
        else if (a == "--headless") cfg.headless = true;
        else if (a == "--frames") g_opt_frames = atoi(next());
        else if (a == "--scale") {
            int n = atoi(next());
            cfg.width = 320 * n;
            cfg.height = 240 * n;
        }
        else if (a == "--screenshot-every") g_opt_screenshot_every = atoi(next());
        else if (a == "--screenshot-dir") g_opt_screenshot_dir = next();
        else if (a == "--gs-replay") replay = next();
        else if (a == "--vu-bench") vu_bench_file = next();
        else if (a == "--import-save") import_card = next();
        else if (a == "--save-edit") return save_edit(argc - i - 1, argv + i + 1);
        else if (a == "--repeat") repeat = atoi(next());
#ifndef _WIN32
        else if (a == "--list-pads") list_pads = true;
        else if (a == "--watch") watch_pads = atoi(next());
#endif
        else if (a == "--help" || a == "-h") {
#ifdef _WIN32
            printf("usage: %s [--data DIR] [--config eod.ini] [--renderer d3d11|software] [--fullscreen|--windowed]\n"
                   "          [--scale N] [--log cats] [--log-file PATH] [--headless] [--frames N]\n"
                   "          [--screenshot-every N] [--screenshot-dir DIR] [--gs-replay FILE [--repeat N]]\n"
                   "          [--vu-bench FILE [--repeat N]] [--import-save PCSX2_CARD.ps2]\n"
                   "          [--save-edit SAVEFILE [money=N] [rep=A,B,POINTS] [option=value]...]\n"
                   "log categories: core kernel thread hw dma gs vif vu sif iop file pad mc snd game all\n",
                   argv[0]);
#else
            printf("usage: %s [--data DIR] [--config eod.ini] [--renderer vulkan|software] [--fullscreen|--windowed]\n"
                   "          [--scale N] [--log cats] [--log-file PATH] [--headless] [--frames N]\n"
                   "          [--screenshot-every N] [--screenshot-dir DIR] [--gs-replay FILE [--repeat N]]\n"
                   "          [--vu-bench FILE [--repeat N]] [--import-save PCSX2_CARD.ps2]\n"
                   "          [--save-edit SAVEFILE [money=N] [rep=A,B,POINTS] [option=value]...]\n"
                   "       %s --list-pads [--watch SECONDS]   (controllers; no game data needed)\n"
                   "log categories: core kernel thread hw dma gs vif vu sif iop file pad mc snd game all\n",
                   argv[0], argv[0]);
#endif
            return 0;
        }
    }
    log_init(log_spec);
#ifndef _WIN32
    if (list_pads || watch_pads > 0) return platform_list_pads(watch_pads);
#endif
#ifdef __APPLE__
    if (!replay && !vu_bench_file) {
        // Honor an explicit environment path before trying local disc discovery.
        if (data_dir.empty()) {
            const char *env_data = getenv("EOD_DATA");
            if (env_data && env_data[0]) data_dir = env_data;
        }
        bool headless = cfg.headless || (getenv("EOD_HEADLESS") && getenv("EOD_HEADLESS")[0] == '1');
        if (headless && data_dir.empty()) {
            for (const auto &candidate : {dir_of(argv[0]), std::string(".")}) {
                if (disc_check_folder(candidate, {ee_elf_size, ee_elf_crc32}).ok()) { data_dir = candidate; break; }
            }
        }
        if (!mac_app_prepare(data_dir, config_path, headless, {ee_elf_size, ee_elf_crc32})) return 1;
    }
#endif
    platform_configure_display(cfg); // Replay also needs startup aspect, without SDL/device initialization.
    if (replay) { int result = gs_replay(replay, repeat); gs_shutdown(); return result; }
    if (vu_bench_file) {
        vu_init();
        return vu_bench(vu_bench_file, repeat);
    }

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
    if (import_card) {  // copy this game's saves from a PCSX2 memory card into port 0's folder
        std::vector<Ps2McSave> saves;
        std::string err;
        if (!ps2mc_read_saves(import_card, "BASLUS-20910", saves, err)) fatal("%s", err.c_str());
        std::string dir = mc_port_dir(0);
        int n = ps2mc_write_saves(dir, saves, err);
        for (const Ps2McSave &s : saves) printf("imported %s (%zu files) into %s\n", s.dir.c_str(), s.files.size(), dir.c_str());
        if (saves.empty()) printf("no Eve of Destruction saves on %s\n", import_card);
        if (n < (int)saves.size()) fatal("%s", err.c_str());
        return 0;
    }

    if (!platform_init(cfg)) fatal("platform initialisation failed");
    platform_thread_fast();  // the EE runs on this thread
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
#ifndef _WIN32
    void debug_watch_init();
    debug_watch_init();
#endif
    kernel_run(entry);
    app_exit(0);
}
