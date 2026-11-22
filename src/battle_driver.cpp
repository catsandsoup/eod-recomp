#include "netplay_hooks.h"
// Battle has no native AI controller: arena factory 0x138288 chooses NullDriver.
// Supply its missing input policy after the native AICar update, not
// at vblank. NullDriver does not join the native driver update list. Native cars retain physics, damage, recovery and weapon ownership.
#include "battle_driver.h"
#include "battle_bots.h"
#include "netplay_puppet.h"
#include "battle_layout.h"
#include "battle_navigation.h"
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>

extern "C" void f_001A2D20_orig();
extern "C" void f_0017E000();
extern "C" void f_001671E8();
namespace {
bool valid(uint32_t a,uint32_t size) { return a && !(a&3) && size<=EE_RAM_SIZE && a<=EE_RAM_SIZE-size; }
float rf(uint32_t a){uint32_t u=ee_r32(a);float f;std::memcpy(&f,&u,4);return f;}
void wf(uint32_t a,float f){uint32_t u;std::memcpy(&u,&f,4);ee_w32(a,u);}
struct State {
    uint32_t car=0, target=UINT32_MAX;
    float last=0, choose_at=0, blocked=0, stalled=0, reverse_until=0, escape_until=0;
    float anchor_x=0,anchor_z=0,sample_at=0,turn=1,trace_at=0;
};
std::array<State,battle_layout::kCapacity> states{};
battle_navigation::Navigation navigation;
uint32_t navigation_word(void *,uint32_t address) { return ee_r32(address); }
void controls(uint32_t car,float steer,float gas,float brake,float handbrake,int gear) {
    // Native setters 0x18AA50/60/68/70/78. -10 requests automatic forward gears.
    wf(car+0x1B6C,steer);wf(car+0x1B70,gas);wf(car+0x1B74,brake);
    // Native Derby 0x1975B0 keeps drivetrain engagement at one.
    // 0x1B7C is the rear-wheel brake input; zero engagement prevents drive.
    wf(car+0x1B78,1);wf(car+0x1B7C,handbrake);ee_w32(car+0x1B90,(uint32_t)gear);
}
uint32_t weapon_interface(uint32_t owner) {
    uint32_t count=ee_r32(0x3CE4B8);
    if(count>16) return 0;
    for(uint32_t i=0;i<count;++i) {
        uint32_t row=0x3CE3F8+i*12,iface=ee_r32(row+8);
        if(ee_r32(row)!=owner || ee_r32(row+4)!=0 || iface<0x1DC || !valid(iface-0x1DC,0x228)) continue;
        if(ee_r32(iface)==0x3F9528 && ee_r32(iface+12)==owner) return iface;
    }
    return 0;
}
void trigger(uint32_t iface,uint32_t car,bool down) {
    uint32_t sp=g_ee.r[29].uw[0];
    if(sp<0x1000 || !valid(sp-0x40,0x40) || (sp&15)) return;
    EEState saved=g_ee;
    std::array<uint8_t,64> scratch;
    ee_read_block(sp-0x40,scratch.data(),64);
    // Reserve input ABOVE the callee's stack frame, not among its saved registers.
    ee_w32(sp-0x10,down?1:0);ee_w32(sp-0x0C,0);
    g_ee.r[29].ud[0]=sp-0x40;
    g_ee.r[4].ud[0]=iface;g_ee.r[5].ud[0]=car;g_ee.r[6].ud[0]=sp-0x10;
    f_0017E000();
    ee_write_block(sp-0x40,scratch.data(),64);
    g_ee=saved;
}
void drive(uint32_t slot,uint32_t car,uint32_t master,uint32_t count) {
    auto &s=states[slot];
    float now=rf(0x3CDACC),x=rf(car+0x88),z=rf(car+0x90);
    if(!std::isfinite(now)||!std::isfinite(x)||!std::isfinite(z)) return;
    if(s.car!=car || now<s.last) s={};
    if(!s.car) {s.car=car;s.last=now;s.anchor_x=x;s.anchor_z=z;s.sample_at=now;}
    float dt=std::clamp(now-s.last,0.0f,0.1f);s.last=now;
    // Same event-active conditions used by native arena drivers; never start the
    // event here. Native 0x1DA618/0x167FE0 own the countdown->combat transition.
    const bool active=ee_r32(master+4)==1 && ee_r32(0x3CF198) && !ee_r32(0x44744C) && !ee_r32(0x447454);
    const bool dead=ee_r32(master+0x458+slot*0x50)!=0;
    if(!active || dead) {
        controls(car,0,0,1,0,-10);s.blocked=0;s.stalled=0;s.reverse_until=0;s.escape_until=0;s.sample_at=now;s.anchor_x=x;s.anchor_z=z;
        if(active && dead && ee_r32(master+0x450)>0 && ee_r32(master+0x450)<=16) {
            // A human presses SELECT to request this native two-second respawn.
            // Bots make the same request; native code owns timer/reset/spawn.
            const EEState saved=g_ee;
            g_ee.r[4].ud[0]=master;g_ee.r[5].ud[0]=slot;
            f_001671E8();g_ee=saved;
        }
        return;
    }
    auto opponent=[&](uint32_t index)->uint32_t {
        if(index>=count || index==slot || ee_r32(master+0x458+index*0x50)) return 0;
        uint32_t c=ee_r32(0x447458+index*4);
        return valid(c,0x1C34) && std::isfinite(rf(c+0x88)) && std::isfinite(rf(c+0x90))?c:0;
    };
    uint32_t target=opponent(s.target);
    if(!target || now>=s.choose_at) {
        float best=1e30f;uint32_t chosen=UINT32_MAX;
        for(uint32_t i=0;i<count;++i) if(uint32_t c=opponent(i)) {
            float dx=rf(c+0x88)-x,dz=rf(c+0x90)-z;
            float score=dx*dx+dz*dz;
            if(i==s.target) score*=.7f; // avoid switching on every near pass
            if(score<best){best=score;chosen=i;}
        }
        s.target=chosen;s.choose_at=now+.65f;target=opponent(chosen);
    }
    if(!target){controls(car,0,0,1,0,-10);s.blocked=0;return;}
    float vx=rf(car+0x104),vz=rf(car+0x10C),speed=std::hypot(vx,vz);
    float rx=rf(car+0x64),rz=rf(car+0x6C),fx=rf(car+0x7C),fz=rf(car+0x84);
    float tx=rf(target+0x88),tz=rf(target+0x90),tvx=rf(target+0x104),tvz=rf(target+0x10C);
    float range=std::hypot(tx-x,tz-z),lead=std::min(range/(speed+12),1.0f);
    float dx=tx+tvx*lead-x,dz=tz+tvz*lead-z;
    battle_navigation::Point waypoint;
    const bool course_available=navigation.refresh({nullptr,navigation_word});
    const bool routing=course_available &&
        navigation.waypoint(slot,s.target,{x,z},{fx,fz},speed,{tx,tz},waypoint);
    if(routing) {dx=waypoint.x-x;dz=waypoint.z-z;}
    static const bool trace=std::getenv("EOD_BATTLE_TRACE")!=nullptr;
    if(trace && now>=s.trace_at) {
        std::fprintf(stderr,"[BATTLE_ROUTE] t=%.3f slot=%u course=%u routing=%u target=%u\n",
                     now,slot,unsigned(course_available),unsigned(routing),s.target);
        s.trace_at=now+1;
    }
    // Routing changes only steering's aim. Range, lead and the native weapon
    // trigger below continue to refer to the actual opponent.
    if(now<s.escape_until){dx+=rx*s.turn*10;dz+=rz*s.turn*10;}
    float distance=std::max(std::hypot(dx,dz),.01f);
    float lateral=(dx*rx+dz*rz)/distance,front=(dx*fx+dz*fz)/distance;
    if(!std::isfinite(speed)||!std::isfinite(lateral)||!std::isfinite(front)) {controls(car,0,0,1,0,-10);return;}
    float angle=std::atan2(lateral,front);
    // A target exactly behind still requires a turn; a dot-only steering law
    // produces zero steering there and drives away forever.
    float steer=std::clamp(angle*1.25f,-1.0f,1.0f);
    if(std::fabs(lateral)<.02f && front<0) steer=(slot&1)?1.0f:-1.0f;
    if(now>=s.escape_until) {
        if(speed<1.5f) s.blocked+=dt;else s.blocked=0;
        if(now-s.sample_at>=.4f){
            if(std::hypot(x-s.anchor_x,z-s.anchor_z)<.5f) s.stalled+=now-s.sample_at;
            else s.stalled=0;
            s.anchor_x=x;s.anchor_z=z;s.sample_at=now;
        }
        if(std::max(s.blocked,s.stalled)>=1.5f){s.turn=steer>=0?-1:1;s.reverse_until=now+1.7f;s.escape_until=now+3.0f;s.blocked=0;s.stalled=0;}
    }
    if(now<s.escape_until) {
        s.blocked=0;s.stalled=0;s.anchor_x=x;s.anchor_z=z;s.sample_at=now;
    }
    bool reversing=now<s.reverse_until;
    float gas=1,brake=0,handbrake=0;
    if(reversing){steer=s.turn*.8f;}
    else {
        // Drive at the opponent, control speed for the turn, and accelerate
        // into aligned contact. Brief handbraking helps turn without permanent spin.
        float desired=front>.8f?25.0f:(front>.2f?12.0f:6.0f);
        if(routing) desired=std::min(desired,18.0f);
        if(speed>desired){gas=.1f;brake=.45f;}
        if(front<.1f && speed>9){handbrake=.35f;}
    }
    controls(car,steer,gas,brake,handbrake,reversing?-1:-10);
    if((ee_r32(0x3C4190)&9)!=9) return;
    if(uint32_t iface=weapon_interface(slot)) {
        float flight=range/(28+speed);
        float ax=tx+tvx*flight-x,az=tz+tvz*flight-z;
        float aim_length=std::max(std::hypot(ax,az),.01f);
        float facing=(ax*fx+az*fz)/aim_length;
        float aim_error=std::fabs(ax*rx+az*rz);
        float elevation=rf(target+0x8C)-rf(car+0x8C);
        // Native state 1 needs a released input first; do not edit reload state.
        float radius=netplay_hooks::tuned(netplay_hooks::ChickenSize,g_config.chicken_radius_scale);
        if(!std::isfinite(radius) || radius<=0) radius=1;
        float safe_range=7.5f*radius+3.0f+speed*.15f;
        bool fire=ee_r32(iface+4)==2 && !reversing && range>safe_range && range<85 &&
            facing>.97f && aim_error<2.0f && std::fabs(elevation)<3.0f;
        trigger(iface,car,fire);
    }
}
} // namespace

