// game_patches.cpp - values the port keeps in game memory every vblank.
//
// Widescreen: the game has its own display modes (its "display_type"
// setting: 0 normal, 1 widescreen, 2 letterbox), applied by f_0034D328: in
// widescreen the 3D view widens and the HUD and menus are laid out for a
// picture stretched to 16:9. The applied mode is at 0x3DD660 (the Options
// screen's own selection, 0x4A807C, only takes effect through that function).
// AspectRatio=16:9 / 4:3 in eod.ini keeps the mode at widescreen / normal;
// with "auto" the display follows the game's mode. Movies are 4:3 either way.
//
// EOD_POKE=addr=value,... (hex, 32-bit words) writes arbitrary words every
// vblank (testing patches).
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

#include "core/config.h"
#include "core/cpu.h"
#include "core/eodsave.h"
#include "core/log.h"
#include "core/memory.h"
#include "funcs.h"
#include "iop/iop.h"
#include "platform/platform.h"

extern std::atomic<bool> g_movie_playing;  // hle_movie.cpp

namespace {
// the applied display mode and what f_0034D328 derives from it
const uint32_t kDisplayMode = 0x3DD660;   // 0 normal, 1 widescreen, 2 letterbox
const uint32_t kViewScaleY = 0x3DD65C;    // 1.0, or 0.75 in letterbox
const uint32_t kAspectScale = 0x3DD658;   // 1.0, or 0.75 in widescreen and letterbox
const uint32_t kWideFlag = 0x3DD64C;      // 1 in widescreen
const uint32_t kLetterboxFlag = 0x3DD650; // 1 in letterbox

// The mode the settings force (-1: the game's own).
int forced_mode() {
    const std::string &a = g_config.aspect;
    return a == "16:9" ? 1 : a == "4:3" ? 0 : -1;
}

uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// What f_0034D328 does with the game's state (without its settings-table
// record and debug message), for switching while the game runs.
void set_mode(int mode) {
    ee_w32(kDisplayMode, (uint32_t)mode);
    ee_w32(kViewScaleY, fbits(mode == 2 ? 0.75f : 1.0f));
    ee_w32(kAspectScale, fbits(mode != 0 ? 0.75f : 1.0f));
    ee_w32(kWideFlag, mode == 1 ? 1 : 0);
    ee_w32(kLetterboxFlag, mode == 2 ? 1 : 0);
}

struct Poke {
    uint32_t addr, value;
};

std::vector<Poke> parse_pokes() {
    std::vector<Poke> out;
    const char *s = getenv("EOD_POKE");
    while (s && *s) {
        char *end;
        uint32_t a = (uint32_t)strtoul(s, &end, 16);
        if (*end != '=') break;
        uint32_t v = (uint32_t)strtoul(end + 1, &end, 16);
        out.push_back({a & ~3u, v});
        LOG(LOG_CORE, "EOD_POKE: %08x = %08x every vblank", a & ~3u, v);
        if (*end != ',') break;
        s = end + 1;
    }
    return out;
}
// ---------------------------------------------------------------- car looks
// The game's vehicle table (a vehicle is a model in one paint job): 84-byte
// records of index, id, model name[32], paint texture[16], ..., body colour,
// class. A career car (vehicle 800-929) can be drawn as any other vehicle --
// an Action mode paint job or a special vehicle -- by giving its record the
// other one's contents (from eod.ini [Career] CarLooks, see eodsave.h). The
// career car keeps its id, so its damage, upgrades and price stay its own.
const uint32_t kVehicleTable = 0x3CBEE4;  // pointer to the records
const uint32_t kVehicleCount = 0x439704;
const uint32_t kVehicleSize = 84;

void apply_car_looks() {
    static std::string parsed_from;
    static std::vector<std::pair<int, int>> looks;
    if (g_config.car_looks != parsed_from) {
        parsed_from = g_config.car_looks;
        looks = eodsave_parse_looks(parsed_from);
    }
    uint32_t base = ee_r32(kVehicleTable);
    int32_t n = (int32_t)ee_r32(kVehicleCount);
    if (looks.empty() || !base || n <= 0 || n > 2000) return;
    // the table as the game built it (records are copied from it)
    static uint32_t snap_base;
    static std::vector<uint8_t> pristine;
    if (base != snap_base || pristine.size() != (size_t)n * kVehicleSize) {
        snap_base = base;
        pristine.resize((size_t)n * kVehicleSize);
        ee_read_block(base, pristine.data(), (uint32_t)pristine.size());
    }
    auto find = [&](int id) -> int {
        for (int32_t i = 0; i < n; i++) {
            int32_t v;
            memcpy(&v, &pristine[(size_t)i * kVehicleSize + 4], 4);
            if (v == id) return i;
        }
        return -1;
    };
    for (const auto &l : looks) {
        int host = find(l.first), look = find(l.second);
        if (host < 0 || look < 0) continue;
        // everything but the index and id
        const uint8_t *want = &pristine[(size_t)look * kVehicleSize + 8];
        uint32_t at = base + (uint32_t)host * kVehicleSize + 8;
        uint8_t cur[kVehicleSize - 8];
        ee_read_block(at, cur, sizeof cur);
        if (memcmp(cur, want, sizeof cur) == 0) continue;
        ee_write_block(at, want, sizeof cur);
        static int logged;
        if (logged++ < 8) LOG(LOG_CORE, "career car %d looks like vehicle %d", l.first, l.second);
    }
}

}  // namespace

