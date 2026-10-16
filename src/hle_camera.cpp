// Native camera aspect selection. See docs/NATIVE_CAMERA_ASPECT.md for the
// target ELF evidence; no GS-space perspective correction is performed.
#include "core/config.h"
#include "core/cpu.h"
#include "core/memory.h"

extern "C" void f_0034D328_orig();

extern "C" void f_0034D328() {
    // Settings are restart-only. Never consult a newly edited aspect while
    // the game has temporarily changed x-scale for a split-screen viewport.
    static const std::string launch_aspect = g_config.aspect;
    const bool wide = launch_aspect == "16:9" || launch_aspect == "16:10";
    if (wide)
        g_ee.r[4].sd[0] = 1; // the game's own widescreen path and HUD policy
    else if (launch_aspect == "4:3")
        g_ee.r[4].sd[0] = 0;
    // Stretch preserves whichever native display mode the game requested.
    f_0034D328_orig();
    if (wide) {
        // Set only the base values here. Native viewport setup derives and
        // restores a temporary x-scale from them for each split-screen view.
        const float scale = launch_aspect == "16:10" ? 5.0f / 6.0f : 0.75f;
        uint32_t bits;
        std::memcpy(&bits, &scale, sizeof bits);
        ee_w32(0x003dd658, bits);
        ee_w32(0x003dd65c, bits);
    }
}