namespace {
struct PieLiveControl {
    bool ai_autodrive = false;
    bool free_cam = false;
    int camera_view = -1;
    float speed_boost = 1.0f;
    float grip_boost = 1.0f;
    float ride_height_delta = 0.0f;
    float applied_ride_height = 0.0f;
    uint32_t ride_height_car = 0;
    int arcade_mode = 0; // 0=none, 1=tag, 2=cat_mouse, 3=juggernaut
    uint32_t it_slot = 0;
    float last_tag_time = 0.0f;
};
PieLiveControl g_pie_ctrl{};
uint32_t g_pie_poll_counter = 0;
bool g_spawn_pos_applied = false;

int parse_arcade_mode_value(const char *v) {
    if (!v) return 0;
    while (*v == ' ' || *v == '\t') ++v;
    if (std::strncmp(v, "tag", 3) == 0) return 1;
    if (std::strncmp(v, "cat_mouse", 9) == 0) return 2;
    if (std::strncmp(v, "juggernaut", 10) == 0) return 3;
    if (std::strncmp(v, "off", 3) == 0 || std::strncmp(v, "none", 4) == 0 ||
        std::strncmp(v, "free_roam", 9) == 0) return 0;
    return std::clamp(std::atoi(v), 0, 3);
}

void poll_pie_live_control() {
    static bool init_done = false;
    if (!init_done) {
        init_done = true;
        if (const char *ad = std::getenv("EOD_AI_AUTODRIVE")) g_pie_ctrl.ai_autodrive = (ad[0] == '1');
        if (const char *fc = std::getenv("EOD_FREE_CAM")) {
            g_pie_ctrl.free_cam = (fc[0] == '1');
        }
        if (const char *am = std::getenv("EOD_ARCADE_MODE")) {
            g_pie_ctrl.arcade_mode = parse_arcade_mode_value(am);
        }
    }
    const char *ctrl_path = std::getenv("EOD_PIE_CONTROL_FILE");
    if (!ctrl_path || !*ctrl_path) return;
    if ((++g_pie_poll_counter & 15) != 0) return;
    FILE *fp = std::fopen(ctrl_path, "rb");
    if (!fp) return;
    char buf[512] = {};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    if (!n) return;
    auto parse_kv = [&](const char *key) -> const char * {
        const char *p = std::strstr(buf, key);
        if (!p) return nullptr;
        p += std::strlen(key);
        while (*p == ' ' || *p == '=') ++p;
        return p;
    };
    if (const char *v = parse_kv("ai_autodrive")) g_pie_ctrl.ai_autodrive = (std::atoi(v) != 0);
    if (const char *v = parse_kv("free_cam")) g_pie_ctrl.free_cam = (std::atoi(v) != 0);
    if (const char *v = parse_kv("camera_view")) g_pie_ctrl.camera_view = std::atoi(v);
    if (const char *v = parse_kv("speed_boost")) g_pie_ctrl.speed_boost = std::clamp((float)std::atof(v), 0.3f, 3.0f);
    if (const char *v = parse_kv("grip_boost")) g_pie_ctrl.grip_boost = std::clamp((float)std::atof(v), 0.3f, 3.0f);
    if (const char *v = parse_kv("ride_height_delta")) g_pie_ctrl.ride_height_delta = std::clamp((float)std::atof(v), -1.5f, 2.5f);
    if (const char *v = parse_kv("arcade_mode")) g_pie_ctrl.arcade_mode = parse_arcade_mode_value(v);
}

void maybe_apply_spawn_pos_override(uint32_t car) {
    if (g_spawn_pos_applied) return;
    const char *sp = std::getenv("EOD_SPAWN_POS");
    if (!sp || !*sp) {
        g_spawn_pos_applied = true;
        return;
    }
    float sx = 0.0f, sy = 0.0f, sz = 0.0f, heading_deg = 0.0f;
    const int got = std::sscanf(sp, "%f,%f,%f,%f", &sx, &sy, &sz, &heading_deg);
    if (got < 3 || !std::isfinite(sx) || !std::isfinite(sy) || !std::isfinite(sz)) {
        g_spawn_pos_applied = true;
        return;
    }
    g_spawn_pos_applied = true;
    // Car transform matrix (3x3 rows + pos at +0x64..+0x90) and linear velocity (+0x104..+0x10C)
    wf(car + 0x88, sx);
    wf(car + 0x8C, sy);
    wf(car + 0x90, sz);
    wf(car + 0x104, 0.0f);
    wf(car + 0x108, 0.0f);
    wf(car + 0x10C, 0.0f);
    if (got >= 4 && std::isfinite(heading_deg)) {
        const float rad = heading_deg * 0.01745329252f;
        const float c = std::cos(rad), s = std::sin(rad);
        // Right = (c, 0, -s), Up = (0, 1, 0), Forward = (s, 0, c)
        wf(car + 0x64, c);  wf(car + 0x68, 0.0f); wf(car + 0x6C, -s);
        wf(car + 0x70, 0.0f); wf(car + 0x74, 1.0f); wf(car + 0x78, 0.0f);
        wf(car + 0x7C, s);  wf(car + 0x80, 0.0f); wf(car + 0x84, c);
    }
}

void sync_opt_terrain_mask() {
    static int env_mask = -2;
    if (env_mask == -2) {
        const char *e = std::getenv("EOD_OPT_TERRAIN_MASK");
        env_mask = (e && *e) ? (int)std::strtol(e, nullptr, 0) : -1;
    }
    if (env_mask >= 0) {
        ee_w32(0x003D1C70, (uint32_t)env_mask);
        return;
    }
    // Check if modded track.bsc has the All-Events optional terrain sync marker (table2[0].aux[1] == 0xFE)
    const uint32_t scol = ee_r32(0x003D2098);
    if (valid(scol, 12)) {
        const uint32_t t2 = ee_r32(scol + 8);
        if (valid(t2, 20) && ee_r8(t2 + 19) == 0xFE) {
            ee_w32(0x003D1C70, 0xFFFFu);
        }
    }
}

void drive_player_along_ilin(uint32_t car) {
    const float x = rf(car + 0x88), z = rf(car + 0x90);
    const float vx = rf(car + 0x104), vz = rf(car + 0x10C), speed = std::hypot(vx, vz);
    const float rx = rf(car + 0x64), rz = rf(car + 0x6C), fx = rf(car + 0x7C), fz = rf(car + 0x84);
    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(fx) || !std::isfinite(fz)) return;

