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
#include <cmath>
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
#include "game_patches.h"
#include "iop/iop.h"
#include "platform/platform.h"

void game_patches_init(const Config &) {
    // Config parameters are accessed directly via g_config
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

// Native handbrake setter (SLUS_209.10:0x0018AA60, car + 0x1B78).
// Provided with weak linkage for standalone unit tests that do not link gen/rec_023.c.
extern "C" __attribute__((weak)) void f_0018AA60(void) {
    uint32_t car = (uint32_t)g_ee.r[4].sd[0];
    if (car && !(car & 3) && car < EE_RAM_SIZE - 0x2000) {
        ee_wf(car + 0x1B78, g_ee.fpr[12].f);
    }
}

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

// Chicken weapon explosion shockwave handler (called on chicken detonation at 0x0017E7FC).
// In EE register context:
//   $a0 = world position (const float*)
//   $a1 = attacker car index
//   $f12 = blast radius (default 7.5f meters)
//   $f13 = blast impulse force (default 25000.0f N)
extern "C" void f_0017DA58_orig(void);
extern "C" void f_0017DA58(void) {
    if (g_config.chicken_radius_scale > 0.0f && g_config.chicken_radius_scale != 1.0f) {
        g_ee.fpr[12].f *= g_config.chicken_radius_scale;
    }
    if (g_config.chicken_force_scale > 0.0f && g_config.chicken_force_scale != 1.0f) {
        g_ee.fpr[13].f *= g_config.chicken_force_scale;
    }
    f_0017DA58_orig();
}

// HumanDriver drive update (called each physics frame for local player cars):
//   $a0 = VehicleController* veh_ctrl
//     veh_ctrl + 0x38 = Car*
//     veh_ctrl + 0x3C = Driver*
extern "C" void f_0018F2A8_orig(void);
extern "C" void f_0018F2A8(void) {
    uint32_t veh_ctrl = (uint32_t)g_ee.r[4].sd[0];
    f_0018F2A8_orig();

    if (g_config.turbo_scale > 1.0f && veh_ctrl && !(veh_ctrl & 3) && veh_ctrl < EE_RAM_SIZE) {
        uint32_t cheats = ee_r32(0x003C4190);
        if ((cheats & 0x1) && (cheats & 0x2)) { // Master cheat bit + Turbo Boost bit
            uint32_t driver = ee_r32(veh_ctrl + 0x3C);
            if (driver && !(driver & 3) && driver < EE_RAM_SIZE) {
                uint32_t vtbl = ee_r32(driver);
                if (vtbl && !(vtbl & 3) && vtbl < EE_RAM_SIZE) {
                    uint32_t is_l2_fn = ee_r32(vtbl + 0x2C);
                    if (is_l2_fn) {
                        uint32_t car = ee_r32(veh_ctrl + 0x38);
                        if (car && !(car & 3) && car < EE_RAM_SIZE - 0x200) {
                            EEState saved_ee = g_ee;
                            g_ee.r[4].sd[0] = driver;
                            ee_call(is_l2_fn);
                            int l2_pressed = (g_ee.r[2].sd[0] != 0);
                            g_ee = saved_ee;

                            if (l2_pressed) {
                                float fwd_x = ee_rf(car + 0x7C);
                                float fwd_y = ee_rf(car + 0x80);
                                float fwd_z = ee_rf(car + 0x84);

                                float vel_x = ee_rf(car + 0x104);
                                float vel_y = ee_rf(car + 0x108);
                                float vel_z = ee_rf(car + 0x10C);

                                float forward_speed = vel_x * fwd_x + vel_y * fwd_y + vel_z * fwd_z;
                                float speed_cap = 44.444447f * g_config.turbo_scale;

                                if (forward_speed < speed_cap) {
                                    // Apply scaled turbo acceleration along the car's forward vector
                                    float extra_accel = (g_config.turbo_scale - 1.0f) * 20.0f;
                                    float dt = 1.0f / 60.0f;
                                    ee_wf(car + 0x104, vel_x + fwd_x * extra_accel * dt);
                                    ee_wf(car + 0x108, vel_y + fwd_y * extra_accel * dt);
                                    ee_wf(car + 0x10C, vel_z + fwd_z * extra_accel * dt);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

// Eve loader (called when loading eves for the active mode at 0x003C4C04).
// Mode: 0x12F = Arcade/Action, 0x130 = Career, 0x131 = Multi.
// When SinglePlayerBattle is enabled, we append a Battle contest (event_type = 0x18000224)
// to Action and Career mode Eves, exposing Battle Mode directly in single-player event menus.
extern "C" void f_001244A8_orig(void);
extern "C" void f_001244A8(void) {
    f_001244A8_orig();

    uint32_t mode = ee_r32(0x003C4C04);
    uint32_t num_eves = ee_r32(0x003C4CF4);
    LOG(LOG_CORE, "f_001244A8: sp_battle_mode=%d, mode=0x%x, num_eves=%u",
        g_config.sp_battle_mode, mode, num_eves);

    if (!g_config.sp_battle_mode) return;

    if (mode != 0x12F && mode != 0x130) return; // Action mode (0x12F) or Career mode (0x130)
    uint32_t eves_base = 0x003C4D08;      // stride 0x84
    uint32_t contests_base = 0x003C61A8;  // stride 0x28
    uint32_t num_contests = ee_r32(0x003C4CF8);

    if (num_eves == 0 || num_eves > 50) return;

    if (num_eves == 17) {
        for (uint32_t i = 0; i < 17; i++) {
            uint32_t ep = eves_base + i * 0x84;
            char name[32] = {};
            ee_read_string(ep + 0x08, name, sizeof(name));
            LOG(LOG_CORE, "[EVE %u] name='%s', contests=%u", i, name, ee_r32(ep + 0x70));
        }
    }
    for (uint32_t i = 0; i < num_eves && i < 2; i++) {
        uint32_t eve_ptr = eves_base + i * 0x84;
        uint32_t cur_contests = ee_r32(eve_ptr + 0x70);
        if (cur_contests == 0 || cur_contests >= 40) continue;

        // Ensure this eve does not already have Battle
        bool has_battle = false;
        for (uint32_t c = 0; c < num_contests; c++) {
            uint32_t cp = contests_base + c * 0x28;
            if (ee_r32(cp + 4) == i && ee_r32(cp + 8) == 0x18000224) {
                has_battle = true;
                break;
            }
        }
        if (has_battle) continue;

        uint32_t c_ptr = contests_base + num_contests * 0x28;
        ee_w32(c_ptr + 0x00, num_contests); // global contest index (offset 0)
        ee_w32(c_ptr + 0x04, i);            // eve index (offset 4)
        ee_w32(c_ptr + 0x08, 0x18000224);   // event_type: BATTLE! (offset 8)
        ee_w32(c_ptr + 0x0C, 1);            // laps: 1 (offset 12)
        ee_w32(c_ptr + 0x10, 4);            // max_cars: 4 (offset 16, within BattleMaster bounds)
        ee_w32(c_ptr + 0x14, 0xFFFFFFFF);   // terrain_mask (offset 20)
        ee_w32(c_ptr + 0x18, 1);            // rounds: 1 (offset 24)
        ee_w32(c_ptr + 0x1C, 500);          // prize points (offset 28)
        ee_w32(c_ptr + 0x20, 0);            // name string ptr (offset 32)
        ee_w32(c_ptr + 0x24, 1);            // enabled flag: 1 (offset 36: MUST BE 1!)

        ee_w32(eve_ptr + 0x70, cur_contests + 1);
        num_contests++;
        ee_w32(0x003C4CF8, num_contests);
        LOG(LOG_CORE, "SP Battle: Injected Battle Mode contest into Eve %u (now %u contests)", i, cur_contests + 1);

        if (i == 0) {
            // Configure Contest 0 as Battle Mode so Event 1 of 5 in Eve 0 is Battle Mode
            uint32_t c0_ptr = contests_base + 0 * 0x28;
            ee_w32(c0_ptr + 0x08, 0x18000224);
            ee_w32(c0_ptr + 0x10, 4);
            ee_w32(c0_ptr + 0x18, 1);
            LOG(LOG_CORE, "SP Battle: Configured Eve 0 Contest 0 as Battle Mode (Event 1)");
        }
    }

    if (g_config.sp_battle_mode) {
        // Also configure Dennison's first contest (Contest 181, Eve 10) as Battle Mode
        // so Event 1 of Dennison loads directly into Battle Mode!
        for (uint32_t c = 0; c < num_contests; c++) {
            uint32_t cp = contests_base + c * 0x28;
            if (ee_r32(cp + 4) == 10) {
                ee_w32(cp + 0x08, 0x18000224);
                ee_w32(cp + 0x0C, 1);
                ee_w32(cp + 0x10, 4);
                ee_w32(cp + 0x18, 1);
                LOG(LOG_CORE, "SP Battle: Configured Dennison first contest (index %u) as Battle Mode (Event 1)", c);
                break;
            }
        }
    }
}

static uint16_t s_suppressed_pad0_buttons = 0;

uint16_t game_patches_suppressed_buttons() {
    return s_suppressed_pad0_buttons;
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

    // ---------------------------------------------------------------- gameplay cheats
    // Out of the box: unlock all feat prizes (0x003D18E4) and career completed feats (0x003D18E0)
    ee_w32(0x003D18E0, 0xFFFFFFFF);
    ee_w32(0x003D18E4, 0xFFFFFFFF);

    // Live controller combo detection (L1 + R1 + D-Pad, L3 + R3, Select hold)
    HostPadState pad;
    platform_get_pad(0, &pad);
    uint16_t b = pad.connected ? pad.buttons : 0;
    static uint16_t s_prev_pad = 0;
    uint16_t pressed = b & ~s_prev_pad;
    s_prev_pad = b;

    static int s_logged_event_types = 0;
    if (!s_logged_event_types && ee_r32(0x003C3A70) != 0) {
        s_logged_event_types = 1;
        for (uint32_t i = 0; i < 30; i++) {
            uint32_t p = 0x003C3A70 + i * 12;
            uint32_t type = ee_r32(p + 0);
            if (!type) break;
            uint32_t name_ptr = ee_r32(p + 4);
            uint32_t disp_ptr = ee_r32(p + 8);
            char name[64] = {}, disp[64] = {};
            if (name_ptr) ee_read_string(name_ptr, name, sizeof(name));
            if (disp_ptr) ee_read_string(disp_ptr, disp, sizeof(disp));
            LOG(LOG_CORE, "[EVENT_TABLE] %u: type=0x%08x, name='%s', disp='%s'", i, type, name, disp);
        }
    }

    static int s_logged_eve0 = 0;
    if (!s_logged_eve0 && ee_r32(0x003C4CF8) >= 200) {
        s_logged_eve0 = 1;
        uint32_t contests_base = 0x003C61A8;
        uint32_t total = ee_r32(0x003C4CF8);
        for (uint32_t c = 0; c < total; c++) {
            uint32_t cp = contests_base + c * 0x28;
            if (ee_r32(cp + 4) == 10) {
                LOG(LOG_CORE, "[DENNISON CONTEST %u] type=0x%08x, enabled=%u, laps=%u, max_cars=%u",
                    c, ee_r32(cp + 8), ee_r32(cp + 0x24), ee_r32(cp + 0x0C), ee_r32(cp + 0x10));
            }
        }
    }

    static int s_logged_c0 = 0;
    if ((s_logged_c0++ % 300) == 0) {
        uint32_t c0 = 0x003C61A8;
        uint32_t c181 = 0x003C61A8 + 181 * 0x28;
        LOG(LOG_CORE, "[VBLANK] contest 0 type=0x%08x, contest 181 type=0x%08x, num_contests=%u",
            ee_r32(c0 + 8), ee_r32(c181 + 8), ee_r32(0x003C4CF8));
    }

    if (g_config.sp_battle_mode) {
        uint32_t num_contests = ee_r32(0x003C4CF8);
        if (num_contests > 181) {
            uint32_t c181 = 0x003C61A8 + 181 * 0x28;
            if (ee_r32(c181 + 4) == 10 && ee_r32(c181 + 8) != 0x18000224) {
                ee_w32(c181 + 0x08, 0x18000224);
                ee_w32(c181 + 0x0C, 1);
                ee_w32(c181 + 0x10, 4);
                ee_w32(c181 + 0x18, 1);
            }
        }
        uint32_t contests_base = 0x003C61A8;
        for (uint32_t c = 0; c < num_contests && c < 320; c++) {
            uint32_t cp = contests_base + c * 0x28;
            if (ee_r32(cp + 8) == 0x18000224) { // Battle mode contest!
                uint32_t curr = ee_r32(cp + 0x20);
                int depth = 0;
                while (curr != 0 && (curr & 3) == 0 && curr < EE_RAM_SIZE && depth < 8) {
                    uint32_t pairs = ee_r32(curr + 12);
                    uint32_t list = ee_r32(curr + 16);
                    uint32_t parent = ee_r32(curr + 20);
                    if (pairs < 64 && list < EE_RAM_SIZE && (list & 3) == 0) {
                        for (uint32_t i = 0; i < pairs; i++) {
                            uint32_t kptr = ee_r32(list + i * 8);
                            uint32_t vptr = ee_r32(list + i * 8 + 4);
                            if (kptr < EE_RAM_SIZE && vptr < EE_RAM_SIZE) {
                                char kstr[32] = {};
                                ee_read_string(kptr, kstr, sizeof(kstr));
                                if (strcmp(kstr, "external_table") == 0) {
                                    char vstr[32] = {};
                                    ee_read_string(vptr, vstr, sizeof(vstr));
                                    if (strcmp(vstr, "c_battle") != 0) {
                                        LOG(LOG_CORE, "SP Battle: Patching contest %u layout '%s' -> 'c_battle' at 0x%08x", c, vstr, vptr);
                                        memcpy(ee_ptr(vptr), "c_battle\0", 9);
                                    }
                                }
                            }
                        }
                    }
                    curr = parent;
                    depth++;
                }
            }
        }
    }

    bool l1_r1_held = (b & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1);
    platform_set_mod_overlay(l1_r1_held);

    // Track button suppression to protect driving and pause menus
    s_suppressed_pad0_buttons = 0;

    // Track holding Select (30 vblanks = ~500ms quick chicken toggle)
    static int s_select_hold_frames = 0;
    if (!l1_r1_held && (b & PAD_SELECT)) {
        s_select_hold_frames++;
        if (s_select_hold_frames == 30) {
            g_config.cheat_chickens = !g_config.cheat_chickens;
            platform_show_hud_banner(g_config.cheat_chickens ? "★ CHICKEN FIGHT: ON (Press L2 to Fire) ★" : "★ CHICKEN FIGHT: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(g_config.cheat_chickens);
            LOG(LOG_CORE, "[HOLD SELECT] Chicken Fight: %s", g_config.cheat_chickens ? "ON" : "OFF");
        }
        if (s_select_hold_frames >= 30) {
            s_suppressed_pad0_buttons |= PAD_SELECT;
        }
    } else {
        s_select_hold_frames = 0;
    }

    if (l1_r1_held) {
        // While L1+R1 modifier is held, suppress chord and navigation buttons from being sent to the game
        s_suppressed_pad0_buttons |= (PAD_L1 | PAD_R1 | PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_TRIANGLE | PAD_SELECT);

        if (pressed & PAD_UP) {
            g_config.cheat_chickens = !g_config.cheat_chickens;
            platform_show_hud_banner(g_config.cheat_chickens ? "★ CHICKEN FIGHT: ON (Press L2 to Fire) ★" : "★ CHICKEN FIGHT: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(g_config.cheat_chickens);
            LOG(LOG_CORE, "[MOD COMBO] Chicken Fight: %s", g_config.cheat_chickens ? "ON" : "OFF");
        }
        if (pressed & PAD_RIGHT) {
            g_config.cheat_turbo = !g_config.cheat_turbo;
            if (g_config.cheat_turbo && g_config.turbo_scale <= 1.0f) g_config.turbo_scale = 1.75f;
            platform_show_hud_banner(g_config.cheat_turbo ? "★ TURBO BOOST: ON (Hold L2) ★" : "★ TURBO BOOST: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(g_config.cheat_turbo);
            LOG(LOG_CORE, "[MOD COMBO] Turbo Boost: %s", g_config.cheat_turbo ? "ON" : "OFF");
        }
        if (pressed & PAD_DOWN) {
            uint32_t c = ee_r32(0x003C4190);
            bool on = !(c & 0x80);
            if (on) {
                ee_w32(0x003C4190, (c | 0x89) & ~0x10);
                g_config.cheat_chickens = false;
            } else {
                ee_w32(0x003C4190, c & ~0x80);
            }
            platform_show_hud_banner(on ? "★ RAINING CHICKENS: ON ★" : "★ RAINING CHICKENS: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(on);
            LOG(LOG_CORE, "[MOD COMBO] Raining Chickens: %s", on ? "ON" : "OFF");
        }
        if (pressed & PAD_LEFT) {
            uint32_t c = ee_r32(0x003C4190);
            bool on = !(c & 0x20);
            if (on) ee_w32(0x003C4190, c | 0x25); // AI Vengeance (0x20) + Hardcore (0x04) + Master (0x01)
            else ee_w32(0x003C4190, c & ~0x24);
            platform_show_hud_banner(on ? "★ AI VENGEANCE & HARDCORE: ON ★" : "★ AI VENGEANCE & HARDCORE: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(on);
            LOG(LOG_CORE, "[MOD COMBO] AI Vengeance: %s", on ? "ON" : "OFF");
        }
        if (pressed & PAD_TRIANGLE) {
            bool on = !(g_config.cheat_chickens && g_config.cheat_turbo);
            g_config.cheat_chickens = on;
            g_config.cheat_turbo = on;
            if (on && g_config.turbo_scale <= 1.0f) g_config.turbo_scale = 1.75f;
            platform_show_hud_banner(on ? "★ CHAOS MODE: ON (Chickens + Turbo) ★" : "★ CHAOS MODE: OFF ★");
            platform_haptic_pulse(0, 150, 1, 200);
            platform_audio_feedback(on);
            LOG(LOG_CORE, "[MOD COMBO] Chaos Mode: %s", on ? "ON" : "OFF");
        }
        if (pressed & PAD_SELECT) {
            g_config.cheat_chickens = !g_config.cheat_chickens;
            platform_show_hud_banner(g_config.cheat_chickens ? "★ CHICKEN FIGHT: ON (Press L2 to Fire) ★" : "★ CHICKEN FIGHT: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(g_config.cheat_chickens);
            LOG(LOG_CORE, "[MOD COMBO L1+R1+SELECT] Chicken Fight: %s", g_config.cheat_chickens ? "ON" : "OFF");
        }
    } else if ((pressed & (PAD_L3 | PAD_R3)) && (b & (PAD_L3 | PAD_R3)) == (PAD_L3 | PAD_R3)) {
        s_suppressed_pad0_buttons |= (PAD_L3 | PAD_R3);
        g_config.cheat_chickens = !g_config.cheat_chickens;
        platform_show_hud_banner(g_config.cheat_chickens ? "★ CHICKEN FIGHT: ON (Press L2 to Fire) ★" : "★ CHICKEN FIGHT: OFF ★");
        platform_haptic_pulse(0, 150, 1, 180);
        platform_audio_feedback(g_config.cheat_chickens);
        LOG(LOG_CORE, "[STICK CLICK] Chicken Fight: %s", g_config.cheat_chickens ? "ON" : "OFF");
    }

    uint32_t cheats = ee_r32(0x003C4190);
    if (g_config.career_cheats || g_config.cheat_chickens || g_config.cheat_turbo) {
        cheats |= 0x1; // Master cheat enable
    }
    if (g_config.cheat_turbo) {
        cheats |= 0x3; // Bit 1: Turbo boost enabled + Bit 0: Master enable
    } else {
        cheats &= ~0x2;
    }
    if (g_config.cheat_chickens) {
        cheats |= 0x19; // Bits 3 & 4 (0x18): Chicken weapon forward firing + Bit 0: Master enable
        cheats &= ~0x80; // Clear raining chickens to avoid projectile slot conflict
    } else {
        cheats &= ~0x10;
        if (!(cheats & 0x80)) cheats &= ~0x8;
    }
    ee_w32(0x003C4190, cheats);

    // ---------------------------------------------------------------- Battle Mode (M_CORE)
    if (g_config.sp_battle_mode) {
        uint32_t cur_event_type = ee_r32(0x003CF19C);
        if (cur_event_type == 0x18000224) { // Battle mode active!
            uint32_t bm = ee_r32(0x003CF194);
            uint32_t num_cars = ee_r32(0x0048522C);
            if (num_cars == 0 && bm && !(bm & 3) && bm < EE_RAM_SIZE - 0x600) {
                num_cars = ee_r32(bm + 0x5A8);
            }

            // Cache active car pointers (f_00179A08 or 0x00447458)
            uint32_t cars[4] = {0};
            if (num_cars > 0 && num_cars <= 4) {
                for (uint32_t i = 0; i < num_cars; i++) {
                    EEState saved_ee = g_ee;
                    g_ee.r[4].sd[0] = i;
                    f_00179A08();
                    cars[i] = (uint32_t)g_ee.r[2].sd[0];
                    g_ee = saved_ee;
                    if (!cars[i] || (cars[i] & 3) || cars[i] >= EE_RAM_SIZE - 0x200) {
                        cars[i] = ee_r32(0x00447458 + i * 4);
                    }
                }
            }

            // R4: Arena Infield Respawn Relocation
            // Relocate BattleMaster respawn positions (+0x10 + i * 0x44) to radius 10-18m
            // within the Demolition Derby infield circle.
            if (bm && !(bm & 3) && bm < EE_RAM_SIZE - 0x600) {
                uint32_t num_respawns = ee_r32(bm + 0x450);
                uint32_t respawn_count = (num_respawns >= 4 && num_respawns <= 16) ? num_respawns : 16;
                for (uint32_t i = 0; i < respawn_count; i++) {
                    float angle = (float)i * (2.0f * 3.14159265f / (float)respawn_count);
                    float r = 12.0f + ((i % 4) * 1.5f); // 12.0m, 13.5m, 15.0m, 16.5m (within 10-18m)
                    float rx = cosf(angle) * r;
                    float rz = sinf(angle) * r;
                    uint32_t pt = bm + 0x10 + i * 0x44;

                    // Position (X, Y, Z)
                    ee_wf(pt + 0x24, rx);
                    ee_wf(pt + 0x2C, rz);

                    // Facing towards center of arena
                    float fx = -cosf(angle);
                    float fz = -sinf(angle);
                    ee_wf(pt + 0x00, -fz);   // right X
                    ee_wf(pt + 0x04, 0.0f);  // right Y
                    ee_wf(pt + 0x08, fx);    // right Z
                    ee_wf(pt + 0x0C, 0.0f);  // up X
                    ee_wf(pt + 0x10, 1.0f);  // up Y
                    ee_wf(pt + 0x14, 0.0f);  // up Z
                    ee_wf(pt + 0x18, fx);    // forward X
                    ee_wf(pt + 0x1C, 0.0f);  // forward Y
                    ee_wf(pt + 0x20, fz);    // forward Z
                }
            }

            // R4: Kinetic Boundary Clamping & Velocity Reflection at radius ~24.5m
            for (uint32_t i = 0; i < num_cars && i < 4; i++) {
                uint32_t c = cars[i];
                if (!c || (c & 3) || c >= EE_RAM_SIZE - 0x200) continue;
                float px = ee_rf(c + 0x88);
                float pz = ee_rf(c + 0x90);
                float d = sqrtf(px * px + pz * pz);
                if (d > 24.5f) {
                    float inv_d = 1.0f / d;
                    float nx = -px * inv_d;
                    float nz = -pz * inv_d;
                    float vx = ee_rf(c + 0x104);
                    float vz = ee_rf(c + 0x10C);
                    float dot = vx * nx + vz * nz;
                    if (dot < 0.0f) {
                        // Velocity heading outward: reflect inward with 0.8 restitution (1.8 * dot * n)
                        ee_wf(c + 0x104, vx - 1.8f * dot * nx);
                        ee_wf(c + 0x10C, vz - 1.8f * dot * nz);
                    }
                    // Clamp position to boundary perimeter (24.0m)
                    ee_wf(c + 0x88, -nx * 24.0f);
                    ee_wf(c + 0x90, -nz * 24.0f);
                }
            }

            // R3: Guard AI updates with battle_master->event_state == 1
            // event_state: 0 = countdown, 1 = active combat, 2 = match finished / victory
            uint32_t event_state = (bm && !(bm & 3) && bm < EE_RAM_SIZE - 0x600) ? ee_r32(bm + 0x04) : 0;
            if (event_state == 1 && num_cars > 1 && num_cars <= 4) {
                struct AiVehicleState {
                    int stuck_frames = 0;
                    int reverse_frames = 0;
                    int fire_cooldown = 0;
                    float prev_x = 0.0f;
                    float prev_z = 0.0f;
                    int reverse_dir = 1;
                };
                static AiVehicleState s_ai_state[4];

                // R3: Dynamic participant scaling: read num_players from 0x003C41AC
                uint32_t num_players = ee_r32(0x003C41AC);
                if (num_players < 1 || num_players > 4) num_players = 1;

                // Loop ai_idx from num_players to num_cars (supports 1P+3AI, 2P+2AI, 3P+1AI, 4P)
                for (uint32_t ai_idx = num_players; ai_idx < num_cars; ai_idx++) {
                    uint32_t ai_car = cars[ai_idx];
                    if (!ai_car || (ai_car & 3) || ai_car >= EE_RAM_SIZE - 0x200) continue;

                    AiVehicleState &st = s_ai_state[ai_idx];

                    float ax = ee_rf(ai_car + 0x88);
                    float az = ee_rf(ai_car + 0x90);
                    float avx = ee_rf(ai_car + 0x104);
                    float avz = ee_rf(ai_car + 0x10C);
                    float aspeed = sqrtf(avx * avx + avz * avz);

                    // R2: Choose closest opponent target (skipping destroyed cars with damage >= 1.0f)
                    uint32_t target_car = 0;
                    float min_dist = 1e9f;
                    for (uint32_t t_idx = 0; t_idx < num_cars; t_idx++) {
                        if (t_idx == ai_idx) continue;
                        uint32_t tc = cars[t_idx];
                        if (!tc || (tc & 3) || tc >= EE_RAM_SIZE - 0x200) continue;

                        float damage = ee_rf(tc + 0x1BEC);
                        if (damage >= 1.0f) continue; // Skip wrecked/respawning vehicles

                        float tpx = ee_rf(tc + 0x88);
                        float tpz = ee_rf(tc + 0x90);
                        float d = sqrtf((tpx - ax) * (tpx - ax) + (tpz - az) * (tpz - az));
                        if (d < min_dist) {
                            min_dist = d;
                            target_car = tc;
                        }
                    }

                    // Fallback to any opponent if all are wrecked
                    if (!target_car) {
                        for (uint32_t t_idx = 0; t_idx < num_cars; t_idx++) {
                            if (t_idx == ai_idx) continue;
                            uint32_t tc = cars[t_idx];
                            if (!tc || (tc & 3) || tc >= EE_RAM_SIZE - 0x200) continue;
                            float tpx = ee_rf(tc + 0x88);
                            float tpz = ee_rf(tc + 0x90);
                            float d = sqrtf((tpx - ax) * (tpx - ax) + (tpz - az) * (tpz - az));
                            if (d < min_dist) {
                                min_dist = d;
                                target_car = tc;
                            }
                        }
                    }

                    if (!target_car) continue;

                    float tpx = ee_rf(target_car + 0x88);
                    float tpz = ee_rf(target_car + 0x90);
                    float tvx = ee_rf(target_car + 0x104);
                    float tvz = ee_rf(target_car + 0x10C);

                    // R2 Lead-Pursuit Interception: calculate lead point from target position and velocity
                    float closing_speed = aspeed + 12.0f;
                    float lead_time = min_dist / closing_speed;
                    if (lead_time > 1.2f) lead_time = 1.2f;
                    if (lead_time < 0.0f) lead_time = 0.0f;

                    float lead_x = tpx + tvx * lead_time;
                    float lead_z = tpz + tvz * lead_time;

                    float ldx = lead_x - ax;
                    float ldz = lead_z - az;
                    float lead_dist = sqrtf(ldx * ldx + ldz * ldz);
                    float inv_ld = lead_dist > 0.1f ? (1.0f / lead_dist) : 1.0f;

                    // Vehicle orientation vectors
                    float rx = ee_rf(ai_car + 0x64), rz = ee_rf(ai_car + 0x6C); // right
                    float fx = ee_rf(ai_car + 0x7C), fz = ee_rf(ai_car + 0x84); // forward

                    float steer_dot = (ldx * rx + ldz * rz) * inv_ld;
                    float face_dot = (ldx * fx + ldz * fz) * inv_ld;

                    // Aggressive pursuit steering with clamping
                    float steer = steer_dot * 2.2f;
                    if (steer > 1.0f) steer = 1.0f;
                    if (steer < -1.0f) steer = -1.0f;

                    // R2: Power-Slide Ramming via native handbrake f_0018AA60() on car + 0x1B78
                    // On sharp turns at high speed (face_dot < 0.15f && aspeed > 9.0f), set handbrake to 0.90f
                    // and footbrake to 0.0f to break rear traction for T-bone collisions while maintaining throttle.
                    // Reset handbrake to 0.0f when exiting the turn.
                    float handbrake = 0.0f;
                    float footbrake = 0.0f;
                    if (face_dot < 0.15f && aspeed > 9.0f) {
                        handbrake = 0.90f;
                        footbrake = 0.0f;
                    }

                    // R2: Barrier Unstick Recovery: detect pin condition (<0.35m displacement over 40 frames)
                    float moved_d = sqrtf((ax - st.prev_x) * (ax - st.prev_x) + (az - st.prev_z) * (az - st.prev_z));
                    st.prev_x = ax;
                    st.prev_z = az;

                    int gear = 1;
                    float gas = 1.0f;

                    if (st.reverse_frames > 0) {
                        st.reverse_frames--;
                        gear = -1; // Reverse gear
                        gas = 1.0f;
                        footbrake = 0.0f;
                        handbrake = 0.0f;
                        steer = st.reverse_dir * 0.9f;
                    } else {
                        if (aspeed < 1.4f && moved_d < 0.35f) {
                            st.stuck_frames++;
                            if (st.stuck_frames > 40) {
                                st.reverse_frames = 55; // 55 frames reverse (<2.0s recovery)
                                st.stuck_frames = 0;
                                st.reverse_dir = (steer_dot >= 0.0f) ? -1 : 1;
                            }
                        } else {
                            st.stuck_frames = 0;
                        }
                    }

                    // Apply AI controls
                    EEState saved_ee = g_ee;
                    g_ee.r[4].sd[0] = ai_car;
                    g_ee.r[5].sd[0] = gear;
                    f_0018AA50(); // gear (1 or -1)

                    g_ee.r[4].sd[0] = ai_car;
                    g_ee.fpr[12].f = steer;
                    f_0018AA80(); // steer [-1.0f, +1.0f]

                    g_ee.r[4].sd[0] = ai_car;
                    g_ee.fpr[12].f = gas;
                    f_0018AA70(); // gas [0.0f, 1.0f]

                    g_ee.r[4].sd[0] = ai_car;
                    g_ee.fpr[12].f = footbrake;
                    f_0018AA68(); // footbrake [0.0f, 1.0f]

                    g_ee.r[4].sd[0] = ai_car;
                    g_ee.fpr[12].f = handbrake;
                    f_0018AA60(); // handbrake [0.0f, 1.0f] (car + 0x1B78)
                    g_ee = saved_ee;

                    // AI combat behavior in Demolition Derby/Battle Mode relies on aggressive
                    // lead-pursuit ramming, power-slides, and barrier unstick maneuvers.
                }
            }
        }
    }

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
