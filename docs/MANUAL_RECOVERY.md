# Human manual recovery

## Player success

With `[Gameplay] ManualRecovery=1`, local humans may remain wrong-way,
off-track or upside-down beyond the native recovery countdown without that
countdown relocating them. Its warning disappears. SELECT remains available
under the native recovery eligibility rules. AI recovery and ordinary race
initialization/restart remain native. Default `0` preserves the original game.
This does not remove solid world boundaries or other out-of-world safety code.

## Situation and task

The owner requested removal of the “Get back in the race!” countdown, while
keeping manual SELECT and AI recovery. Hiding text alone would still teleport
the car; clearing its deadline would disable the direct SELECT recovery path.

## Action and native evidence

SLUS-209.10 function `001B8370` evaluates the native recovery conditions and
updates their timers. Its automatic-expiry branch calls `001B63E8` at return
address `001B8884`, gated by the word at `00485098`. The human wrapper disables
that word only during this native call and restores its exact value afterward.
It retains native detection/timers and native EE register side effects.

The participant is argument 1, its car is at +4, and its HUD stats are +0x40.
The stats warning flag +128 is participant +192; the deadline +132 is participant
+196. The original deadline is the PS2 float sum of timer +68 and +72. The
wrapper reproduces that same deadline when active, then clears only the warning.
Manual input `0018EA04` requires a positive deadline before calling `001B63E8`;
when ineligible, its existing `001DA3E0` event-manager path remains unchanged.

Only PCAR (`car+12 == 0x50434152`) local-player type `0x50` qualifies.
Constructor `001D92A8` creates these local-player cars; AI constructor
`001D92A0` uses type `0x51`. AI/remote/special cars, disabled settings and invalid
host-inspected pointers fall through to the original. No reset function, spawn,
restart, world resource or physics implementation is patched.

Reuse check: [RoadTripAdventure recomp overrides](https://github.com/silentsudin/RoadTripAdventure-recomp/blob/0f6d3613e8e32ccda5305227a578b1fb659983a8/src/game/overrides.cpp)
was rechecked for this issue. Its native-wrapper approach fits; its game-specific
addresses and recovery ABI do not. This uses EOD's existing HLE wrap mechanism,
not an independent recovery implementation.

## Result and limits

CPU contract: a passthrough wrapper first failed “human expired countdown cannot
reset”; the implemented wrapper passes. Coverage includes native register effects,
default-off behavior, AI passthrough, exact global restoration, native SELECT
deadline, non-countdown modes, invalid pointers, and config save/load.

Live evidence and installation are recorded below after validation. Automated
runs isolate memory cards and use headless/muted mode. A controlled native
predicate test is not proof of physically driving every track, overturning every
car or traversing every out-of-world boundary.

Live controlled test (2026-10-17): two 7,500-vblank Dennison split-screen runs,
separate empty memory cards, native 1x rendering. Generated-code-only validation
fed the upside-down predicate from native race time 10–50 seconds, without
changing vehicle physics. The production wrapper and original timer/reset/input
functions executed. Human car indices 8/9 were both type 0x50; AI indices 0–7
were 0x51. A separate AI index-2 predicate attempt did not produce an AI reset,
so runtime AI recovery itself is not claimed tested.

- ManualRecovery=0: each human had four automatic reset calls at RA `001B8884`,
  and one SELECT reset at RA `0018EA04`. Both HUDs showed the countdown.
- ManualRecovery=1: zero automatic reset calls; each human still had one SELECT
  reset at `0018EA04`. All sampled HUD warning flags were zero. P2 retained
  deadline 18.013 at native time 45.7624 before its manual reset, proving manual
  eligibility survives beyond expiry. Both runs reached their bound and exited 0.
- Screenshots at frame 5100 confirm the visible warning in baseline and absence
  in both modified viewports. Positions differ between runs; this is a behavior
  comparison, not deterministic image parity.

Evidence (ignored): `work/recovery/injected-{off,on}/out.log`,
`frame_05100.png`, script `work/recovery/race.sh`. The instrumented generated
sources are not shipped; the production sources were restored before final build.
Remaining limit: no exhaustive physical upside-down/off-track drive across all
tracks, no runtime AI recovery event, no claim that independent hard-world safety
teleports are disabled. AI passthrough is covered by the CPU contract and unchanged
native call sites. Next decisive regression test is ordinary human exploration
with SELECT on each player's physical controller.

Production validation: all 48 CTest cases passed (15.95s), including
ManualRecovery. Full Release app build completed; strict deep codesign
verification passed. Neither generated validation predicate nor diagnostic log
strings remains in the restored generated files.