    // Locate active ILIN resource in cache table 0x3CD190..0x3CD1B4
    const uint32_t entries = ee_r32(0x3CD1B4);
    uint32_t resource = 0;
    if (entries >= 1 && entries <= 8) {
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t p = 0x3CD190 + i * 12;
            const uint32_t cand = ee_r32(p);
            if (valid(cand, 48) && ee_r32(cand) == 0x104F128B && ee_r32(cand + 8) == 60) {
                resource = cand;
                if (ee_r32(p + 4) == 0x101) break;
            }
        }
    }
    float tx = x + fx * 25.0f, tz = z + fz * 25.0f;
    if (resource) {
        const uint32_t count = ee_r32(resource + 28) >> 16;
        const uint32_t root = ee_r32(resource + 32);
        if (count >= 3 && count <= 4096 && valid(root, count * 60)) {
            uint32_t best_idx = 0;
            float best_score = 1e30f;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t np = root + i * 60;
                const float nx = rf(np), nz = rf(np + 4);
                const float d2 = (nx - x) * (nx - x) + (nz - z) * (nz - z);
                if (d2 < best_score) {
                    best_score = d2;
                    best_idx = i;
                }
            }
            const uint32_t target_idx = (best_idx + 2) % count;
            const uint32_t tp = root + target_idx * 60;
            tx = rf(tp);
            tz = rf(tp + 4);
        }
    }
    const float dx = tx - x, dz = tz - z;
    const float dist = std::max(std::hypot(dx, dz), 0.01f);
    const float lateral = (dx * rx + dz * rz) / dist;
    const float front = (dx * fx + dz * fz) / dist;
    const float angle = std::atan2(lateral, front);
    const float steer = std::clamp(angle * 1.35f, -1.0f, 1.0f);
    const float desired = front > 0.85f ? 32.0f : (front > 0.35f ? 18.0f : 9.0f);
    const float gas = (speed > desired) ? 0.15f : 1.0f;
    const float brake = (speed > desired + 4.0f) ? 0.45f : 0.0f;
    const float handbrake = (front < 0.1f && speed > 10.0f) ? 0.35f : 0.0f;
    controls(car, steer, gas, brake, handbrake, -10);
}

