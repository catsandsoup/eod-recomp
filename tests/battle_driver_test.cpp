#include "core/config.h"
#include "battle_driver.h"
#include "core/cpu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t, uint32_t) { std::abort(); }
extern "C" uint8_t ee_read8_slow(uint32_t a) { return g_ram[a]; }
extern "C" void ee_write8_slow(uint32_t a,uint8_t v) { g_ram[a]=v; }
extern "C" void *ee_ptr(uint32_t a) { return g_ram+a; }
extern "C" void f_001A2D20();
extern "C" void f_001A2D20_orig() {}
static int fire_calls, respawn_calls;
static uint32_t respawn_master,respawn_slot;
extern "C" void f_001671E8(){++respawn_calls;respawn_master=g_ee.r[4].uw[0];respawn_slot=g_ee.r[5].uw[0];g_ee.r[16].ud[0]=0xBAD;}
static bool bots_active;
static uint32_t fire_owner, fire_car, pressed;
extern "C" void f_0017E000() {
    ++fire_calls; fire_owner=g_ee.r[4].uw[0]; fire_car=g_ee.r[5].uw[0];
    pressed=MEM_LW(g_ee.r[6].uw[0]);
    g_ee.r[16].ud[0]=0xBAD; // ABI wrapper must preserve caller state
}
static std::vector<uint8_t> ram(EE_RAM_SIZE);
constexpr uint32_t bm=0x10000, human=0x20000, bot=0x24000, ctrl=0x30000, weapon=0x40000;
static void putf(uint32_t a,float f){ std::memcpy(g_ram+a,&f,4); }
static float getf(uint32_t a){ float f;std::memcpy(&f,g_ram+a,4);return f; }
static void require(bool ok,const char*msg){if(!ok){std::fprintf(stderr,"FAIL: %s\n",msg);std::exit(1);}}
bool battle_bots_is_ai_slot(uint32_t slot) { return bots_active && slot >= MEM_LW(0x3C41AC); }
static void reset() {
    battle_driver_reset(); bots_active=true;
    std::fill(ram.begin(),ram.end(),0);g_ram=ram.data();g_ee={};g_config=Config{};
    MEM_SW(0x3C4190,9);MEM_SW(0x3CF19C,0x18000224);MEM_SW(0x3CF194,bm);MEM_SW(bm,0x3F7770);
    MEM_SW(bm+4,1);MEM_SW(bm+0x5A8,2);MEM_SW(0x3CF198,1);MEM_SW(0x3C41AC,1);
    MEM_SW(0x447458,human);MEM_SW(0x44745C,bot);MEM_SW(bot+0x1C1C,ctrl);
    MEM_SW(ctrl,0x3F2FB0);putf(bot+0x64,1);putf(bot+0x84,1);putf(human+0x90,25);
    MEM_SW(0x3CE4B8,1);MEM_SW(0x3CE3F8,1);MEM_SW(0x3CE3FC,0);
    MEM_SW(0x3CE400,weapon+0x1DC);MEM_SW(weapon+0x1DC,0x3F9528);MEM_SW(weapon+0x1E0,2);
    MEM_SW(weapon+0x1E8,1);putf(0x3CDACC,1);fire_calls=0;respawn_calls=0;MEM_SW(bm+0x450,4);
    g_ee.r[29].ud[0]=0x100000;g_ee.r[16].ud[0]=123;
}
static void tick(float time){putf(0x3CDACC,time);g_ee.r[4].ud[0]=bot;f_001A2D20();}
int main(){
    reset();tick(1);require(getf(bot+0x1B70)>0.5f,"bot accelerates toward opponent");
    require((int)MEM_LW(bot+0x1B90)==-10,"forward drive uses native automatic gearing");
    require(getf(bot+0x1B78)==1 && getf(bot+0x1B7C)==0,"drive coupling stays engaged with rear brake released");
    require(MEM_LW(g_ee.r[29].uw[0]-12)==0,"trigger scratch restored");
    require(fire_calls==1 && fire_owner==weapon+0x1DC && fire_car==bot && pressed==1,"bot calls native trigger with owner interface/car/input");
    require(g_ee.r[16].ud[0]==123 && g_ee.r[29].uw[0]==0x100000,"trigger preserves guest ABI");
    reset();putf(human+0x88,20);tick(1);require(getf(bot+0x1B6C)>0.1f,"bot steers at actual native control offset");
    require(getf(bot+0x1B80)==0,"bot leaves unrelated car field intact");require(pressed==0,"off-axis shot suppressed");
    reset();MEM_SW(weapon+0x1E0,1);tick(1);require(pressed==0,"bot releases to rearm native weapon");
    reset();MEM_SW(0x3C41AC,2);putf(bot+0x1B70,.37f);tick(1);require(getf(bot+0x1B70)==.37f && fire_calls==0,"player 2 input remains untouched");
    reset();MEM_SW(bm+4,0);tick(1);require(getf(bot+0x1B70)==0 && !fire_calls,"no countdown driving or firing");
    reset();MEM_SW(bm+4,2);tick(1);require(!fire_calls && getf(bot+0x1B70)==0,"no driving after match end");
    reset();MEM_SW(bm+0x458+0x50,1);tick(1);require(!fire_calls && getf(bot+0x1B70)==0,"wrecked bot waits for native respawn");
    require(respawn_calls==1 && respawn_master==bm && respawn_slot==1 && g_ee.r[16].ud[0]==123,"bot requests native respawn with slot and preserved context");
    reset();MEM_SW(0x44744C,1);tick(1);require(!fire_calls && getf(bot+0x1B70)==0,"pause gate honored");
    reset();MEM_SW(bm+0x458,1);tick(1);require(!fire_calls && getf(bot+0x1B70)==0,"no attacking wrecked opponent");
    reset();g_config.battle_bots=false;bots_active=false;tick(1);require(!fire_calls && getf(bot+0x1B70)==0,"disabled bots preserve native behavior");
    reset();tick(1);g_config.battle_bots=false;tick(1.1f);
    require(getf(bot+0x1B70)>0 && fire_calls==2,"setting applies next event, not mid-drive");
    for(uint32_t flags:{0u,1u,8u}) {reset();MEM_SW(0x3C4190,flags);tick(1);require(!fire_calls,"native master and weapon gates required");}
    reset();putf(bot+0x104,10);tick(1);for(int i=1;i<=19;++i)tick(1+i*.1f);
    require((int)MEM_LW(bot+0x1B90)==-1,"no displacement triggers recovery even with nonzero reported velocity");
    reset();MEM_SW(bm+0x5A8,5);tick(1);require(!fire_calls,"reject unsupported participant count");
    reset();tick(1);for(int i=1;i<=19;++i)tick(1+i*.1f);
    require((int)MEM_LW(bot+0x1B90)==-1,"stationary bot reverses out of pin");
    tick(5.0f);require((int)MEM_LW(bot+0x1B90)==-10,"recovery returns to forward driving");
    std::puts("PASS: Battle bot input, ABI, human separation and lifecycle guards");
}
