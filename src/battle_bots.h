#pragma once

#include <cstdint>

// Snapshot of native type-0x51 records from the most recent world car build.
// Human identity comes from the native record type, never from slot ordering
// or viewport count. Only the four slots supported by BattleMaster are exposed.
bool battle_bots_is_ai_slot(uint32_t slot);

extern "C" void f_0012C158(void);
extern "C" void f_0020FD48(void);