void apply_live_car_tuner_and_arcade(uint32_t player_car) {
    if (g_pie_ctrl.camera_view >= 0 && g_pie_ctrl.camera_view <= 18) {
        ee_w32(0x3C411C, (uint32_t)g_pie_ctrl.camera_view);
        ee_w32(0x48CB90, (uint32_t)g_pie_ctrl.camera_view);
    }
    const bool is_it = g_pie_ctrl.arcade_mode == 1 &&
        ee_r32(0x447458 + g_pie_ctrl.it_slot * 4) == player_car;
    const bool is_juggernaut = (g_pie_ctrl.arcade_mode == 3);
    const float boost = g_pie_ctrl.speed_boost * (is_it ? 1.18f : (is_juggernaut ? 1.10f : 1.0f));
    if (std::fabs(boost - 1.0f) > 0.02f) {
        const float gas = rf(player_car + 0x1B70);
        if (gas > 0.2f) {
            const float fx = rf(player_car + 0x7C), fz = rf(player_car + 0x84);
            const float vx = rf(player_car + 0x104), vz = rf(player_car + 0x10C);
            const float spd = std::hypot(vx, vz);
            if (spd < 45.0f * boost && std::isfinite(fx) && std::isfinite(fz)) {
                const float accel = (boost - 1.0f) * 0.18f * gas;
                wf(player_car + 0x104, vx + fx * accel);
                wf(player_car + 0x10C, vz + fz * accel);
            }
        }
    }
    if (std::fabs(g_pie_ctrl.grip_boost - 1.0f) > 0.02f) {
        const float rx = rf(player_car + 0x64), rz = rf(player_car + 0x6C);
        const float vx = rf(player_car + 0x104), vz = rf(player_car + 0x10C);
        if (std::isfinite(rx) && std::isfinite(rz) && std::isfinite(vx) && std::isfinite(vz)) {
            const float lat = vx * rx + vz * rz;
            // grip_boost > 1 damps lateral slip; < 1 increases drift slip slightly
            const float damp = std::clamp((g_pie_ctrl.grip_boost - 1.0f) * 0.08f, -0.04f, 0.20f);
            wf(player_car + 0x104, vx - rx * lat * damp);
            wf(player_car + 0x10C, vz - rz * lat * damp);
        }
    }
    if (g_pie_ctrl.ride_height_car != player_car) {
        g_pie_ctrl.ride_height_car = player_car;
        g_pie_ctrl.applied_ride_height = 0.0f;
    }
    const float rh_diff = g_pie_ctrl.ride_height_delta - g_pie_ctrl.applied_ride_height;
    if (std::fabs(rh_diff) > 0.01f) {
        const float y = rf(player_car + 0x8C);
        if (std::isfinite(y)) {
            wf(player_car + 0x8C, y + rh_diff);
            g_pie_ctrl.applied_ride_height = g_pie_ctrl.ride_height_delta;
        }
    }
    // Tag / Infected contact check across active cars
    if (g_pie_ctrl.arcade_mode == 1) {
        const float now = rf(0x3CDACC);
        if (now - g_pie_ctrl.last_tag_time > 2.0f) {
            const uint32_t it_car = ee_r32(0x447458 + g_pie_ctrl.it_slot * 4);
            if (valid(it_car, 0x1C34)) {
                const float ix = rf(it_car + 0x88), iz = rf(it_car + 0x90);
                for (uint32_t s = 0; s < battle_layout::kCapacity; ++s) {
                    if (s == g_pie_ctrl.it_slot) continue;
                    const uint32_t oc = ee_r32(0x447458 + s * 4);
                    if (!valid(oc, 0x1C34)) continue;
                    const float d = std::hypot(rf(oc + 0x88) - ix, rf(oc + 0x90) - iz);
                    if (d > 0.1f && d < 5.2f) {
                        g_pie_ctrl.it_slot = s;
                        g_pie_ctrl.last_tag_time = now;
                        break;
                    }
                }
            }
        }
    }
}
} // namespace

