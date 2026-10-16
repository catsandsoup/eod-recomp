// Production HUD wrapper contract, without a game process or graphics device.
#include "core/cpu.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
EEState g_ee{};
uint8_t *g_ram;
static int aspect, view, side, count;
static bool enabled=true;
static float expected_shift;
static unsigned calls;
static EEState native_after;
static std::vector<uint8_t> original_ram;
static float quad[4];
static void check(bool ok,const char *s) { if(!ok){std::fprintf(stderr,"FAIL: %s\n",s);std::exit(1);} }
extern "C" uint32_t ee_read32_slow(uint32_t) {std::abort();}
extern "C" void ee_write32_slow(uint32_t,uint32_t) {std::abort();}
int eod_presentation_aspect() {return aspect;}
static float readf(uint32_t a) {uint32_t b=MEM_LW(a);float f;std::memcpy(&f,&b,4);return f;}
static void putf(uint32_t a,float f) {uint32_t b;std::memcpy(&b,&f,4);MEM_SW(a,b);}
extern "C" void f_00119888();
extern "C" void f_00119F40();
static void native(int draw_side) {
    ++calls;check(draw_side==side,"correct original called");
    check(g_ee.r[4].sw[0]==view,"native receives original viewport argument");
    auto expected=original_ram;
    if(view>=0 && view<2) {
        const uint32_t anchor=0x3c4310+view*24+(side?8:4);
        float old;std::memcpy(&old,expected.data()+anchor,4);
        const float shifted=old+expected_shift;
        if(expected_shift!=0)std::memcpy(expected.data()+anchor,&shifted,4);
        check(std::fabs(readf(anchor)-shifted)<0.0001f,"native sees selected semantic edge shift");
        // All four native quad X coordinates use this one anchor. Neither
        // original dimensions nor offsets enter the wrapper.
        quad[0]=readf(anchor)+3;quad[1]=quad[0]+143;
        quad[2]=readf(anchor)+9;quad[3]=quad[2]+37;
    }
    check(std::memcmp(g_ram,expected.data(),expected.size())==0,"only selected anchor changes during original");
    std::memset(&g_ee,0xa5,sizeof g_ee);native_after=g_ee;
    MEM_SW(0x100,0x98765432); // native side effect outside layout
}
extern "C" void f_00119888_orig() {native(0);}
extern "C" void f_00119F40_orig() {native(1);}
int main(int argc,char **) {
    if(argc>1){setenv("EOD_HUD_EDGE_ANCHORS","0",1);enabled=false;}
    std::vector<uint8_t> ram(0x400000);g_ram=ram.data();
    for(aspect=0;aspect<4;++aspect)for(count=0;count<=4;++count)for(view=-1;view<=2;++view)for(side=0;side<2;++side) {
        std::fill(ram.begin(),ram.end(),0x6c);MEM_SW(0x3c41ac,count);
        // Asymmetric overscan and distinct Y/centre/other-view words detect
        // accidental stretching, table-wide updates or stale viewport IDs.
        for(unsigned i=0;i<2;++i) {
            putf(0x3c4310+i*24+4,-272.0f+i*3);
            putf(0x3c4310+i*24+8,267.0f-i*7);
        }
        putf(0x3dd658,aspect==3?5.0f/6.0f:.75f);putf(0x3dd65c,readf(0x3dd658));
        expected_shift=0;
        if(enabled && (aspect==1||aspect==3) && count>=1 && count<=2 && view>=0 && view<count)
            expected_shift=(side?1:-1)*(aspect==3?64.0f:320.0f/3.0f);
        original_ram=ram;std::memset(&g_ee,0x18,sizeof g_ee);g_ee.r[4].sd[0]=view;
        (side?f_00119F40:f_00119888)();
        check(std::memcmp(&g_ee,&native_after,sizeof g_ee)==0,"all native EE register results preserved");
        auto expected=original_ram;uint32_t effect=0x98765432;std::memcpy(expected.data()+0x100,&effect,4);
        check(ram==expected,"exact original anchor restored and native memory effect retained");
        if(view>=0&&view<2)check(std::fabs(quad[1]-quad[0]-143)<0.001f&&std::fabs(quad[3]-quad[2]-37)<0.001f,"native shape widths preserved");
    }
    check(calls==160,"each original called once");std::puts("HUD edge CPU contract passed");
}
