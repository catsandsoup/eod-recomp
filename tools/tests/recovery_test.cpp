// CPU boundary contract, not a substitute for native game validation.
#include "core/cpu.h"
#include "core/config.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
EEState g_ee{};
uint8_t *g_ram;
extern "C" uint32_t ee_read32_slow(uint32_t) { std::abort(); }
extern "C" void ee_write32_slow(uint32_t,uint32_t) { std::abort(); }
extern "C" void f_001B8370();
static constexpr uint32_t p=0x10000,car=0x20000,timer=p+256,automatic=0x485098;
static unsigned calls, resets;
static bool active=true;
static EEState native_after;
static void check(bool b,const char *s) { if(!b) { std::fprintf(stderr,"FAIL: %s\n",s); std::exit(1); } }
static void putf(uint32_t a,float f) { uint32_t u; std::memcpy(&u,&f,4); MEM_SW(a,u); }
static float getf(uint32_t a) { uint32_t u=MEM_LW(a); float f; std::memcpy(&f,&u,4); return f; }
extern "C" void f_001B8370_orig() {
    ++calls;
    MEM_SW(p+192,active?0xffffffff:0); putf(p+196,-1);
    if(active&&MEM_LW(automatic)) { ++resets; putf(p+196,15); }
    std::memset(&g_ee,0xa5,sizeof g_ee); native_after=g_ee;
}
static void invoke() {
    g_ee={}; g_ee.r[4].uw[0]=timer; g_ee.r[5].uw[0]=p;
    f_001B8370();
    check(std::memcmp(&g_ee,&native_after,sizeof g_ee)==0,"native full EE effects preserved");
}
int main() {
    std::vector<uint8_t> ram(EE_RAM_SIZE);g_ram=ram.data();
    MEM_SW(p+4,car); MEM_SW(car+12,0x50434152); MEM_SW(car+480,0x50);
    MEM_SW(automatic,7); putf(timer+68,10); putf(timer+72,5);
    invoke(); check(resets==1&&MEM_LW(p+192)==0xffffffff,"default is native");
    g_config.manual_recovery=true; invoke();
    check(resets==1,"human expired countdown cannot reset");
    check(MEM_LW(p+192)==0&&getf(p+196)==15,"hide warning, preserve native SELECT deadline");
    check(MEM_LW(automatic)==7,"exact automatic flag restored for AI");
    MEM_SW(car+480,0x51);invoke();check(resets==2&&MEM_LW(p+192)==0xffffffff,"AI recovery unchanged");
    MEM_SW(car+480,0x50);active=false;invoke();check(getf(p+196)==-1,"safe car has no synthetic recovery");
    active=true;MEM_SW(automatic,0);invoke();check(getf(p+196)==-1&&MEM_LW(automatic)==0,"native non-countdown mode retained");
    MEM_SW(automatic,1);MEM_SW(car+12,0);invoke();check(resets==3,"non-PCAR native passthrough");
    MEM_SW(p+4,EE_RAM_SIZE-4);invoke();check(resets==4,"invalid car native passthrough");
    g_ee={};g_ee.r[4].uw[0]=EE_RAM_SIZE-4;g_ee.r[5].uw[0]=p;
    f_001B8370();check(resets==5,"invalid timer native passthrough");
    check(calls==8,"native exactly once");
    // The settings UI updates g_config in-process: no reload or restart.
    MEM_SW(p+4,car); MEM_SW(car+12,0x50434152); MEM_SW(car+480,0x50);
    g_config.manual_recovery=false; invoke();
    check(resets==6 && MEM_LW(p+192)==0xffffffff,"live toggle off restores automatic recovery");
    g_config.manual_recovery=true; invoke();
    check(resets==6 && MEM_LW(p+192)==0 && getf(p+196)==15,"live toggle on suppresses recovery again");
    Config c; c.manual_recovery=true;
    const char *path="recovery-test.ini";
    check(config_save(path,c),"save");Config read;check(config_load(path,read)&&read.manual_recovery,"config round trip");std::remove(path);
    std::puts("manual recovery contract passed");
}
