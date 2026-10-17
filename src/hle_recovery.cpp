// SLUS-209.10: preserve native recovery detection and SELECT eligibility while
// disabling only the countdown's automatic relocation for local human cars.
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
    if (!g_config.manual_recovery || !ram(person,200) || !ram(timer,84)) {
        f_001B8370_orig(); return;
    }
    const uint32_t car=MEM_LW(person+4);
    // PCAR local-player type 0x50 is constructed by 001D92A8. AI constructor
    // 001D92A0 uses 0x51; remote/special types also retain native recovery.
    if (!ram(car,484) || MEM_LW(car+12)!=0x50434152 || MEM_LW(car+480)!=0x50) {
        f_001B8370_orig(); return;
    }
    const uint32_t enabled=MEM_LW(auto_reset);
    MEM_SW(auto_reset,0);
    f_001B8370_orig();
    MEM_SW(auto_reset,enabled);
    // 001B888C writes this same deadline when a recovery condition is active.
    // Manual SELECT at 0018EA04 requires deadline>0 even after it has expired.
    // Keep that native eligibility; only the HUD's separate warning flag clears.
    if (enabled && MEM_LW(person+192))
        storef(person+196,fadd_ps2(loadf(timer+68),loadf(timer+72)));
    MEM_SW(person+192,0);
}
