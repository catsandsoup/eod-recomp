# Player success and STAR handoff

Define success **before** creating or starting each issue, project or task.
Write what the player can observe, how acceptance will be demonstrated, and what
the evidence cannot establish. Carry those criteria into the final handoff.

## Success for the current native baseline

“I open the app with my own installed disc, reach a race without terminal setup,
and play alone or with a local second player. At native 1x PS2 resolution and
original 4:3, heavy carnage stays at the game's original timing and full speed,
without frame skipping. Steering and braking respond promptly; physics, damage,
camera, world and HUD behave faithfully.”

| Player acceptance | Required evidence and limits |
| --- | --- |
| Open the installed app and return to play | First-launch/import and returning-launch journeys on the current packaged app; build success does not prove native dialogs or startup experience. |
| Heavy races remain at original speed | Reproducible solo and local multiplayer runs at 1x/4:3, FrameSkip=0, enhancements off; record game timing, frame cadence, drawn frames and stalls. Verify the original game's cadence for the tested mode against a reference; do not assume every PS2 mode draws at 60 FPS. |
| Controls feel immediate and simulation stays faithful | Hands-on keyboard/pad driving compared with the original reference, including heavy carnage, physics and damage. Headless input tests do not establish visible latency or driving feel. |
| Rendering and presentation match the original | Reference comparisons of representative world/HUD scenes, including split screen and known thin black lines; a byte-exact optimization comparison proves only equivalence for its tested input. |

## STAR handoff — native speed investigation, 2026-10-16

**Situation — what the player experiences.** The Mac port reaches races, but
native full-speed acceptance remains unmet. All source is consolidated on `main`;
`origin/main` is the maintained remote branch. Upstream review is draft
[PR #2](https://github.com/pareidolico/eod-recomp/pull/2). Read
`DEVELOPER_HANDOFF.md` for reproduction details and current implementation state.

**Task — what success requires.** Establish the native player experience above
before expanding quality. Priority is native accuracy/full speed, then LOD/draw
distance and proper widescreen, then 2x/4x/6x resolution, then higher FPS. DLSS is
last. Keep original simulation timing and disable enhancements for the baseline.

**Action — what has been done.** Conservative alpha-test all-fail rejection
passed all 39 CTests. Same-binary race and font comparisons retained byte-exact
displayed images and full 4-MiB VRAM at both 1x and 6x. In the fixed five-frame
replay, draws fell from 143,373 to 102,931 (28.2%). Alternating three runs per
mode, with four replays per run and the cold first replay excluded, measured a
warm median of 230.4 ms before and 184.3 ms after (20.0% lower). Baseline samples
were 223.0–243.2 ms; optimized samples were 180.1–193.1 ms. See the renderer
handoff for the final source checkpoint and packaged-app validation.

**Result — what is verified, and what remains.** The optimized native 1x/4:3
two-player race reached 6,600 vblanks and exited with code 0 in 110.23 seconds.
Race drawn/game-frame samples were 17.3–32.0 FPS (final sample 20.2), with GS at
97–99% and EE waiting at 56–75%. The earlier baseline was roughly 13–25 drawn FPS,
but scene/timing variation prevents a precise live-game speedup claim; the fixed
replay supports the measured 20.0% reduction. Clean exit, exact comparisons and
passing tests establish useful evidence, but **player success is not complete**.
Full-game speed, visible input latency and exhaustive original-reference fidelity
remain unproven.

**Open questions.** What cadence does the original game use in each tested
mode? Does rejection retain correctness beyond the tested alpha/depth/framebuffer
cases and consistently improve heavy full-game runs? Are thin lines and visible control delay still
present on the current native build? Do solo and local multiplayer both meet the
same acceptance criteria?

**Next decisive test.** Verify the original cadence for each mode and compare
matched heavy solo/local multiplayer scenes against that reference, with no
competing GPU workload and automated runs headless and muted. Record timing,
cadence, drawn frames, stalls, fidelity evidence and the exact source revision.
Then arrange hands-on
original-reference comparison for visible latency and driving feel. Update this
Result with measured outcomes and remaining gaps; do not promote expected gains
to verified player acceptance.

## Reusable handoff

- **Success first:** “As a player, I can …”; observable acceptance, proof needed,
  and evidence limits.
- **Situation:** the player-visible problem and reproducible starting state.
- **Task:** the outcome required, scope and priority.
- **Action:** changes actually made, source checkpoint and validation performed.
- **Result:** measured outcomes against acceptance; explicitly state unmet
  criteria, uncertainties and the next decisive test. Keep proposed work separate.
