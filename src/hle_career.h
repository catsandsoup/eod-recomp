// hle_career.h - Career unlocks (src/hle_career.cpp, docs/CAREER_UNLOCKS.md)
#pragma once
#include <cstdint>

// Every vblank (game_patches_vblank): the money floor and the rank (while the Career's free-roam world runs), the gate table, the dealer's
// L2 / R2. `pressed` is the pad-0 buttons that went down this vblank.
void career_vblank(uint16_t pressed);

// Turn the dealer's page (L2 / R2 on the dealer screen); the stock is rebuilt on the next AutoShopSetAvailable.
void career_dealer_turn_page(int delta);
