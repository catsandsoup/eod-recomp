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

// Native drivetrain engagement setter (SLUS_209.10:0x0018AA60, car + 0x1B78).
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
        bool found = false;
        const char *env_mod = getenv("EOD_MOD_DIR");
        if (env_mod && *env_mod)
            found = platform_find_file(env_mod, rel.c_str(), path, sizeof path);
        if (!found && !platform_find_file(g_game_root.c_str(), rel.c_str(), path, sizeof path)) continue;
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

// Native damage callbacks are separate from collision impulse resolution:
// 189CD8 invokes CA98 after collision response; CA98 owns panel deformation,
// damage state transitions and destruction. Suppress those callbacks only.
// Native car setup copies the driver identity to car+1E0 (50 = human).
// This covers P1-P4 regardless of world slot, leaving AI damage native.
// Existing damage and already detached panels are not repaired.
static bool invincible_human(uint32_t car) {
    return g_config.cheat_invincible && car && !(car & 3) &&
        car <= EE_RAM_SIZE - 0x1E4 && ee_r32(car + 0x1E0) == 0x50;
}
extern "C" void f_0018AD20_orig(void);
extern "C" void f_0018CA98_orig(void);
extern "C" void f_0018AD20(void) {
    if (!invincible_human(g_ee.r[4].uw[0])) f_0018AD20_orig();
}
extern "C" void f_0018CA98(void) {
    if (!invincible_human(g_ee.r[4].uw[0])) f_0018CA98_orig();
}

// The native world update has one inline damage write for the timed-damage
// feat (master + bit 0x40), bypassing the collision damage callbacks. Keep the
// rest of that update intact, and preserve changes to every unrelated flag.
extern "C" void f_001B5840_orig(void);
extern "C" void f_001B5840(void) {
    const uint32_t flags = ee_r32(0x003C4190);
    // The inline feat targets the selected driver, not every world car:
    // 1B5C00 indexes 485230 by 485280; driver+4 is its car.
    const uint32_t index = ee_r32(0x00485280), count = ee_r32(0x0048522C);
    const uint32_t driver = index < count && count <= 16 ? ee_r32(0x00485230 + index * 4) : 0;
    const uint32_t car = driver && !(driver & 3) && driver <= EE_RAM_SIZE - 8 ? ee_r32(driver + 4) : 0;
    const bool suppress_timer = (flags & 0x40) && invincible_human(car);
    if (suppress_timer) ee_w32(0x003C4190, flags & ~0x40u);
    f_001B5840_orig();
    if (suppress_timer) ee_w32(0x003C4190, ee_r32(0x003C4190) | 0x40u);
}

// Chicken weapon explosion shockwave handler (called on chicken detonation at 0x0017E7FC).
// In EE register context:
//   $a0 = world position (const float*)
//   $a1 = attacker car index
//   $f12 = blast radius (default 7.5f meters)
//   $f13 = blast impulse force (default 25000.0f N)
extern "C" void f_0017DA58_orig(void);
extern "C" void f_0017DA58(void) {
    static const bool trace = getenv("EOD_BATTLE_TRACE") != nullptr;
    if (trace) {
        const uint32_t position = g_ee.r[4].uw[0];
        if (position && !(position & 3) && position <= EE_RAM_SIZE - 12)
            LOG(LOG_CORE, "[BATTLE_BLAST] t=%.3f attacker=%u pos=%.3f,%.3f,%.3f radius=%.3f force=%.3f",
                ee_rf(0x003CDACC), g_ee.r[5].uw[0], ee_rf(position),
                ee_rf(position + 4), ee_rf(position + 8),
                g_ee.fpr[12].f, g_ee.fpr[13].f);
    }
    if (g_config.chicken_radius_scale > 0.0f && g_config.chicken_radius_scale != 1.0f) {
        g_ee.fpr[12].f *= g_config.chicken_radius_scale;
    }
    if (g_config.chicken_force_scale > 0.0f && g_config.chicken_force_scale != 1.0f) {
        g_ee.fpr[13].f *= g_config.chicken_force_scale;
    }
    f_0017DA58_orig();
}

