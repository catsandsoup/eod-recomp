// Tests the production HLE wrapper without a game process or graphics device.
#include "core/config.h"
#include "core/cpu.h"
#include "platform/presentation.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;

static int calls, original_mode;
static bool explicit_wide;
static int wanted_aspect;
static EEState expected_after;
static uint32_t expected_globals[5];
static const uint32_t display_globals[]={0x3dd64c,0x3dd650,0x3dd658,0x3dd65c,0x3dd660};
static void check(bool,const char *);
static float scale(uint32_t);
extern "C" void f_0034D328();
extern "C" void f_00114020();
extern "C" void f_0034E530();
extern "C" void f_0034E530_orig() { g_ee.r[2].ud[1]++; }
static void projection_passthrough() {
    EEState expected=g_ee;expected.r[2].ud[1]++;
    f_0034E530();
    check(std::memcmp(&expected,&g_ee,sizeof g_ee)==0,"projection trace leaves native EE state and arguments untouched");
}
extern "C" void f_00114020_orig() {
    check(eod_presentation_aspect()==wanted_aspect,"race composition follows launch aspect");
    if(explicit_wide) {
        check(MEM_LW(0x3dd660)==1 && MEM_LW(0x3dd64c)==1 && MEM_LW(0x3dd650)==0,"race native mode1 flags");
        check(scale(0x3dd658)==(wanted_aspect==3 ? 5.0f/6.0f : .75f),"race camera matches composition");
    }
    for(int i=0;i<24;i++) projection_passthrough();
    // A native mode reset inside the loop stays paired, without consulting
    // later settings. It must not change the wrapper return/register effects.
    g_ee.r[4].sd[0]=0;f_0034D328();
    g_ee.r[2].ud[0]=0x1122334455667788ull;
    g_ee.r[7].ud[1]=0xaabbccddeeff0011ull;
    expected_after=g_ee;
    for(int i=0;i<5;i++) expected_globals[i]=MEM_LW(display_globals[i]);
    MEM_SW(0x100,0xbadc0ffe);
}
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
    MEM_SW(0x003dd650,original_mode==2);
    ret_s32(37); // wrapper must preserve native return/register effects
}
static void check(bool ok,const char *why) {if(!ok){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static float scale(uint32_t address) {uint32_t bits=MEM_LW(address);float f;std::memcpy(&f,&bits,4);return f;}
int main(int argc,char **argv) {
    check(argc==2,"aspect argument");std::vector<uint8_t> ram(EE_RAM_SIZE);g_ram=ram.data();
    std::string aspect=argv[1];g_config.aspect=aspect;
    wanted_aspect=aspect=="16:9" ? 1 : aspect=="16:10" ? 3 : aspect=="stretch" ? 2 : 0;
    explicit_wide=wanted_aspect==1 || wanted_aspect==3;
    int expected_mode=aspect=="stretch" ? 2 : 0;
    float expected_scale=aspect=="stretch" ? .75f : 1.0f;
    g_ee.r[4].sd[0]=2;f_0034D328();
    check(calls==1 && original_mode==expected_mode,"menu native mode0, stretch preserves caller");
    check(scale(0x3dd658)==expected_scale && scale(0x3dd65c)==expected_scale,"menu camera uses original proportions");
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
    check(eod_presentation_aspect()==(explicit_wide ? 0 : wanted_aspect),"nonrace presentation");
    for(int i=0;i<24;i++) projection_passthrough();
    f_00114020();
    check(std::memcmp(&g_ee,&expected_after,sizeof g_ee)==0,"race wrapper preserves all native EE-register effects");
    check(eod_presentation_aspect()==(explicit_wide ? 0 : wanted_aspect),"race exit restores menu composition");
    check(MEM_LW(0x100)==0xbadc0ffe,"original loop memory effects retained");
    if(!explicit_wide) for(int i=0;i<5;i++) check(MEM_LW(display_globals[i])==expected_globals[i],"4:3/stretch race exit leaves original display effects");
    if(explicit_wide) {
        check(scale(0x3dd658)==1 && scale(0x3dd65c)==1,"race exit restores original menu/preview camera");
        check(MEM_LW(0x3dd660)==0 && MEM_LW(0x3dd64c)==0 && MEM_LW(0x3dd650)==0,"race exit restores mode0 flags");
    }
    std::puts("native menu/race camera and presentation pairing passed");
}