void battle_driver_reset(){
    states={};
    navigation.reset();
    g_spawn_pos_applied=false;
    g_pie_ctrl.applied_ride_height=0.0f;
    g_pie_ctrl.ride_height_car=0;
}

bool battle_driver_is_free_cam() {
    poll_pie_live_control();
    return g_pie_ctrl.free_cam;
}

void pie_update_player(uint32_t car) {
    if (!valid(car, 0x1C34) || netplay_car::is_puppet(car)) return;
    sync_opt_terrain_mask();
    poll_pie_live_control();
    maybe_apply_spawn_pos_override(car);
    // The HumanDriver writes the pedals and steering even when the pad is idle.
    // Apply ILIN input here after those writes and before the next physics step.
    if (g_pie_ctrl.ai_autodrive && !ee_r32(0x44744C) && !ee_r32(0x447454)) {
        drive_player_along_ilin(car);
    }
    apply_live_car_tuner_and_arcade(car);
}

extern "C" void f_001A2D20(){
    uint32_t updated_car=g_ee.r[4].uw[0];
    if(netplay_car::is_puppet(updated_car)) return;  // remote player's car: state comes from the network
    f_001A2D20_orig();
    if(!valid(updated_car,0x1C34)) return;
    sync_opt_terrain_mask();
    if(ee_r32(0x3CF19C)!=0x18000224) return;
    uint32_t master=ee_r32(0x3CF194);
    if(!valid(master,battle_layout::kMasterSize) || ee_r32(master)!=battle_layout::kMasterVtable) return;
    uint32_t count=ee_r32(master+battle_layout::kCount);
    if(count<2 || count>battle_layout::kCapacity) return;
    for(uint32_t i=0;i<count;++i) {
        if(!battle_bots_is_ai_slot(i)) continue;
        uint32_t car=ee_r32(0x447458+i*4);
        if(car==updated_car){drive(i,car,master,count);return;}
    }
}