// HumanDriver must run for every human, including P2-P4. Its native update
// owns weapon input, release-to-rearm, shifting, death and countdown handling.
extern "C" void f_0018F2A8_orig(void);
extern "C" void f_0018F2A8(void) {
    const uint32_t controller = (uint32_t)g_ee.r[4].ud[0];
    const bool valid = controller && !(controller & 3) && controller <= EE_RAM_SIZE - 0x44;
    const uint32_t car = valid ? ee_r32(controller + 0x38) : 0;
    const uint32_t driver = valid ? ee_r32(controller + 0x3C) : 0;
    f_0018F2A8_orig();
    if (g_config.cheat_turbo && car && !(car & 3) && car <= EE_RAM_SIZE - 0x2000 &&
        driver && !(driver & 3) && driver <= EE_RAM_SIZE - 4) {
        uint32_t vtbl = ee_r32(driver);
        uint32_t is_l2_fn = (vtbl && !(vtbl & 3) && vtbl <= EE_RAM_SIZE - 0x30) ? ee_r32(vtbl + 0x2C) : 0;
        int l2_pressed = 0;
        if (is_l2_fn) {
            EEState saved_ee = g_ee;
            g_ee.r[4].sd[0] = driver;
            ee_call(is_l2_fn);
            l2_pressed = (g_ee.r[2].sd[0] != 0);
            g_ee = saved_ee;
        }

        const uint32_t cheats = ee_r32(0x003C4190);
        if (l2_pressed) {
            // Turbo boost while L2 is held:
            if (g_config.cheat_turbo && (cheats & 0x3) == 0x3 && g_config.turbo_scale > 1.0f) {
                float fwd_x = ee_rf(car + 0x7C);
                float fwd_y = ee_rf(car + 0x80);
                float fwd_z = ee_rf(car + 0x84);

                float vel_x = ee_rf(car + 0x104);
                float vel_y = ee_rf(car + 0x108);
                float vel_z = ee_rf(car + 0x10C);

                float forward_speed = vel_x * fwd_x + vel_y * fwd_y + vel_z * fwd_z;
                float speed_cap = 44.444447f * g_config.turbo_scale;

                if (forward_speed < speed_cap) {
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

// Opt-in evidence only: sample native Battle state without changing simulation.
static void trace_battle() {
    static const bool enabled = getenv("EOD_BATTLE_TRACE") != nullptr;
    static unsigned tick;
    if (!enabled || ee_r32(0x003CF19C) != 0x18000224 || (++tick % 10)) return;
    uint32_t master = ee_r32(0x003CF194);
    if (!master || (master & 3) || master > EE_RAM_SIZE - 0x5AC) return;
    const uint32_t count = ee_r32(master + 0x5A8);
    LOG(LOG_CORE, "[BATTLE] t=%.3f phase=%u humans=%u cars=%u gates=%u,%u bound=%08x flags=%x",
        ee_rf(0x003CDACC), ee_r32(master + 4), ee_r32(0x003C41AC), count,
        ee_r32(0x0044744C), ee_r32(0x00447454), ee_r32(0x003CD580), ee_r32(0x003C4190));
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        uint32_t car = ee_r32(0x00447458 + i * 4);
        if (!car || (car & 3) || car > EE_RAM_SIZE - 0x1C34) continue;
        LOG(LOG_CORE, "[BATTLE_CAR] slot=%u car=%08x pos=%.3f,%.3f,%.3f vel=%.3f,%.3f,%.3f steer=%.3f gas=%.3f gear=%d dead=%u score=%d",
            i, car, ee_rf(car+0x88), ee_rf(car+0x8C), ee_rf(car+0x90),
            ee_rf(car+0x104), ee_rf(car+0x108), ee_rf(car+0x10C),
            ee_rf(car+0x1B6C), ee_rf(car+0x1B70), (int)ee_r32(car+0x1B90),
            ee_r32(master+0x458+i*0x50), (int)ee_r32(master+0x460+i*0x50)-(int)ee_r32(master+0x468+i*0x50));
    }
    for (uint32_t i = 0; i < ee_r32(0x003CE4B8) && i < 16; ++i) {
        uint32_t row = 0x003CE3F8 + i * 12, iface = ee_r32(row + 8);
        if (iface < 0x1DC || (iface & 3) || iface > EE_RAM_SIZE - 0x4C) continue;
        uint32_t body = iface - 0x1DC;
        LOG(LOG_CORE, "[BATTLE_SHOT] slot=%u iface=%08x state=%u flight=%u pos=%.3f,%.3f,%.3f vel=%.3f,%.3f,%.3f effects=%08x,%08x",
            ee_r32(row), iface, ee_r32(body+0x1E0), ee_r32(body+0x1F4),
            ee_rf(body+0x88), ee_rf(body+0x8C), ee_rf(body+0x90),
            ee_rf(body+0x104), ee_rf(body+0x108), ee_rf(body+0x10C),
            ee_r32(body+0x210), ee_r32(body+0x214));
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
    trace_battle();
    static const std::vector<Poke> pokes = parse_pokes();
    for (const Poke &p : pokes) ee_w32(p.addr, p.value);

    // ---------------------------------------------------------------- gameplay cheats
    // Mirror the retail unlock routines without invoking the full career cheat
    // (2B6DB8 also changes money, cars and career state). 205218 supplies feat
    // prizes; bit 7 of their union with completed feats makes Home visible in
    // 2C71B0 -> 2E0AF8 -> 205368, including its native multiplayer Battle.
    // 2CBC18 writes exactly seven prize-mask bytes; do not overwrite byte +7.
    // 2CD418 supplies the event-selection override consumed by 2CD4A8.
    if (g_config.unlock_all) {
        ee_w32(0x003D18E0, 0xFFFFFFFF);
        ee_w32(0x003D18E4, 0xFFFFFFFF);
        for (uint32_t i = 0; i < 7; ++i) ee_w8(0x004A8AF8 + i, 0xFF);
        ee_w32(0x003D8180, 1);
        // Prize masks do not unlock the separate points-gated TRACK/CAR
        // roster. 12C940 loads up to 32 rows; 12CAB8 marks row i unlocked
        // at base+0x10+i*0x10. Track picker 12CC40 consumes that exact flag.
        const uint32_t unlock_count = ee_r32(0x003CBF48);
        if (unlock_count <= 32)
            for (uint32_t i = 0; i < unlock_count; ++i)
                ee_w32(0x003CBF58 + i * 0x10, 1);
    }

    // Live controller combo detection (L1 + R1 + D-Pad, L3 + R3, Select hold)
    HostPadState pad;
    platform_get_pad(0, &pad);
    uint16_t b = pad.connected ? pad.buttons : 0;
    static uint16_t s_prev_pad = 0;
    uint16_t pressed = b & ~s_prev_pad;
    s_prev_pad = b;

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
        s_suppressed_pad0_buttons |= (PAD_L1 | PAD_R1 | PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT | PAD_TRIANGLE | PAD_SELECT | PAD_SQUARE);

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
        if (pressed & PAD_SQUARE) {
            g_config.cheat_invincible = !g_config.cheat_invincible;
            platform_show_hud_banner(g_config.cheat_invincible ? "★ INVINCIBILITY: ON ★" : "★ INVINCIBILITY: OFF ★");
            platform_haptic_pulse(0, 150, 1, 180);
            platform_audio_feedback(g_config.cheat_invincible);
            LOG(LOG_CORE, "[MOD COMBO L1+R1+SQUARE] Invincibility: %s", g_config.cheat_invincible ? "ON" : "OFF");
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
    static bool turbo_mod_was_enabled = false;
    if (g_config.cheat_turbo) {
        cheats |= 0x3; // Bit 1: Turbo boost enabled + Bit 0: Master enable
    } else if (turbo_mod_was_enabled) {
        cheats &= ~0x2u;
    }
    turbo_mod_was_enabled = g_config.cheat_turbo;
    static bool chicken_mod_was_enabled = false;
    const bool battle = ee_r32(0x003CF19C) == 0x18000224;
    if (g_config.cheat_chickens) {
        // Native trigger 0x17E000: bit 0x10 is the feat mortar, NOT weapon enable.
        cheats = (cheats | 0x09) & ~0x90u;
    } else if (chicken_mod_was_enabled) {
        if (!battle && !(cheats & 0x80)) cheats &= ~0x08u;
    }
    chicken_mod_was_enabled = g_config.cheat_chickens;
    // Battle owns its weapon even when the optional Chicken Fight mod is off.
    // Leave native feat selections alone when the mod has never been enabled.
    ee_w32(0x003C4190, cheats);

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
