#pragma once

#include <cstdint>

struct Config;

// Initializes game patch parameters and state from configuration
void game_patches_init(const Config &config);

// Main per-frame hook called every vblank from the engine loop
void game_patches_vblank();

// Bitmask of controller buttons currently suppressed during combo chords
uint16_t game_patches_suppressed_buttons();