// The game applies its display mode (at start from its settings, and from the
// Options screen): the forced mode replaces the one asked for.
extern "C" void f_0034D328(void) {
    int want = forced_mode();
    if (want >= 0) g_ee.r[4].sd[0] = want;
    f_0034D328_orig();
}

static float ee_rf(uint32_t a) {
    uint32_t v = ee_r32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void ee_wf(uint32_t a, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    ee_w32(a, v);
}

// damage_scale from a car's physics.val (DATA/CAR/<model>.car; 8.3 names on the
// disc: Gladiator -> GLADIA_1.CAR), 1 when the file or the key is missing.
// physics.val is a "VALT" resource: sections of key/value string offsets.
static float car_file_damage_scale(const std::string &model) {
    std::vector<std::string> names = {model};
    if (model.size() > 8) names.push_back(model.substr(0, 6) + "_1");
    std::vector<uint8_t> d;
    for (const std::string &n : names) {
        char path[1024];
        std::string rel = "DATA/CAR/" + n + ".car";
        if (!platform_find_file(g_game_root.c_str(), rel.c_str(), path, sizeof path)) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        d.resize(size > 0 ? (size_t)size : 0);
        if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
        fclose(f);
        if (!d.empty()) break;
    }
    auto u32 = [&](size_t o) -> uint32_t {
        uint32_t v = 0;
        if (o + 4 <= d.size()) memcpy(&v, &d[o], 4);
        return v;
    };
    // the table of contents: name[32], tag, version, size, offset
    static const char kName[] = "physics.val";
    size_t at = 0;
    for (size_t o = 0; o + 48 <= d.size() && o < 0x4000; o++)
        if (!memcmp(&d[o], kName, sizeof kName) && !memcmp(&d[o + 32], "TLAV", 4)) {
            at = o;
            break;
        }
    if (!at) return 1.0f;
    size_t base = (size_t)u32(at + 44) + 0x80;
    auto str = [&](uint32_t o) -> std::string {
        size_t a = base + o, e = a;
        while (e < d.size() && d[e]) e++;
        return a < d.size() ? std::string((const char *)&d[a], e - a) : std::string();
    };
    uint32_t sections = u32(base + 8);
    for (uint32_t k = 0; k < sections && k < 64; k++) {
        size_t r = base + 16 + k * 28;
        if (str(u32(r + 4)) != "Chassis") continue;
        uint32_t pairs = u32(r + 12), list = u32(r + 16);
        for (uint32_t i = 0; i < pairs && i < 512; i++) {
            std::string key = str(u32(base + list + i * 8));
            for (char &c : key) c = (char)tolower((unsigned char)c);
            if (key != "damage_scale") continue;
            float v = (float)atof(str(u32(base + list + i * 8 + 4)).c_str());
            return v > 0.05f && v < 10.0f ? v : 1.0f;
        }
    }
    return 1.0f;
}

static float car_damage_scale(const std::string &model) {
    static std::map<std::string, float> cache;
    auto it = cache.find(model);
    if (it != cache.end()) return it->second;
    float v = car_file_damage_scale(model);
    cache[model] = v;
    return v;
}

// Car setup (car in a0, its chassis parameters in a1: the mass at +8, the
// model name at +0xAC). It sets the car's damage multiplier (+0x4B0):
// event tweak * 4000 lb / mass * the upgrades' factor (+0x2EC); the
// damage_scale of the car files (which the menu's durability ratings follow)
// is never used. CarDurability=1 multiplies it in.
extern "C" void f_0018AFE0(void) {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0], par = (uint32_t)g_ee.r[5].sd[0];
    f_0018AFE0_orig();
    char name[32] = {};
    for (int i = 0; i < 31; i++) name[i] = (char)ee_r8(par + 0xAC + i);
    float damage = ee_rf(car + 0x4B0), scale = g_config.car_durability ? car_damage_scale(name) : 1.0f;
    if (scale != 1.0f) ee_wf(car + 0x4B0, damage * scale);
    LOG(LOG_CORE, "car %s: mass %.0f, damage x%.3f%s", name, ee_rf(par + 8), damage * scale,
        scale != 1.0f ? " (with its damage_scale)" : "");
}

void game_patches_vblank() {
    // a forced mode the game hasn't applied (the setting changed while running):
    // set it (not during a movie, which restores the view it saved when it ends)
    int want = forced_mode();
    if (want >= 0 && !g_movie_playing && ee_r32(kDisplayMode) != (uint32_t)want) {
        LOG(LOG_CORE, "display mode %s", want ? "widescreen" : "normal");
        set_mode(want);
    }
    apply_car_looks();
    static const std::vector<Poke> pokes = parse_pokes();
    for (const Poke &p : pokes) ee_w32(p.addr, p.value);

    int aspect;
    if (g_config.aspect == "stretch")
        aspect = 2;
    else if (g_movie_playing)
        aspect = 0;
    else
        aspect = ee_r32(kDisplayMode) == 1 ? 1 : 0;  // (letterbox is drawn inside a 4:3 picture)
    static int last = -1;
    if (aspect != last) {
        LOG(LOG_CORE, "display aspect %s", aspect == 2 ? "stretched" : aspect == 1 ? "16:9" : "4:3");
        last = aspect;
        platform_set_aspect(aspect);
    }
}
