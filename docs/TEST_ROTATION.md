# Verification scene rotation (owner rule, 2026-11-19)

Anoka alone is too small to reach every code path. Every Phase A smoke test and every
Phase B suite rotates through these, and never repeats the same track/vehicle pair twice
in a row:

1. **Biggest map, maximum load:** Dundas (or the largest track in the `[CATALOG]` list) with
   the most cars the event allows, `DrawDistance` / `Detail` at maximum, weapons firing from
   the start (`EOD_CHICKENS=1`, l2 every 30 vblanks).
2. **Night with heavy lighting:** a night track or night event with many flood or stadium
   lights (Anoka Night, arena or stadium events), so flares, glows, light sprites and night
   fog run.
3. **Odd vehicles, cycled:** the bus, the hearse, the limo and every other special or unique
   vehicle in the `[CATALOG]` list, one per run in rotation. They use different skins, bones,
   panels and debris than ordinary cars.
4. **Variety beyond that:** a different event type each run (race, trailer race, arena or
   demolition, gauntlet, flag, explosive chicken), plus 2P/4P split-screen. Head-to-head
   events pick their event in the native lobby (`src/session_start.cpp:572`).

Use names exactly as the run's `[CATALOG]` list prints them, and check that
`[SESSION] start:` is in the log. Keep a coverage list (`EOD_COV_LOG=1` /
`EOD_RENDER_TRACE`) across runs. A replaced function no scene calls gets a deterministic
replay.

Runs and builds go in the foreground (blocking, timeout <= 600000 ms per command, chained
for longer runs). An agent never ends its turn just to wait.
