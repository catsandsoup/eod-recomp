// Pair native camera/display state with composition: original menus and media
// are 4:3; only the actual driving loop uses the selected wide projection.
#include <cstdio>
#include <cstdlib>
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"
#include "platform/display_rect.h"
#include "platform/presentation.h"

extern "C" void f_0034D328_orig();
extern "C" void f_00114020_orig();
extern "C" void f_0034E530_orig();
namespace {
bool racing; // EE thread only; GS receives ordered per-vblank snapshots.
int launch_aspect() { static const int aspect=display_aspect(g_config.aspect);return aspect; }
bool wide() { return launch_aspect()==1 || launch_aspect()==3; }
void native_policy() {
    if (!wide()) return;
    const bool use_wide=racing;
    const float scale=use_wide ? (launch_aspect()==3 ? 5.0f/6.0f : .75f) : 1.0f;
    uint32_t bits;std::memcpy(&bits,&scale,sizeof bits);
    // Exactly the globals established by SetDisplayMode mode0/mode1. No
    // extra guest options/OSD calls, nor forced writes during RTT setup.
    ee_w32(0x003dd660,use_wide ? 1 : 0);
    ee_w32(0x003dd64c,use_wide ? 1 : 0);
    ee_w32(0x003dd650,0);
    ee_w32(0x003dd658,bits);
    ee_w32(0x003dd65c,bits);
}
}
int eod_presentation_aspect() { return presentation::for_context(launch_aspect(),racing); }
extern "C" void f_0034D328() {
    if (wide()) g_ee.r[4].sd[0]=racing ? 1 : 0;
    else if (launch_aspect()==0) g_ee.r[4].sd[0]=0;
    // Stretch preserves the native requested mode and all of its effects.
    f_0034D328_orig();
    native_policy();
}
extern "C" void f_00114020() {
    const bool previous=racing;
    racing=true;native_policy();
    f_00114020_orig(); // all native EE-register, memory and loop effects retained
    racing=previous;native_policy();
}

// Diagnostic only: keep the native projection calculation entirely unchanged.
extern "C" void f_0034E530() {
    static const bool trace=std::getenv("EOD_CAMERA_TRACE") != nullptr;
    static unsigned counts[2];
    if (trace && counts[racing ? 1 : 0] < 8) {
        ++counts[racing ? 1 : 0];
        float xscale,base;uint32_t xb=ee_r32(0x3dd658),bb=ee_r32(0x3dd65c);
        std::memcpy(&xscale,&xb,sizeof xscale);std::memcpy(&base,&bb,sizeof base);
        std::fprintf(stderr,"[camera] projection race=%d ra=%08x near=%.9g far=%.9g fov=%.9g xscale=%.9g base=%.9g mode=%u wide=%u letterbox=%u\n",
            int(racing),g_ee.r[31].uw[0],double(g_ee.fpr[12].f),double(g_ee.fpr[13].f),double(g_ee.fpr[14].f),
            double(xscale),double(base),ee_r32(0x3dd660),ee_r32(0x3dd64c),ee_r32(0x3dd650));
    }
    f_0034E530_orig();
}
