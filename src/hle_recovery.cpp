// SLUS-209.10: preserve native recovery detection and SELECT eligibility while
// disabling only the countdown's automatic relocation for local human cars.
#include <cstring>
#include "netplay_hooks.h"
#include "mutator_hooks.h"
#include "core/cpu.h"
#include "core/config.h"
extern "C" void f_001B8370_orig();
namespace {
constexpr uint32_t auto_reset=0x485098;
bool ram(uint32_t a,uint32_t n) { return a && !(a&3) && a<EE_RAM_SIZE && n<=EE_RAM_SIZE-a; }
float loadf(uint32_t a) { uint32_t u=MEM_LW(a); float f; std::memcpy(&f,&u,4); return f; }
void storef(uint32_t a,float f) { uint32_t u; std::memcpy(&u,&f,4); MEM_SW(a,u); }
}
extern "C" void f_001B8370() {
    const uint32_t timer=arg_u32(0),person=arg_u32(1);
    if (!ram(person,200) || !ram(timer,84)) {
        f_001B8370_orig(); return;
    }
    const uint32_t car=MEM_LW(person+4);
    const bool valid_pcar = ram(car,484) && MEM_LW(car+12)==0x50434152;
    const bool reversed_slot = valid_pcar && mutator_hooks::suppress_wrong_way(car);
    const bool manual_human = valid_pcar && MEM_LW(car+480)==0x50 &&
        netplay_hooks::rule(netplay_hooks::ManualRecovery, g_config.manual_recovery);
    if (!reversed_slot && !manual_human) {
        f_001B8370_orig(); return;
    }
    const uint32_t enabled=MEM_LW(auto_reset);
    MEM_SW(auto_reset,0);
    f_001B8370_orig();
    MEM_SW(auto_reset,enabled);
    if (reversed_slot) {
        // RF_SUICIDE / RF_WHIP_AROUND reversed slot: suppress false wrong-way
        // auto-reset and warning while leaving rollover/manual SELECT intact.
        MEM_SW(person+192,0);
        return;
    }
    // 001B888C writes this same deadline when a recovery condition is active.
    // Manual SELECT at 0018EA04 requires deadline>0 even after it has expired.
    // Keep that native eligibility; only the HUD's separate warning flag clears.
    if (enabled && MEM_LW(person+192))
        storef(person+196,fadd_ps2(loadf(timer+68),loadf(timer+72)));
    MEM_SW(person+192,0);
}
