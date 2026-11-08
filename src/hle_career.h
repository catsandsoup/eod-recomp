// hle_career.h - Career unlocks (src/hle_career.cpp, docs/CAREER_UNLOCKS.md)
#pragma once
#include <cstdint>

// Every vblank (game_patches_vblank): the money floor and the rank (while the Career's free-roam world runs), the gate table, the dealer's page banner. `pressed` is the pad-0 buttons that went down this vblank.
void career_vblank(uint16_t pressed);

