// Battle has no native AI controller: arena factory 0x138288 chooses NullDriver.
// Supply its missing input policy after the native AICar update, not
// at vblank. NullDriver does not join the native driver update list. Native cars retain physics, damage, recovery and weapon ownership.
#include "battle_driver.h"
#include "battle_bots.h"
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
    uint32_t car=0, target=4;
    float last=0, choose_at=0, blocked=0, stalled=0, reverse_until=0, escape_until=0;
    float anchor_x=0,anchor_z=0,sample_at=0,turn=1,trace_at=0;
};
std::array<State,4> states{};
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
        float best=1e30f;uint32_t chosen=4;
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
        float radius=g_config.chicken_radius_scale;
        if(!std::isfinite(radius) || radius<=0) radius=1;
        float safe_range=7.5f*radius+3.0f+speed*.15f;
        bool fire=ee_r32(iface+4)==2 && !reversing && range>safe_range && range<85 &&
            facing>.97f && aim_error<2.0f && std::fabs(elevation)<3.0f;
        trigger(iface,car,fire);
    }
}
} // namespace
void battle_driver_reset(){states={};navigation.reset();}
extern "C" void f_001A2D20(){
    uint32_t updated_car=g_ee.r[4].uw[0];
    f_001A2D20_orig();
    if(ee_r32(0x3CF19C)!=0x18000224 || !valid(updated_car,0x1C34)) return;
    uint32_t master=ee_r32(0x3CF194);
    if(!valid(master,0x5AC) || ee_r32(master)!=0x3F7770) return;
    uint32_t count=ee_r32(master+0x5A8);
    if(count<2 || count>4) return;
    for(uint32_t i=0;i<count;++i) {
        if(!battle_bots_is_ai_slot(i)) continue;
        uint32_t car=ee_r32(0x447458+i*4);
        if(car==updated_car){drive(i,car,master,count);return;}
    }
}
