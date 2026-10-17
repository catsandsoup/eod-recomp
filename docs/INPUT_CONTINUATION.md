# Controller/input continuation (October 17, 2026)

## Player success

With automatic assignment a first pad controls P1, a keyboard can join P2, and a
second pad controls P2. Disconnecting a controller never moves another connected
controller or the keyboard to a different player. Reconnecting a known controller
restores its free slot. Held gas/steering survives countdown/menu context changes;
background windows publish neutral input. Inputs received during GS backpressure
must reach the same vblank's EE pad packet. Physical controller, focus restoration,
motor and switch-to-pixel latency acceptance require hands-on testing.

## Situation/task

Claude's controller job stopped during investigation, with no edits. The mapper
blocked every held key until release after a context change, and the keyboard's
default P1 reservation made a first pad P2. Existing actuator DMA support was
already merged and did not need to be reimplemented.

## Action and reuse

- Keep the existing mapper and SDL gamepad mapping/rumble paths. Restrict context
  suppression to Enter/Escape (prevents double menu actions); continuous keys and
  unchanged custom bindings remain active. Focus gates both keyboard and pads;
  focus loss stops motors, and background actuator requests cannot restart them.
- Add `KeyboardPort=-1` automatic default. Without a pad keyboard starts on P1;
  after a P1 pad joins, keyboard stays P2 for the session, including disconnects.
  Explicit `0` (off), `1`, `2` settings and `EOD_KEYBOARD_PORT=none|1|2` retain
  their legacy assignment semantics. `EOD_KEYBOARD_PORT=auto` selects automatic.
  An existing saved `KeyboardPort=1` is preserved: change it to -1 in Settings
  and restart to use the new policy. Its prior provenance cannot be inferred.
- Remember per-port serial, else path, else GUID; never evict a connected pad.
  A waiting third pad can occupy a vacant port. Identical pads without unique
  serial/path cannot be distinguished if both are disconnected; GUID fallback
  restores available matching slots in reconnect order, not physical identity.
- Sample SDL input after GS completion and immediately before EE vblank wakeup.
  Windows retains its existing input/GS/IRQ execution order. No renderer queue,
  frame pacing, prediction or simulation changes are introduced.

Reference checked: RoadTripAdventure-recomp at
`0f6d3613e8e32ccda5305227a578b1fb659983a8`:
[Players.cpp](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/src/platform/input/Players.cpp)
remembers device-to-player assignments, and
[Input.cpp](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/src/platform/input/Input.cpp)
publishes neutral pads/stops motors while unfocused. These patterns fit EOD;
its complete input subsystem does not: it targets a different PS2 runtime,
settings/profile system and ImGui frontend. Adapted the patterns within EOD's
existing code rather than replacing its subsystem. SDL's
[serial](https://wiki.libsdl.org/SDL3/SDL_GetGamepadSerial),
[path](https://wiki.libsdl.org/SDL3/SDL_GetGamepadPath), and
[keyboard snapshot](https://wiki.libsdl.org/SDL3/SDL_GetKeyboardState) APIs are
used with explicit fallback/OS focus limitations.

## Result and remaining acceptance

Release build and own signed app succeeded. All 44 CTests passed (none
skipped, 20.59 seconds), including 62 keyboard checks, 92 SDL virtual-pad checks,
and real pad RPC/EE DMA pressure/actuator/freshness checks. Tests reproduced
continuous-key blocking, automatic assignment and reconnect failures before the
fix; removing the focus gates and restoring pre-GS sampling fails the new
regressions. Own app packaging initially failed under Documents due to Finder
resource-fork metadata; packaging succeeded in Application Support instead.

Baseline `ec55ff2` and candidate each completed `tools/test_2p_race.sh` at native
1x/4:3, headless and muted, 6,600 vblanks and exit 0. Both contact sheets show
independent moving players in a split-screen race; race logs reach 59.9 drawn
frames/s. Runs use isolated memory cards, not the owner's saves.

`EOD_INPUT_TIMING=1`, 38 one-second race windows (vblank >= 4300), before/after:

| Counter (microseconds) | Median before/after | p95 before/after | Maximum before/after |
| --- | --- | --- | --- |
| One-second maximum pump gap | 17646.5 / 25217 | 28173 / 48306 | 57783 / 53638 |
| One-second maximum pump + DMA | 24 / 24 | 70 / 367 | 113 / 402 |
| One-second maximum GS block | 18.5 / 43 | 18151 / 31925 | 33840 / 42340 |

These are distributions of per-second maxima, not individual input-event ages.
The pump moved across GS, so its phase and gaps changed; counters do **not**
establish an overall latency improvement. The DMA regression proves the narrower
improvement: input arriving during GS blocking reaches this vblank's packet.
Headless scripts generate no native key events and cannot measure physical or
visible latency. Logs/sheets: `work/input-validation/` in the input worktree;
baseline `work/codex-input-baseline/` in the main worktree. Timings are from one
sequential run per build, with scene variation, not a performance benchmark.
Shared mapper changes and the config parser are separable; SDL assignment,
focus and sampling integration belong to the non-Windows platform. Existing
pad actuator protocol support is unchanged. Automated tests do not establish
physical Rock Candy HIDAPI behavior, motor operation, OS key restoration after
focus changes, or physical-switch-to-visible-pixel latency. The next decisive
hands-on test is two real controllers plus keyboard in a local race, unplug/
replug each, hold gas through countdown, then focus away/back and check no stuck
controls or rumble. Identical controllers without device identity remain limited.
