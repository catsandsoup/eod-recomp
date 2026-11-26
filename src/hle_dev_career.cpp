// hle_dev_career.cpp - developer shortcut: EOD_CAREER_EVE=<Eve index> runs that Eve of the
// loaded Career (CareerDoEve, 0x2DDC08) right where the Career driver (0x112298) would start the
// free-roam drive (0x2D58E8), as if the player had just driven into the Eve. Once per run; when
// the Eve ends the game carries on with the normal free roam. Off unless the variable is set.
#include "core/cpu.h"
#include "core/memory.h"
#include "funcs.h"
#include "session_start.h"
#include "battle_layout.h"
#include "netplay_hooks.h"

#include <array>
#include <cstdio>
#include <cstdlib>

extern "C" {
void f_002D58E8_orig(void);
}

extern "C" void f_002D58E8(void) {
    static bool env_done = false;
    // EOD_CAREER_EVE=<n>, eve=<n> in EOD_DIRECT_START, or resettable PendingCareerWarp::target_eve.
    const char *text = std::getenv("EOD_CAREER_EVE");
    const SessionRequest *warp_req = career_warp_request();
    PendingCareerWarp &pending = career_warp_state();
    int target_eve = -1;
    if (pending.target_eve >= 0) {
        target_eve = pending.target_eve;
        pending.target_eve = -1;
    } else if (!env_done && ((text && *text) || (warp_req && warp_req->eve >= 0))) {
        env_done = true;
        target_eve = (warp_req && warp_req->eve >= 0) ? warp_req->eve
                                                      : static_cast<int>(std::strtoul(text, nullptr, 0));
    }
    if (target_eve >= 0) {
        const uint32_t eve = static_cast<uint32_t>(target_eve);
        ee_w32(0x004AAFC8, eve);  // chosen Eve (read by 0x2E2F28)
        ee_w32(0x004AAFCC, 1);    // its "chosen" flag (set by 0x2E2F38)
        std::fprintf(stderr, "[CAREER_EVE] entering Eve %u\n", eve);
        const EEState saved = g_ee;
        g_ee.r[4].ud[0] = eve;
        f_002DDC08();
        g_ee = saved;
        std::fprintf(stderr, "[CAREER_EVE] Eve %u returned to the Career driver\n", eve);
    }
    f_002D58E8_orig();
}

// EOD_BATTLE_END_AFTER=<updates>: after that many Battle updates (0x167D58, once per frame) the second participant
// is given the winning score (a lead of 5, which 0x167D58 turns into the finished state), so a headless run
// reaches the end-of-event path without playing a match. Developer only.
extern "C" {
void f_00167D58_orig(void);
}
extern "C" void f_00167D58(void) {
    static long updates = 0;
    static const char *text = std::getenv("EOD_BATTLE_END_AFTER");
    if (text && *text && ee_r32(arg_u32(0) + 4) == 1 && ++updates == std::atol(text)) {
        const uint32_t master = arg_u32(0);
        ee_w32(master + 0x460 + 0x50 * 1, ee_r32(master + 0x468 + 0x50 * 1) + 5);
        std::fprintf(stderr, "[BATTLE_END] gave participant 1 a winning lead after %ld updates\n", updates);
    }
    const uint32_t master = arg_u32(0);
    const uint16_t eligible = netplay_hooks::eligible_battle_slots ? netplay_hooks::eligible_battle_slots() : UINT16_MAX;
    if (eligible == UINT16_MAX || ee_r32(master + 4) != 1) { f_00167D58_orig(); return; }

    // Only the host's eligible competitors may trigger a native victory. Keep
    // client combat/HUD updates running, including temporary deaths and respawns.
    const uint32_t count = ee_r32(master + battle_layout::kCount);
    std::array<uint32_t, battle_layout::kCapacity> saved_kills{};
    uint16_t masked = 0;
    for (uint32_t slot = 0; slot < count && slot < saved_kills.size(); ++slot) {
        if (eligible & (1u << slot)) continue;
        const uint32_t record = master + battle_layout::kParticipants + slot * battle_layout::kParticipantStride;
        const uint32_t kills = ee_r32(record + 0x0C), penalties = ee_r32(record + 0x14);
        if (static_cast<int32_t>(kills - penalties) < 5) continue;
        saved_kills[slot] = kills;
        masked |= static_cast<uint16_t>(1u << slot);
        ee_w32(record + 0x0C, penalties + 4);
    }
    f_00167D58_orig();
    for (uint32_t slot = 0; masked; ++slot, masked >>= 1) {
        if (masked & 1) ee_w32(master + battle_layout::kParticipants + slot * battle_layout::kParticipantStride + 0x0C,
                              saved_kills[slot]);
    }
}
