#pragma once
#include <cstdint>

// Called before a new native world is constructed; guest pointers can be reused.
void battle_driver_reset();

// Apply World Editor controls after the native HumanDriver has produced its input.
void pie_update_player(uint32_t car);
