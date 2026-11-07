#pragma once

#include <cstdint>

// Appends "Battle (4 AI)" and "Battle (8 AI)" to every visible Action and
// multiplayer track Eve after the outermost native Eve VAL load.
extern "C" void f_001244A8(void);
// Display names for the added contests; native names otherwise.
extern "C" void f_001252C0(void);
extern "C" void f_0010AFA0(void);
// Battle respawn table for a track whose archive has none (Welch).
extern "C" void f_001515B8(void);

// Variant of an added Battle contest: 1 = "Battle (1v1)" (humans only),
// 4 or 8 = that many AI; 0 for every native contest.
uint32_t battle_events_ai_for_contest(uint32_t contest);
// Career (mode 0x130): AI count of the one Battle added to the Career Eve with
// this name, or 0 when that Eve stays native. Early Career 4 AI, mid 6, late 8
// (cap: ten cars), by the Eve's native rank window; deterministic, no save state.
uint32_t battle_events_career_ai(const char *eve_name);
// Test hook: directory holding TRACKS/<TRACK>/c_battle.va0 (default
// $EOD_MOD_DIR/battle-alternates).
void battle_events_set_alternate_root(const char *path);
