#pragma once

#include <cstdint>

// Snapshot of native type-0x51 records from the most recent world car build.
// Human identity comes from the native record type, never from slot ordering
// or viewport count. Only slots inside the enlarged BattleMaster are exposed.
bool battle_bots_is_ai_slot(uint32_t slot);

extern "C" void f_0012C158(void);
extern "C" void f_0020FD48(void);
extern "C" void f_00167C70(void);

// Mixed-class AI for an added Battle (N AI) contest: 0x12AEF0 notes the
// contest, 0x1116E0 resolves the description after the AI numbers change.
void battle_bots_mix_classes(uint32_t description);
extern "C" void f_0012AEF0(void);
extern "C" void f_001116E0(void);
extern "C" void f_00119428(void);
extern "C" void f_00166B50(void);
