// Native semantic HUD rows: translate their anchor while they build draw data.
// Never classify or modify GS vertices, world geometry, or generic UI objects.
#include "core/cpu.h"
#include "platform/display_rect.h"
#include "core/memory.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" void f_00119888_orig();
extern "C" void f_00119F40_orig();
extern "C" void f_0011DC30_orig();
int eod_presentation_aspect();
namespace {
float read_float(uint32_t address) {
    const uint32_t bits=ee_r32(address);float value;
    std::memcpy(&value,&bits,sizeof value);return value;
}
void edge_rows(bool right,void (*original)()) {
    static const bool enabled=[] {
        const char *value=std::getenv("EOD_HUD_EDGE_ANCHORS");
        return !value || std::strcmp(value,"0")!=0;
    }();
    static const bool trace=std::getenv("EOD_HUD_TRACE")!=nullptr;
    static unsigned calls[2],lines[2];
    const int aspect=eod_presentation_aspect();
    const int view=g_ee.r[4].sw[0];
    const uint32_t count=ee_r32(0x003c41ac);
    // 0011D2C8 establishes this count and these native per-viewport rectangles.
    // Only the supported full-width solo / vertically split 2P layouts qualify.
    const bool valid=count>=1 && count<=2 && view>=0 && uint32_t(view)<count;
    const uint32_t address=valid ? 0x003c4310+uint32_t(view)*0x18+(right?8:4) : 0;
    const uint32_t saved=valid ? ee_r32(address) : 0;
    const float anchor=valid ? read_float(address) : 0;
    // The half-width is 320 native units in both supported layouts. Preserve
    // native overscan, panel widths, padding, animation, Y and centre positions.
    const auto ratio = aspect_ratio(aspect);
    const bool wide = 3 * ratio.width > 4 * ratio.height;
    float delta = 320.0f * ((3.0f * ratio.width) / (4.0f * ratio.height) - 1.0f);
    // 0011D2C8 copies each viewport's actual overscan alongside its bounds
    // (top, left, right, bottom; 20-byte stride). The retail wide layout
    // has already moved the left edge by 106 units and the right by 101.
    if (valid) {
        const float inset = read_float(0x003c42c0 + uint32_t(view)*20 + (right ? 8 : 4));
        if (inset == (right ? -53.0f : -58.0f)) delta -= 48.0f - inset;
    }
    const bool change=enabled && valid && wide && std::isfinite(anchor);
    if(trace) {
        const unsigned call=++calls[right];
        if(lines[right]<32 && (call<=8 || call%120==0)) {
            ++lines[right];
            std::fprintf(stderr,"[hud] side=%s view=%d count=%u aspect=%d anchor=%.9g shift=%.9g xscale=%.9g base=%.9g caller=%08x\n",
                right?"right":"left",view,count,aspect,double(anchor),
                change?double(right?delta:-delta):0.0,double(read_float(0x003dd658)),
                double(read_float(0x003dd65c)),g_ee.r[31].uw[0]);
        }
    }
    if(change) {
        const float shifted=anchor+(right?delta:-delta);uint32_t bits;
        std::memcpy(&bits,&shifted,sizeof bits);ee_w32(address,bits);
    }
    original(); // retain all native EE-register and draw-list effects
    if(change) ee_w32(address,saved); // host locals survive native register changes
}
}
extern "C" void f_00119888() {edge_rows(false,f_00119888_orig);}
extern "C" void f_00119F40() {edge_rows(true,f_00119F40_orig);}

// 00128090 draws the skull indicator and its "x%d" count separately from
// the speed rows. It obtains the right edge at this call before placing both.
extern "C" void f_0011DC30() {
    if (g_ee.r[31].uw[0] == 0x001281b8)
        edge_rows(true, f_0011DC30_orig);
    else
        f_0011DC30_orig();
}
