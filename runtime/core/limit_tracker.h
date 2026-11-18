// limit_tracker.h - watches every fixed-size pool, buffer and table the game had on the PS2
// (docs/PS2_LIMITS.md), so a run that reaches one says so.
//
//   limit_note("draw queue records", used, capacity, ps2_cap);
//
// `used` is the current use (call it at a natural sample point, e.g. once per frame), `capacity`
// what the storage holds right now (0 when it grows without bound), `ps2_cap` the original fixed
// size (0 when unknown). Logged to stderr as `[limit] ...`:
//   - once per run when use reaches 75% of the PS2 cap, and once when it goes past it;
//   - once per run when use reaches the current capacity (storage that could not grow in time);
//   - each new high-water mark of a scene (in steps of 1/8 of the previous mark);
//   - at limit_scene() and at exit: every limit's high-water mark for the scene that ended.
// Cheap enough for per-frame calls; not for per-element calls. EE thread only.
#pragma once
#include <cstdint>

void limit_note(const char *name, uint64_t used, uint64_t capacity, uint64_t ps2_cap = 0);
void limit_scene(const char *label);  // a new scene starts (e.g. a world load)
