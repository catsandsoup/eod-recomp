// Tests the production HLE wrapper without a game process or graphics device.
#include "core/config.h"
#include "core/cpu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;

static int calls, original_mode;
extern "C" void f_0034D328();
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" void f_0034D328_orig() {
    calls++;
    original_mode=arg_u32(0);
    // Native mode0 is 4:3, mode1/2 set the engine's two base scales to .75.
    float scale=original_mode==0 ? 1.0f : .75f;
    uint32_t bits;std::memcpy(&bits,&scale,4);
    MEM_SW(0x003dd658,bits);MEM_SW(0x003dd65c,bits);
    MEM_SW(0x003dd660,original_mode);
    MEM_SW(0x003dd64c,original_mode==1);
    ret_s32(37); // wrapper must preserve native return/register effects
}
static void check(bool ok,const char *why) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static float scale(uint32_t address) {uint32_t bits=MEM_LW(address);float f;std::memcpy(&f,&bits,4);return f;}
int main(int argc,char **argv) {
    check(argc==2,"aspect argument");std::vector<uint8_t> ram(EE_RAM_SIZE);g_ram=ram.data();
    std::string aspect=argv[1];g_config.aspect=aspect;
    int expected_mode=aspect=="stretch" ? 2 : aspect=="4:3" ? 0 : 1;
    float expected_scale=aspect=="16:10" ? 5.0f/6.0f : aspect=="4:3" ? 1.0f : .75f;
    g_ee.r[4].sd[0]=2;f_0034D328();
    check(calls==1 && original_mode==expected_mode,"native mode follows host aspect, stretch preserves caller");
    check(scale(0x3dd658)==expected_scale && scale(0x3dd65c)==expected_scale,"both base scales follow host aspect");
    check(arg_u32(0)==unsigned(expected_mode) && g_ee.r[2].sw[0]==37,"native call effects preserved");
    check(MEM_LW(0x3dd64c)==unsigned(expected_mode==1),"native border policy preserved");
    // Later settings changes must not affect the launched camera or clobber a
    // temporary split viewport scale until the game itself sets display mode.
    g_config.aspect=aspect=="4:3" ? "16:9" : "4:3";
    float temporary=.375f;uint32_t bits;std::memcpy(&bits,&temporary,4);MEM_SW(0x3dd658,bits);
    check(scale(0x3dd658)==temporary,"temporary viewport scale unchanged outside native call");
    g_ee.r[4].sd[0]=0;f_0034D328();
    if(aspect=="stretch"){expected_mode=0;expected_scale=1.0f;}
    check(calls==2 && original_mode==expected_mode,"startup aspect remains cached");
    check(scale(0x3dd658)==expected_scale && scale(0x3dd65c)==expected_scale,"native mode reset restores base scales");
    std::puts("native camera wrapper passed");
}
