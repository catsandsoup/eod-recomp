#include "core/cpu.h"
#include "core/memory.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

extern "C" void f_002A8EF0();
void arcade_mod_badges_run_menu(uint32_t caller, void (*run_menu)());

EEState g_ee{};
uint8_t *g_ram = nullptr;
std::string g_game_root;
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" void ee_write8_slow(uint32_t, uint8_t) { std::abort(); }
extern "C" void *ee_ptr(uint32_t address) { return address < EE_RAM_SIZE ? g_ram + address : nullptr; }
extern "C" void f_002A8EF0_orig() { ee_w32(g_ee.r[4].uw[0] + 4, g_ee.r[5].uw[0]); }

bool platform_find_file(const char *root, const char *rel, char *out, size_t size) {
    auto path = fs::u8path(root);
    for (const auto &component : fs::u8path(rel)) {
        if (!fs::is_directory(path)) return false;
        fs::path match;
        const auto want = component.u8string();
        for (const auto &entry : fs::directory_iterator(path)) {
            const auto have = entry.path().filename().u8string();
            if (std::equal(want.begin(), want.end(), have.begin(), have.end(), [](unsigned char a, unsigned char b) {
                    return std::tolower(a) == std::tolower(b);
                })) {
                match = entry.path();
                break;
            }
        }
        if (match.empty()) return false;
        path = match;
    }
    return std::snprintf(out, size, "%s", path.u8string().c_str()) < static_cast<int>(size);
}

static int failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void text(uint32_t address, const char *value) {
    ee_write_block(address, value, std::strlen(value) + 1);
}

static std::string read_text(uint32_t address) {
    char label[80];
    ee_read_string(address, label, sizeof label);
    return label;
}

