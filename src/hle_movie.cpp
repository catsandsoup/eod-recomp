// hle_movie.cpp - MoviePlay(name): plays data\video\<name>.pss with the
// ezmpegstr player (libmpeg + IPU).  With EOD_SKIP_MOVIES=1 movies are
// skipped the same way the game skips them when it is short of memory.
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "core/cpu.h"
#include "core/log.h"
#include "core/memory.h"
#include "platform/platform.h"
#include "kernel/kernel.h"
#include "funcs.h"
#include "movie_bits.h"
#include "gs/gs.h"
#include "platform/editor_test_startup.h"

// set while a movie plays (shown in 4:3 whatever the game's aspect setting)
std::atomic<bool> g_movie_playing{false};

extern "C" {

void f_0039AFB0_orig(void);
void f_0039AFB0(void) {
    if (!movie_bits_advance_fast()) f_0039AFB0_orig();
}

void f_003670C8(void) {
    static bool skip = (getenv("EOD_SKIP_MOVIES") && atoi(getenv("EOD_SKIP_MOVIES"))) ||
                       editor_test_startup::state().enabled();
    char name[256];
    ee_read_string(arg_u32(0), name, sizeof name);
    if (skip) {
        LOG(LOG_CORE, "skipping movie %s", name);
        g_ee.r[2].sd[0] = 0;
        return;
    }
    LOG(LOG_CORE, "playing movie %s", name);
    g_movie_playing = true;
    uint64_t t0 = platform_time_us();
    f_003670C8_orig();
    g_movie_playing = false;
    LOG(LOG_CORE, "movie %s ended after %.1f s", name, (platform_time_us() - t0) / 1e6);
}

// SLUS_209.10: 113FF8 dispatches ALL session UI states (including loading).
// State zero's callback is 106338, read directly from ELF table 3C3CF8+0x0C.
// That callback draws the actual gameplay viewport world/HUD via 101618/101C00.
// Do not publish readiness until the later race-loop FrameWait has completed.
static editor_test_startup::NativeFrameGate g_editor_native_frame;
void f_00113FF8_orig(void);
void f_00113FF8(void) {
    const bool enabled = editor_test_startup::state().enabled();
    if (enabled) g_editor_native_frame.begin_draw(g_ee.r[31].uw[0] == 0x001140E0);
    f_00113FF8_orig();
    if (enabled) g_editor_native_frame.end_draw();
}

void f_00106338_orig(void);
void f_00106338(void) {
    const bool enabled = editor_test_startup::state().enabled();
    const uint32_t views = enabled ? ee_r32(0x003D1A74) : 0;
    const bool gameplay = enabled && views >= 1 && views <= 4 && ee_r32(0x003C3CE8) == 0;
    const uint32_t world = enabled ? ee_r32(0x00495C48) : 0;
    const bool valid_world = world && !(world & 3) && world <= EE_RAM_SIZE - 0x1968;
    // 175960 stores FadeGetAlpha(176278) at world+1964; 210FB8 reads it.
    // 113EE8 draws a full-screen rectangle with that alpha when it is nonzero.
    // Zero is the native loading transition's cleared overlay, not a delay.
    const uint32_t fade_alpha = valid_world ? ee_r32(world + 0x1964) : 255;
    f_00106338_orig();
    if (enabled) {
        g_editor_native_frame.gameplay_drawn(gameplay, fade_alpha);
        static bool logged = false;
        if (gameplay && fade_alpha == 0 && !logged) {
            logged = true;
            std::fprintf(stderr, "[editor-test] native gameplay world/HUD draw completed: callback=00106338 views=%u state=0 world=%08x fade_alpha=0\n", views, world);
        }
    }
}

// Bounded native-state evidence for startup investigation; never used as a gate.
static void editor_trace_native_publication(unsigned call, const char *where) {
    const uint32_t world = ee_r32(0x00495C48);
    const bool valid_world = world && !(world & 3) && world <= EE_RAM_SIZE - 0x196C;
    const uint32_t master = ee_r32(0x003CF194);
    const bool valid_master = master && !(master & 3) && master <= EE_RAM_SIZE - 16;
    std::fprintf(stderr, "[editor-trace] call=%u where=%s vblank=%llu scene=%u simframe=%d clock=%08x phase=%u world=%08x fade_alpha=%u history=%08x renderstate=%u target=%x blend=%u published=%u vif=%08x gif=%08x event=%08x master=%08x masterwords=%08x/%08x/%08x/%08x\n",
        call, where, (unsigned long long)kernel_vblank_count(), ee_r32(0x003C3CE8),
        (int32_t)ee_r32(0x003CDA8C), ee_r32(0x003CDACC), ee_r32(0x003CDAE8), world,
        valid_world ? ee_r32(world + 0x1964) : 0, valid_world ? ee_r32(world + 0x1968) : 0,
        ee_r32(0x003E57F0), ee_r32(0x003E57F8), ee_r32(0x003E57FC), ee_r32(0x003E5798),
        ee_r32(0x004D8C3C), ee_r32(0x004D8C40), ee_r32(0x003CF19C), master,
        valid_master ? ee_r32(master) : 0, valid_master ? ee_r32(master + 4) : 0,
        valid_master ? ee_r32(master + 8) : 0, valid_master ? ee_r32(master + 12) : 0);
}

void f_0033EC78_orig(void);
void f_0033EC78(void) {
    const bool race_loop = g_ee.r[31].uw[0] == 0x0011412C;
    static const bool trace = editor_test_startup::state().enabled() && std::getenv("EOD_EDITOR_TRACE");
    static unsigned traced_calls = 0;
    const unsigned trace_call = trace && race_loop ? ++traced_calls : 0;
    if (trace_call && trace_call <= 160) editor_trace_native_publication(trace_call, "before");
    f_0033EC78_orig(); // includes 35C3F8: FrameWaitVblank/DrawDone and display publication
    if (trace_call && trace_call <= 160) editor_trace_native_publication(trace_call, "after");
    if (editor_test_startup::state().enabled() && g_editor_native_frame.frame_published(race_loop)) {
        std::fputs("[editor-test] native gameplay frame publication completed: caller=0011412c\n", stderr);
        gs_editor_race_frame_submitted(); // queued after draw AND native display work
    }
}

}
