#pragma once
#include <cstdint>

// Called before a new native world is constructed; guest pointers can be reused.
void battle_driver_reset();

// Apply World Editor controls after the native HumanDriver has produced its input.
void pie_update_player(uint32_t car);

// True when PIE 6-DOF Free Cam is active (EOD_FREE_CAM=1 or pie_live_control.ini free_cam=1).
bool battle_driver_is_free_cam();