static void set_env(const char *key, const std::string &value) {
#ifdef _WIN32
    _putenv_s(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}

static void archive(const fs::path &path, const char *member, char payload, uint32_t directory = 40) {
    fs::create_directories(path.parent_path());
    std::vector<char> data(directory + 69);
    auto set = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<char>(value >> (i * 8));
    };
    std::memcpy(data.data(), "0TSR", 4);
    set(8, 6);
    set(20, directory);
    set(24, 1);
    std::memcpy(data.data() + directory, member, std::strlen(member));
    set(directory + 40, 1);
    set(directory + 44, 68);
    data.back() = payload;
    std::ofstream(path, std::ios::binary).write(data.data(), data.size());
}

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    const fs::path root = fs::u8path(argv[1]);
    fs::remove_all(root);
    const auto retail = root / "retail", active = root / "active";
    g_game_root = retail.u8string();
    set_env("EOD_MOD_DIR", active.u8string());
    set_env("EOD_HOST_TEXTURES", "1");
    archive(retail / "DATA/TRACKS/AFTON.TRK", "ground.tex", 'A');
    archive(active / "DATA/TRACKS/AFTON.TRK", "ground.tex", 'B');
    archive(retail / "DATA/TRACKS/ANOKA.TRK", "ground.tex", 'A');
    archive(active / "DATA/TRACKS/ANOKA.TRK", "ground.tex", 'A', 128); // repacked, same member
    archive(retail / "DATA/RES/COMMON.RES", "welch_eves.val", 'A');
    archive(active / "DATA/RES/COMMON.RES", "welch_eves.val", 'B');
    archive(retail / "DATA/CAR/GLADIA_1.CAR", "body.tex", 'A');
    archive(active / "DATA/CAR/GLADIA_1.CAR", "body.tex", 'B');
    archive(retail / "DATA/RES/UICAR.RES", "uicar_42.tex", 'A');
    archive(active / "DATA/RES/UICAR.RES", "uicar_42.tex", 'B');
    fs::create_directories(active / "HOSTTEX");
    const char header[] = {'E', 'O', 'D', 'H', 'T', 'X', '1', 0, 8, 0, 8, 0, 4, 0, 0, 0};
    std::array<char, 340> pixels{}; // 4-byte RGBA, 8x8 + 4x4 + 2x2 + 1x1
    std::ofstream(active / "HOSTTEX/ground.eodhtx", std::ios::binary)
        .write(header, sizeof header).write(pixels.data(), pixels.size());
    std::ofstream(active / "HOSTTEX/replacements.tsv")
        << "eod-host-textures 1\n123456789abcdef0\t*\t4\t4\tmissing.eodhtx\tDATA/TRACKS/ANOKA.TRK#ground.tex\tpack\n"
        << "123456789abcdef1\t*\t4\t4\tground.eodhtx\tdata/tracks/Dundas.trk#ground.tex\tpack\n";

    std::vector<uint8_t> ram(0x400000);
    g_ram = ram.data();
    const uint32_t stack = 0x100000, labels = 0x200000, eves = 0x003C4D08;
    g_ee.r[29].uw[0] = stack;
    ee_w32(0x003C4CF4, 4);
    text(eves + 8, "Afton");
    text(eves + 0x84 + 8, "Anoka");
    text(eves + 2 * 0x84 + 8, "Welch");
    text(eves + 3 * 0x84 + 8, "Dundas");
    text(labels, "Woodly");
    text(labels + 0x50, "Anoka");
    text(labels + 0xA0, "Welch");
    text(labels + 0xF0, "Dundas");
    for (const auto &which : {std::array<uint32_t, 4>{0x2C7ACC, 0x24, 0xC8, 0x16C},
                              {0x2C8304, 0x10, 0xC0, 0x374},
                              {0x2C8A20, 0x20, 0xD0, 0x264}}) {
        ee_w32(stack + which[3], 4);
        for (uint32_t row = 0; row < 4; ++row) {
            ee_w32(stack + which[1] + 4 * row, labels + 0x50 * row);
            ee_w32(stack + which[2] + 4 * row, row);
        }
        arcade_mod_badges_run_menu(which[0], [] {
            CHECK(read_text(0x200000) == "Woodly [MOD]");
            CHECK(read_text(0x200050) == "Anoka");
            CHECK(read_text(0x2000A0) == "Welch [MOD]");
            CHECK(read_text(0x2000F0) == "Dundas [MOD]");
            g_ee.r[2].uw[0] = 12;
        });
        CHECK(read_text(labels) == "Woodly");
        CHECK(ee_r32(stack + which[2] + 8) == 2);
        CHECK(g_ee.r[2].uw[0] == 12);
    }
    set_env("EOD_HOST_TEXTURES", "0");
    arcade_mod_badges_run_menu(0x2C8A20, [] {
        CHECK(read_text(0x200000) == "Woodly [MOD]");
        CHECK(read_text(0x2000F0) == "Dundas");
    });
    const uint32_t widget = 0x210000, chassis = 0x220000;
    g_ee.r[19].uw[0] = widget;
    g_ee.r[20].uw[0] = chassis;
    g_ee.r[4].uw[0] = widget + 0xA4;
    g_ee.r[5].uw[0] = widget + 0x6A;
    ee_w32(chassis + 4, 7);
    ee_w32(chassis + 12, 42);
    for (uint32_t caller : {0x2A7FF4u, 0x2B9EACu}) {
        g_ee.r[31].uw[0] = caller;
        text(widget + 0x49, "Gladiator");
        text(widget + 0x6A, "Paint 1");
        ee_w32(widget + 0x24, 0);
        f_002A8EF0();
        CHECK(read_text(widget + 0x49) == "Gladiator");
        CHECK(read_text(widget + 0x6A) == "Paint 1 [MOD]");
        CHECK(ee_r32(widget + 0xA8) == widget + 0x6A);
        text(widget + 0x49, "Algar");
        text(widget + 0x6A, "Paint 2");
        ee_w32(widget + 0x24, 1);
        f_002A8EF0();
        CHECK(read_text(widget + 0x6A) == "Paint 2 [MOD]");
        text(widget + 0x6A, "Paint 1");
        ee_w32(widget + 0x24, 0);
        f_002A8EF0();
        CHECK(read_text(widget + 0x6A) == "Paint 1");
    }
    set_env("EOD_MOD_DIR", "");
    arcade_mod_badges_run_menu(0x2C8A20, [] { CHECK(read_text(0x200000) == "Woodly"); });
    fs::remove_all(root);
    return failures ? 1 : 0;
}
