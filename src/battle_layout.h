#pragma once
// Battle layout shared by the runtime hooks and tools/recomp.py (keep the two in
// step; tools/tests/test_battle_layout.py compares them).
//
// Native SLUS_209.10 BattleMaster (vtable 0x3F7770) was 0x5AC bytes: sixteen
// 0x44-byte respawn records at +0x10, their count at +0x450, four 0x50-byte
// participant records at +0x454, a four-word ranking at +0x594, the spawn
// rotation at +0x5A4 and the participant count at +0x5A8. Only the Battle class
// methods (0x166B00-0x168000) touch these fields. The recompiler moves the
// ranking/spawn/count words past a twelve-record participant array and grows
// the allocation; record offsets and every other field keep their native place.
#include <cstdint>

namespace battle_layout {
constexpr uint32_t kMasterVtable = 0x003F7770;
constexpr uint32_t kRespawnCount = 0x450;
constexpr uint32_t kParticipants = 0x454;
constexpr uint32_t kParticipantStride = 0x50;
constexpr uint32_t kCapacity = 12;
constexpr uint32_t kRanking = kParticipants + kCapacity * kParticipantStride;  // 0x814
constexpr uint32_t kSpawnIndex = kRanking + kCapacity * 4;                     // 0x844
constexpr uint32_t kCount = kSpawnIndex + 4;                                   // 0x848
constexpr uint32_t kMasterSize = kCount + 4;                                   // 0x84C
static_assert(kRanking == 0x814 && kSpawnIndex == 0x844 && kCount == 0x848 && kMasterSize == 0x84C);

// Most cars a Battle may hold. Native 0x12C158 caps two humans at ten cars;
// the twelve records leave headroom so no hook ever writes the last slot.
constexpr uint32_t kMaxCars = 10;

// Runtime-owned guest memory in EE kernel RAM (0x00083000-0x000FE000 is never
// touched by the game or the HLE kernel: syscall table 0x10000, thread-exit
// trampoline 0x81000, interrupt stack below 0xFF000).
constexpr uint32_t kGuestBase = 0x00090000;
// The Eve contest table, relocated from 0x003C61A8 (320 x 0x28) by the
// recompiler. Native contests keep their index; added Battle contests follow.
constexpr uint32_t kContestTable = 0x00090000;
constexpr uint32_t kContestStride = 0x28;
constexpr uint32_t kContestCapacity = 512;
static_assert(kContestTable + kContestCapacity * kContestStride <= 0x00096000);
constexpr uint32_t kStrings = 0x00096000;      // display names and VAL key/value text
constexpr uint32_t kSections = 0x00097000;     // synthesized VAL sections
constexpr uint32_t kExternalTable = 0x00098000; // alternate c_battle.va0 for tracks without one
constexpr uint32_t kExternalTableSize = 0x2000;
constexpr uint32_t kScratch = 0x0009A000;      // native call arguments
constexpr uint32_t kGuestEnd = 0x0009B000;
} // namespace battle_layout
